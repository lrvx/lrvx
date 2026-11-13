"""``lrvx engine sim`` — bootstrap a paper-trading engine in one command.

Replaces the 50-line ``engine.py`` users would otherwise hand-roll
to wire a strategy, ``SimulatedExecutor``, ``ControlServer``, and the
runtime-state writer that tier-4 MCP read tools poll.

Out of scope for v1:

- Live connector orchestration (use ``lrvx tape record`` to capture a
  tape first, then replay it through the sim).
- Process supervision; use a normal supervisor (systemd, tmux) if
  the engine should outlive a terminal.
- Real-money trading. ``sim`` is exclusively the simulator surface;
  a separate ``lrvx engine live`` would carry the bigger safety
  blast radius.
"""
from __future__ import annotations

import argparse
import json
import os
import secrets
import signal
import sys
import threading
import time
from pathlib import Path
from typing import Any, Dict, List, Optional


_DEFAULT_STATE_FILE = "$HOME/.lrvx/runtime.json"


# ── Strategy loading ─────────────────────────────────────────────────


def _load_strategy_class(path: Path):
    """Import a single ``lrvx.Strategy`` subclass from a .py file.

    Same convention ``lrvx bundle pack`` already uses — keeps the
    user-authoring surface consistent across the CLI.
    """
    import importlib.util
    import lrvx

    if not path.is_file():
        raise SystemExit(f"lrvx engine sim: strategy file not found: {path}")
    spec = importlib.util.spec_from_file_location("lrvx_engine_strategy", path)
    if spec is None or spec.loader is None:
        raise SystemExit(f"lrvx engine sim: could not load {path}")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["lrvx_engine_strategy"] = mod
    spec.loader.exec_module(mod)
    candidates = [
        v for v in vars(mod).values()
        if isinstance(v, type)
        and issubclass(v, lrvx.Strategy)
        and v is not lrvx.Strategy
    ]
    if not candidates:
        raise SystemExit(
            f"lrvx engine sim: {path} has no lrvx.Strategy subclass. "
            f"Define one as `class MyStrat(lrvx.Strategy): ...`"
        )
    if len(candidates) > 1:
        raise SystemExit(
            f"lrvx engine sim: {path} has multiple lrvx.Strategy subclasses "
            f"({[c.__name__ for c in candidates]}). engine sim runs one."
        )
    return candidates[0]


# ── Minimal kill-switch stub ────────────────────────────────────────


class _KillSwitch:
    """Tiny duck-typed kill-switch. ControlServer only needs ``set`` /
    ``state``; the production runtime uses a richer hook.

    The sim engine is paper-trading — flipping this here just gates
    the ControlServer's mutating endpoints, which is the operator
    behaviour we want to mirror locally.
    """

    def __init__(self) -> None:
        self._active = False
        self._reason: Optional[str] = None
        self._since_ns: Optional[int] = None

    def set(self, active: bool, reason: Optional[str] = None) -> None:
        self._active = bool(active)
        self._reason = reason if active else None
        self._since_ns = time.time_ns() if active else None

    def state(self) -> Dict[str, Any]:
        return {
            "active": self._active,
            "reason": self._reason,
            "since_ns": self._since_ns,
        }


# ── Runtime state writer ────────────────────────────────────────────


class _StateWriter:
    """Periodically dumps a runtime snapshot for tier-4 MCP read tools.

    The schema matches ``docs/reference/runtime-state-schema.md`` v1.
    For v1 of the sim engine, positions / pnl / open_orders are
    populated as empty arrays — the snapshot's value here is that it
    *exists with valid schema*, so the read tools always get back an
    actionable response instead of erroring. Deriving real
    positions/PnL from the SimulatedExecutor fill stream is a
    follow-up — tracked separately.
    """

    def __init__(self, path: Path, kill_switch: _KillSwitch,
                 strategy_name: str, symbol_id: int,
                 symbol_name: str, period_s: float = 1.0) -> None:
        self._path = path
        self._ks = kill_switch
        self._strategy_name = strategy_name
        self._symbol_id = symbol_id
        self._symbol_name = symbol_name
        self._period_s = period_s
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        # Serialize writes — the loop thread and explicit out-of-band
        # callers both run _write_once, and both producing tmp files
        # at once collides on the rename.
        self._lock = threading.Lock()

    def _snapshot(self) -> Dict[str, Any]:
        return {
            "schema_version": 1,
            "captured_at_ns": time.time_ns(),
            "kill_switch": self._ks.state(),
            "strategies": [{
                "name": self._strategy_name,
                "status": "running",
                "symbols": [self._symbol_id],
            }],
            "positions": [],
            "open_orders": [],
            "pnl": {
                "by_strategy": [{
                    "strategy": self._strategy_name,
                    "realized": 0.0,
                    "unrealized": 0.0,
                    "fees": 0.0,
                    "trades": 0,
                }],
                "total": {"realized": 0.0, "unrealized": 0.0, "fees": 0.0},
            },
        }

    def _write_once(self) -> None:
        with self._lock:
            self._path.parent.mkdir(parents=True, exist_ok=True)
            # Atomic-ish write so a tier-4 reader never sees a partial
            # JSON document mid-flush.
            tmp = self._path.with_suffix(self._path.suffix + ".tmp")
            tmp.write_text(json.dumps(self._snapshot(), indent=2, sort_keys=True))
            os.replace(tmp, self._path)

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                self._write_once()
            except Exception as exc:  # pragma: no cover
                print(f"lrvx engine sim: state writer error: {exc}",
                      file=sys.stderr)
            self._stop.wait(self._period_s)

    def start(self) -> None:
        # Write immediately so the file exists by the time the user
        # sees the banner — they may run an MCP query straight away.
        self._write_once()
        self._thread = threading.Thread(
            target=self._run, daemon=True, name="lrvx-state-writer",
        )
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=2.0)


# ── Banner ───────────────────────────────────────────────────────────


def _print_banner(strategy_name: str, strategy_path: Path,
                  symbol_name: str, tape_path: Path,
                  state_path: Path, control_url: str, token: str) -> None:
    print("lrvx engine sim")
    print(f"  strategy:   {strategy_path} ({strategy_name})")
    print(f"  symbol:     {symbol_name}")
    print(f"  source:     tape {tape_path}")
    print(f"  state file: {state_path}")
    print(f"  control:    {control_url}")
    print(f"  token:      {token}")
    print()
    print("To wire into MCP, run in another shell:")
    print()
    print(f"    lrvx-mcp init --overwrite \\")
    print(f"        --engine-url {control_url} \\")
    print(f"        --token {token}")
    print()
    print("Press Ctrl-C to stop.")
    print()


# ── Tape feed ────────────────────────────────────────────────────────


def _resolve_tape_symbol(tape_path: Path, requested: Optional[int]) -> int:
    """Pick which symbol id in the tape feeds the one registered symbol.

    The tape's ids are assigned by whoever recorded it (``lrvx tape record``
    numbers them 1..N in encounter order); they carry no relation to the
    single symbol this command registers. Forwarding them unchanged meant a
    multi-symbol tape delivered some other instrument's trades under the
    requested name, with rc=0 and a success banner -- silently wrong prices
    rather than an error.
    """
    from . import tape as tape_mod

    ids = list(getattr(tape_mod.inspect_tape(tape_path), "symbol_ids", []) or [])
    if requested is not None:
        if ids and requested not in ids:
            raise SystemExit(
                f"lrvx engine sim: --tape-symbol-id {requested} is not in the "
                f"tape. present: {ids}")
        return requested
    if len(ids) > 1:
        raise SystemExit(
            f"lrvx engine sim: the tape holds {len(ids)} symbols ({ids}) but "
            f"this command registers one. Re-run with --tape-symbol-id <id> "
            f"to choose which one feeds the strategy.")
    return ids[0] if ids else 0


def _feed_tape(runner, tape_path: Path, tape_symbol_id: int,
               registered_symbol_id: int, sim_bridge: Any = None) -> int:
    """Drive a Runner from a recorded ``.lrvx`` tape. Returns the
    number of trades dispatched to the strategy.

    With ``sim_bridge`` (see :func:`_make_sim_bridge`) the replay is
    ``Runner.replay_tape``: the loop ``BacktestRunner.run_tape`` uses, with
    the bridge's simulator in the place of the backtest's own. Every event
    the tape holds -- trades, book snapshots, book deltas, top-of-book
    frames -- moves the simulator's clock and is shown to the simulator
    before the strategy sees it, in time order, so a resting limit order
    fills from the book as it would in a backtest. The bridge's lock is held
    while the simulator takes an event, and the orders the simulator has
    filled completely are reported to the runner after each one. Trades
    alone used to be replayed, and a resting order never filled.

    Without a bridge only the runner is fed, trades only, and a simulator
    behind the runner's executor never fills."""
    from . import tape as tape_mod

    if sim_bridge is not None:
        stats = runner.replay_tape(
            str(tape_path), sim_bridge.simulator,
            tape_symbol_id=tape_symbol_id, symbol=registered_symbol_id,
            lock=sim_bridge.lock,
            after_event=lambda: sim_bridge.report_fills(runner))
        return int(stats["trades"])

    counter = [0]

    def _on_trade(ts_ns: int, sym_id: int, price: float,
                  qty: float, side: int) -> None:
        # Only the chosen instrument, remapped onto the registered id. The
        # counter reports trades *delivered*, not trades read -- the old one
        # counted the whole tape and so claimed success even when nothing
        # reached the strategy.
        if sym_id != tape_symbol_id:
            return
        # side: 0 = buy, 1 = sell. Runner.on_trade wants is_buy=bool.
        runner.on_trade(registered_symbol_id, price, qty, side == 0, ts_ns)
        counter[0] += 1

    tape_mod.replay_tape(tape_path, on_trade=_on_trade)
    return counter[0]


# ── Command ──────────────────────────────────────────────────────────


def _make_sim_bridge(sim: Any) -> Any:
    """Adapt a native ``SimulatedExecutor`` to the runner's executor hook.

    ``Runner.set_executor`` takes a ``lrvx.Executor`` -- the Python-side
    hook whose calls are marshalled back through the C API.
    ``SimulatedExecutor`` is an unrelated binding of the C++ matching engine
    and is not a subclass of it, so handing one straight to the runner raises
    TypeError. This adapter is the join: the runner sees an ``Executor``, and
    the ControlServer talks to ``bridge.control`` (see :class:`_SimControl`),
    so orders from the strategy and orders placed over MCP land in one book,
    behind one lock and in one account of what is still working.

    Built inside a function because ``lrvx`` is imported lazily here --
    binding the base class at module scope would drag the extension into
    every ``lrvx --help``.
    """
    import lrvx

    class _SimBridge(lrvx.Executor):
        def __init__(self) -> None:
            super().__init__()
            self._sim = sim
            # The bridge is called on the runner's thread and the tape replay
            # on the caller's; with a threaded runner those are two threads.
            self._lock = threading.Lock()
            # order id -> [symbol, quantity still to fill], for the orders
            # this bridge sent that the simulator has not finished.
            self._working: Dict[int, list] = {}
            self._fills_seen = 0
            # What the ControlServer is given as its executor.
            self.control = _SimControl(self)

        @property
        def simulator(self) -> Any:
            return self._sim

        @property
        def lock(self) -> Any:
            """Held around every call into the simulator. The tape replay
            takes it while the simulator is shown an event."""
            return self._lock

        def _submit(self, order: Any) -> None:
            order_type = order.order_type or "market"
            offset = float(order.trailing_offset)
            if order_type == "trailing_stop" and offset <= 0:
                # A percent trailing stop: the order the runner hands an
                # executor has no field for the rate.
                raise ValueError(
                    f"order {order.id}: a trailing stop reached the simulator with no "
                    "trailing offset; trailing_stop_percent cannot be routed through an "
                    "executor, use trailing_stop(offset=...)")
            # The trigger of a stop or take-profit and the offset of a
            # trailing stop: without them the simulator took the order as one
            # triggered at its limit price, or at nothing.
            self._sim.submit_order(
                order.id,
                order.side or "buy",
                float(order.price),
                float(order.quantity),
                type=order_type,
                symbol=order.symbol,
                tif=order.time_in_force or "gtc",
                reduce_only=bool(order.reduce_only),
                trigger=float(order.trigger_price),
                trailing_offset=offset,
            )
            self._working[int(order.id)] = [int(order.symbol), float(order.quantity)]

        def submit(self, order: Any) -> None:
            with self._lock:
                self._submit(order)

        def replace(self, old_order_id: int, new_order: Any) -> None:
            # The simulator's Python surface has no replace. The runner sends
            # the order as it is after the modify, under the id it keeps:
            # cancel that id, then submit the order again.
            with self._lock:
                self._sim.cancel_order(old_order_id)
                self._working.pop(int(old_order_id), None)
                self._submit(new_order)

        def cancel(self, order_id: int) -> None:
            with self._lock:
                self._sim.cancel_order(order_id)
                self._working.pop(int(order_id), None)

        def cancel_all(self, symbol: int) -> None:
            with self._lock:
                self._sim.cancel_all(symbol)
                for order_id in [i for i, w in self._working.items() if w[0] == int(symbol)]:
                    del self._working[order_id]

        # ── orders that come from outside the runner (the ControlServer) ──

        def control_submit(self, order_id: int, side: str, price: float, quantity: float,
                           *, type: str = "market", symbol: int = 1) -> None:
            with self._lock:
                self._sim.submit_order(order_id, side, price, quantity,
                                       type=type, symbol=symbol)
                self._working[int(order_id)] = [int(symbol), float(quantity)]

        def control_cancel(self, order_id: int) -> bool:
            """True when the order was one this bridge still had working."""
            with self._lock:
                self._sim.cancel_order(order_id)
                return self._working.pop(int(order_id), None) is not None

        def control_cancel_all(self, symbol: int) -> List[int]:
            """The ids that were working on ``symbol``."""
            with self._lock:
                self._sim.cancel_all(symbol)
                gone = [i for i, w in self._working.items() if w[0] == int(symbol)]
                for order_id in gone:
                    del self._working[order_id]
                return gone

        def report_fills(self, runner: Any) -> int:
            """Tell ``runner`` (``Runner.order_closed``) about every order the
            simulator has filled completely since the last call. Returns how
            many orders that was."""
            finished = []
            with self._lock:
                if int(self._sim.fill_count) == self._fills_seen:
                    return 0
                fills = self._sim.fills_list()
                for fill in fills[self._fills_seen:]:
                    order_id = int(fill["order_id"])
                    working = self._working.get(order_id)
                    if working is None:
                        continue
                    working[1] -= float(fill["quantity"])
                    if working[1] <= 1e-12:
                        del self._working[order_id]
                        finished.append(order_id)
                self._fills_seen = len(fills)
            for order_id in finished:
                runner.order_closed(order_id)
            return len(finished)

    return _SimBridge()


class _SimControl:
    """The executor the ControlServer acts through in ``lrvx engine sim``.

    The server used to hold the simulator itself and call it from its own
    thread with no lock, while the runner's thread and the tape replay were
    inside it; and an order it cancelled stayed in the runner's record of
    forwarded orders and in the bridge's fill accounting. Its three calls go
    through the bridge instead: under the bridge's lock, into the bridge's
    account of working orders, and, for a cancel, on to
    ``Runner.order_closed`` so a strategy's later modify of that order is
    refused instead of resubmitting it.
    """

    def __init__(self, bridge: Any) -> None:
        self._bridge = bridge
        self._runner: Any = None

    def attach_runner(self, runner: Any) -> None:
        self._runner = runner

    def submit_order(self, order_id: int, side: str, price: float, quantity: float,
                     *, type: str = "market", symbol: int = 1) -> None:
        self._bridge.control_submit(order_id, side, price, quantity, type=type, symbol=symbol)

    def cancel_order(self, order_id: int) -> None:
        if self._bridge.control_cancel(order_id) and self._runner is not None:
            self._runner.order_closed(int(order_id))

    def cancel_all(self, symbol: int) -> None:
        for order_id in self._bridge.control_cancel_all(symbol):
            if self._runner is not None:
                self._runner.order_closed(order_id)


def _make_kill_switch_bridge(state: "_KillSwitch") -> Any:
    """Adapt the duck-typed ``_KillSwitch`` to the runner's kill-switch hook.

    ``_KillSwitch`` exists for ControlServer and the state writer, which want
    ``set`` / ``state``. The runner wants a ``lrvx.KillSwitch`` whose
    ``check(signal)`` returns False to halt trading. Without this adapter the
    two were never connected, so flipping the switch over MCP gated the
    control endpoints while the strategy kept trading -- the opposite of what
    an operator kill switch is for.

    Lazily built for the same reason as _make_sim_bridge.
    """
    import lrvx

    class _KillSwitchBridge(lrvx.KillSwitch):
        def check(self, signal: Any) -> bool:
            return not state.state()["active"]

    return _KillSwitchBridge()


def cmd_engine_sim(args: argparse.Namespace) -> int:
    import lrvx
    from . import control_server as cs_mod

    strategy_path = Path(args.strategy).expanduser().resolve()
    tape_path = Path(args.tape).expanduser().resolve()
    if not tape_path.exists():
        print(f"lrvx engine sim: tape not found: {tape_path}", file=sys.stderr)
        return 1

    state_path = Path(
        os.path.expandvars(args.state_file or _DEFAULT_STATE_FILE)
    ).expanduser()

    StratCls = _load_strategy_class(strategy_path)

    registry = lrvx.SymbolRegistry()
    # add_symbol returns a Symbol, not an int. Strategy accepts either, but
    # the control server and the state writer put this straight into JSON,
    # where a Symbol is not serialisable.
    symbol = registry.add_symbol(
        args.exchange, args.symbol_name, tick_size=args.tick_size,
    )
    sym_id = int(symbol.id)
    tape_symbol_id = _resolve_tape_symbol(
        tape_path, getattr(args, "tape_symbol_id", None))

    runner = lrvx.Runner(registry, on_signal=lambda s: None,
                            threaded=bool(args.threaded))
    sim = lrvx.SimulatedExecutor()
    sim_bridge = _make_sim_bridge(sim)
    sim_bridge.control.attach_runner(runner)
    runner.set_executor(sim_bridge)

    ks = _KillSwitch()
    runner.set_kill_switch(_make_kill_switch_bridge(ks))

    strategy = StratCls([symbol])
    runner.add_strategy(strategy)
    runner.start()

    # Token: explicit override > generated.  Generated tokens are
    # url-safe so they paste cleanly into shell commands without
    # quoting.
    token = args.token or secrets.token_urlsafe(16)
    server = cs_mod.ControlServer(
        tokens={token: "paper"},
        executor=sim_bridge.control,
        kill_switch=ks,
        runner=runner,
        strategies=lambda: [{
            "name": StratCls.__name__,
            "status": "running",
            "symbols": [sym_id],
        }],
        host=args.host,
        port=args.port,
    )
    server.start()

    writer = _StateWriter(
        path=state_path, kill_switch=ks,
        strategy_name=StratCls.__name__,
        symbol_id=sym_id, symbol_name=args.symbol_name,
    )
    writer.start()

    _print_banner(
        strategy_name=StratCls.__name__,
        strategy_path=strategy_path,
        symbol_name=args.symbol_name,
        tape_path=tape_path,
        state_path=state_path,
        control_url=server.url,
        token=token,
    )

    stop_evt = threading.Event()

    def _on_signal(_signum, _frame):
        stop_evt.set()

    signal.signal(signal.SIGINT, _on_signal)
    signal.signal(signal.SIGTERM, _on_signal)

    try:
        n = _feed_tape(runner, tape_path, tape_symbol_id, sym_id, sim_bridge)
        print(f"tape replay complete: {n} trade(s) dispatched.")
        print("engine still running — Ctrl-C to stop.")
        # Hold until SIGINT so the user can introspect via MCP after
        # the tape ends.
        stop_evt.wait()
    finally:
        print("\nstopping engine...")
        try:
            runner.stop()
        except Exception:
            pass
        server.stop()
        writer.stop()

    return 0


# ── Argparse wiring (called from lrvx.cli) ───────────────────────


def add_engine_subparser(sub: Any) -> None:
    """Attach the ``engine`` subcommand tree onto an existing argparse
    sub-parsers handle. Callable from ``lrvx.cli`` so the verb
    appears under ``lrvx engine ...`` next to ``lrvx tape ...``.
    """
    p = sub.add_parser(
        "engine",
        help="Run a lrvx engine — sim (paper-trading) is the only "
             "mode wired today.",
    )
    eng_sub = p.add_subparsers(dest="engine_command", required=True)

    p_sim = eng_sub.add_parser(
        "sim",
        help="Bootstrap a paper-trading engine: Runner + "
             "SimulatedExecutor + ControlServer + state writer.",
    )
    p_sim.add_argument(
        "--strategy", required=True,
        help="Path to a .py file with one lrvx.Strategy subclass.",
    )
    p_sim.add_argument(
        "--tape", required=True,
        help="Path to a .lrvx tape directory (the data source).",
    )
    p_sim.add_argument(
        "--symbol-name", default="BTCUSDT",
        help="Symbol name to register (default: BTCUSDT).",
    )
    p_sim.add_argument(
        "--tape-symbol-id", type=int, default=None,
        help="Which symbol id in the tape feeds the strategy. Required when "
             "the tape holds more than one.")
    p_sim.add_argument(
        "--exchange", default="sim",
        help="Exchange tag for the symbol (default: sim).",
    )
    p_sim.add_argument(
        "--tick-size", type=float, default=0.01,
        help="Tick size for the symbol (default: 0.01).",
    )
    p_sim.add_argument(
        "--port", type=int, default=8765,
        help="ControlServer port (default 8765).",
    )
    p_sim.add_argument(
        "--host", default="127.0.0.1",
        help="ControlServer bind host (default 127.0.0.1).",
    )
    p_sim.add_argument(
        "--token", default=None,
        help="Explicit ControlServer token. Default: generated.",
    )
    p_sim.add_argument(
        "--state-file", default=None,
        help=f"Runtime state file (default: {_DEFAULT_STATE_FILE}).",
    )
    p_sim.add_argument(
        "--threaded", action="store_true",
        help="Use the Disruptor (threaded) Runner. Default: sync.",
    )
    p_sim.set_defaults(handler=cmd_engine_sim)
