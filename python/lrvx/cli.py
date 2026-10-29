"""``lrvx`` CLI — scaffolds new lrvx projects from templates.

Installed as a console_script via [project.scripts] in pyproject.toml,
so `lrvx new my-strategy` works after `pip install lrvx`.

Usage::

    lrvx new <project-name>                     # default template (research)
    lrvx new <project-name> --template=research
    lrvx new <project-name> --template=live
    lrvx new <project-name> --template=indicator-library
    lrvx new <project-name> --here              # scaffold into the current dir
    lrvx templates                              # list available templates
    lrvx report <stats.json>                    # render an HTML backtest report
    lrvx report <stats.json> -o report.html --equity equity.json --trades trades.json

Each template is a directory under ``lrvx/templates/<name>/`` shipped
with the wheel. ``lrvx new`` copies the directory verbatim, then
substitutes ``__PROJECT_NAME__``, ``__PROJECT_SLUG__``,
``__PROJECT_PREFIX__`` (upper-cased slug — used as a generic env-var
prefix, e.g. ``MYBOT``), and ``__PROJECT_ENV__``
(``<PREFIX>_DATA`` — used by the research template for the CSV path
env var).

Substitution applies to file *contents* AND to file/directory
*names* — the indicator-library template ships its package directory
as ``__PROJECT_SLUG__/`` so it lands at e.g. ``my_indicators/`` for
``lrvx new my-indicators``.
"""

from __future__ import annotations

import argparse
import re
import shutil
import sys
from importlib import resources
from pathlib import Path
from typing import Any, Iterable, Optional


# Tape and run-trace directory suffixes; the FLOX-era names are accepted
# alongside the current ones.
_TAPE_SUFFIXES = (".lrvx", ".floxlog")
_RUN_SUFFIXES = (".lrvxrun", ".floxrun")


# ── Template helpers ───────────────────────────────────────────────────


def _slug(name: str) -> str:
    """Lower-snake-case slug from a project name."""
    s = re.sub(r"[^A-Za-z0-9_-]+", "_", name).strip("_")
    return s.lower().replace("-", "_") or "project"


def _templates_root():
    """Return Traversable for the bundled templates directory, or None."""
    try:
        return resources.files("lrvx").joinpath("templates")
    except ModuleNotFoundError:
        return None


def _list_templates() -> list[str]:
    """List bundled template names by walking lrvx/templates/."""
    root = _templates_root()
    if root is None or not root.is_dir():
        return []
    out: list[str] = []
    for entry in root.iterdir():
        if entry.is_dir():
            out.append(entry.name)
    out.sort()
    return out


def _copy_template(template: str, dest: Path, project_name: str) -> int:
    """Copy bundled template to ``dest`` with placeholder substitution.

    Returns 0 on success, non-zero on error (with a printed message).
    """
    root = _templates_root()
    src_root = root.joinpath(template) if root is not None else None
    if src_root is None or not src_root.is_dir():
        avail = ", ".join(_list_templates()) or "(none)"
        print(f"lrvx new: unknown template '{template}'. "
              f"Available: {avail}",
              file=sys.stderr)
        return 1

    if dest.exists() and any(dest.iterdir()):
        print(f"lrvx new: destination '{dest}' is not empty. "
              f"Pick a fresh path or remove existing files.",
              file=sys.stderr)
        return 1

    dest.mkdir(parents=True, exist_ok=True)
    slug = _slug(project_name)
    upper = slug.upper()

    def _subst(s: str) -> str:
        return (s.replace("__PROJECT_NAME__", project_name)
                 .replace("__PROJECT_SLUG__", slug)
                 .replace("__PROJECT_PREFIX__", upper)
                 .replace("__PROJECT_ENV__", upper + "_DATA"))

    def _walk(node, target: Path) -> None:
        for child in node.iterdir():
            # Substitute placeholders in file/directory names too — the
            # indicator-library template ships its package directory as
            # `__PROJECT_SLUG__/` which becomes e.g. `my_indicators/`.
            child_target = target / _subst(child.name)
            if child.is_dir():
                child_target.mkdir(exist_ok=True)
                _walk(child, child_target)
            else:
                # Read the template file as text and substitute. Binary
                # files (data, images) shouldn't appear in templates;
                # fall back to a raw copy if decoding fails.
                try:
                    text = child.read_text()
                except UnicodeDecodeError:
                    child_target.write_bytes(child.read_bytes())
                    continue
                child_target.write_text(_subst(text))

    _walk(src_root, dest)
    return 0


# ── Command handlers ───────────────────────────────────────────────────


def cmd_new(args: argparse.Namespace) -> int:
    project_name: str = args.name
    template: str = args.template

    if args.here:
        dest = Path.cwd()
    else:
        dest = Path.cwd() / project_name

    rc = _copy_template(template, dest, project_name)
    if rc != 0:
        return rc

    print(f"created '{project_name}' from template '{template}' at {dest}")
    print()
    print("next steps:")
    print(f"  cd {dest.relative_to(Path.cwd()) if not args.here else '.'}")
    if template == "indicator-library":
        print('  pip install -e ".[dev]"')
        print(f"  pytest")
        print(f"  python examples/use_in_strategy.py")
    else:
        print(f"  pip install -r requirements.txt")
        print(f"  python main.py")
    return 0


def cmd_templates(_args: argparse.Namespace) -> int:
    names = _list_templates()
    if not names:
        print("no templates found (is lrvx installed?)", file=sys.stderr)
        return 1
    for n in names:
        print(n)
    return 0


def cmd_report(args: argparse.Namespace) -> int:
    import json
    from . import report as report_mod

    try:
        with open(args.stats) as f:
            stats = json.load(f)
    except OSError as e:
        print(f"lrvx report: cannot read stats file '{args.stats}': {e}",
              file=sys.stderr)
        return 1

    equity = None
    if args.equity:
        try:
            with open(args.equity) as f:
                equity = json.load(f)
        except OSError as e:
            print(f"lrvx report: cannot read equity file: {e}", file=sys.stderr)
            return 1

    trades = None
    if args.trades:
        try:
            with open(args.trades) as f:
                trades = json.load(f)
        except OSError as e:
            print(f"lrvx report: cannot read trades file: {e}", file=sys.stderr)
            return 1

    out = Path(args.output)
    report_mod.write_html(
        out,
        stats=stats,
        equity_curve=equity,
        trades=trades,
        title=args.title or "lrvx backtest report",
        subtitle=args.subtitle or "",
    )
    print(f"wrote {out}")
    return 0


# ── tape command handlers ─────────────────────────────────────────────


_DURATION_UNITS = {"s": 1.0, "m": 60.0, "h": 3600.0, "d": 86400.0}


def _parse_duration(s: str) -> float:
    """`'60s'` → 60.0, `'5m'` → 300.0, `'1h'` → 3600.0. Bare digits = seconds."""
    if not s:
        raise ValueError("empty duration")
    if s[-1].lower() in _DURATION_UNITS:
        return float(s[:-1]) * _DURATION_UNITS[s[-1].lower()]
    return float(s)


def _normalize_ccxt_symbol(sym: str) -> str:
    """Allow either 'BTCUSDT' or 'BTC/USDT' on the CLI; pass through to ccxt
    in its preferred slash form."""
    if "/" in sym:
        return sym
    # Heuristic split for the common quote currencies. Anything else
    # the user can pass with a slash explicitly.
    for quote in ("USDT", "USDC", "BUSD", "USD", "BTC", "ETH"):
        if sym.endswith(quote) and len(sym) > len(quote):
            return f"{sym[: -len(quote)]}/{quote}"
    return sym


def _parse_venue_arg(arg: str) -> tuple[str, list[str]]:
    """Parse `EXCHANGE:SYM1,SYM2,...` into `(exchange, [sym1, sym2, ...])`.

    Raises ValueError on a malformed token so argparse-level errors
    surface before we open any websocket.
    """
    if ":" not in arg:
        raise ValueError(
            f"--venue expects 'exchange:symbol[,symbol2]...', got {arg!r}"
        )
    exchange, _, sym_list = arg.partition(":")
    exchange = exchange.strip()
    if not exchange:
        raise ValueError(f"--venue missing exchange in {arg!r}")
    syms = [s.strip() for s in sym_list.split(",") if s.strip()]
    if not syms:
        raise ValueError(f"--venue {arg!r} has no symbols after ':'")
    return exchange, syms


async def _record_one_venue(exchange: str, ccxt_syms: list[str],
                            out: Path, max_segment_mb: int,
                            book_depth: int | None, testnet: bool,
                            duration: float | None,
                            stop_event: Any) -> dict:
    """Drive a single CcxtBroker → BinaryLogRecorderHook pipeline.
    Used by both the single-venue and multi-venue CLI paths."""
    from .ccxt import CcxtBroker
    from . import tape as tape_mod

    out.mkdir(parents=True, exist_ok=True)
    instrument_type = "perpetual" if any(":" in s for s in ccxt_syms) else "spot"
    recorder = tape_mod.make_recorder_hook(
        out, max_segment_mb=max_segment_mb,
        exchange_name=exchange, instrument_type=instrument_type,
    )

    broker = CcxtBroker(exchange_id=exchange, sandbox=testnet)
    async with broker:
        broker.set_market_data_recorder(recorder)
        for sym in ccxt_syms:
            await broker.add_symbol(sym)
        import asyncio
        run_task = asyncio.create_task(
            broker.run(streams=("trades", "book"),
                       book_depth=book_depth, reconcile=False)
        )
        try:
            if duration is not None:
                try:
                    await asyncio.wait_for(stop_event.wait(), timeout=duration)
                except asyncio.TimeoutError:
                    pass
            else:
                await stop_event.wait()
        finally:
            run_task.cancel()
            try:
                await run_task
            except (asyncio.CancelledError, Exception):
                pass
            recorder.close()

    return {
        "exchange": exchange,
        "symbols": list(ccxt_syms),
        "out": str(out),
        "stats": recorder.stats(),
    }


def cmd_tape_record(args: argparse.Namespace) -> int:
    import asyncio
    import signal

    duration = _parse_duration(args.duration) if args.duration else None
    out_root = Path(args.output).expanduser().resolve()

    # Resolve the venue list. Either:
    #   - one or more `--venue EXCHANGE:SYM1[,SYM2]` flags, OR
    #   - the legacy positional `exchange symbol` (one venue, one symbol).
    venues: list[tuple[str, list[str]]] = []
    if args.venue:
        for raw in args.venue:
            try:
                venues.append(_parse_venue_arg(raw))
            except ValueError as exc:
                print(f"lrvx tape record: {exc}", file=sys.stderr)
                return 1
        if args.exchange or args.symbol:
            print("lrvx tape record: pass either --venue or the positional "
                  "exchange/symbol form, not both.",
                  file=sys.stderr)
            return 1
    else:
        if not args.exchange or not args.symbol:
            print("lrvx tape record: need either --venue or "
                  "`exchange symbol` positional args.",
                  file=sys.stderr)
            return 1
        venues.append((args.exchange, [_normalize_ccxt_symbol(args.symbol)]))

    # Normalise ccxt symbol shape for every venue (accept BTCUSDT shorthand).
    venues = [
        (exch, [_normalize_ccxt_symbol(s) for s in syms])
        for exch, syms in venues
    ]

    # Single-venue keeps the original output-dir contract (everything
    # under `<output>/`). Multi-venue spreads into `<output>/<exchange>/`
    # so each venue produces an independent `.lrvx` directory the
    # merge can consume.
    multi = len(venues) > 1
    print(f"lrvx tape record: {len(venues)} venue(s) → {out_root}")
    for exch, syms in venues:
        print(f"  {exch}: {', '.join(syms)}"
              + (f"  → {out_root}/{exch}" if multi else ""))
    if duration is not None:
        print(f"  duration: {args.duration} ({duration:.0f}s)")
    else:
        print("  duration: until SIGINT")

    try:
        from .ccxt import CcxtBroker  # noqa: F401 — import-time check
    except Exception as exc:  # pragma: no cover — ccxt is an optional dep
        print(f"lrvx tape record: ccxt-side helpers unavailable: {exc}",
              file=sys.stderr)
        return 1

    async def _run() -> int:
        stop_event = asyncio.Event()
        loop = asyncio.get_running_loop()

        def _stop(*_args: Any) -> None:
            stop_event.set()

        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                loop.add_signal_handler(sig, _stop)
            except (NotImplementedError, RuntimeError):
                pass  # Windows / non-main thread

        tasks = []
        for exch, syms in venues:
            venue_out = (out_root / exch) if multi else out_root
            tasks.append(asyncio.create_task(_record_one_venue(
                exchange=exch, ccxt_syms=syms, out=venue_out,
                max_segment_mb=args.max_segment_mb,
                book_depth=args.book_depth, testnet=args.testnet,
                duration=duration, stop_event=stop_event,
            )))
        results = await asyncio.gather(*tasks, return_exceptions=True)

        exit_code = 0
        for r in results:
            if isinstance(r, Exception):
                print(f"  ERROR: {r!r}", file=sys.stderr)
                exit_code = 1
                continue
            s = r["stats"]
            tag = f"{r['exchange']}" + (f" ({r['out']})" if multi else "")
            print(f"  {tag}: trades={s.get('trades_written', 0)} "
                  f"books={s.get('book_updates_written', 0)}")
            if s.get("errors"):
                print(f"    WRITE-ERRORS: {s['errors']}", file=sys.stderr)
                exit_code = 1
        return exit_code

    return asyncio.run(_run())


def cmd_bundle_pack(args: argparse.Namespace) -> int:
    from . import bundle as bundle_mod

    cfg = {
        "slippage_model": args.slippage_model,
        "slippage_params": {"bps": float(args.slippage_bps)} if args.slippage_model == "fixed_bps" else {},
    }
    out = bundle_mod.pack_bundle(
        strategy=Path(args.strategy),
        tape=Path(args.tape),
        output=Path(args.output),
        config=cfg,
        symbol_name=args.symbol_name,
        exchange=args.exchange,
        tick_size=float(args.tick_size),
    )
    print(f"lrvx bundle pack: wrote {out}")
    return 0


def cmd_bundle_replay(args: argparse.Namespace) -> int:
    from . import bundle as bundle_mod

    try:
        res = bundle_mod.replay_bundle(Path(args.path))
    except (FileNotFoundError, ValueError, bundle_mod.BundleSecurityError) as exc:
        # Every sibling verb reports a bad path as a message; this one dumped
        # a raw traceback, which reads like a crash rather than bad input.
        print(f"lrvx bundle replay: {exc}", file=sys.stderr)
        return 1
    print(f"lrvx bundle replay: {args.path}")
    print(f"  lrvx_version:           {res.manifest.get('lrvx_version')}")
    print(f"  bundle_format_version:  {res.manifest.get('bundle_format_version')}")
    print(f"  trade_count actual:     {res.actual['trade_count']}")
    print(f"  fill_count actual:      {res.actual['fill_count']}")
    print(f"  total_filled actual:    {res.actual['total_filled_quantity']}")
    print(f"  trade_count expected:   {res.expected['trade_count']}")
    print(f"  fill_count expected:    {res.expected['fill_count']}")
    return 0


def cmd_bundle_validate(args: argparse.Namespace) -> int:
    from . import bundle as bundle_mod

    try:
        res = bundle_mod.validate_bundle(Path(args.path))
    except (FileNotFoundError, ValueError, bundle_mod.BundleSecurityError) as exc:
        print(f"lrvx bundle validate: {exc}", file=sys.stderr)
        return 1
    if res.matches:
        print(f"lrvx bundle validate: OK ({args.path}) — actual matches expected")
        return 0
    print(f"lrvx bundle validate: DIVERGENCE ({args.path})", file=sys.stderr)
    for d in res.diff:
        print(f"  {d}", file=sys.stderr)
    return 1


def cmd_tape_diff(args: argparse.Namespace) -> int:
    from . import tape as tape_mod

    left = Path(args.left).expanduser()
    right = Path(args.right).expanduser()
    if not left.is_dir() or not right.is_dir():
        print(f"lrvx tape diff: both paths must be .lrvx directories",
              file=sys.stderr)
        return 2
    diff = tape_mod.diff_tapes(
        left, right,
        max_mismatches=int(args.max_mismatches),
        field_tolerance_ns=int(args.ts_tolerance_ns),
    )
    if args.json:
        import json as _json
        print(_json.dumps(diff.to_dict(), indent=2, sort_keys=True))
    else:
        if diff.equal:
            print(f"lrvx tape diff: EQUAL "
                  f"({diff.left_count} trade(s) on each side)")
        else:
            print(f"lrvx tape diff: DIVERGENCE")
            print(f"  left:  {diff.left_path}  trades={diff.left_count}")
            print(f"  right: {diff.right_path}  trades={diff.right_count}")
            print(f"  first_divergence_index: {diff.first_divergence_index}")
            for m in diff.mismatches[:10]:
                print(f"  [{m['index']}] left={m['left']} right={m['right']}")
            if len(diff.mismatches) > 10:
                print(f"  ... {len(diff.mismatches) - 10} more recorded mismatches")
    return 0 if diff.equal else 1


def cmd_lint_lookahead(args: argparse.Namespace) -> int:
    from . import lookahead as lookahead_mod

    p = Path(args.path).expanduser()
    if not p.is_file():
        print(f"lrvx lint lookahead: file not found: {p}", file=sys.stderr)
        return 2
    report = lookahead_mod.analyze_path(p)
    if args.json:
        import json as _json
        print(_json.dumps(report.to_dict(), indent=2, sort_keys=True))
    else:
        if report.ok:
            print(f"lrvx lint lookahead: OK ({p}); no patterns flagged")
        else:
            print(f"lrvx lint lookahead: {len(report.findings)} finding(s) in {p}")
            for f in report.findings:
                print(f"  {f.line}:{f.col} [{f.rule}] {f.message}")
                if f.snippet:
                    print(f"      {f.snippet}")
    return 0 if report.ok else 1


def cmd_tape_inspect(args: argparse.Namespace) -> int:
    from . import tape as tape_mod

    p = Path(args.path).expanduser().resolve()
    if not p.exists():
        print(f"lrvx tape inspect: path not found: {p}", file=sys.stderr)
        return 1
    stats = tape_mod.inspect_tape(p)
    print(f"lrvx tape inspect: {stats.path}")
    print(f"  trades       : {stats.trade_count}")
    if stats.trade_count > 0:
        span_ns = stats.last_ts_ns - stats.first_ts_ns
        print(f"  first ts (ns): {stats.first_ts_ns}")
        print(f"  last  ts (ns): {stats.last_ts_ns}")
        print(f"  span         : {span_ns / 1e9:.3f}s")
        print(f"  symbols      : {stats.symbol_ids}")
    return 0


def cmd_tape_replay(args: argparse.Namespace) -> int:
    from . import tape as tape_mod

    p = Path(args.path).expanduser().resolve()
    if not p.exists():
        print(f"lrvx tape replay: path not found: {p}", file=sys.stderr)
        return 1

    counter = [0]
    limit = args.limit

    def _on_trade(ts_ns: int, sym_id: int, price: float,
                  qty: float, side: int) -> None:
        counter[0] += 1
        if limit is not None and counter[0] >= limit:
            raise StopIteration  # stops the loop cleanly via inner try

    try:
        n = tape_mod.replay_tape(p, on_trade=_on_trade)
    except StopIteration:
        n = counter[0]
    print(f"lrvx tape replay: dispatched {counter[0]} trade(s) "
          f"from {p} (tape contains {n} total)")
    return 0


def _materialize_merged_tape(paths: list[Path], out_dir: Path) -> None:
    """Write a synthetic `.lrvx` directory at `out_dir` containing
    the merged event stream from `paths`. Symbols are rekeyed to global
    IDs; metadata.json carries the merged symbol table so the viewer can
    label them.

    MVP for `lrvx tape view path1 path2 ...`. The streaming HTTP `/merge`
    endpoint that the spec preferred lives behind SPA-side multi-tape
    support — until that lands this materialization gives the viewer a
    file it already knows how to render.
    """
    import json
    import lrvx
    import numpy as np

    out_dir.mkdir(parents=True, exist_ok=True)
    reader = lrvx.MergedTapeReader([str(p) for p in paths])

    writer = lrvx.DataWriter(str(out_dir), max_segment_mb=256,
                                exchange_id=0, compression="none")

    # Trades: convert raw int64 → double for the existing
    # DataWriter.write_trade signature. Acceptable precision loss for
    # the viewer use case (prices have plenty of headroom under 1e15).
    for row in reader.read_trades():
        writer.write_trade(
            exchange_ts_ns=int(row["exchange_ts_ns"]),
            recv_ts_ns=int(row["recv_ts_ns"]),
            price=float(row["price_raw"]) / 1e8,
            qty=float(row["qty_raw"]) / 1e8,
            trade_id=int(row["trade_id"]),
            symbol_id=int(row["symbol_id"]),
            side=int(row["side"]),
        )

    # Books: write_book takes raw int64 directly, lossless round-trip.
    headers, levels = reader.read_books()
    if headers.size:
        for h in headers:
            offset = int(h["level_offset"])
            n_bid = int(h["bid_count"])
            n_ask = int(h["ask_count"])
            bids = levels[offset:offset + n_bid]
            asks = levels[offset + n_bid:offset + n_bid + n_ask]
            writer.write_book(
                exchange_ts_ns=int(h["exchange_ts_ns"]),
                recv_ts_ns=int(h["recv_ts_ns"]),
                seq=int(h["seq"]),
                symbol_id=int(h["symbol_id"]),
                is_snapshot=int(h["event_type"]) == 2,
                bids=bids,
                asks=asks,
            )
    writer.close()

    # Synthesize a metadata.json that maps the global symbol IDs back to
    # readable (exchange, name) tuples. The viewer reads this for chart
    # legends / tooltips.
    sym_table = reader.symbol_table()
    metadata = {
        "exchange": "merged",
        "exchange_type": "synthetic",
        "instrument_type": "merged",
        "symbols": [
            {
                "symbol_id": int(s["global_id"]),
                "name": f"{s['exchange']}/{s['name']}",
                "base_asset": "",
                "quote_asset": "",
                "price_precision": int(s["price_precision"]),
                "qty_precision": int(s["qty_precision"]),
            }
            for s in sym_table
        ],
        "has_trades": True,
        "has_book_snapshots": bool(headers.size),
        "has_book_deltas": False,
        "has_book_tops": False,
        "price_scale": 100000000,
        "qty_scale": 100000000,
        "description": (
            "Merged view of: " + ", ".join(str(p) for p in paths)
        ),
    }
    (out_dir / "metadata.json").write_text(json.dumps(metadata, indent=2))


def cmd_tape_view(args: argparse.Namespace) -> int:
    """Serve the static replay viewer locally with the requested tape /
    run directory pre-staged, then open a browser."""
    import http.server
    import socketserver
    import threading
    import webbrowser
    import shutil as _shutil

    pkg_dir = Path(__file__).resolve().parent
    # Look up the bundled viewer SPA. It is shipped under
    # `lrvx/data/replay-viewer/dist/` for installed wheels and falls
    # back to the in-repo source build during development.
    candidates = [
        pkg_dir / "data" / "replay-viewer",
        pkg_dir.parent.parent / "tools" / "replay-viewer" / "dist",
    ]
    viewer_root = next((c for c in candidates if c.exists()), None)
    if viewer_root is None:
        print("lrvx tape view: viewer build not found. Run "
              "`cd tools/replay-viewer && npm install && npm run build` "
              "first, or install a published wheel.", file=sys.stderr)
        return 1

    tape_paths: list[Path] = []
    run_path: Optional[Path] = None
    for raw in args.paths:
        p = Path(raw).expanduser().resolve()
        if not p.exists():
            print(f"lrvx tape view: path not found: {p}", file=sys.stderr)
            return 1
        suffix = p.suffix.lower()
        if suffix in _TAPE_SUFFIXES or any(
                any(p.glob(f"*{ext}")) for ext in _TAPE_SUFFIXES):
            tape_paths.append(p)
        elif suffix in _RUN_SUFFIXES:
            run_path = p
        else:
            print(f"lrvx tape view: cannot tell artifact kind from {p}",
                  file=sys.stderr)
            return 1

    # Stage a copy of the artifacts inside a temp serve root so the
    # viewer can fetch them without same-origin gymnastics.
    import tempfile
    work = Path(tempfile.mkdtemp(prefix="lrvx-view-"))
    serve_root = work / "site"
    _shutil.copytree(viewer_root, serve_root)
    fixtures = serve_root / "fixture-cli"
    fixtures.mkdir()

    if len(tape_paths) == 1:
        _shutil.copytree(tape_paths[0], fixtures / "tape")
    elif len(tape_paths) >= 2:
        # Multi-tape view — materialize a merged synthetic tape into
        # fixtures/tape so the existing single-tape SPA renders the
        # union. The merged tape carries rekeyed (global) symbol IDs.
        # SPA-side multi-venue legend / per-venue book chart is a
        # separate follow-up; for now the viewer sees one synthetic
        # stream with N distinct symbol IDs and colours them
        # accordingly.
        _materialize_merged_tape(tape_paths, fixtures / "tape")
    if run_path is not None:
        _shutil.copytree(run_path, fixtures / "run")

    port = args.port

    class Handler(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *_a, **_kw):  # quiet
            return

    Handler.directory = str(serve_root)  # type: ignore[attr-defined]

    httpd = socketserver.TCPServer(("127.0.0.1", port),
                                    lambda *a, **kw: Handler(*a,
                                                              directory=str(serve_root),
                                                              **kw))
    url = f"http://127.0.0.1:{port}/"
    if tape_paths or run_path is not None:
        url += "?autoload=fixture-cli"

    def _serve() -> None:
        httpd.serve_forever()

    t = threading.Thread(target=_serve, daemon=True)
    t.start()
    print(f"lrvx tape view: serving {serve_root} at {url}")
    if not args.no_open:
        webbrowser.open(url)
    print("ctrl+c to stop")
    try:
        t.join()
    except KeyboardInterrupt:
        httpd.shutdown()
    return 0


# ── archive ─────────────────────────────────────────────────────────


def cmd_archive_binance(args: argparse.Namespace) -> int:
    """Convert Binance public archives into a `.lrvx` tape.

    Default product is aggTrades. Pass ``--book t1`` or
    ``--book depth20`` to import bookTicker / bookDepth instead. The
    two book products write to the same tape directory as trades, so a
    single ``--out`` can carry both event streams interleaved.
    """
    from . import archives  # local import keeps the CLI cheap to start

    if args.book:
        return _cmd_archive_binance_book(args, archives)

    if args.csv:
        stats = archives.binance.aggtrades_to_lrvx(
            csv_path=args.csv,
            out_tape=args.out,
            symbol_id=args.symbol_id,
            symbol_name=args.symbol or "",
            market=args.market,
            exchange_id=args.exchange_id,
            exchange_name=args.exchange_name,
            append=not args.no_append,
            max_segment_mb=args.max_segment_mb,
            compression=args.compression,
        )
    else:
        if not (args.symbol and args.date_from and args.date_to):
            print("lrvx archive binance: --symbol, --from and --to are "
                  "required when --csv is not used.",
                  file=sys.stderr)
            return 2
        stats = archives.binance.range_to_lrvx(
            symbol=args.symbol,
            market=args.market,
            date_from=args.date_from,
            date_to=args.date_to,
            out_tape=args.out,
            mirror=args.mirror,
            parallel=args.parallel,
            symbol_id=args.symbol_id,
            exchange_id=args.exchange_id,
            exchange_name=args.exchange_name,
            append=not args.no_append,
            max_segment_mb=args.max_segment_mb,
            compression=args.compression,
            skip_missing=args.skip_missing,
        )

    print(
        f"lrvx archive binance: trades_written={stats.trades_written} "
        f"rows_read={stats.rows_read} rows_skipped={stats.rows_skipped} "
        f"files={stats.files_processed}"
    )
    return 0


def _cmd_archive_binance_book(args: argparse.Namespace, archives) -> int:
    """Implementation for `lrvx archive binance --book {t1|depth20} ...`.

    Mirrors `cmd_archive_binance` for the book products: --csv path for
    one day, otherwise the date-range form with optional download
    mirror."""
    book_type = args.book
    book_kwargs = dict(
        symbol_id=args.symbol_id,
        symbol_name=args.symbol or "",
        market=args.market,
        exchange_id=args.exchange_id,
        exchange_name=args.exchange_name,
        append=not args.no_append,
        max_segment_mb=args.max_segment_mb,
        compression=args.compression,
    )

    if args.csv:
        if book_type == "t1":
            stats = archives.binance.t1_to_lrvx(
                csv_path=args.csv, out_tape=args.out, **book_kwargs)
        else:
            stats = archives.binance.depth20_to_lrvx(
                csv_path=args.csv, out_tape=args.out,
                levels=args.book_levels, **book_kwargs)
    else:
        if not (args.symbol and args.date_from and args.date_to):
            print("lrvx archive binance --book: --symbol, --from and --to "
                  "are required when --csv is not used.",
                  file=sys.stderr)
            return 2
        stats = archives.binance.range_book_to_lrvx(
            symbol=args.symbol,
            market=args.market,
            book_type=book_type,
            date_from=args.date_from,
            date_to=args.date_to,
            out_tape=args.out,
            mirror=args.mirror,
            parallel=args.parallel,
            levels=args.book_levels,
            symbol_id=args.symbol_id,
            exchange_id=args.exchange_id,
            exchange_name=args.exchange_name,
            append=not args.no_append,
            max_segment_mb=args.max_segment_mb,
            compression=args.compression,
            skip_missing=args.skip_missing,
        )

    print(
        f"lrvx archive binance --book {book_type}: "
        f"snapshots={stats.snapshots_written} "
        f"deltas={stats.deltas_written} "
        f"rows_read={stats.rows_read} rows_skipped={stats.rows_skipped} "
        f"files={stats.files_processed}"
    )
    return 0


def cmd_archive_bybit(args: argparse.Namespace) -> int:
    """Convert Bybit public trade archives into a `.lrvx` tape."""
    from . import archives

    if args.csv:
        stats = archives.bybit.trades_to_lrvx(
            csv_path=args.csv,
            out_tape=args.out,
            symbol_id=args.symbol_id,
            symbol_name=args.symbol or "",
            market=args.market,
            exchange_id=args.exchange_id,
            exchange_name=args.exchange_name,
            append=not args.no_append,
            max_segment_mb=args.max_segment_mb,
            compression=args.compression,
        )
    else:
        if not (args.symbol and args.date_from and args.date_to):
            print("lrvx archive bybit: --symbol, --from and --to are "
                  "required when --csv is not used.", file=sys.stderr)
            return 2
        stats = archives.bybit.range_to_lrvx(
            symbol=args.symbol,
            market=args.market,
            date_from=args.date_from,
            date_to=args.date_to,
            out_tape=args.out,
            mirror=args.mirror,
            parallel=args.parallel,
            symbol_id=args.symbol_id,
            exchange_id=args.exchange_id,
            exchange_name=args.exchange_name,
            append=not args.no_append,
            max_segment_mb=args.max_segment_mb,
            compression=args.compression,
            skip_missing=args.skip_missing,
        )

    print(
        f"lrvx archive bybit: trades_written={stats.trades_written} "
        f"rows_read={stats.rows_read} rows_skipped={stats.rows_skipped} "
        f"files={stats.files_processed}"
    )
    return 0


def cmd_archive_okx(args: argparse.Namespace) -> int:
    """Convert OKX public trade archives into a `.lrvx` tape."""
    from . import archives

    if args.csv:
        stats = archives.okx.trades_to_lrvx(
            csv_path=args.csv,
            out_tape=args.out,
            symbol_id=args.symbol_id,
            symbol_name=args.symbol or "",
            market=args.market,
            exchange_id=args.exchange_id,
            exchange_name=args.exchange_name,
            append=not args.no_append,
            max_segment_mb=args.max_segment_mb,
            compression=args.compression,
        )
    else:
        if not (args.symbol and args.date_from and args.date_to):
            print("lrvx archive okx: --symbol, --from and --to are "
                  "required when --csv is not used.", file=sys.stderr)
            return 2
        stats = archives.okx.range_to_lrvx(
            symbol=args.symbol,
            market=args.market,
            date_from=args.date_from,
            date_to=args.date_to,
            out_tape=args.out,
            mirror=args.mirror,
            parallel=args.parallel,
            symbol_id=args.symbol_id,
            exchange_id=args.exchange_id,
            exchange_name=args.exchange_name,
            append=not args.no_append,
            max_segment_mb=args.max_segment_mb,
            compression=args.compression,
            skip_missing=args.skip_missing,
        )

    print(
        f"lrvx archive okx: trades_written={stats.trades_written} "
        f"rows_read={stats.rows_read} rows_skipped={stats.rows_skipped} "
        f"files={stats.files_processed}"
    )
    return 0


def cmd_archive_bitget(args: argparse.Namespace) -> int:
    """Convert Bitget public trade archives into a `.lrvx` tape."""
    from . import archives

    if args.csv:
        stats = archives.bitget.trades_to_lrvx(
            csv_path=args.csv,
            out_tape=args.out,
            symbol_id=args.symbol_id,
            symbol_name=args.symbol or "",
            market=args.market,
            exchange_id=args.exchange_id,
            exchange_name=args.exchange_name,
            append=not args.no_append,
            max_segment_mb=args.max_segment_mb,
            compression=args.compression,
        )
    else:
        if not (args.symbol and args.date_from and args.date_to):
            print("lrvx archive bitget: --symbol, --from and --to are "
                  "required when --csv is not used.", file=sys.stderr)
            return 2
        stats = archives.bitget.range_to_lrvx(
            symbol=args.symbol,
            market=args.market,
            date_from=args.date_from,
            date_to=args.date_to,
            out_tape=args.out,
            mirror=args.mirror,
            parallel=args.parallel,
            symbol_id=args.symbol_id,
            exchange_id=args.exchange_id,
            exchange_name=args.exchange_name,
            append=not args.no_append,
            max_segment_mb=args.max_segment_mb,
            compression=args.compression,
            skip_missing=args.skip_missing,
        )

    print(
        f"lrvx archive bitget: trades_written={stats.trades_written} "
        f"rows_read={stats.rows_read} rows_skipped={stats.rows_skipped} "
        f"files={stats.files_processed}"
    )
    return 0


def cmd_archive_deribit(args: argparse.Namespace) -> int:
    """Convert Deribit public trade archives into a `.lrvx` tape."""
    from . import archives

    if args.csv:
        stats = archives.deribit.trades_to_lrvx(
            csv_path=args.csv,
            out_tape=args.out,
            symbol_id=args.symbol_id,
            symbol_name=args.symbol or "",
            market=args.market,
            exchange_id=args.exchange_id,
            exchange_name=args.exchange_name,
            append=not args.no_append,
            max_segment_mb=args.max_segment_mb,
            compression=args.compression,
        )
    else:
        if not (args.symbol and args.date_from and args.date_to):
            print("lrvx archive deribit: --symbol, --from and --to are "
                  "required when --csv is not used.", file=sys.stderr)
            return 2
        stats = archives.deribit.range_to_lrvx(
            symbol=args.symbol,
            market=args.market,
            date_from=args.date_from,
            date_to=args.date_to,
            out_tape=args.out,
            mirror=args.mirror,
            parallel=args.parallel,
            symbol_id=args.symbol_id,
            exchange_id=args.exchange_id,
            exchange_name=args.exchange_name,
            append=not args.no_append,
            max_segment_mb=args.max_segment_mb,
            compression=args.compression,
            skip_missing=args.skip_missing,
        )

    print(
        f"lrvx archive deribit: trades_written={stats.trades_written} "
        f"rows_read={stats.rows_read} rows_skipped={stats.rows_skipped} "
        f"files={stats.files_processed}"
    )
    return 0


# ── Argparse setup ─────────────────────────────────────────────────────


def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="lrvx",
        description="lrvx project scaffolding & utilities.",
    )
    sub = p.add_subparsers(dest="command", required=True)

    p_new = sub.add_parser("new", help="Scaffold a new lrvx project.")
    p_new.add_argument("name", help="project name (used as directory & slug)")
    p_new.add_argument(
        "--template", default="research",
        help="template to use (default: research). "
             "Run `lrvx templates` to list.",
    )
    p_new.add_argument(
        "--here", action="store_true",
        help="scaffold into the current directory instead of "
             "creating a new one",
    )
    p_new.set_defaults(handler=cmd_new)

    p_t = sub.add_parser("templates", help="List available templates.")
    p_t.set_defaults(handler=cmd_templates)

    p_r = sub.add_parser("report", help="Render a backtest stats JSON to HTML.")
    p_r.add_argument("stats", help="path to a JSON file with backtest stats")
    p_r.add_argument(
        "-o", "--output", default="report.html",
        help="output HTML path (default: report.html)",
    )
    p_r.add_argument(
        "--equity",
        help="optional path to a JSON dump of the equity curve "
             "(keys: timestamp_ns, equity, drawdown_pct)",
    )
    p_r.add_argument(
        "--trades",
        help="optional path to a JSON dump of trades "
             "(keys: symbol, side, entry_price, exit_price, quantity, pnl, "
             "fee, entry_time_ns, exit_time_ns)",
    )
    p_r.add_argument("--title", default=None, help="report title")
    p_r.add_argument("--subtitle", default=None, help="subtitle")
    p_r.set_defaults(handler=cmd_report)

    # ── engine ─────────────────────────────────────────────────────
    from . import engine_cli
    engine_cli.add_engine_subparser(sub)

    # ── tape ───────────────────────────────────────────────────────
    p_tape = sub.add_parser(
        "tape",
        help="Record / replay / inspect deterministic .lrvx tapes.",
    )
    tape_sub = p_tape.add_subparsers(dest="tape_command", required=True)

    p_rec = tape_sub.add_parser(
        "record",
        help="Stream a CCXT exchange feed into a .lrvx directory.",
    )
    p_rec.add_argument("exchange", nargs="?", default=None,
                       help="CCXT exchange id (single-venue form). "
                            "Optional when --venue is used.")
    p_rec.add_argument("symbol", nargs="?", default=None,
                       help="Symbol in CCXT spelling (single-venue form). "
                            "Optional when --venue is used.")
    p_rec.add_argument(
        "--venue", action="append", default=[],
        metavar="EXCHANGE:SYM1[,SYM2,...]",
        help=("Multi-venue form. Repeatable. Each --venue records one "
              "exchange × N symbols into <output>/<exchange>/. Example: "
              "`--venue bybit:BTC/USDT:USDT --venue binance:BTC/USDT:USDT`. "
              "Mutually exclusive with the positional exchange/symbol "
              "form (which stays as a one-venue alias)."),
    )
    p_rec.add_argument(
        "--duration", default=None,
        help="Stop after this much wall time (e.g. '60s', '5m', '1h'). "
             "Default: run until SIGINT.",
    )
    p_rec.add_argument(
        "--output", "-o", required=True,
        help="Output directory for the .lrvx segments + metadata.",
    )
    p_rec.add_argument(
        "--max-segment-mb", type=int, default=256,
        help="Rotate to a new segment after this many MB (default: 256).",
    )
    p_rec.add_argument(
        "--testnet", action="store_true",
        help="Use the exchange's testnet/sandbox endpoint.",
    )
    p_rec.add_argument(
        "--book-depth", type=int, default=None,
        help=("Order-book depth subscribed via watchOrderBook. Exchanges have "
              "different valid ranges (e.g. bitget perp only accepts books5 / "
              "books15, bybit accepts 1/50/200/1000). Leave unset to let the "
              "broker pick a venue-appropriate default."),
    )
    p_rec.set_defaults(handler=cmd_tape_record)

    p_rep = tape_sub.add_parser(
        "replay",
        help="Replay a recorded tape — by default print summary stats.",
    )
    p_rep.add_argument("path", help="Path to the .lrvx directory.")
    p_rep.add_argument(
        "--limit", type=int, default=None,
        help="Stop after dispatching this many trades (debugging aid).",
    )
    p_rep.set_defaults(handler=cmd_tape_replay)

    p_ins = tape_sub.add_parser(
        "inspect",
        help="Print summary stats (event count, time range, symbols) for a tape.",
    )
    p_ins.add_argument("path", help="Path to the .lrvx directory.")
    p_ins.set_defaults(handler=cmd_tape_inspect)

    p_view = tape_sub.add_parser(
        "view",
        help="Open the replay viewer in a browser, pre-loaded with the "
             "given .lrvx and / or .lrvxrun directory.",
    )
    p_view.add_argument("paths", nargs="+",
                        help="One or more .lrvx directories and/or one "
                             ".lrvxrun. With multiple .lrvx dirs the "
                             "viewer renders a merged synthetic tape "
                             "(symbols rekeyed into a global id space "
                             "keyed by (exchange, name) — see "
                             "docs/how-to/multi-tape.md).")
    p_view.add_argument("--port", type=int, default=8765,
                        help="Local port to serve the viewer on (default 8765).")
    p_view.add_argument("--no-open", action="store_true",
                        help="Do not auto-open a browser tab; just print the URL.")
    p_view.set_defaults(handler=cmd_tape_view)

    p_diff = tape_sub.add_parser(
        "diff",
        help="Compare two .lrvx directories trade-by-trade. "
             "Exits 0 when equal, 1 when tapes diverge.",
    )
    p_diff.add_argument("left", help="Path to the first .lrvx directory.")
    p_diff.add_argument("right", help="Path to the second .lrvx directory.")
    p_diff.add_argument(
        "--max-mismatches", type=int, default=16,
        help="Stop recording per-trade mismatches after this many. Default 16.",
    )
    p_diff.add_argument(
        "--ts-tolerance-ns", type=int, default=0,
        help="Allow this much jitter on exchange_ts_ns when comparing. Default 0.",
    )
    p_diff.add_argument(
        "--json", action="store_true",
        help="Emit the diff as JSON instead of human-readable lines.",
    )
    p_diff.set_defaults(handler=cmd_tape_diff)

    # ── bundle ─────────────────────────────────────────────────────
    p_bundle = sub.add_parser(
        "bundle",
        help="Pack / replay / validate a reproducibility bundle "
             "(strategy + tape + expected output).",
    )
    bundle_sub = p_bundle.add_subparsers(dest="bundle_command", required=True)

    p_pack = bundle_sub.add_parser(
        "pack",
        help="Build a bundle from a strategy file + tape directory.",
    )
    p_pack.add_argument(
        "--strategy", required=True,
        help="Path to a .py file containing one lrvx.Strategy subclass.",
    )
    p_pack.add_argument(
        "--tape", required=True,
        help="Path to a .lrvx directory the strategy will replay against.",
    )
    p_pack.add_argument(
        "--output", "-o", required=True,
        help="Bundle output path (.tar).",
    )
    p_pack.add_argument(
        "--symbol-name", default="BTCUSDT",
        help="Symbol name to register (default: BTCUSDT).",
    )
    p_pack.add_argument(
        "--exchange", default="bundle",
        help="Exchange tag for the symbol (default: bundle).",
    )
    p_pack.add_argument(
        "--tick-size", type=float, default=0.01,
        help="Tick size for the symbol (default: 0.01).",
    )
    p_pack.add_argument(
        "--slippage-model", default="none",
        choices=("none", "fixed_ticks", "fixed_bps", "volume_impact"),
        help="Slippage model for the simulator (default: none).",
    )
    p_pack.add_argument(
        "--slippage-bps", type=float, default=0.0,
        help="Slippage in basis points (used when slippage-model=fixed_bps).",
    )
    p_pack.set_defaults(handler=cmd_bundle_pack)

    p_replay = bundle_sub.add_parser(
        "replay",
        help="Extract a bundle, replay the strategy, print actual vs expected. "
             "WARNING: this runs strategy/strategy.py from inside the bundle "
             "-- only replay bundles from a source you trust.",
    )
    p_replay.add_argument("path", help="Path to the bundle .tar file.")
    p_replay.set_defaults(handler=cmd_bundle_replay)

    p_validate = bundle_sub.add_parser(
        "validate",
        help="Replay a bundle and compare the result with its expected output "
             "field by field: trade count, every fill's symbol, side, price "
             "and quantity, total filled quantity, plus the strategy and tape "
             "hashes and the engine version. "
             "Exit 0 on match, 1 on divergence (with diff printed). "
             "WARNING: this runs strategy/strategy.py from inside the bundle "
             "-- only validate bundles from a source you trust.",
    )
    p_validate.add_argument("path", help="Path to the bundle .tar file.")
    p_validate.set_defaults(handler=cmd_bundle_validate)

    # ── archive ─────────────────────────────────────────────────────
    p_archive = sub.add_parser(
        "archive",
        help="Import public historical archives (Binance public aggTrades "
             "today; other venues land as separate subcommands).",
    )
    archive_sub = p_archive.add_subparsers(dest="archive_command", required=True)

    p_binance = archive_sub.add_parser(
        "binance",
        help="Convert Binance public aggTrades zip archives into a "
             "`.lrvx` tape (single day via --csv, or a date range "
             "via --symbol/--from/--to).",
    )
    p_binance.add_argument(
        "--csv", default=None,
        help="Path to a single aggTrades zip or extracted CSV. When set, "
             "--from / --to are ignored and a one-file conversion is run.",
    )
    p_binance.add_argument(
        "--symbol", default=None,
        help="Symbol (e.g. BTCUSDT). Required for the date-range form.",
    )
    p_binance.add_argument(
        "--market", default="um-futures",
        choices=("spot", "um-futures", "cm-futures"),
        help="Binance market segment (default: um-futures).",
    )
    p_binance.add_argument("--from", dest="date_from", default=None,
                           help="Start date YYYY-MM-DD (inclusive).")
    p_binance.add_argument("--to", dest="date_to", default=None,
                           help="End date YYYY-MM-DD (inclusive).")
    p_binance.add_argument(
        "--out", required=True,
        help="Output `.lrvx` directory.",
    )
    p_binance.add_argument(
        "--mirror", default=None,
        help="Local mirror cache for downloaded zips. Without this a "
             "tempdir is used and discarded.",
    )
    p_binance.add_argument(
        "--parallel", type=int, default=4,
        help="Concurrent downloads (default: 4). Conversion stays serial.",
    )
    p_binance.add_argument("--symbol-id", dest="symbol_id", type=int, default=1,
                           help="Symbol ID to stamp on every trade (default: 1).")
    p_binance.add_argument("--exchange-id", dest="exchange_id", type=int, default=0,
                           help="Exchange ID stamped into the tape header.")
    p_binance.add_argument("--exchange-name", dest="exchange_name", default="binance",
                           help="Exchange tag written into metadata.json.")
    p_binance.add_argument("--max-segment-mb", dest="max_segment_mb",
                           type=int, default=256,
                           help="Rotate to a new segment after this many MB "
                                "(default: 256).")
    p_binance.add_argument("--compression", default="none",
                           choices=("none", "lz4"),
                           help="Segment compression (default: none).")
    p_binance.add_argument("--no-append", action="store_true",
                           help="Disable agg_trade_id dedup against an "
                                "existing tape; every row is written even "
                                "if its trade_id was already present.")
    p_binance.add_argument("--skip-missing", action="store_true",
                           help="Skip days that fail to download instead of "
                                "aborting the whole range.")
    p_binance.add_argument("--book", default=None,
                           choices=("t1", "depth20"),
                           help="Switch to the book archive product. "
                                "`t1` imports bookTicker (best bid/ask "
                                "snapshots); `depth20` imports bookDepth "
                                "(top-20 levels per side, delta-encoded "
                                "against the previous snapshot). Without "
                                "this flag, aggTrades is imported.")
    p_binance.add_argument("--book-levels", dest="book_levels", type=int,
                           default=20,
                           help="Maximum levels per side to keep when "
                                "importing depth20 (default: 20).")
    p_binance.set_defaults(handler=cmd_archive_binance)

    p_bybit = archive_sub.add_parser(
        "bybit",
        help="Convert Bybit public archive (`public.bybit.com`) trade "
             "ticks into a `.lrvx` tape. Single day via `--csv` or "
             "a date range via `--symbol/--from/--to`.",
    )
    p_bybit.add_argument("--csv", default=None,
                         help="Path to a single Bybit archive `.csv.gz` "
                              "or extracted `.csv`. When set, --from / "
                              "--to are ignored.")
    p_bybit.add_argument("--symbol", default=None,
                         help="Symbol in Bybit naming (e.g. BTCUSDT for "
                              "linear, BTCUSD for inverse, BTCUSDT for spot).")
    p_bybit.add_argument("--market", default="linear",
                         choices=("spot", "linear", "inverse"),
                         help="Bybit market segment (default: linear).")
    p_bybit.add_argument("--from", dest="date_from", default=None,
                         help="Start date YYYY-MM-DD (inclusive).")
    p_bybit.add_argument("--to", dest="date_to", default=None,
                         help="End date YYYY-MM-DD (inclusive).")
    p_bybit.add_argument("--out", required=True,
                         help="Output `.lrvx` directory.")
    p_bybit.add_argument("--mirror", default=None,
                         help="Local mirror cache for downloaded files. "
                              "Defaults to the shared archive cache "
                              "(`~/.lrvx/archive-cache/bybit`).")
    p_bybit.add_argument("--parallel", type=int, default=4,
                         help="Concurrent downloads (default: 4). "
                              "Conversion stays serial.")
    p_bybit.add_argument("--symbol-id", dest="symbol_id", type=int, default=1,
                         help="Symbol ID stamped on every trade.")
    p_bybit.add_argument("--exchange-id", dest="exchange_id", type=int, default=0,
                         help="Exchange ID stamped into the tape header.")
    p_bybit.add_argument("--exchange-name", dest="exchange_name", default="bybit",
                         help="Exchange tag written into metadata.json.")
    p_bybit.add_argument("--max-segment-mb", dest="max_segment_mb",
                         type=int, default=256,
                         help="Rotate to a new segment after N MB (default: 256).")
    p_bybit.add_argument("--compression", default="none",
                         choices=("none", "lz4"),
                         help="Segment compression (default: none).")
    p_bybit.add_argument("--no-append", action="store_true",
                         help="Disable trade_id dedup against an existing tape.")
    p_bybit.add_argument("--skip-missing", action="store_true",
                         help="Skip days that fail to download.")
    p_bybit.set_defaults(handler=cmd_archive_bybit)

    p_okx = archive_sub.add_parser(
        "okx",
        help="Convert OKX public archive (`cdn/okex/traderecords/`) "
             "trade ticks into a `.lrvx` tape. Single day via "
             "`--csv` or a date range via `--symbol/--from/--to`.",
    )
    p_okx.add_argument("--csv", default=None,
                       help="Path to a single OKX archive `.zip` / `.csv.gz` "
                            "/ `.csv`. When set, --from / --to are ignored.")
    p_okx.add_argument("--symbol", default=None,
                       help="Symbol in OKX naming "
                            "(e.g. BTC-USDT-SWAP for perp, BTC-USDT for spot, "
                            "BTC-29MAR24-50000-C for options).")
    p_okx.add_argument("--market", default="swap",
                       choices=("spot", "swap", "futures", "option"),
                       help="OKX market segment (default: swap).")
    p_okx.add_argument("--from", dest="date_from", default=None,
                       help="Start date YYYY-MM-DD (inclusive).")
    p_okx.add_argument("--to", dest="date_to", default=None,
                       help="End date YYYY-MM-DD (inclusive).")
    p_okx.add_argument("--out", required=True,
                       help="Output `.lrvx` directory.")
    p_okx.add_argument("--mirror", default=None,
                       help="Local mirror cache for downloads. Defaults to "
                            "the shared archive cache (`~/.lrvx/archive-cache/okx`).")
    p_okx.add_argument("--parallel", type=int, default=4,
                       help="Concurrent downloads (default: 4). "
                            "Conversion stays serial.")
    p_okx.add_argument("--symbol-id", dest="symbol_id", type=int, default=1,
                       help="Symbol ID stamped on every trade.")
    p_okx.add_argument("--exchange-id", dest="exchange_id", type=int, default=0,
                       help="Exchange ID stamped into the tape header.")
    p_okx.add_argument("--exchange-name", dest="exchange_name", default="okx",
                       help="Exchange tag written into metadata.json.")
    p_okx.add_argument("--max-segment-mb", dest="max_segment_mb",
                       type=int, default=256,
                       help="Rotate to a new segment after N MB (default: 256).")
    p_okx.add_argument("--compression", default="none",
                       choices=("none", "lz4"),
                       help="Segment compression (default: none).")
    p_okx.add_argument("--no-append", action="store_true",
                       help="Disable trade_id dedup against an existing tape.")
    p_okx.add_argument("--skip-missing", action="store_true",
                       help="Skip days that fail to download.")
    p_okx.set_defaults(handler=cmd_archive_okx)

    p_bitget = archive_sub.add_parser(
        "bitget",
        help="Convert Bitget public archive trade ticks into a "
             "`.lrvx` tape. Single day via `--csv` or a date range "
             "via `--symbol/--from/--to`.",
    )
    p_bitget.add_argument("--csv", default=None,
                          help="Path to a single Bitget archive `.zip` / "
                               "`.csv.gz` / `.csv`.")
    p_bitget.add_argument("--symbol", default=None,
                          help="Symbol in Bitget naming "
                               "(e.g. BTCUSDT for spot / umcbl, "
                               "BTCUSD for cmcbl).")
    p_bitget.add_argument("--market", default="umcbl",
                          choices=("spot", "umcbl", "cmcbl"),
                          help="Bitget market segment (default: umcbl).")
    p_bitget.add_argument("--from", dest="date_from", default=None,
                          help="Start date YYYY-MM-DD (inclusive).")
    p_bitget.add_argument("--to", dest="date_to", default=None,
                          help="End date YYYY-MM-DD (inclusive).")
    p_bitget.add_argument("--out", required=True,
                          help="Output `.lrvx` directory.")
    p_bitget.add_argument("--mirror", default=None,
                          help="Local mirror cache. Defaults to "
                               "`~/.lrvx/archive-cache/bitget`.")
    p_bitget.add_argument("--parallel", type=int, default=4,
                          help="Concurrent downloads (default: 4).")
    p_bitget.add_argument("--symbol-id", dest="symbol_id", type=int, default=1,
                          help="Symbol ID stamped on every trade.")
    p_bitget.add_argument("--exchange-id", dest="exchange_id", type=int, default=0,
                          help="Exchange ID stamped into the tape header.")
    p_bitget.add_argument("--exchange-name", dest="exchange_name", default="bitget",
                          help="Exchange tag written into metadata.json.")
    p_bitget.add_argument("--max-segment-mb", dest="max_segment_mb",
                          type=int, default=256,
                          help="Rotate to a new segment after N MB (default: 256).")
    p_bitget.add_argument("--compression", default="none",
                          choices=("none", "lz4"),
                          help="Segment compression (default: none).")
    p_bitget.add_argument("--no-append", action="store_true",
                          help="Disable trade_id dedup against an existing tape.")
    p_bitget.add_argument("--skip-missing", action="store_true",
                          help="Skip days that fail to download.")
    p_bitget.set_defaults(handler=cmd_archive_bitget)

    p_deribit = archive_sub.add_parser(
        "deribit",
        help="Convert Deribit public archive trade ticks into a "
             "`.lrvx` tape. Covers perpetual / dated future / "
             "option instruments. Single day via `--csv` or a date "
             "range via `--symbol/--from/--to`.",
    )
    p_deribit.add_argument("--csv", default=None,
                           help="Path to a single Deribit archive "
                                "`.csv.gz` / `.csv` / `.zip`.")
    p_deribit.add_argument("--symbol", default=None,
                           help="Instrument in Deribit naming "
                                "(e.g. BTC-PERPETUAL, BTC-29MAR24, "
                                "BTC-29MAR24-50000-C).")
    p_deribit.add_argument("--market", default="perpetual",
                           choices=("perpetual", "future", "option"),
                           help="Deribit market segment (default: perpetual).")
    p_deribit.add_argument("--from", dest="date_from", default=None,
                           help="Start date YYYY-MM-DD (inclusive).")
    p_deribit.add_argument("--to", dest="date_to", default=None,
                           help="End date YYYY-MM-DD (inclusive).")
    p_deribit.add_argument("--out", required=True,
                           help="Output `.lrvx` directory.")
    p_deribit.add_argument("--mirror", default=None,
                           help="Local mirror cache. Defaults to "
                                "`~/.lrvx/archive-cache/deribit`.")
    p_deribit.add_argument("--parallel", type=int, default=4,
                           help="Concurrent downloads (default: 4).")
    p_deribit.add_argument("--symbol-id", dest="symbol_id", type=int, default=1,
                           help="Symbol ID stamped on every trade.")
    p_deribit.add_argument("--exchange-id", dest="exchange_id", type=int, default=0,
                           help="Exchange ID stamped into the tape header.")
    p_deribit.add_argument("--exchange-name", dest="exchange_name", default="deribit",
                           help="Exchange tag written into metadata.json.")
    p_deribit.add_argument("--max-segment-mb", dest="max_segment_mb",
                           type=int, default=256,
                           help="Rotate to a new segment after N MB (default: 256).")
    p_deribit.add_argument("--compression", default="none",
                           choices=("none", "lz4"),
                           help="Segment compression (default: none).")
    p_deribit.add_argument("--no-append", action="store_true",
                           help="Disable trade_id dedup against an existing tape.")
    p_deribit.add_argument("--skip-missing", action="store_true",
                           help="Skip days that fail to download.")
    p_deribit.set_defaults(handler=cmd_archive_deribit)

    # ── lint (lookahead) ────────────────────────────────────────────
    p_lint = sub.add_parser(
        "lint",
        help="Static-analysis lints over strategy code.",
    )
    lint_sub = p_lint.add_subparsers(dest="lint_command", required=True)

    p_lookahead = lint_sub.add_parser(
        "lookahead",
        help="Detect common lookahead-bias patterns "
             "(.shift(-N), forward index arithmetic, open-upper "
             "slices in per-bar callbacks, future-named attributes).",
    )
    p_lookahead.add_argument("path", help="Path to the strategy .py file.")
    p_lookahead.add_argument(
        "--json", action="store_true",
        help="Emit findings as JSON instead of human-readable lines.",
    )
    p_lookahead.set_defaults(handler=cmd_lint_lookahead)

    return p


def main(argv: Optional[Iterable[str]] = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(list(argv) if argv is not None else None)
    return args.handler(args)


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
