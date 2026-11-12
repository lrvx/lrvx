"""lrvx_overview — single-call capability summary for AI agents.

Returns a markdown narrative covering every MCP tool category and
the canonical workflows. Lives bundled (no live data); update with
each major release.

Why this exists: each tool individually carries a detailed
description in its JSONSchema registration, but there is no
top-level "what can you do?" affordance. An agent or human
connecting to the server can call this tool first and get the
overall map instead of re-deriving it from per-tool descriptions.
"""
from __future__ import annotations


_OVERVIEW = """# lrvx toolkit overview

Call this tool first when you don't know which tool to use or
what lrvx is capable of. lrvx is a polyglot trading framework
with a backtest core, paper and live trading paths, a replay
visualizer, and an AI-native MCP control plane. This page
covers the full surface — not just MCP tools, but the CLI and
companion modules an agent should reach for when the user wants
to do anything beyond a quick lookup.

## What lrvx is (and isn't)

lrvx is an event-driven trading framework: C++ core, first-class
bindings for Python / Node.js / Codon / QuickJS, and a stable C
ABI. Targets crypto perpetuals and spot. Backtest physics model
venue-realistic fees, funding, liquidation cascade, ADL, rate
limits, queue position, and venue downtime — surfaces that retail
frameworks (vectorbt / backtrader / freqtrade) don't cover.

Out of scope: equity / options markets, vector-matrix retail
backtest patterns (use vectorbt for those).

## CLI (`lrvx` after `pip install lrvx`)

- `lrvx new <project>` — scaffold a project. Templates: `research`
  (backtest-first), `live` (live trading), `indicator-library`
  (custom indicators).
- `lrvx templates` — list templates.
- `lrvx tape record <exchange> <symbol> --output <out>` — capture a live ccxt feed into a `.lrvx`.
- `lrvx tape replay <tape>` — drive a strategy off a recorded tape.
- `lrvx tape inspect <tape>` — header + summary.
- `lrvx tape diff <a> <b>` — byte-level diff between two tapes.
- `lrvx tape view <tape> [run]` — open the replay viewer SPA in
  a browser tab.
- `lrvx report <stats.json> [-o report.html]` — render backtest
  stats to an HTML report with equity curve + trades.
- `lrvx bundle pack / replay / validate` — reproducibility bundles
  (`.lrvx` + `.lrvxrun` + strategy code + config + dep hashes).
- `lrvx lint lookahead <strategy.py>` — static lookahead-bias
  detection.
- `lrvx archive {binance,bybit,okx,bitget} <symbol>` — pull
  public venue archives into lrvx tapes.

## Discovery — MCP tools (offline, no engine needed)

- `docs_search(query, k?)` — full-text search over docs.
- `lookup_symbol(name, language?)` — cross-binding symbol resolver
  (C ABI / Python / Node / Codon / QuickJS).
- `list_bindings(language, filter?, limit?)` — browse a binding.
- `get_example(topic, language?)` — fetch a recipe to copy from
  `docs/examples/`.
- `list_capi_functions(filter?, limit?)` — C ABI catalog.
- `list_indicators(filter?)` — indicator catalog.
- `lookup_error_code(code)` — resolve a LrvxError to its docs page.

## Building a backtest

1. `scaffold_strategy(language, kind, name)` — starter Strategy class.
2. `docs_search("realistic backtest")` — venue-realistic harness via
   `lrvx.VenueStack`.
3. `get_example(topic="backtest")` — copy a runnable recipe.
4. `validate_strategy(code)` / `validate_strategy_no_lookahead(code)` —
   sanity gate before running.
5. `run_backtest(strategy_code, dataset_path, ...)` — run the strategy
   locally over a CSV dataset (subprocess, no control plane).

### Realistic backtest stack

`lrvx.VenueStack.binance_um_futures(account_id, equity)` (also
`bybit_linear`, `okx_swap`, `deribit`) wires every venue subsystem
in one call:

- Executor + cross-margin Account + LiquidationEngine
- FeeSchedule (30d VIP ladder + per-symbol breakdown)
- FundingSchedule (8h interval or per-symbol tape)
- RateLimitPolicy (per-endpoint families: trading / market-data / account)
- VenueAvailability (partial outage / slow degradation / stale book)
- Iceberg refresh + queue model + ack-latency profile

For non-canonical venues:
`lrvx.assemble_custom_venue(account=..., fees=..., funding=...,
liquidation=..., rate_limits=...)` returns the same accessor surface.

## Paper trading

`lrvx.paper.PaperBroker` runs strategy against a LIVE market
data feed but routes orders to a SimulatedExecutor. Same fill
model as backtest. The bridge between backtest and live without
real capital.

Install: `pip install "lrvx[ccxt]"` for the ccxt live feed.

## Live trading via CCXT

`lrvx.ccxt.CcxtBroker` wraps `ccxt.pro`. Strategy code is
identical to backtest — `self.market_buy(...)`, `self.limit_sell(...)`
in `on_trade` / `on_bar` translate to `create_*_order` on the
exchange. Market data flows in (trades + L2 books + order
updates), orders flow out.

Workflow: backtest → paper (PaperBroker) → live (CcxtBroker) with
the same strategy class. `lrvx new --template=live` scaffolds a
ready-to-run live project.

## Visualization — replay viewer

`tools/replay-viewer/` is a static SPA. `lrvx tape view tape.lrvx
[run.lrvxrun]` stages the artifacts, launches a local server, opens
the browser. Six views on a shared timeline:

- Price chart with trade dots and signal bands
- Order book depth heatmap at the cursor
- Recent trades tail
- Strategy signal cards from `.lrvxrun`
- Orders timeline + rolling fill totals
- Equity curve

Drag, scrub, share via URL.

## Live engine inspection (running engine required)

- `get_positions / get_pnl / get_open_orders / get_strategy_state` — read.
- `explain_decision / explain_event` — narrative on what happened.
- `list_strategies` — what's loaded.

## Live engine control via MCP (`lrvx-mcp` Phase 2)

The MCP server can send mutating commands to a running engine under
explicit safety semantics. Three-tier authorization:

- `read` token — inspection only.
- `paper` token — orders only into accounts named `paper-*`.
- `live` token — anything; per-call out-of-band one-shot approval
  required for `place_order` on `live` scope.

Defaults are dry-run. Token-bucket rate limits per token + op
family (place_order = 1/sec sustained). Every mutating call logged
to an audit trail with bearer-token prefix + scope + redacted args
+ effects.

Mutating tools:
- `place_order / cancel_order / cancel_all / flatten_positions`
- `set_kill_switch / get_kill_switch`

## Reproducibility

- `lrvx bundle pack` — deterministic archive of (tape + run + code
  + config + dep hashes). `lrvx bundle replay` reruns it and prints
  actual vs expected. `lrvx bundle validate` compares the rerun with
  the expected output field by field (trade count, fills, filled
  quantity, strategy and tape hashes, engine version).
- The venue-stack is bit-deterministic across runs (verified
  via `test_venue_stack_reproducibility.cpp`): identical inputs → identical
  engine state.

## Calibration + analysis

- `compute_indicator / get_indicator_values / suggest_indicator` —
  indicator data over a window.
- `price_amm_swap(venue, pool, amount_in)` — quote a DEX swap exact
  to the wei on a constant-product / Raydium CP / Uniswap v3 pool.
- `route_amm_swap(venues, amount, into)` — best execution across pools;
  the winning venue, the fill, and a per-venue comparison.
- `amm_price_impact(pool, sizes)` — the depth / slippage table for a
  pool, to size a trade before quoting.
- `replay_pool_tape(pool, swaps | evm_logs)` — replay a recorded
  history into a price / LP-value / impermanent-loss / drift series.
- `whatif(...)` — counterfactual replay with overridden inputs.
- `validate_strategy_no_lookahead(code)` — bias detection.
- `replay_window(from_ts_ns, to_ts_ns)` — re-run a slice for debugging.
- `record_data(...)` — capture market data into `.lrvx`.

## Recent additions (venue-stack round 4 + 5)

- VenueStack single-call factory + `assemble_custom_venue` helper
- Cross-margin Account shared across venue-stack subsystems
- `LiquidationEngine.on_marks(...)` atomic multi-symbol update + walk
- Stale-mark guard (`Account.has_stale_marks`)
- Cross-account ADL routing across attached accounts
- Bit-deterministic reproducibility verified end-to-end
- Real-event calibration harness with analytical baselines
- 4 backtest recipes under `docs/examples/python_*`
- 10+ how-to pages on cross-margin, liquidation-and-adl,
  perpetual-funding, rate-limits, venue-downtime, matching-modes,
  self-match-prevention, order-tif-flags, iceberg-orders,
  calibrate-live-queue, realistic-backtest

## Canonical workflows

### "make me a realistic backtest"

1. `lookup_symbol("VenueStack")` — confirm the factory.
2. `get_example("realistic backtest")` — get the recipe.
3. Adapt account_id / equity / venue / strategy signal.
4. `validate_strategy(code)` to catch issues.
5. `run_backtest(...)` to execute.

### "promote backtest to paper / live"

1. Same strategy class.
2. Paper: `from lrvx.paper import PaperBroker; broker = PaperBroker(...)`.
3. Live: `from lrvx.ccxt import CcxtBroker; broker = CcxtBroker(...)`
   with venue credentials.
4. MCP `place_order` on `paper` scope works without OOB; `live`
   scope requires per-call approval.
5. `lrvx new --template=live` scaffolds a complete live project.

### "inspect what just happened"

1. `lrvx tape view tape.lrvx run.lrvxrun` — visualize end-to-end.
2. Or: `get_strategy_state` + `get_positions` + `explain_decision`
   via MCP for narrative on a single tick.
3. Or: `lrvx report stats.json -o report.html` — static HTML
   summary for sharing.

### "find the right indicator"

1. `list_indicators(filter)` to browse.
2. `suggest_indicator(description)` if user describes intent.
3. `lookup_symbol(name)` to confirm the binding-local spelling.

## What lrvx deliberately does NOT do

- Pick the user's binding — always ask "Python or TypeScript".
- Modify strategy code — use scaffold + validate, not autoedit.
- Reach external services in MCP — every tool reads bundled data
  or talks to a control-plane endpoint the user already runs.
"""


def lrvx_overview() -> str:
    return _OVERVIEW
