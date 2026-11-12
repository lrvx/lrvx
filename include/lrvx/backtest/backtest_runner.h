/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/aggregator/bar.h"
#include "lrvx/aggregator/events/bar_event.h"
#include "lrvx/backtest/backtest_result.h"
#include "lrvx/backtest/simulated_clock.h"
#include "lrvx/backtest/simulated_executor.h"
#include "lrvx/execution/abstract_execution_listener.h"
#include "lrvx/execution/abstract_executor.h"
#include "lrvx/killswitch/abstract_killswitch.h"
#include "lrvx/metrics/abstract_pnl_tracker.h"
#include "lrvx/position/multi_mode_position_tracker.h"
#include "lrvx/replay/abstract_event_reader.h"
#include "lrvx/risk/abstract_risk_manager.h"
#include "lrvx/strategy/abstract_signal_handler.h"
#include "lrvx/strategy/strategy.h"
#include "lrvx/validation/abstract_order_validator.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace lrvx
{

/// Execution mode for interactive backtest
enum class BacktestMode
{
  Run,       ///< Run continuously until end or breakpoint
  Step,      ///< Execute one event then pause
  StepTrade  ///< Execute until next trade
};

/// Breakpoint condition
struct Breakpoint
{
  enum class Type
  {
    Time,        ///< Break at specific timestamp
    EventCount,  ///< Break after N events
    TradeCount,  ///< Break after N trades
    Signal,      ///< Break when strategy emits signal
    Custom       ///< Custom predicate
  };

  Type type{Type::EventCount};
  UnixNanos timestampNs{0};
  uint64_t count{0};
  std::function<bool(const replay::ReplayEvent&)> predicate;

  static Breakpoint atTime(UnixNanos ts) { return {.type = Type::Time, .timestampNs = ts}; }
  static Breakpoint afterEvents(uint64_t n) { return {.type = Type::EventCount, .count = n}; }
  static Breakpoint afterTrades(uint64_t n) { return {.type = Type::TradeCount, .count = n}; }
  static Breakpoint onSignal() { return {.type = Type::Signal}; }
  static Breakpoint when(std::function<bool(const replay::ReplayEvent&)> pred)
  {
    return {.type = Type::Custom, .predicate = std::move(pred)};
  }
};

/// Current state snapshot for inspection
struct BacktestState
{
  UnixNanos currentTimeNs{0};
  uint64_t eventCount{0};
  uint64_t tradeCount{0};
  uint64_t bookUpdateCount{0};
  uint64_t signalCount{0};
  // Option-quote and pool-state records read off the tape. No strategy
  // callback, market-data subscriber or the simulated executor consumes
  // either kind, so the runner delivers them to nobody: these counts are how
  // a run says how much of the tape it did not act on. Both are part of
  // eventCount.
  uint64_t optionQuoteCount{0};
  uint64_t poolStateCount{0};
  bool isRunning{false};
  bool isPaused{false};
  bool isFinished{false};

  std::optional<replay::EventType> lastEventType;
};

/// Backtest runner with optional interactive mode (pause/step/breakpoints)
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

  /// Replace the built-in SimulatedExecutor with a binding-supplied
  /// IOrderExecutor. When set, all signals (submit / cancel / replace /
  /// OCO / cancelAll) are routed to the custom executor instead of the
  /// built-in simulator. The simulator is left intact for matching live
  /// data into BacktestResult; the custom executor is responsible for
  /// reporting fills via its own ExecutionListener path. Pass nullptr
  /// to revert to the simulated executor.
  /// Caller retains ownership; the runner does not delete the executor.
  void setExecutor(IOrderExecutor* executor) noexcept { _customExecutor = executor; }
  IOrderExecutor* customExecutor() const noexcept { return _customExecutor; }

  /// Drive the run against an externally owned SimulatedExecutor —
  /// typically VenueStack::executor().
  ///
  /// This differs from setExecutor() in both directions: the runner
  /// FEEDS this executor market data (bars, trades, book updates) and
  /// HARVESTS its fills into BacktestResult, so a venue stack does not
  /// have to be driven by hand. Order events are routed through the same
  /// position tracker, PnL tracker and listener chain as the built-in
  /// executor.
  ///
  /// `clock` must be the clock the executor was constructed with
  /// (VenueStack::clock()). The runner advances it in lockstep with its
  /// own, otherwise ack latency and queue timing would never progress.
  ///
  /// What this DOES bring in from the stack:
  ///   - The execution mechanics its executor was wired with — queue model
  ///     and depth, iceberg refresh latency, venue availability (downtime)
  ///     and the rate-limit policy. BacktestConfig is not applied to the
  ///     executor, so those settings are not overwritten.
  ///   - Fees. result() and extractResult() hand the executor's
  ///     FeeSchedule to BacktestResult, which bills each fill at the tier
  ///     the account's 30-day notional resolves to, maker or taker by
  ///     Fill::isMaker. A ladder with no tiers falls back to BacktestConfig's
  ///     flat rates, and BacktestConfig::usePercentageFee = false replaces
  ///     both with fixedFeePerTrade.
  ///
  /// What it does NOT bring in: funding and liquidation. Both are driven
  /// by explicit calls (FundingSchedule settlement,
  /// LiquidationEngine::onMarks), not by the replay loop. Read the result
  /// as "this venue's fill mechanics and fees", not "this venue's full
  /// economics".
  ///
  /// Caller retains ownership. Pass (nullptr, nullptr) to revert to the
  /// built-in executor.
  void setSimulatedExecutor(SimulatedExecutor* executor, SimulatedClock* clock);
  SimulatedExecutor* simulatedExecutorOverride() const noexcept { return _venueExecutor; }

  /// Where the simulated executor reads each symbol's tick; it refuses
  /// orders priced off a known tick (SimulatedExecutor::setSymbolRegistry).
  /// A venue stack attached with setSimulatedExecutor gets the same source.
  ///
  /// Without this call the runner uses the symbols the strategy was built
  /// with: an lrvx::Strategy keeps a copy of each symbol's registry entry, so
  /// its declared ticks are checked by default, as in every binding. A
  /// registry passed here takes precedence over the strategy's; nullptr goes
  /// back to the strategy's. A strategy that is an IStrategy but not an
  /// lrvx::Strategy carries no symbol entries, and its orders are checked
  /// only against a registry passed here. Caller retains ownership.
  void setSymbolRegistry(const SymbolRegistry* registry);
  /// The registry the executor reads ticks from: the one passed to
  /// setSymbolRegistry, else the strategy's symbols, else nullptr.
  const SymbolRegistry* symbolRegistry() const noexcept
  {
    return _registry != nullptr ? _registry : _strategySymbols.get();
  }

  /// Pre-trade gate parity with the live `Runner`. All four hooks are
  /// optional; an unset hook is a no-op (let the order through).
  ///
  /// Gates fire on entry-type signals (Market / Limit / Stop* / TP* /
  /// TrailingStop) and on the order they produce. Cancel / CancelAll
  /// / Modify pass through without gating — they reduce, not add,
  /// exposure. Reduce-only orders also bypass: when caps tighten you
  /// do not want to be stuck in a position. This matches how the live
  /// runner is conventionally wired and is documented as a gotcha
  /// surfaced through `lookup_symbol`.
  ///
  /// Caller retains ownership of every hook; the runner holds raw
  /// pointers and does not delete them.
  void setRiskManager(IRiskManager* rm) noexcept { _riskManager = rm; }
  void setOrderValidator(IOrderValidator* ov) noexcept { _orderValidator = ov; }
  void setKillSwitch(IKillSwitch* ks) noexcept { _killSwitch = ks; }
  void setPnLTracker(IPnLTracker* tracker) noexcept { _pnlTracker = tracker; }

  // ========== Non-interactive mode ==========

  /// Run backtest synchronously from start to end
  BacktestResult run(replay::IMultiSegmentReader& reader);

  /// Every run entry point starts from a clean state: the executor's fills,
  /// live orders and market state, the built-in position tracker, the clock
  /// and the event counters are cleared before the first event. A second run
  /// therefore reports that run. What is NOT cleared is configuration --
  /// slippage, queue model, latencies, rate limits, gates, listeners -- and
  /// anything owned outside the runner: a VenueStack's account, fee schedule
  /// and funding state, and any book the caller pushed by hand before the run.
  ///
  /// Replay a sequence of pre-built BarEvents through the strategy.
  /// Each bar updates the SimulatedExecutor (so resting orders / SL/TP get
  /// matched against bar.high / bar.low / bar.close) and is dispatched to
  /// Strategy::onBar and any registered IMarketDataSubscriber.
  /// Bars must be in non-decreasing endTime order.
  BacktestResult runBars(const std::vector<BarEvent>& bars);

  /// Run the backtest off a `.lrvx` tape directory. Opens the tape
  /// via `replay::createMultiSegmentReader` and dispatches every event
  /// through `processEvent`. Use this to drive the strategy off a
  /// recorded session (the canonical path: `lrvx tape record` → write
  /// `.lrvx` → `bt.run_tape(path)`). Throws if `data_dir` is not a
  /// `.lrvx` directory or contains no segments.
  BacktestResult runTape(const std::filesystem::path& data_dir);

  /// Run the backtest off N `.lrvx` tapes merged on read. Symbols
  /// are rekeyed into the engine's symbol registry via
  /// `(metadata.exchange, name)` — strategies that pre-resolved
  /// venue-tagged symbols see the same ids the merger uses. Throws
  /// if any input is not a `.lrvx` directory, or if two inputs
  /// declare overlapping book streams for the same symbol
  /// (`OverlappingBookStreamError`). `runTapes({t})` is equivalent
  /// to `runTape(t)` modulo the rekey.
  BacktestResult runTapes(const std::vector<std::filesystem::path>& data_dirs);

  /// Replay a `.lrvx` tape as time bars: the tape's trades are aggregated
  /// into wall-clock-aligned bars of `interval` while the tape streams, and
  /// every closed bar goes through the same per-bar step as runBars(). The
  /// executor is fed bars, not ticks, so the fill rules are runBars' rules:
  /// an order sent from onBar fills at that symbol's next bar open, and
  /// resting and conditional orders walk open, low, high, close.
  ///
  /// The bars are the ones TimeBarPolicy produces over the same trades
  /// (the batch `aggregate_time_bars` helpers, BarAggregator): a trade later
  /// than its bar's bucket is dropped, and the last bar, still open when the
  /// tape ends, is not delivered. Each BarEvent carries BarType::Time,
  /// barTypeParam = interval in ns, and the bar's buy volume and trade count.
  /// Bar times are Unix nanoseconds, as runBars() takes them. Book records
  /// in the tape are not used.
  ///
  /// One symbol per run. With `symbol` set, only that symbol's trades are
  /// read; a tape with no trade for it throws std::invalid_argument after
  /// the (empty) run. Without it the tape must hold a single symbol: a
  /// trade of a second symbol stops the run and throws std::invalid_argument.
  /// Throws std::invalid_argument for a non-positive interval and
  /// std::runtime_error when `data_dir` is not a readable `.lrvx` directory.
  BacktestResult runTapeBars(const std::filesystem::path& data_dir,
                             std::chrono::nanoseconds interval,
                             std::optional<SymbolId> symbol = std::nullopt);

  /// The strategy decides on bars, the executor matches on the tape. The
  /// tape's records drive the simulated executor exactly as run() / runTape()
  /// drive it: every trade and book record, in time order, with the latency,
  /// queue and slippage settings of tick replay. Time bars of `interval` are
  /// built from the same trades as runTapeBars() builds them (same bars, bit
  /// for bit; the bar still open at the end is not delivered) and reach
  /// Strategy::onBar and the market-data subscribers. Strategy::onTrade and
  /// onBookUpdate are not called.
  ///
  /// Bar n is delivered when the first trade with a timestamp at or after its
  /// end arrives, before that trade reaches the executor. The callback runs on
  /// a clock set to the bar's end (or later, when book records past the end
  /// came first). An order sent from it is submitted then, with its sampled
  /// latency, and is never matched inside the call: with no latency its first
  /// possible match is that trade, the first one stamped at or after the end
  /// of bar n; with latency L, the first market record at or after
  /// submit time + L. Orders, stops and take-profits already working are
  /// triggered and filled by the trades inside the bars, by tick-replay rules
  /// (a conditional never fills better than its trigger).
  ///
  /// One symbol per run, as runTapeBars(): with `symbol` set only its records
  /// are read; without it a trade or book record of a second symbol stops the
  /// run and throws std::invalid_argument. Option-quote and pool-state records
  /// are counted, as in run(). Throws std::invalid_argument for a non-positive
  /// interval or a `symbol` with no trade on the tape, and std::runtime_error
  /// when `data_dir` is not a readable `.lrvx` directory.
  BacktestResult runTapeBarsOnTicks(const std::filesystem::path& data_dir,
                                    std::chrono::nanoseconds interval,
                                    std::optional<SymbolId> symbol = std::nullopt);

  // ========== Interactive mode ==========

  /// Start backtest in interactive mode (starts paused, must call from separate thread)
  void start(replay::IMultiSegmentReader& reader);

  /// Run until end or breakpoint
  void resume();

  /// Execute one event then pause
  void step();

  /// Execute until condition (trade, etc.)
  void stepUntil(BacktestMode mode);

  /// Pause execution
  void pause();

  /// Stop execution completely
  void stop();

  // Breakpoints
  void addBreakpoint(Breakpoint bp);
  void clearBreakpoints();
  void setBreakOnSignal(bool enable);

  // State inspection
  [[nodiscard]] BacktestState state() const;
  bool isPaused() const { return _paused.load(std::memory_order_acquire); }
  bool isFinished() const { return _finished.load(std::memory_order_acquire); }

  // Callbacks (interactive mode only)
  void setEventCallback(EventCallback cb) { _eventCallback = std::move(cb); }
  void setPauseCallback(PauseCallback cb) { _pauseCallback = std::move(cb); }

  // Results
  [[nodiscard]] BacktestResult result() const;
  [[nodiscard]] BacktestResult extractResult();

  // ISignalHandler
  void onSignal(const Signal& signal) override;

  // Access internals
  SimulatedExecutor& executor() noexcept { return _executor; }
  IClock& clock() noexcept { return _clock; }
  const BacktestConfig& config() const noexcept { return _config; }

 private:
  // Feeds one record to the executor and, with dispatchMarketData, hands it
  // to the strategy and the market-data subscribers.
  void processEvent(const replay::ReplayEvent& event, bool dispatchMarketData = true);
  // Clears everything a previous run left behind so run() / runBars() /
  // runTape() report their own run and not the sum of every run so far.
  void resetRunState();
  // The bar replay loop, split so runBars() and runTapeBars() share every
  // step: beginBarRun() resets and starts, stepBar() is one bar, and
  // endBarRun() stops and returns the result.
  void beginBarRun();
  void stepBar(const BarEvent& ev);
  BacktestResult endBarRun();
  bool checkBreakpoints(const replay::ReplayEvent& event);
  void waitForResume();
  void notifyPaused();
  Order signalToOrder(const Signal& sig);
  bool passesPreTradeGate(const Order& order);
  // Routes an executor's order events into the position tracker, PnL
  // tracker and listener chain. Applied to the built-in executor in the
  // constructor and to a venue executor in setSimulatedExecutor().
  void installOrderEventCallback(SimulatedExecutor& exec);
  // Points the built-in and venue executors at symbolRegistry().
  void attachSymbolRegistry();
  // The executor that receives market data and owns the fills for this
  // run: the venue override when set, otherwise the built-in one.
  SimulatedExecutor& sim() noexcept
  {
    return _venueExecutor != nullptr ? *_venueExecutor : _executor;
  }
  const SimulatedExecutor& sim() const noexcept
  {
    return _venueExecutor != nullptr ? *_venueExecutor : _executor;
  }
  // Keeps a venue executor's clock in lockstep with the runner's.
  void advanceClocks(UnixNanos ns);
  // Hands the venue's fee ladder (when there is one) to a result before it
  // replays the fills, so the fills are priced at the venue's tier.
  void attachFeeSchedule(BacktestResult& res) const;

  BacktestConfig _config;
  SimulatedClock _clock;
  SimulatedExecutor _executor;
  // Optional binding-supplied executor. When non-null, signals are routed
  // here instead of to the built-in SimulatedExecutor.
  IOrderExecutor* _customExecutor{nullptr};
  // Optional externally owned SimulatedExecutor (VenueStack::executor()).
  // When non-null it receives market data and owns the run's fills.
  SimulatedExecutor* _venueExecutor{nullptr};
  SimulatedClock* _venueClock{nullptr};
  const SymbolRegistry* _registry{nullptr};
  // The strategy's own symbol entries, rebuilt by setStrategy; the tick
  // source when no registry was passed.
  std::unique_ptr<SymbolRegistry> _strategySymbols;
  IStrategy* _strategy{nullptr};
  // Pre-trade gate stack — parity with live Runner. Caller-owned;
  // runner does not delete.
  IRiskManager* _riskManager{nullptr};
  IOrderValidator* _orderValidator{nullptr};
  IKillSwitch* _killSwitch{nullptr};
  IPnLTracker* _pnlTracker{nullptr};
  // Built-in position tracker. Wired as an execution listener at
  // construction and attached to the strategy in setStrategy() so
  // `ctx.position` / `ctx.is_long()` / `ctx.is_flat()` reflect fills
  // the simulator dispatches. Subscriber id is internal — does not
  // collide with user-registered listeners.
  MultiModePositionTracker _positionTracker{0xFEFEFEFEu};
  std::vector<IMarketDataSubscriber*> _marketDataSubscribers;
  std::vector<IOrderExecutionListener*> _executionListeners;
  OrderId _nextOrderId{1};

  // Interactive state
  std::atomic<bool> _running{false};
  std::atomic<bool> _paused{false};
  std::atomic<bool> _finished{false};
  std::atomic<bool> _stepRequested{false};
  std::atomic<BacktestMode> _stepMode{BacktestMode::Step};
  bool _interactiveMode{false};

  uint64_t _eventCount{0};
  uint64_t _tradeCount{0};
  uint64_t _bookUpdateCount{0};
  uint64_t _signalCount{0};
  uint64_t _optionQuoteCount{0};  // read, delivered to no consumer (see BacktestState)
  uint64_t _poolStateCount{0};    // read, delivered to no consumer (see BacktestState)
  std::optional<replay::EventType> _lastEventType;

  // Breakpoints
  std::vector<Breakpoint> _breakpoints;
  bool _breakOnSignal{false};
  bool _signalEmitted{false};

  // Synchronization
  mutable std::mutex _mutex;
  std::condition_variable _cv;

  // Callbacks
  EventCallback _eventCallback;
  PauseCallback _pauseCallback;

  // Memory pool for book updates
  std::unique_ptr<std::pmr::monotonic_buffer_resource> _pool;
};

}  // namespace lrvx
