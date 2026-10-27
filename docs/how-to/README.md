# How-To Guides

Solve specific problems. Assumes you know the basics.

## Project setup

| Guide | Problem |
|-------|---------|
| [Scaffold a project (`lrvx new`)](lrvx-new.md) | Bootstrap research / live / indicator-library projects from a template |
| [Project layout](project-layout.md) | What a scaffolded project contains and where each piece belongs |
| [Strategy classes](strategy-classes.md) | Structure strategy code idiomatically |
| [Strategy hot-reload](strategy-hot-reload.md) | Swap a strategy at runtime without dropping connections |
| [TypeScript strategy authoring](typescript-strategy-authoring.md) | Write strategies as TS classes with `@strategy` / `@onTrade` decorators |
| [CCXT adapter](ccxt-adapter.md) | Connect to an exchange via the CCXT bridge |

## Backtesting and analysis

| Guide | Problem |
|-------|---------|
| [Backtesting](backtest.md) | Run strategy backtests on historical data |
| [Realistic backtest in one call](realistic-backtest.md) | Venue-typed factory that wires executor, account, fees, funding, liquidation, and rate limits |
| [Backtest recipes](backtest-recipes.md) | Runnable end-to-end recipes for the common backtest assembly patterns |
| [Sweep a signal over a tape](strategy-pump.md) | Monomorphic replay for indicator studies and parameter sweeps |
| [Realistic fills](backtest-realistic-fills.md) | Slippage, queue position, partial fills |
| [Record and replay tapes](tape-record.md) | Capture live market data to `.lrvx`, replay deterministically |
| [Run a paper engine (`lrvx engine sim`)](engine-sim.md) | Replay a tape through a strategy behind a control server an agent can drive |
| [Merge multiple tapes on read](multi-tape.md) | Cross-exchange research over N tapes with symbols rekeyed into one global id space |
| [Aggregate tape events in a single pass](aggregate-tape-events.md) | Run a panel of streaming aggregators over a tape without materialising it |
| [Import Binance public archives](import-binance-archive.md) | Convert `data.binance.vision` aggTrades zips into a `.lrvx` tape |
| [Import Binance book archives](import-binance-book-archive.md) | Convert `bookTicker` / `bookDepth` zips into delta-encoded book events on the same tape |
| [Import multi-exchange archives](import-multi-exchange-archives.md) | Binance / Bybit / OKX / Bitget / Deribit public archive importers |
| [Iterate the order book from a tape](iterate-orderbook.md) | Reconstruct ladder state per bucket or at a chosen instant for offline book analysis |
| [Rolling top-K thresholds](rolling-thresholds.md) | Vectorized sliding K-th-largest for extreme-event filters at any timeframe |
| [Cross-sectional panel builder](cross-sectional-panel.md) | Aligned (T × S) close / OHLC / returns panel over N lrvx tapes |
| [Delta book compression](delta-book-compression.md) | Shrink L2 tapes 10-30x by emitting only the changed levels |
| [Record a strategy run as `.lrvxrun`](lrvxrun.md) | Capture signals, orders, and fills as a per-run trace alongside the tape |
| [Capture a strategy run automatically with `.lrvxrun`](lrvxrun-auto-capture.md) | `TraceSignalHandler` + `TraceExecutionListener` adapters; no per-strategy instrumentation |
| [Inspect a tape and run in the replay viewer](replay-viewer.md) | Single-page UI for scrubbing through a captured tape and strategy trace |
| [HTML report](backtest-html-report.md) | Render an equity curve + trade table to one HTML file |
| [Interactive backtest](interactive-backtest.md) | Inspect state mid-run from a notebook |
| [Grid search](grid-search.md) | Sweep parameters over a backtest |
| [Sweep parameters over bars](sweep-over-bars.md) | Load bars once, run one bar strategy per combination, in processes or on threads |
| [Walk-forward](walk-forward.md) | Out-of-sample validation with rolling / anchored folds |
| [Heatmap](heatmap.md) | SVG heatmap from a 2D parameter sweep |
| [White's reality check](whites-reality-check.md) | Multiple-comparison-aware significance test |
| [Overfitting checks for a parameter sweep](robustness-statistics.md) | Probability of backtest overfitting, deflated Sharpe ratio, neighbourhood of a grid point |
| [Log to MLflow](mlflow.md) | Send a backtest run + artifacts into an MLflow tracking server |
| [Backtest an LP position](backtest-an-lp-position.md) | Run a concentrated-liquidity position through a pool tape |
| [Choose a matching model](matching-modes.md) | FIFO, pro-rata, `pro_rata_with_fifo`, TOP-PRO-LMM, and priority-weighted matching |
| [Read queue position from a strategy](queue-position.md) | Queue-ahead / queue-total events off the backtest queue tracker |
| [Estimate queue position from live events](live-queue-position.md) | `LiveQueuePositionEstimator` over live trade + book feeds |
| [Calibrate the live queue estimator](calibrate-live-queue.md) | Fit half-life and shrink factor against observed fills |
| [Attribute hidden / iceberg flow](hidden-order-attribution.md) | Stop counting hidden fills as cancellations in the queue estimate |
| [Detect an empty price level](level-empty-detection.md) | Fire when a resting order is alone at its level |
| [Track a resting order's market position](resting-order-market-position.md) | Categorical best / behind-best / mid-spread / crossed transitions |
| [Read maker / taker on a fill](maker-taker-classification.md) | Fill-role classification and why it drives the fee ladder |
| [Per-stage order timestamps](order-state-timestamps.md) | Submitted / accepted / first-fill / cancelled / rejected snapshot per order |
| [Record and analyse order journeys](order-journey-forensics.md) | `OrderJourneyTracer` for post-trade latency and cancel-race forensics |
| [Apply a named latency profile](latency-profiles.md) | Canned per-venue ack-latency defaults |
| [Ack-latency distributions](latency-distributions.md) | Lognormal / empirical draws with burst correlation instead of a scalar |
| [Model submit-side ack latency](submit-latency-modeling.md) | Defer `ACCEPTED` so the submit race is reproducible |
| [Model cancellation ack latency](cancel-latency-modeling.md) | Reproduce the lost-to-fill cancel race |
| [Model order replace acknowledgement](order-replace-flow.md) | The three-event async replace sequence and the late-replace race |
| [Simulate venue downtime](venue-downtime.md) | Maintenance windows, random disconnects, and buffered request flush |
| [Model venue rate limits](rate-limits.md) | Per-endpoint budgets and rejection behaviour |
| [Model volume-tiered fees](tiered-fees.md) | 30-day rolling VIP ladder for maker / taker rates |
| [Model perpetual funding](perpetual-funding.md) | Funding schedules, per-symbol tapes, and settlement bookkeeping |
| [Model liquidation and ADL](liquidation-and-adl.md) | Cascades, insurance fund, ADL ranking, mark impact |
| [Cross-margin accounts](cross-margin.md) | Shared-equity account with per-symbol marks and maintenance margin |
| [Self-trade prevention](self-match-prevention.md) | STP modes and account groups in the simulator |
| [Price options and back out IV](options-pricing.md) | Black-Scholes, implied vol, SVI surfaces, and the vol cone |
| [Compute option greeks](options-greeks.md) | First- and second-order greeks |

## Live trading

| Guide | Problem |
|-------|---------|
| [Advanced orders](advanced-orders.md) | Stop-loss, take-profit, brackets |
| [Submit a native bracket order](bracket-orders.md) | One entry plus linked TP / stop children with OCO semantics |
| [Submit a native iceberg order](iceberg-orders.md) | Visible slice, hidden remainder, refresh latency and size randomisation |
| [Extended TIF and reduce-only](order-tif-flags.md) | `ioc` / `fok` / `gtd` / `post_only` and the reduce-only flag |
| [Replace a leg of an active OrderGroup](order-group-replace.md) | Re-price one basket leg without tearing down the group |
| [Multi-exchange trading](multi-exchange-trading.md) | Aggregate books and route across venues |
| [Price a DEX swap](price-a-dex-swap.md) | Constant-product and concentrated-liquidity quote maths |
| [Route, arb, and ingest DEX pools](route-and-arb-dex-pools.md) | Multi-hop routing, arb detection, and pool-tape replay |
| [Price and backtest DEX pools on Node](dex-on-node.md) | The same AMM surface from the Node binding |
| [Inspect a running engine over MCP](mcp-runtime-inspection.md) | Read positions / PnL / kill-switch from a running engine via an AI client |
| [Paper trading](paper-trading.md) | Drive a strategy off live data but route orders to a simulator |
| [Reproducibility bundles](reproducibility-bundles.md) | Pack strategy + tape + expected output into a single tarball; replay and compare the fills field by field |
| [Control a running engine over MCP](mcp-control-plane.md) | Send place / cancel / flatten / kill-switch from an AI client over a token-scoped HTTP control plane |
| [Live engine analytics over MCP](mcp-live-analytics.md) | Read-only introspection of a running engine: list strategies, walk causal chains, replay-with-overrides |
| [Lookahead-bias detector](lookahead-detector.md) | Static-analysis lint that flags `.shift(-N)`, forward-index arithmetic, future-named attributes |
| [Portfolio-level risk aggregator](portfolio-risk.md) | Combine PnL and exposure across N strategies; portfolio-level kill switch on drawdown / loss / gross / concentration |
| [Backtest with latency](backtest-with-latency.md) | Add feed / order / fill latency samples to a backtest for HFT-grade fill realism |
| [Execution algorithms](execution-algorithms.md) | TWAP / VWAP / Iceberg / POV slicers that work on top of any executor |
| [RL environment](rl-environment.md) | Gymnasium-compatible env over a lrvx tape for training RL agents |

## Indicators and aggregation

| Guide | Problem |
|-------|---------|
| [Add an indicator](add-an-indicator.md) | Wire a custom indicator into a strategy |
| [Indicator graph](indicator-graph.md) | Compose indicators into a DAG |
| [Multi-symbol indicators](multi-symbol-indicators.md) | One indicator across many symbols |
| [Cross-symbol indicators](cross-symbol-indicators.md) | Indicators over two synchronised symbol streams — correlation, hedge ratios, benchmark regime filters |
| [Read multi-timeframe context from a strategy](multi-tf-context.md) | `last_closed_bar(symbol, tf)` + `last_n_closed_bars(...)` ring helpers |
| [Compose multi-symbol multi-TF entry conditions declaratively](composite-conditions.md) | `when(self, btc, H4).ema(50) > when(self, btc, H4).ema(200)` composable DSL |
| [Submit a multi-leg order group](multi-leg-order-group.md) | Bundle pair-trade / basket legs under one `parent_signal_id` with status + cancel |
| [Wait for multiple feeds with a known staleness budget](multi-feed-clock.md) | `MultiFeedClock` with WaitForAll / FireOnAny / LeaderFollower policies |
| [Bar aggregation](bar-aggregation.md) | Pre-aggregate bars for fast backtesting |
| [Custom bar policy](custom-bar-policy.md) | Hand-roll a new bar aggregation rule |
| [Volume profile](use-volume-profile.md) | Build a volume profile from trades |

## Performance and project

| Guide | Problem |
|-------|---------|
| [Optimize performance](optimize-performance.md) | Tune for minimum latency |
| [CPU affinity](cpu-affinity.md) | Pin threads to isolated cores |
| [Configuration](configuration.md) | Runtime configuration options |
| [CI configuration](ci.md) | Understand the CI pipeline |
| [Custom connector](custom-connector.md) | Add a new exchange to the connector tree |
| [Contributing](contributing.md) | Contribute to lrvx development |
| [Migrate from FLOX](migrate-from-flox.md) | Move a FLOX project, its tapes, environment and build flags to the lrvx names |

## Prerequisites

These guides assume you've worked through the [tutorials](../tutorials/README.md) and understand the core lrvx concepts.
