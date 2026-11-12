# BacktestRunner

`BacktestRunner` replays historical market data through a strategy, simulates order execution, and collects performance statistics. Supports both batch and interactive modes.

```cpp
class BacktestRunner : public ISignalHandler
{
public:
  using EventCallback = std::function<void(const replay::ReplayEvent&, const BacktestState&)>;
  using PauseCallback = std::function<void(const BacktestState&)>;

  explicit BacktestRunner(const BacktestConfig& config = {});

  // Strategy setup
  void setStrategy(IStrategy* strategy);
  void addMarketDataSubscriber(IMarketDataSubscriber* subscriber);
  void addExecutionListener(IOrderExecutionListener* listener);

  // Custom executor / pre-trade gates
  void setExecutor(IOrderExecutor* executor) noexcept;
  IOrderExecutor* customExecutor() const noexcept;
  void setRiskManager(IRiskManager* rm) noexcept;
  void setOrderValidator(IOrderValidator* ov) noexcept;
  void setKillSwitch(IKillSwitch* ks) noexcept;
  void setPnLTracker(IPnLTracker* tracker) noexcept;

  // Non-interactive mode
  BacktestResult run(replay::IMultiSegmentReader& reader);
  BacktestResult runBars(const std::vector<BarEvent>& bars);
  BacktestResult runTape(const std::filesystem::path& data_dir);
  BacktestResult runTapes(const std::vector<std::filesystem::path>& data_dirs);
  BacktestResult runTapeBars(const std::filesystem::path& data_dir,
                             std::chrono::nanoseconds interval,
                             std::optional<SymbolId> symbol = std::nullopt);

  // Interactive mode
  void start(replay::IMultiSegmentReader& reader);
  void resume();
  void step();
  void stepUntil(BacktestMode mode);
  void pause();
  void stop();

  // Breakpoints
  void addBreakpoint(Breakpoint bp);
  void clearBreakpoints();
  void setBreakOnSignal(bool enable);

  // State inspection
  BacktestState state() const;
  bool isPaused() const;
  bool isFinished() const;

  // Callbacks (interactive mode)
  void setEventCallback(EventCallback cb);
  void setPauseCallback(PauseCallback cb);

  // Results
  BacktestResult result() const;
  BacktestResult extractResult();  // Move results out (clears internal state)

  // ISignalHandler
  void onSignal(const Signal& signal) override;

  // Access internals
  SimulatedExecutor& executor() noexcept;
  IClock& clock() noexcept;
  const BacktestConfig& config() const noexcept;
};
```

## Entry Points

| Method | Input | Notes |
|--------|-------|-------|
| `run(reader)` | An `IMultiSegmentReader` | The general path |
| `runBars(bars)` | A `std::vector<BarEvent>` | Each bar updates the `SimulatedExecutor` — open first, then low, high and close, so resting orders and SL/TP match against the intrabar extremes — and is then dispatched to `Strategy::onBar` and any registered subscriber. An order the callback emits is held for the next bar's open (see below). Bars must be in non-decreasing `endTime` order |
| `runTape(dir)` | One `.lrvx` directory | Opens the tape via `replay::createMultiSegmentReader`. Throws if `dir` is not a `.lrvx` directory or holds no segments |
| `runTapeBars(dir, interval, symbol)` | One `.lrvx` directory, replayed as time bars | The tape's trades are aggregated into wall-clock-aligned bars of `interval` while it streams (`TimeBarPolicy`, the policy `aggregate_time_bars` uses: late trades dropped, the last still-open bar not delivered), and each closed bar goes through the same step as `runBars` -- so the next-open rule below applies. Each `BarEvent` carries `BarType::Time`, `barTypeParam` = interval in ns, buy volume and trade count; bar times are Unix nanoseconds, as `runBars` takes them. One symbol per run: with `symbol` only its trades are read, and a tape with none throws `std::invalid_argument`; without it a second symbol's trade stops the run with `std::invalid_argument`. A non-positive interval throws `std::invalid_argument`. Book records are not used |
| `runTapes(dirs)` | N `.lrvx` directories, merged on read | Symbols are rekeyed into the engine registry via `(metadata.exchange, name)`, so strategies that pre-resolved venue-tagged symbols see the merger's ids. Throws if any input is not `.lrvx`, or if two inputs declare overlapping book streams for the same symbol (`OverlappingBookStreamError`). `runTapes({t})` equals `runTape(t)` modulo the rekey |

## Records tick replay does not deliver

`run`, `runTape` and `runTapes` deliver trades and book records (snapshot, delta, top of book) to
the simulator, the strategy and every subscriber. A tape can also carry option-quote records
(`EventType::OptionQuote`) and DEX pool-state records (`EventType::PoolState`). Nothing in the
runner consumes either: there is no strategy or subscriber callback for them and the
`SimulatedExecutor` does not match against them. They are read, advance the clock and count in
`eventCount`, and are otherwise skipped -- counted per kind in `state().optionQuoteCount` and
`state().poolStateCount`, with one warning logged on the first. The Python `run_tape` / `run_tapes`
stats report the same counts as `skipped_option_quotes` and `skipped_pool_states`. The C API,
Node and Codon `LrvxBacktestStats` carry no such field; read the tape's option quotes directly
(`DataReader.read_option_quotes_from`) or drive pool state through `PoolStateReplay`.

## Bar callbacks fill at the next open

The strategy is shown a bar the simulator has already walked, so open, high, low and close are all
in the market state when its callback runs. An order emitted from there is held and released at the
**next bar's open** for that symbol — the first price that exists after the strategy could have
decided to send it. It is not matched against the bar it was shown, and not against the next bar's
extremes either.

- An order emitted on the last bar never fills: there is no next open. It is still held when the run
  ends, visible as `executor().heldOrderCount()`.
- Deferring does not drop, split or resize the order, and a cancel in the same callback pulls it
  before it is ever submitted.
- An order emitted for a *different* symbol waits for that symbol's next bar, not for the next bar
  of any symbol.
- This applies to the built-in simulator. A custom executor installed with `setExecutor` receives
  signals directly and owns its own matching, so it is on its own here.

Tick and book replay (`run`, `runTape`, `runTapes`) is not affected: there the next event is the
next price, and the strategy callback runs after the event has been matched. `runTapeBars` is bar
replay and follows the rule above.

On bar replay the clock reads each bar's end time. A held order released at the next open with a
positive submit deferral (`BacktestConfig::latency` order delay plus the submit ack latency) is
therefore accepted at a later bar's open, never inside the bar it was released in: see
[Latency on bar replay](../../../how-to/backtest-with-latency.md#latency-on-bar-replay).

## Reruns

Every entry point — `run`, `runBars`, `runTape`, `runTapes`, `runTapeBars` and interactive `start` — clears the
previous run first: the executor's fills, live, held and conditional orders, market state and
ladder, the built-in position tracker, the clock, and the event counters. A second run therefore
reports that run, not the sum of every run so far, and repeats bit for bit.

What is **not** cleared: configuration of any kind (slippage, queue model, latencies, rate limits,
gates, listeners, subscribers, breakpoints) and anything owned outside the runner — a `VenueStack`'s
account, fee schedule, funding state and liquidation engine, and any book pushed by hand into the
executor before the run. Push it again if the run depends on it.

## Pre-Trade Gates

All four hooks are optional; an unset hook is a no-op. The runner holds raw pointers and does not
delete them.

Gates fire on entry-type signals (`Market`, `Limit`, `Stop*`, `TakeProfit*`, `TrailingStop`) and on
the order they produce. `Cancel`, `CancelAll` and `Modify` pass through ungated — they reduce, not
add, exposure. Reduce-only orders also bypass: when caps tighten you do not want to be stuck in a
position.

`setExecutor` replaces the built-in `SimulatedExecutor` for signal routing (submit, cancel, replace,
OCO, cancelAll). The simulator is left intact to match live data into `BacktestResult`; the custom
executor must report fills through its own execution-listener path. Pass `nullptr` to revert. The
caller retains ownership.

## Two Modes

### Non-Interactive Mode

Synchronous execution from start to end:

```cpp
BacktestRunner runner(config);
runner.setStrategy(&strategy);

// Blocks until complete
BacktestResult result = runner.run(*reader);
```

### Interactive Mode

Async execution with pause/step control. See [Interactive Backtest Mode](../../../how-to/interactive-backtest.md) for full documentation.

```cpp
BacktestRunner runner(config);
runner.setStrategy(&strategy);

// Start in background (begins paused)
std::thread t([&]() { runner.start(*reader); });

// Control execution
runner.step();    // One event
runner.resume();  // Run until breakpoint/end
runner.pause();   // Pause execution

t.join();
```

## Strategy Setup

```cpp
void setStrategy(IStrategy* strategy);
void addMarketDataSubscriber(IMarketDataSubscriber* subscriber);
void setSymbolRegistry(const SymbolRegistry* registry);
const SymbolRegistry* symbolRegistry() const noexcept;
```

`setStrategy` connects the strategy to receive market events. Use `addMarketDataSubscriber` to add additional subscribers (e.g., bar aggregators, analytics).

The simulator reads each symbol's tick and rejects an order priced off a known tick the way a venue rejects it (see [SimulatedExecutor](./simulated_executor.md#order-prices-and-the-tick)). By default a C++ `BacktestRunner` takes the ticks from the strategy: an `lrvx::Strategy` keeps a copy of each symbol's registry entry from its constructor (`symbolInfos()`), so the same strategy and data are checked the same way in C++ as in the Python, Node and Codon `BacktestRunner` and `lrvx_backtest_runner_create`, which pass the registry they are constructed with. The copy means the registry does not have to outlive the strategy.

`setSymbolRegistry` overrides that source, whether it is called before or after `setStrategy`; `nullptr` goes back to the strategy's symbols, and `symbolRegistry()` returns the one in use. A strategy that implements `IStrategy` directly, without deriving from `lrvx::Strategy`, carries no symbol entries: its orders are checked only when a registry is passed here. A venue stack attached with `setSimulatedExecutor` reads the same source. `WalkForwardRunner` builds a runner per fold and gets the same default from the strategy each fold's factory returns.

## Data Flow

```mermaid
flowchart TB
    RE[ReplayEvent] --> BR[BacktestRunner]

    BR --> SE1[SimulatedExecutor.onTrade/onBookUpdate]
    BR --> ST[Strategy.onTrade/onBookUpdate]

    ST --> Emit[emitMarketBuy / emitMarketSell]
    Emit --> Signal[BacktestRunner.onSignal]
    Signal --> Submit[SimulatedExecutor.submitOrder]
    Submit --> Hold[held to the next bar open, bar runs only]
    Hold --> Fill[Fill]
    Submit --> Fill
    Fill --> Result[BacktestResult]
```

## Usage

```cpp
// 1. Config
BacktestConfig config;
config.initialCapital = 10000.0;
config.feeRate = 0.0004;

// Optional: realistic fill simulation.
config.defaultSlippage = {SlippageModel::FIXED_BPS, 0, Price{}, 1.0, 0.0};
config.queueModel = QueueModel::TOB;
config.riskFreeRate = 0.0;
config.metricsAnnualizationFactor = 252.0;

BacktestRunner runner(config);

// 2. Strategy
MyStrategy strategy(/*params*/);
runner.setStrategy(&strategy);

// 3. Execution listeners (optional)
runner.addExecutionListener(&positionTracker);

// 4. Data
replay::ReaderFilter filter;
filter.symbols = {1};
auto reader = replay::createMultiSegmentReader("./data", filter);

// 5. Run
BacktestResult result = runner.run(*reader);
auto stats = result.computeStats();

std::cout << "Return: " << stats.returnPct << "%\n";
std::cout << "Sharpe: " << stats.sharpeRatio << "\n";
```

## Notes

- Virtual clock advances based on event timestamps from reader.
- Strategy receives events in the same order as in real-time.
- Signals are converted to orders and submitted to `SimulatedExecutor`.
- The runner calls `executor.applyConfig(config)` at construction, so slippage, per-symbol overrides, and queue simulation are ready before any events are processed.
- Trade events from the replay stream pass their quantities to the executor via `onTrade(symbol, price, qty, isBuy)`, which is required for queue-simulated fills.
- All fills are recorded in `BacktestResult`.
- Each run starts from a clean executor and position tracker; see [Reruns](#reruns).

## See Also

- [Interactive Backtest Mode](../../../how-to/interactive-backtest.md) — Pause, step, breakpoints
- [SimulatedExecutor](./simulated_executor.md) — Order execution simulation
- [BacktestResult](./backtest_result.md) — Performance statistics
- [Slippage](./slippage.md)
- [Queue simulation](./queue_simulation.md)
