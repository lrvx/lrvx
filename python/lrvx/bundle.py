"""Reproducibility bundle format — `lrvx bundle pack/replay/validate`.

A bundle is a single self-contained tarball that encodes everything
needed to reproduce a backtest result on another machine: strategy
code, the captured tape it ran against, the engine version it was
built with, and the expected output it should reproduce. Packing
captures the run; validating replays it and compares the result with
the expected output field by field (trade count, every fill's symbol,
side, price and quantity, total filled quantity, and the number of
orders cancelled and modified for a bundle packed since cancels and
modifies are replayed), along with the strategy and tape hashes, the
engine version and, when the bundle ships an expected `.lrvxrun`, the
trace: every signal, order-event and fill record field by field for a
bundle whose manifest says its records are comparable (all but
`run_ts_ns`, the wall clock), the record counts
for one packed before that, and the fill count alone for one packed
before its trace recorded signals (see `_trace_holds_signals`).

Bundle layout:

    bundle.tar.zst
    ├── manifest.json          versions + integrity hashes + config
    ├── strategy/
    │   └── strategy.py        the user's strategy module
    ├── config/
    │   └── params.json        runtime params (slippage, queue model,
    │                          initial capital, ...)
    ├── tape/                  the tape this run drove against
    │   ├── manifest.json
    │   └── trades-*.bin
    └── expected_output.json   PnL, fill sequence, final positions

Phase 1 (this build) covers the pack-replay-validate cycle for
single-strategy single-tape bundles. Multi-strategy ensembles and
network/exchange-stub bundles land in follow-up work.
"""
from __future__ import annotations

import hashlib
import importlib.util
import io
import json
import logging
import shutil
import sys
import tarfile
import tempfile
import time
import warnings
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional


log = logging.getLogger("lrvx.bundle")


class BundleTrustWarning(RuntimeWarning):
    """Raised via ``warnings.warn`` every time a bundle's strategy code
    is about to execute. Bundles are meant to be shared, so this is not
    an error -- replaying a bundle from someone else's machine is the
    feature -- but callers should see it rather than have arbitrary
    code run silently."""


BUNDLE_FORMAT_VERSION = 1
_MANIFEST_NAME = "manifest.json"
_EXPECTED_OUTPUT_NAME = "expected_output.json"
_EXPECTED_LRVXRUN_DIR = "expected.lrvxrun"
# Bundles packed under the FLOX name carry these instead.
_LEGACY_EXPECTED_RUN_DIR = "expected.floxrun"
_LEGACY_VERSION_KEY = "flox_version"
_SIGNALS_RECORDED_KEY = "lrvxrun_signals_recorded"
_RECORDS_COMPARABLE_KEY = "lrvxrun_records_comparable"
# Pack sent the strategy's cancels and modifies to the simulator and recorded
# them. A bundle without the key was packed when they were dropped, and its
# replay drops them too.
_CANCELS_MODIFIES_KEY = "cancels_and_modifies_routed"
_CANCEL_MODIFY_SIGNALS = frozenset({"cancel", "cancel_all", "modify"})
_COMPARED_RECORDS = "signal, order event and fill records"
_COMPARED_ALL = "signals, order events, fills"
# The trace record fields two replays of one bundle need not agree on.
# run_ts_ns is the wall clock when the record was produced.
_NOT_REPRODUCIBLE_FIELDS = frozenset({"run_ts_ns"})
_RECORD_KINDS = (
    ("lrvxrun_signals", "read_all_signals"),
    ("lrvxrun_order_events", "read_all_order_events"),
    ("lrvxrun_fills", "read_all_fills"),
)
_COMPARED_FILLS = "fills"
_STRATEGY_DIR = "strategy"
_CONFIG_DIR = "config"
_TAPE_DIR = "tape"


def _lrvx_version() -> str:
    """Best-effort lrvx version. Falls back to 'unknown' if the
    binding does not expose one — bundles still pin a SHA via
    package_dir hashing as a backup integrity gate."""
    try:
        import lrvx
        return getattr(lrvx, "__version__", "unknown")
    except Exception:
        return "unknown"


def _sha256_file(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as fh:
        for chunk in iter(lambda: fh.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def _sha256_dir(p: Path) -> str:
    """Stable SHA over directory: sort entries, hash (relpath, content)
    pairs in order. Same input tree always produces the same hash."""
    h = hashlib.sha256()
    base = p.resolve()
    for f in sorted(base.rglob("*")):
        if not f.is_file():
            continue
        rel = f.relative_to(base).as_posix().encode("utf-8")
        h.update(rel)
        h.update(b"\0")
        h.update(_sha256_file(f).encode("ascii"))
        h.update(b"\n")
    return h.hexdigest()


def _load_strategy_module(path: Path):
    spec = importlib.util.spec_from_file_location("lrvx_bundle_strategy", path)
    if spec is None or spec.loader is None:
        raise ImportError(f"could not load strategy module at {path}")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["lrvx_bundle_strategy"] = mod
    spec.loader.exec_module(mod)
    return mod


def _find_strategy_class(mod) -> type:
    import lrvx

    candidates = [
        v for v in vars(mod).values()
        if isinstance(v, type)
        and issubclass(v, lrvx.Strategy)
        and v is not lrvx.Strategy
    ]
    if not candidates:
        raise ValueError(
            f"no lrvx.Strategy subclass found in {mod.__name__}"
        )
    if len(candidates) > 1:
        raise ValueError(
            f"multiple lrvx.Strategy subclasses in {mod.__name__}: "
            f"{[c.__name__ for c in candidates]}; pack only supports one"
        )
    return candidates[0]


# ── Run helper: drive a strategy through a tape with a simulator ──


def _run_strategy_against_tape(
    strategy_path: Path,
    tape_path: Path,
    *,
    symbol_name: str = "BTCUSDT",
    exchange: str = "bundle",
    tick_size: Optional[float] = 0.01,
    slippage_model: str = "none",
    slippage_params: Optional[Dict[str, float]] = None,
    lrvxrun_out: Optional[Path] = None,
    route_cancels_and_modifies: bool = True,
) -> Dict[str, Any]:
    """Replay the trades in ``tape_path`` through the strategy at
    ``strategy_path`` using a SimulatedExecutor with the given
    config. Returns a dict capturing fill sequence and totals; this
    is what `pack` records as expected output and `validate`
    compares against.

    ``tick_size`` is declared on the symbol, so order prices off it are
    refused; ``None`` declares no tick and leaves prices unchecked, as a
    symbol registered without one is.

    ``route_cancels_and_modifies`` sends the strategy's cancel, cancel-all
    and modify signals to the simulator, traces them and counts them in the
    returned dict. False drops them, as pack did before the bundle's manifest
    had ``cancels_and_modifies_routed``, so such a bundle replays as it was
    packed."""
    import lrvx

    mod = _load_strategy_module(strategy_path)
    StratCls = _find_strategy_class(mod)

    registry = lrvx.SymbolRegistry()
    sym = registry.add_symbol(exchange, symbol_name, tick_size=tick_size)

    sim = lrvx.SimulatedExecutor()
    sim.set_symbol_registry(registry)
    sim.set_default_slippage(
        model=slippage_model,
        ticks=int((slippage_params or {}).get("ticks", 0)),
        tick_size=float((slippage_params or {}).get("tick_size", 0.0)),
        bps=float((slippage_params or {}).get("bps", 0.0)),
        impact_coeff=float((slippage_params or {}).get("impact_coeff", 0.0)),
    )

    next_id = [1]
    # The timestamp of the tape event being replayed: the feed_ts_ns of every
    # record the trace takes while that event is handled.
    feed_ts = [0]

    # Optional .lrvxrun capture. When `lrvxrun_out` is set we mirror
    # every signal / order / fill into a TraceRecorder so the bundle
    # can ship the per-run trace alongside the legacy JSON summary.
    from lrvx._lrvx import FillLiquidity, OrderEventKind, TraceRecorder

    rec: Any = None
    trace_errors: List[Exception] = []
    if lrvxrun_out is not None:
        rec = TraceRecorder(
            path=str(lrvxrun_out),
            strategy_id=strategy_path.stem,
            strategy_hash=_sha256_file(strategy_path)[:16],
            run_started_ns=time.time_ns(),
        )

    # Signal.order_type and SimulatedExecutor.submit_order now spell the
    # order types the same way, so this is the accepted-signal filter
    # rather than a translation table. The index is also the
    # bundle-local trace code (0=market, 1=limit predate the conditional
    # types; the rest extend that convention in enum order), so the
    # order is frozen: appending is safe, reordering is not.
    _EXEC_ORDER_TYPES = (
        "market",
        "limit",
        "stop_market",
        "stop_limit",
        "tp_market",
        "tp_limit",
        "trailing_stop",
    )
    _TRACE_ORDER_TYPE = {
        name: code for code, name in enumerate(_EXEC_ORDER_TYPES)
    }
    # order id -> (side, order type, price, quantity) as last sent, for every
    # order this run placed: what a cancel or modify record names.
    sent: Dict[int, tuple] = {}
    actions = {"cancel_count": 0, "modify_count": 0}

    def write_action(sig_name: str, signal_id: int,
                     events: List[tuple]) -> None:
        """A cancel, cancel-all or modify signal and the order event of each
        order it acted on, written like the run's submits."""
        if rec is None or trace_errors:
            return
        try:
            ts = time.time_ns()
            rec.write_signal(
                run_ts_ns=ts, feed_ts_ns=feed_ts[0], signal_id=signal_id,
                name=sig_name, symbol_ids=[int(sym)],
            )
            for kind, oid, (side, order_type, price, qty) in events:
                rec.write_order_event(
                    run_ts_ns=ts, feed_ts_ns=feed_ts[0], order_id=oid,
                    parent_signal_id=oid, symbol_id=int(sym),
                    event_kind=kind,
                    side=0 if side == "buy" else 1,
                    order_type=_TRACE_ORDER_TYPE[order_type],
                    price_raw=int(price * 1e8), qty_raw=int(qty * 1e8),
                )
        except Exception as e:
            trace_errors.append(e)

    def on_cancel_or_modify(sig: Any, order_type: str) -> None:
        # Routed as BacktestRunner routes them to its simulator. An order
        # counts as cancelled, and gets a CANCEL record, when the simulator
        # still held it; as modified, with a MODIFY record, when the
        # simulator took the replacement.
        oid = int(getattr(sig, "order_id", 0) or 0)
        if order_type == "cancel":
            live = oid in sent and sim.order_is_live(oid)
            sim.cancel_order(oid)
            done = [(OrderEventKind.CANCEL, oid, sent[oid])] if live else []
            actions["cancel_count"] += len(done)
        elif order_type == "cancel_all":
            live = [o for o in sent if sim.order_is_live(o)]
            sim.cancel_all(int(sym))
            done = [(OrderEventKind.CANCEL, o, sent[o]) for o in live]
            actions["cancel_count"] += len(done)
        else:
            new_price = float(getattr(sig, "new_price", 0.0) or 0.0)
            new_qty = float(getattr(sig, "new_quantity", 0.0) or 0.0)
            taken = sim.modify_order(oid, new_price, new_qty, int(sym))
            done = []
            if taken and oid in sent:
                side, kind = sent[oid][0], sent[oid][1]
                sent[oid] = (side, kind, new_price, new_qty)
                done = [(OrderEventKind.MODIFY, oid, sent[oid])]
            actions["modify_count"] += len(done)
        write_action(order_type, oid, done)

    def on_signal(sig: Any) -> None:
        order_type = (getattr(sig, "order_type", "") or "").lower()
        if order_type in _CANCEL_MODIFY_SIGNALS and route_cancels_and_modifies:
            on_cancel_or_modify(sig, order_type)
            return
        if order_type not in _TRACE_ORDER_TYPE:
            return
        side = (getattr(sig, "side", "") or "").lower()
        oid = int(getattr(sig, "order_id", 0) or 0)
        if oid == 0:
            oid = next_id[0]
            next_id[0] += 1
        price = float(getattr(sig, "price", 0.0))
        qty = float(getattr(sig, "quantity", 0.0))
        sim.submit_order(
            oid, side, price, qty,
            type=order_type, symbol=int(sym),
            trigger=float(getattr(sig, "trigger_price", 0.0) or 0.0),
            trailing_offset=float(getattr(sig, "trailing_offset", 0.0) or 0.0),
            trailing_bps=int(getattr(sig, "trailing_bps", 0) or 0),
        )
        sent[oid] = (side, order_type, price, qty)
        if rec is not None and not trace_errors:
            # The runner logs what a signal callback raises and carries on,
            # so a failed write is kept here and raised once the run is over.
            try:
                ts = time.time_ns()
                rec.write_signal(
                    run_ts_ns=ts, feed_ts_ns=feed_ts[0], signal_id=oid, name=order_type,
                    symbol_ids=[int(sym)],
                )
                rec.write_order_event(
                    run_ts_ns=ts, feed_ts_ns=feed_ts[0], order_id=oid, parent_signal_id=oid,
                    symbol_id=int(sym),
                    event_kind=OrderEventKind.SUBMIT,
                    side=0 if side == "buy" else 1,
                    order_type=_TRACE_ORDER_TYPE[order_type],
                    price_raw=int(price * 1e8), qty_raw=int(qty * 1e8),
                )
            except Exception as e:
                trace_errors.append(e)

    runner = lrvx.Runner(registry, on_signal=on_signal)
    try:
        strat = StratCls([int(sym)])
    except TypeError:
        strat = StratCls()
    runner.add_strategy(strat)
    runner.start()

    from lrvx.tape import replay_tape

    # The tape's symbol ids are assigned by whoever recorded it and bear no
    # relation to the single symbol registered above. Forwarding them
    # unchanged fed another instrument's trades to the strategy under this
    # symbol's name, and `bundle validate` still reported OK. Feed only the
    # tape symbol this bundle is about, remapped onto the registered id.
    from lrvx.tape import inspect_tape

    tape_ids = list(getattr(inspect_tape(tape_path), "symbol_ids", []) or [])
    if len(tape_ids) > 1:
        raise ValueError(
            f"bundle replay: the tape holds {len(tape_ids)} symbols "
            f"({tape_ids}) but the bundle registers one ({symbol_name}). "
            f"Re-pack the bundle from a single-symbol tape.")
    tape_sym = tape_ids[0] if tape_ids else int(sym)
    delivered = [0]
    fills_traced = [0]

    def trace_new_fills() -> None:
        """Write the fills the simulator has made since the last call.

        A fill is traced when it happens, so its run_ts_ns is the wall clock
        of that moment and orders with the run's other records; feed_ts_ns
        is the simulator's own time of the fill, which is the tape event it
        filled on; and fill_id numbers the run's fills 1, 2, 3, ... Every
        fill used to be written after the run with one shared run_ts_ns, no
        feed_ts_ns, and its order's id as its fill id, so the two fills of
        one order had the same id."""
        if rec is None or int(sim.fill_count) == fills_traced[0]:
            return
        fills = sim.fills_list()
        for f in fills[fills_traced[0]:]:
            fills_traced[0] += 1
            rec.write_fill(
                run_ts_ns=time.time_ns(),
                feed_ts_ns=int(f.get("timestamp_ns", 0) or 0) or feed_ts[0],
                order_id=int(f.get("order_id", 0) or 0),
                fill_id=fills_traced[0],
                price_raw=int(float(f["price"]) * 1e8),
                qty_raw=int(float(f["quantity"]) * 1e8),
                symbol_id=int(f["symbol"]),
                side=0 if str(f["side"]).lower() == "buy" else 1,
                liquidity=FillLiquidity.TAKER,
            )

    def on_trade(ts_ns, sym_id, price, qty, side):
        if int(sym_id) != tape_sym:
            return
        feed_ts[0] = int(ts_ns)
        sim.advance_clock(int(ts_ns))
        sim.on_trade_qty(int(sym), float(price), float(qty), side == 0)
        trace_new_fills()
        runner.on_trade(int(sym), float(price), float(qty), side == 0, int(ts_ns))
        trace_new_fills()
        delivered[0] += 1

    replay_tape(tape_path, on_trade=on_trade)
    n = delivered[0]
    runner.stop()

    fills = sim.fills_list()
    total_qty = sum(float(f["quantity"]) for f in fills)

    if rec is not None:
        if trace_errors:
            raise RuntimeError(
                f"bundle trace {lrvxrun_out}: a signal or order event could "
                f"not be written: {type(trace_errors[0]).__name__}: "
                f"{trace_errors[0]}") from trace_errors[0]
        trace_new_fills()
        rec.set_run_ended_ns(time.time_ns())
        rec.close()
    # The JSON summary identifies a fill by (symbol, side, price,
    # quantity), as it always has; order and fill ids are compared in the
    # trace of a bundle whose manifest has lrvxrun_records_comparable.
    # The cancel and modify counts are only in the summary of a run that
    # routed them: a bundle packed before has neither key.
    summary = actions if route_cancels_and_modifies else {}
    return {
        **summary,
        "trade_count": int(n),
        "fill_count": len(fills),
        "fills": [
            {
                "symbol": int(f["symbol"]),
                "side": str(f["side"]),
                "price": float(f["price"]),
                "quantity": float(f["quantity"]),
            }
            for f in fills
        ],
        "total_filled_quantity": total_qty,
    }


# ── Pack ───────────────────────────────────────────────────────────


def pack_bundle(
    *,
    strategy: Path,
    tape: Path,
    output: Path,
    config: Optional[Dict[str, Any]] = None,
    symbol_name: str = "BTCUSDT",
    exchange: str = "bundle",
    tick_size: Optional[float] = None,
) -> Path:
    """Build a bundle from a strategy file and a tape directory.
    Writes a tar (or tar.zst if zstandard is available) at ``output``.
    Returns the output path.

    ``tick_size`` is the symbol's tick, if the run declares one: order
    prices off it are refused, at pack and at every replay. ``None`` (the
    default) declares none, so prices are not checked against a tick --
    the same as running the strategy on a symbol registered without one.
    The tick is taken from this argument only; nothing is read from the
    tape. The manifest records ``tick_size_declared`` so replay does the
    same."""
    strategy = Path(strategy).expanduser().resolve()
    tape = Path(tape).expanduser().resolve()
    output = Path(output).expanduser().resolve()

    if not strategy.is_file():
        raise FileNotFoundError(f"strategy file not found: {strategy}")
    if not tape.is_dir():
        raise FileNotFoundError(f"tape directory not found: {tape}")

    cfg = dict(config or {})
    slippage_model = str(cfg.get("slippage_model", "none"))
    slippage_params = dict(cfg.get("slippage_params", {}))

    lrvxrun_dir = Path(tempfile.mkdtemp(prefix="lrvx-bundle-lrvxrun-")) / "expected.lrvxrun"
    expected = _run_strategy_against_tape(
        strategy, tape,
        symbol_name=symbol_name,
        exchange=exchange,
        tick_size=tick_size,
        slippage_model=slippage_model,
        slippage_params=slippage_params,
        lrvxrun_out=lrvxrun_dir,
    )

    manifest = {
        "bundle_format_version": BUNDLE_FORMAT_VERSION,
        "lrvx_version": _lrvx_version(),
        "created_at_ns": time.time_ns(),
        "strategy_filename": strategy.name,
        "strategy_sha256": _sha256_file(strategy),
        "tape_sha256": _sha256_dir(tape),
        "symbol_name": symbol_name,
        "exchange": exchange,
        # 0.01 is the registry's default tick, recorded so a reader that
        # predates tick_size_declared still finds a number.
        "tick_size": 0.01 if tick_size is None else tick_size,
        "tick_size_declared": tick_size is not None,
        # The expected trace holds this run's signals and order events. A
        # bundle without the key was packed when they could be missing
        # (see _trace_holds_signals).
        _SIGNALS_RECORDED_KEY: True,
        # Every field of its records but the wall clock is the same at each
        # replay, so validate compares them one by one. A bundle without the
        # key is compared on its record counts.
        _RECORDS_COMPARABLE_KEY: True,
        _CANCELS_MODIFIES_KEY: True,
        "slippage_model": slippage_model,
        "slippage_params": slippage_params,
    }

    output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(output, "w") as tf:
        # manifest
        m_bytes = json.dumps(manifest, indent=2, sort_keys=True).encode("utf-8")
        info = tarfile.TarInfo(_MANIFEST_NAME)
        info.size = len(m_bytes)
        tf.addfile(info, io.BytesIO(m_bytes))

        # strategy
        tf.add(str(strategy), arcname=f"{_STRATEGY_DIR}/{strategy.name}")

        # config
        c_bytes = json.dumps(cfg, indent=2, sort_keys=True).encode("utf-8")
        info = tarfile.TarInfo(f"{_CONFIG_DIR}/params.json")
        info.size = len(c_bytes)
        tf.addfile(info, io.BytesIO(c_bytes))

        # tape (as a directory tree)
        tf.add(str(tape), arcname=_TAPE_DIR)

        # expected output (legacy JSON summary, kept for back-compat)
        e_bytes = json.dumps(expected, indent=2, sort_keys=True).encode("utf-8")
        info = tarfile.TarInfo(_EXPECTED_OUTPUT_NAME)
        info.size = len(e_bytes)
        tf.addfile(info, io.BytesIO(e_bytes))

        # expected output (.lrvxrun directory). Older bundles produced
        # before the .lrvxrun ride-along landed have no `.lrvxrun/`
        # entry; `replay_bundle` falls back to JSON-only diff for those.
        if lrvxrun_dir.is_dir():
            tf.add(str(lrvxrun_dir), arcname=_EXPECTED_LRVXRUN_DIR)

    return output


# ── Replay / validate ──────────────────────────────────────────────


class BundleSecurityError(Exception):
    """Raised when a bundle's tar archive contains a member that would
    write outside the extraction directory, or when replaying it would
    run code the caller has not acknowledged as trusted."""


@dataclass
class BundleResult:
    actual: Dict[str, Any]
    expected: Dict[str, Any]
    manifest: Dict[str, Any]
    bundle_path: str
    matches: bool = False
    diff: List[str] = field(default_factory=list)


def _extract(bundle_path: Path) -> Path:
    """Extract `bundle_path` into a fresh temp directory and return it.

    A bundle is, by design, something you hand to someone else to
    reproduce a result -- the whole feature only makes sense for
    tarballs from people you have not audited. Two things need
    guarding against on that path:

    * Path traversal (`../etc/passwd`, an absolute member path, a
      symlink whose target resolves outside the tree). `tarfile`
      picked up a "data" extraction filter under PEP 706 that refuses
      these, but it only rejects by default starting with Python
      3.14; bundles are documented to support 3.10+, and CI runs
      those older interpreters, where `extractall()` without a filter
      still follows `../` members. Checking every member's resolved
      destination against the extraction root here does not depend on
      which interpreter this happens to run under.
    * Arbitrary code execution: `replay_bundle`/`validate_bundle` load
      and run `strategy/strategy.py` from whatever was inside the
      tarball (see the warning where that happens). Nothing about
      *extraction* can fix that -- the strategy module has to run for
      the bundle to do its job -- so that risk is called out loudly at
      the point it actually executes, not here.
    """
    tmp = Path(tempfile.mkdtemp(prefix="lrvx-bundle-"))
    tmp_resolved = tmp.resolve()
    with tarfile.open(bundle_path, "r") as tf:
        for member in tf.getmembers():
            dest = (tmp / member.name).resolve()
            try:
                dest.relative_to(tmp_resolved)
            except ValueError:
                raise BundleSecurityError(
                    f"refusing to extract {bundle_path}: member "
                    f"{member.name!r} would land outside the bundle root"
                ) from None
            if member.issym() or member.islnk():
                link_dest = ((tmp / member.name).parent / member.linkname).resolve()
                try:
                    link_dest.relative_to(tmp_resolved)
                except ValueError:
                    raise BundleSecurityError(
                        f"refusing to extract {bundle_path}: link "
                        f"{member.name!r} points outside the bundle root"
                    ) from None
        if hasattr(tarfile, "data_filter"):
            # Python's own "data" filter: also strips device/FIFO
            # members, high-bit permission modes, and the like. Belt
            # and suspenders with the manual check above, which is the
            # part that still applies on interpreters without this.
            tf.extractall(tmp, filter="data")
        else:
            tf.extractall(tmp)
    return tmp


def _trace_counts(trace: Path) -> Dict[str, int]:
    """Signal, order-event and fill counts of a `.lrvxrun` directory.
    Raises whatever the reader raises on a trace it cannot read."""
    from lrvx._lrvx import TraceReader

    reader = TraceReader(str(trace))
    return {
        "lrvxrun_signal_count": len(reader.read_all_signals()),
        "lrvxrun_order_count": len(reader.read_all_order_events()),
        "lrvxrun_fill_count": len(reader.read_all_fills()),
    }


def _trace_records(trace: Path) -> Dict[str, List[Dict[str, Any]]]:
    """Every signal, order-event and fill record of a `.lrvxrun`
    directory, without the fields that differ between replays."""
    from lrvx._lrvx import TraceReader

    reader = TraceReader(str(trace))
    return {
        key: [{k: v for k, v in rec.items() if k not in _NOT_REPRODUCIBLE_FIELDS}
              for rec in getattr(reader, read)()]
        for key, read in _RECORD_KINDS
    }


def _trace_holds_signals(manifest: Dict[str, Any],
                         expected_counts: Dict[str, int]) -> bool:
    """Whether a bundle's expected trace can be compared on its signals
    and order events.

    A manifest with ``lrvxrun_signals_recorded`` says so. One without the
    key was packed when a signal whose id did not fit 32 bits was dropped
    from the trace together with its order event, and the runner's order
    ids do not fit: such a trace has neither. A trace from that time that
    does hold a signal or an order event recorded them all, because the
    ids of one run are all in the same range."""
    if _SIGNALS_RECORDED_KEY in manifest:
        return bool(manifest[_SIGNALS_RECORDED_KEY])
    return (expected_counts["lrvxrun_signal_count"] > 0
            or expected_counts["lrvxrun_order_count"] > 0)


def _manifest_tick(manifest: Dict[str, Any]) -> Optional[float]:
    """The tick a bundle's replay declares, or None for no tick.

    A manifest with ``tick_size_declared`` says it outright. One without
    the key was packed before the flag existed, when pack always declared
    its tick (0.01 unless given): it keeps declaring ``tick_size`` (0.01
    when absent), so an old bundle replays exactly as it always has."""
    if "tick_size_declared" in manifest:
        if not manifest["tick_size_declared"]:
            return None
        return float(manifest.get("tick_size", 0.01))
    return float(manifest.get("tick_size", 0.01))


def _diff_records(key: str, actual: List[Dict[str, Any]],
                  expected: List[Dict[str, Any]]) -> List[str]:
    """One line per record that differs, naming its index and fields."""
    diffs: List[str] = []
    for i in range(max(len(actual), len(expected))):
        if i >= len(actual) or i >= len(expected):
            side = "actual" if i >= len(actual) else "expected"
            other = expected[i] if i < len(expected) else actual[i]
            diffs.append(f"{key}[{i}]: missing from {side}; the other has {other!r}")
            continue
        a, e = actual[i], expected[i]
        fields = [f for f in sorted(set(a) | set(e)) if a.get(f, "<missing>") != e.get(f, "<missing>")]
        if fields:
            diffs.append(f"{key}[{i}]: " + ", ".join(
                f"{f} actual={a.get(f, '<missing>')!r} expected={e.get(f, '<missing>')!r}"
                for f in fields))
    return diffs


def _diff_dicts(actual: Dict[str, Any], expected: Dict[str, Any]) -> List[str]:
    diffs: List[str] = []
    keys = sorted(set(actual.keys()) | set(expected.keys()))
    record_keys = {key for key, _ in _RECORD_KINDS}
    for k in keys:
        a = actual.get(k, "<missing>")
        e = expected.get(k, "<missing>")
        if a == e:
            continue
        if k in record_keys and isinstance(a, list) and isinstance(e, list):
            diffs.extend(_diff_records(k, a, e))
        else:
            diffs.append(f"{k}: actual={a!r} expected={e!r}")
    return diffs


def replay_bundle(bundle_path: Path) -> BundleResult:
    """Extract a bundle, replay the strategy against the bundled tape
    with the bundled config, and return the actual output alongside
    the expected output. Does not compare; use :func:`validate_bundle`
    for the assertion.

    **This runs the strategy code shipped inside the bundle.** A
    bundle is a tarball someone else can hand you -- that is the
    entire point of the format -- and replaying one calls
    ``exec_module`` on ``strategy/strategy.py`` from it exactly the
    way ``python -c "import untrusted_module"`` would. Only call this
    (directly or via :func:`validate_bundle`, or ``lrvx bundle
    validate`` / ``lrvx bundle replay`` on the CLI) on bundles from a
    source you trust, the same way you would before running any other
    downloaded code. Nothing in the bundle format authenticates who
    produced it.
    """
    bundle_path = Path(bundle_path).expanduser().resolve()
    if not bundle_path.is_file():
        raise FileNotFoundError(f"bundle not found: {bundle_path}")

    msg = (
        f"replay_bundle({bundle_path}) is about to execute "
        "strategy/strategy.py from inside this bundle. A bundle is "
        "designed to be shared, so treat this exactly like running "
        "code you downloaded from someone else -- only replay or "
        "validate bundles from a source you trust."
    )
    log.warning(msg)
    warnings.warn(msg, BundleTrustWarning, stacklevel=2)

    work = _extract(bundle_path)
    try:
        manifest = json.loads((work / _MANIFEST_NAME).read_text())
        if manifest.get("bundle_format_version") != BUNDLE_FORMAT_VERSION:
            raise ValueError(
                f"bundle format {manifest.get('bundle_format_version')!r} "
                f"is not supported by this build (expected "
                f"{BUNDLE_FORMAT_VERSION})"
            )
        expected = json.loads((work / _EXPECTED_OUTPUT_NAME).read_text())
        strat_name = manifest["strategy_filename"]
        strat_path = work / _STRATEGY_DIR / strat_name
        tape_path = work / _TAPE_DIR

        # Capture a fresh `.lrvxrun` from the replay so we can diff
        # signal / order / fill counts against the bundled expected
        # trace. Older bundles have no `expected.lrvxrun/`; we still
        # produce a fresh one for the
        # actual side and the comparison degrades to "expected has
        # no .lrvxrun, skipping that diff dimension".
        actual_lrvxrun = work / "actual.lrvxrun"
        actual = _run_strategy_against_tape(
            strat_path, tape_path,
            symbol_name=manifest.get("symbol_name", "BTCUSDT"),
            exchange=manifest.get("exchange", "bundle"),
            tick_size=_manifest_tick(manifest),
            slippage_model=manifest.get("slippage_model", "none"),
            slippage_params=manifest.get("slippage_params") or {},
            lrvxrun_out=actual_lrvxrun,
            route_cancels_and_modifies=bool(manifest.get(_CANCELS_MODIFIES_KEY)),
        )

        # Integrity check: the manifest carries a hash of the strategy
        # file and of the tape directory plus the engine version the
        # bundle was built with, but until now nothing ever read them
        # back -- a bundle whose strategy.py was swapped for something
        # else after packing (with expected_output.json re-recorded to
        # match) reported `matches=True` as long as
        # bundle_format_version was untouched. Recomputing both hashes
        # from what actually got extracted and folding them into the
        # same actual/expected dicts the JSON diff already compares
        # makes a swap show up as an ordinary diff line instead of
        # passing silently.
        #
        # This is not a substitute for signing the manifest: it is
        # stored unsigned in the same archive it describes, so a
        # repacker who edits strategy.py can also recompute these
        # hashes and the lrvx_version string to match. What this does
        # catch is the common case the format is actually for --
        # accidental drift and unintentional edits -- and it makes a
        # deliberate tamper attempt do strictly more work than editing
        # one file.
        actual["strategy_sha256"] = _sha256_file(strat_path)
        expected["strategy_sha256"] = manifest.get("strategy_sha256")
        actual["tape_sha256"] = _sha256_dir(tape_path)
        expected["tape_sha256"] = manifest.get("tape_sha256")
        actual["lrvx_version"] = _lrvx_version()
        expected["lrvx_version"] = manifest.get(
            "lrvx_version", manifest.get(_LEGACY_VERSION_KEY))

        expected_lrvxrun = work / _EXPECTED_LRVXRUN_DIR
        if not expected_lrvxrun.is_dir():
            expected_lrvxrun = work / _LEGACY_EXPECTED_RUN_DIR
        if expected_lrvxrun.is_dir():
            actual["lrvxrun_present"] = actual_lrvxrun.is_dir()
            expected["lrvxrun_present"] = True
            if actual_lrvxrun.is_dir():
                # A trace that is there but cannot be read is a failed
                # comparison, never a skipped one: the side that failed
                # carries the error under lrvxrun_read_error, the other
                # side None, so the diff names it. Each message names its
                # side, so two unreadable traces still disagree.
                by_record = bool(manifest.get(_RECORDS_COMPARABLE_KEY))
                counts: Dict[str, Dict[str, int]] = {}
                records: Dict[str, Dict[str, List[Dict[str, Any]]]] = {}
                for side, out, trace in (("actual", actual, actual_lrvxrun),
                                         ("expected", expected, expected_lrvxrun)):
                    try:
                        counts[side] = _trace_counts(trace)
                        if by_record:
                            records[side] = _trace_records(trace)
                        out["lrvxrun_read_error"] = None
                    except Exception as e:
                        out["lrvxrun_read_error"] = (
                            f"{side} trace {trace.name} unreadable: "
                            f"{type(e).__name__}: {e}")
                # An expected trace packed without its signals supports a
                # fill-count comparison and no more; lrvxrun_compared says
                # which of the two this was.
                whole = ("expected" not in counts
                         or _trace_holds_signals(manifest, counts["expected"]))
                for side, out in (("actual", actual), ("expected", expected)):
                    if side not in counts:
                        continue
                    if whole:
                        out.update(counts[side])
                    else:
                        out["lrvxrun_fill_count"] = counts[side]["lrvxrun_fill_count"]
                    if side in records:
                        out.update(records[side])
                if "expected" in counts:
                    if "expected" in records:
                        compared = _COMPARED_RECORDS
                    else:
                        compared = _COMPARED_ALL if whole else _COMPARED_FILLS
                    actual["lrvxrun_compared"] = compared
                    expected["lrvxrun_compared"] = compared
        # else: this bundle predates the .lrvxrun ride-along. Leave
        # `lrvxrun_present` out of both dicts entirely rather than
        # comparing "actual has a fresh one" against "expected never
        # had one" -- that pair always disagrees, which would fail
        # every old bundle even when every other field matches.

        return BundleResult(
            actual=actual,
            expected=expected,
            manifest=manifest,
            bundle_path=str(bundle_path),
        )
    finally:
        shutil.rmtree(work, ignore_errors=True)


def validate_bundle(bundle_path: Path) -> BundleResult:
    """Replay a bundle and assert the output matches the recorded
    expected output. Sets ``matches`` and populates ``diff``.

    Executes the bundle's strategy code -- see the warning on
    :func:`replay_bundle`, which this calls directly.
    """
    res = replay_bundle(bundle_path)
    res.diff = _diff_dicts(res.actual, res.expected)
    res.matches = not res.diff
    return res


__all__ = [
    "BUNDLE_FORMAT_VERSION",
    "BundleResult",
    "BundleSecurityError",
    "BundleTrustWarning",
    "pack_bundle",
    "replay_bundle",
    "validate_bundle",
]
