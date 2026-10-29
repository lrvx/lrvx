#!/usr/bin/env python3
"""
scripts/gen_llms_txt.py

Generate `docs/llms.txt` and `docs/llms-full.txt` per the llmstxt.org spec
so AI agents (Cursor, Claude Code, Cline, etc.) can ground themselves in
lrvx docs.

`llms.txt` is a short curated map (sections + links). `llms-full.txt` is
the concatenated body of the same curated set — small enough to fit a
modern model's context window. Auto-generated C++ core API reference
(`docs/reference/api/{aggregator,backtest,book,...}`) is intentionally
excluded: it adds bulk without helping binding users write valid code,
and C++ extension authors will read the headers anyway.

CI gate: run this before push; if `git diff docs/llms*.txt` is non-empty
after running, the files are stale.

Usage:
    python3 scripts/gen_llms_txt.py            # write
    python3 scripts/gen_llms_txt.py --check    # verify in sync, exit 1 if not
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DOCS = REPO / "docs"
DOCS_BASE_URL = "https://docs.lrvx.dev"

LLMS_TXT_PATH = DOCS / "llms.txt"
LLMS_FULL_PATH = DOCS / "llms-full.txt"

PROJECT_SUMMARY = (
    "lrvx is a C++23 high-performance trading framework with first-class "
    "bindings for Python, Node.js, Codon, and embedded JavaScript. Use it "
    "to build strategies, backtest on historical data, and run live trading "
    "from any of these languages — the same C++ core powers all of them, "
    "so behavior is identical across runtimes."
)

# (relative path, link title, short description)
Entry = tuple[str, str, str]

# Single source of truth: an ordered list of sections, each with entries.
# llms.txt lists titles+descriptions; llms-full.txt concatenates the bodies
# in the same order. C++ core API reference is omitted on purpose — see
# the module docstring.
SECTIONS: list[tuple[str, list[Entry]]] = [
    ("Overview", [
        ("docs/index.md", "Project overview", "What lrvx is and how the pieces fit"),
        ("docs/bindings/README.md", "Bindings overview", "Pick a language: Python, Node, Codon, JavaScript, C"),
    ]),
    ("Bindings (language entry points)", [
        ("docs/bindings/python.md", "Python binding", "pybind11 module: install, import, lifecycle"),
        ("docs/bindings/node.md", "Node.js binding", "NAPI addon: install, require, lifecycle"),
        ("docs/bindings/codon.md", "Codon binding", "Codon strategy module and runner"),
        ("docs/bindings/javascript.md", "Embedded JavaScript", "QuickJS runner for in-process JS strategies"),
        ("docs/bindings/capi.md", "C API", "ABI-stable C surface for FFI from any language"),
    ]),
    ("Quickstart tutorials", [
        ("docs/tutorials/README.md", "Tutorials index", "Learning-oriented walkthroughs"),
        ("docs/tutorials/python-quickstart.md", "Python quickstart", "First Python strategy in ~10 minutes"),
        ("docs/tutorials/node-quickstart.md", "Node.js quickstart", "First Node strategy"),
        ("docs/tutorials/codon-strategy.md", "Codon quickstart", "First Codon strategy"),
        ("docs/tutorials/quickstart.md", "C++ quickstart", "First C++ strategy"),
        ("docs/tutorials/first-strategy.md", "First strategy walkthrough", "End-to-end strategy structure"),
        ("docs/tutorials/multi-timeframe-strategy.md", "Multi-timeframe strategy", "Aggregating multiple bar intervals"),
        ("docs/tutorials/recording-data.md", "Recording market data", "Capture trades and books for replay"),
        ("docs/tutorials/backtesting.md", "Backtesting tutorial", "Replay recorded data through a strategy"),
        ("docs/tutorials/demo.md", "Run the demo", "Bundled end-to-end demo"),
    ]),
    ("Working examples", [
        ("docs/examples/index.md", "Examples index", "Complete runnable examples"),
        ("docs/examples/python-backtest-vs-live.md", "Python: backtest and live", "Same strategy, two runners"),
        ("docs/examples/python-multi-symbol.md", "Python: multi-symbol", "Strategy across multiple symbols"),
        ("docs/examples/python-ccxt-live.md", "Python: live via CCXT", "CCXT-backed live execution"),
        ("docs/examples/node-backtest-vs-live.md", "Node: backtest and live", "Same strategy in Node"),
        ("docs/examples/codon-backtest-vs-live.md", "Codon: backtest and live", "Same strategy in Codon"),
        ("docs/examples/codon-full-backtest.md", "Codon: full backtest API", "All Codon backtest pieces wired"),
        ("docs/examples/cpp-sma-backtest.md", "C++: SMA backtest", "Plain C++ backtest of an SMA crossover"),
    ]),
    ("How-to: strategy and backtest", [
        ("docs/how-to/README.md", "How-to index", "Task-oriented recipes"),
        ("docs/how-to/strategy-classes.md", "Strategy class shapes", "Strategy, SignalStrategy, when to pick which"),
        ("docs/how-to/backtest.md", "Run a backtest", "Replay-driven backtest setup"),
        ("docs/how-to/backtest-realistic-fills.md", "Realistic fills", "Slippage and queue simulation"),
        ("docs/how-to/interactive-backtest.md", "Interactive backtest", "Step through a backtest"),
        ("docs/how-to/grid-search.md", "Grid search", "Parameter sweep with the optimizer"),
        ("docs/how-to/sweep-over-bars.md", "Sweep parameters over bars", "Load bars once, one bar strategy per combination, processes or threads"),
        ("docs/how-to/robustness-statistics.md", "Overfitting checks for a parameter sweep", "PBO by CSCV, deflated Sharpe ratio, neighbourhood of a grid point"),
        ("docs/how-to/bar-exits.md", "Exits for bar strategies", "ATR stop, Chandelier exit, break-even trail"),
        ("docs/how-to/advanced-orders.md", "Advanced orders", "Bracket, OCO, and conditional orders"),
        ("docs/how-to/multi-exchange-trading.md", "Multi-exchange trading", "Route across venues"),
    ]),
    ("How-to: indicators and aggregation", [
        ("docs/how-to/indicator-graph.md", "Indicator graph", "Wire indicators into the engine"),
        ("docs/how-to/multi-symbol-indicators.md", "Multi-symbol indicators", "Per-symbol state isolation"),
        ("docs/how-to/bar-aggregation.md", "Bar aggregation", "Aggregate trades into bars"),
        ("docs/how-to/custom-bar-policy.md", "Custom bar policy", "Build your own bar trigger"),
        ("docs/how-to/use-volume-profile.md", "Volume profile", "Volume-by-price analysis"),
        ("docs/how-to/aggregate-tape-events.md", "Aggregate tape events", "Streaming aggregator framework: 5 native aggregators in one decompression pass"),
    ]),
    ("How-to: performance and extension (C++)", [
        ("docs/how-to/optimize-performance.md", "Optimize performance", "System-level performance guide"),
        ("docs/how-to/cpu-affinity.md", "CPU affinity", "Pin threads for latency"),
        ("docs/how-to/add-an-indicator.md", "Add a new indicator", "Add an indicator to the C++ registry"),
        ("docs/how-to/custom-connector.md", "Custom connector", "Add a new exchange connector"),
    ]),
    ("How-to: project ops", [
        ("docs/how-to/configuration.md", "Configuration", "Engine and strategy configuration"),
        ("docs/how-to/ci.md", "CI configuration", "Continuous integration setup"),
        ("docs/how-to/contributing.md", "Contributing", "How to contribute"),
    ]),
    ("Errors", [
        ("docs/errors/index.md", "Error code reference", "Catalog of LrvxError codes, format, and lifecycle"),
        ("docs/errors/E_SYM_001.md", "E_SYM_001 — Symbol is not registered", "Fix: call Engine.add_symbol() before referencing the symbol"),
    ]),
    ("AI tooling (MCP)", [
        ("mcp/README.md", "lrvx-mcp — Model Context Protocol server", "Local MCP server that gives AI agents grounded access to indicators, error codes, and the C API"),
    ]),
    ("Reference: Python", [
        ("docs/reference/python/index.md", "Python reference index", "All Python binding modules"),
        ("docs/reference/python/_api_index.md", "Full Python API index", "Auto-generated alphabetical list of every public symbol with signature"),
        ("docs/reference/python/engine.md", "Engine and Backtest", "Engine + backtest entry points"),
        ("docs/reference/python/strategy.md", "Strategy class", "Base Strategy from Python"),
        ("docs/reference/python/indicators.md", "Indicators", "Indicator wrappers"),
        ("docs/reference/python/aggregators.md", "Aggregators", "Bar aggregators"),
        ("docs/reference/python/books.md", "Order books", "Order book wrappers"),
        ("docs/reference/python/profiles.md", "Profiles", "Volume profile, market profile"),
        ("docs/reference/python/positions.md", "Positions", "Position tracking"),
        ("docs/reference/python/replay.md", "Replay", "Binary log replay"),
        ("docs/reference/python/segment_ops.md", "Segment ops", "Segment manipulation"),
        ("docs/reference/python/backtest.md", "Backtest components", "Slippage, simulated executor, etc."),
        ("docs/reference/python/optimizer.md", "Optimizer", "Backtest optimizer"),
    ]),
    ("Reference: Node.js", [
        ("docs/reference/node/index.md", "Node reference index", "All Node binding modules"),
        ("docs/reference/node/strategy.md", "Strategy and Runner", "Base Strategy from Node"),
        ("docs/reference/node/backtest.md", "Backtest components", "Backtest pieces"),
        ("docs/reference/node/indicators.md", "Indicators", "Indicator wrappers"),
        ("docs/reference/node/aggregators.md", "Bar aggregation", "Bar aggregators"),
        ("docs/reference/node/books.md", "Order books", "Order book wrappers"),
        ("docs/reference/node/positions.md", "Position tracking", "Position trackers"),
        ("docs/reference/node/profiles.md", "Profiles and stats", "Volume profile, statistics"),
        ("docs/reference/node/data.md", "Data I/O", "Loading and recording data"),
    ]),
    ("Reference: Codon", [
        ("docs/reference/codon/index.md", "Codon reference index", "All Codon modules"),
        ("docs/reference/codon/strategy.md", "Strategy", "Codon Strategy class"),
        ("docs/reference/codon/types.md", "Types", "Core Codon types"),
        ("docs/reference/codon/indicators.md", "Indicators", "Codon indicators"),
        ("docs/reference/codon/runner.md", "Runner and BacktestRunner", "Codon runners"),
        ("docs/reference/codon/backtest.md", "Backtest components", "Codon backtest pieces"),
        ("docs/reference/codon/tools.md", "Tools", "Codon tooling"),
    ]),
    ("Reference: embedded JavaScript", [
        ("docs/reference/quickjs/index.md", "QuickJS reference index", "All embedded-JS APIs"),
        ("docs/reference/quickjs/strategy.md", "Strategy", "JS Strategy"),
        ("docs/reference/quickjs/indicators.md", "Indicators", "JS indicators"),
        ("docs/reference/quickjs/tools.md", "Tools", "JS tools"),
        ("docs/reference/quickjs/backtest.md", "Backtest and runtime", "JS backtest entry points"),
    ]),
    ("Reference: C API", [
        ("docs/reference/api/capi/lrvx_capi.md", "C API surface", "ABI-stable C symbols and lifecycle"),
    ]),
    ("Concepts (explanation)", [
        ("docs/explanation/README.md", "Explanation index", "Conceptual background"),
        ("docs/explanation/architecture.md", "Architecture", "How the engine is structured"),
        ("docs/explanation/integration-flow.md", "Integration flow", "Data flow end-to-end"),
        ("docs/explanation/bar-types.md", "Bar types", "Time, tick, volume, dollar bars"),
        ("docs/explanation/indicators.md", "Indicators concept", "How indicators participate in the graph"),
        ("docs/explanation/disruptor.md", "Disruptor pattern", "Single-producer ringbuffer model"),
        ("docs/explanation/memory-model.md", "Memory model", "Pools, refcounting, lifetime"),
    ]),
]


def page_url(rel_path: str) -> str:
    """Convert `docs/foo/bar.md` to the deployed mkdocs URL."""
    p = rel_path.removeprefix("docs/").removesuffix(".md")
    if p in ("index", "README"):
        return f"{DOCS_BASE_URL}/"
    if p.endswith("/README") or p.endswith("/index"):
        p = p.rsplit("/", 1)[0]
    return f"{DOCS_BASE_URL}/{p}/"


def render_llms_txt() -> str:
    out: list[str] = []
    out.append("# lrvx")
    out.append("")
    out.append(f"> {PROJECT_SUMMARY}")
    out.append("")
    out.append(
        "Documentation is grouped below by Diátaxis category — pick the "
        "section matching the task. The companion `llms-full.txt` contains "
        "the full body of every page listed here, in the same order, for "
        "agents that prefer one large context dump."
    )
    out.append("")
    for section, entries in SECTIONS:
        out.append(f"## {section}")
        out.append("")
        for path, title, desc in entries:
            out.append(f"- [{title}]({page_url(path)}): {desc}")
        out.append("")
    return "\n".join(out).rstrip() + "\n"


def render_llms_full_txt() -> str:
    out: list[str] = []
    out.append("# lrvx — full documentation bundle")
    out.append("")
    out.append(PROJECT_SUMMARY)
    out.append("")
    out.append(
        "This file concatenates the curated documentation set listed in "
        "`llms.txt`. Each page is preceded by a `# Source: <path>` header "
        "and a horizontal rule so an agent can locate the original file."
    )
    out.append("")
    for section, entries in SECTIONS:
        out.append("=" * 78)
        out.append(f"# Section: {section}")
        out.append("=" * 78)
        out.append("")
        for path, _title, _desc in entries:
            file = REPO / path
            if not file.exists():
                raise FileNotFoundError(f"Manifest references missing file: {path}")
            body = file.read_text().rstrip()
            out.append("-" * 78)
            out.append(f"# Source: {path}")
            out.append(f"# URL: {page_url(path)}")
            out.append("-" * 78)
            out.append("")
            out.append(body)
            out.append("")
    return "\n".join(out).rstrip() + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="Exit non-zero if existing files differ from generated content.",
    )
    args = parser.parse_args()

    new_short = render_llms_txt()
    new_full = render_llms_full_txt()

    if args.check:
        ok = True
        for path, new in ((LLMS_TXT_PATH, new_short), (LLMS_FULL_PATH, new_full)):
            current = path.read_text() if path.exists() else ""
            if current != new:
                print(f"::error::{path.relative_to(REPO)} is out of sync", file=sys.stderr)
                ok = False
        if not ok:
            print("Run: python3 scripts/gen_llms_txt.py", file=sys.stderr)
            return 1
        return 0

    LLMS_TXT_PATH.write_text(new_short)
    LLMS_FULL_PATH.write_text(new_full)
    print(f"wrote {LLMS_TXT_PATH.relative_to(REPO)} ({len(new_short):,} bytes)")
    print(f"wrote {LLMS_FULL_PATH.relative_to(REPO)} ({len(new_full):,} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
