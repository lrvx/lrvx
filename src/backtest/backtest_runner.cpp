/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/backtest/backtest_runner.h"

#include "lrvx/book/events/book_update_event.h"
#include "lrvx/book/events/trade_event.h"
#include "lrvx/log/log.h"
#include "lrvx/replay/merged_tape_reader.h"
#include "lrvx/strategy/strategy.h"

#include <memory_resource>

namespace lrvx
{

BacktestRunner::BacktestRunner(const BacktestConfig& config)
    : _config(config), _executor(_clock), _pool(std::make_unique<std::pmr::monotonic_buffer_resource>(1024 * 1024))
{
  _executor.applyConfig(_config);
  installOrderEventCallback(_executor);
}

void BacktestRunner::installOrderEventCallback(SimulatedExecutor& exec)
{
  exec.setOrderEventCallback(
      [this](const OrderEvent& ev)
      {
        // The built-in position tracker has to receive fills before
        // any user listener — strategies that read `ctx.position`
        // inside an order-event handler need the tracker already
        // updated when their listener fires. User listeners are
        // appended after via addExecutionListener().
        ev.dispatchTo(_positionTracker);
        // PnL tracker parity with live Runner: an attached tracker
        // sees every fill (PARTIALLY_FILLED + FILLED) regardless of
        // whether user listeners are wired. Mirrors the live path,
        // so a strategy validated in backtest produces the same
        // tracker timeline in production.
        if (_pnlTracker != nullptr &&
            (ev.status == OrderEventStatus::FILLED ||
             ev.status == OrderEventStatus::PARTIALLY_FILLED))
        {
          _pnlTracker->onOrderFilled(ev.order);
        }
        for (auto* listener : _executionListeners)
        {
          // dispatchTo() ends with listener->onOrderEvent(ev) itself now, so
          // the call that used to sit here would deliver every event twice.
          ev.dispatchTo(*listener);
        }
        // Forward to the attached strategy so user-side `on_fill` /
        // `on_order_update` hooks fire. Without this the strategy
        // never learns its emitted orders filled — native stops are
        // unusable, and exit logic that wants to confirm a close
        // has no callback to hang on.
        if (_strategy)
        {
          _strategy->onOrderEvent(ev);
        }
      });
}

void BacktestRunner::setSimulatedExecutor(SimulatedExecutor* executor, SimulatedClock* clock)
{
  _venueExecutor = executor;
  _venueClock = clock;
  if (executor != nullptr)
  {
    // Deliberately no applyConfig(): the stack arrives already configured
    // for its venue, and applyConfig() would overwrite that with
    // BacktestConfig's flat defaults. Fees come from the stack's own
    // FeeSchedule, which the venue wiring hands to the executor and which
    // result() forwards to BacktestResult, so each fill is billed at the
    // tier the account's 30-day notional resolves to.
    installOrderEventCallback(*executor);
    if (clock != nullptr)
    {
      clock->advanceTo(_clock.nowNs());
    }
  }
}

void BacktestRunner::advanceClocks(UnixNanos ns)
{
  _clock.advanceTo(ns);
  if (_venueClock != nullptr)
  {
    _venueClock->advanceTo(ns);
  }
}

void BacktestRunner::setStrategy(IStrategy* strategy)
{
  if (strategy == nullptr)
  {
    // Every other use of _strategy is already null-checked, so a runner with
    // no strategy replays the data and reports an empty result. Refusing the
    // null here keeps that behaviour instead of dereferencing it.
    LRVX_LOG_ERROR("BacktestRunner: setStrategy(nullptr) ignored");
    return;
  }
  _strategy = strategy;
  strategy->setSignalHandler(this);
  // Without this wire `ctx.position` / `ctx.is_long()` / `ctx.is_flat()`
  // never reflect fills the executor dispatches. Strategy::onTrade /
  // onBar / onBookUpdate refresh the per-symbol context from this
  // tracker before each handler call.
  strategy->setPositionManager(&_positionTracker);
}

void BacktestRunner::addMarketDataSubscriber(IMarketDataSubscriber* subscriber)
{
  _marketDataSubscribers.push_back(subscriber);
}

void BacktestRunner::addExecutionListener(IOrderExecutionListener* listener)
{
  _executionListeners.push_back(listener);
}

// ========== Non-interactive mode ==========

// Everything a finished run leaves behind. Without it a second run reports the
// sum of both: cumulative fills, a position carried over from the first pass,
// and a clock that never rewinds, so the equity curve still starts at the
// first run's timestamps.
void BacktestRunner::resetRunState()
{
  sim().reset();
  _positionTracker.reset();
  _clock.reset();
  if (_venueClock != nullptr)
  {
    _venueClock->reset();
  }
  _nextOrderId = 1;
  _eventCount = 0;
  _tradeCount = 0;
  _bookUpdateCount = 0;
  _signalCount = 0;
  _skippedRecordCount = 0;
  _lastEventType.reset();
  _signalEmitted = false;
}

BacktestResult BacktestRunner::run(replay::IMultiSegmentReader& reader)
{
  resetRunState();
  _interactiveMode = false;
  _running.store(true, std::memory_order_release);
  _paused.store(false, std::memory_order_release);
  _finished.store(false, std::memory_order_release);

  if (_strategy)
  {
    _strategy->start();
  }

  reader.forEach([this](const replay::ReplayEvent& event) -> bool
                 {
    processEvent(event);
    return true; });

  if (_strategy)
  {
    _strategy->stop();
  }

  _finished.store(true, std::memory_order_release);
  _running.store(false, std::memory_order_release);

  return result();
}

BacktestResult BacktestRunner::runTape(const std::filesystem::path& data_dir)
{
  if (!std::filesystem::exists(data_dir))
  {
    throw std::runtime_error(
        "BacktestRunner.runTape: directory does not exist: " +
        data_dir.string());
  }
  if (!std::filesystem::is_directory(data_dir))
  {
    throw std::runtime_error(
        "BacktestRunner.runTape: path is not a directory: " +
        data_dir.string());
  }
  auto reader = replay::createMultiSegmentReader(data_dir);
  if (!reader)
  {
    throw std::runtime_error(
        "BacktestRunner.runTape: cannot open `.lrvx` directory: " +
        data_dir.string());
  }
  return run(*reader);
}

BacktestResult BacktestRunner::runTapes(
    const std::vector<std::filesystem::path>& data_dirs)
{
  if (data_dirs.empty())
  {
    throw std::runtime_error("BacktestRunner.runTapes: empty paths list");
  }
  for (const auto& d : data_dirs)
  {
    if (!std::filesystem::exists(d) || !std::filesystem::is_directory(d))
    {
      throw std::runtime_error(
          "BacktestRunner.runTapes: not a directory: " + d.string());
    }
  }
  replay::MergedTapeReaderConfig cfg{};
  cfg.tape_dirs = data_dirs;
  replay::MergedTapeReader merger(std::move(cfg));
  auto adapter = merger.asMultiSegmentReader();
  return run(*adapter);
}

BacktestResult BacktestRunner::runBars(const std::vector<BarEvent>& bars)
{
  resetRunState();
  _interactiveMode = false;
  _running.store(true, std::memory_order_release);
  _paused.store(false, std::memory_order_release);
  _finished.store(false, std::memory_order_release);

  if (_strategy)
  {
    _strategy->start();
  }

  for (const auto& ev : bars)
  {
    advanceClocks(UnixNanos::fromRaw(ev.bar.endTime.time_since_epoch().count()));
    ++_eventCount;

    // Feed the bar's full range so resting stops/take-profits match against the
    // intrabar high/low, not just the close. The open leads: it is where the
    // orders held back from the previous bar's callback are matched.
    sim().onBar(ev.symbol, ev.bar.open, ev.bar.high, ev.bar.low, ev.bar.close);

    {
      // The strategy is shown a bar the simulator has already walked, so every
      // price it can react to is in the market state. Anything it submits from
      // here is held until the next bar for that symbol opens. The scope closes
      // the window on every way out of this block -- including a callback that
      // throws, which would otherwise leave it open and hold every later order
      // for the rest of the run.
      SimulatedExecutor::BarCallbackScope window(sim());
      if (_strategy)
      {
        _strategy->onBar(ev);
      }
      for (auto* sub : _marketDataSubscribers)
      {
        sub->onBar(ev);
      }
    }
  }

  if (_strategy)
  {
    _strategy->stop();
  }

  _finished.store(true, std::memory_order_release);
  _running.store(false, std::memory_order_release);

  return result();
}

// ========== Interactive mode ==========

void BacktestRunner::start(replay::IMultiSegmentReader& reader)
{
  resetRunState();
  _interactiveMode = true;
  _running.store(true, std::memory_order_release);
  _paused.store(true, std::memory_order_release);
  _finished.store(false, std::memory_order_release);

  if (_strategy)
  {
    _strategy->start();
  }

  reader.forEach(
      [this](const replay::ReplayEvent& event) -> bool
      {
        if (!_running.load(std::memory_order_acquire))
        {
          return false;
        }

        // Check if we should pause
        if (_paused.load(std::memory_order_acquire) && !_stepRequested.load(std::memory_order_acquire))
        {
          notifyPaused();
          waitForResume();
          if (!_running.load(std::memory_order_acquire))
          {
            return false;
          }
        }

        // Process the event
        processEvent(event);

        // Check breakpoints after processing
        if (checkBreakpoints(event))
        {
          _paused.store(true, std::memory_order_release);
          notifyPaused();
        }

        // Handle step mode
        if (_stepRequested.load(std::memory_order_acquire))
        {
          BacktestMode mode = _stepMode.load(std::memory_order_acquire);
          bool shouldPause = false;

          switch (mode)
          {
            case BacktestMode::Step:
              shouldPause = true;
              break;
            case BacktestMode::StepTrade:
              shouldPause = (event.type == replay::EventType::Trade);
              break;
            case BacktestMode::Run:
              shouldPause = false;
              break;
          }

          if (shouldPause)
          {
            _stepRequested.store(false, std::memory_order_release);
            _paused.store(true, std::memory_order_release);
          }
        }

        return true;
      });

  _finished.store(true, std::memory_order_release);
  _running.store(false, std::memory_order_release);

  if (_strategy)
  {
    _strategy->stop();
  }

  notifyPaused();
}

void BacktestRunner::processEvent(const replay::ReplayEvent& event)
{
  advanceClocks(UnixNanos::fromRaw(event.timestamp_ns));
  ++_eventCount;
  _lastEventType = event.type;

  if (event.type == replay::EventType::Trade)
  {
    ++_tradeCount;

    TradeEvent trade_ev;
    trade_ev.trade.symbol = event.trade.symbol_id;
    trade_ev.trade.price = Price::fromRaw(event.trade.price_raw);
    trade_ev.trade.quantity = Quantity::fromRaw(event.trade.qty_raw);
    trade_ev.trade.isBuy = (event.trade.side == 0);
    trade_ev.trade.exchangeTsNs = UnixNanos::fromRaw(event.trade.exchange_ts_ns);
    trade_ev.trade.instrument = static_cast<InstrumentType>(event.trade.instrument);
    trade_ev.exchangeMsgTsNs = UnixNanos::fromRaw(event.trade.exchange_ts_ns);

    sim().onTrade(trade_ev.trade.symbol, trade_ev.trade.price, trade_ev.trade.quantity,
                  trade_ev.trade.isBuy);

    if (_strategy)
    {
      _strategy->onTrade(trade_ev);
    }

    for (auto* subscriber : _marketDataSubscribers)
    {
      subscriber->onTrade(trade_ev);
    }
  }
  else if (event.type == replay::EventType::BookSnapshot ||
           event.type == replay::EventType::BookDelta ||
           event.type == replay::EventType::BookTop)
  {
    ++_bookUpdateCount;

    BookUpdateEvent book_ev(_pool.get());
    book_ev.update.symbol = event.book_header.symbol_id;
    book_ev.update.type = (event.type == replay::EventType::BookSnapshot) ? BookUpdateType::SNAPSHOT
                          : (event.type == replay::EventType::BookTop)
                              ? BookUpdateType::TOP_OF_BOOK
                              : BookUpdateType::DELTA;
    book_ev.update.instrument = static_cast<InstrumentType>(event.book_header.instrument);
    book_ev.update.exchangeTsNs = UnixNanos::fromRaw(event.book_header.exchange_ts_ns);
    book_ev.seq = event.book_header.seq;

    book_ev.update.bids.reserve(event.bids.size());
    book_ev.update.asks.reserve(event.asks.size());

    for (const auto& bid : event.bids)
    {
      book_ev.update.bids.emplace_back(Price::fromRaw(bid.price_raw), Quantity::fromRaw(bid.qty_raw));
    }
    for (const auto& ask : event.asks)
    {
      book_ev.update.asks.emplace_back(Price::fromRaw(ask.price_raw), Quantity::fromRaw(ask.qty_raw));
    }

    // The simulated venue's ladder and queue tracker take depth frames; a
    // one-level top would wipe every other tracked level.
    if (book_ev.update.type != BookUpdateType::TOP_OF_BOOK)
    {
      sim().onBookUpdate(book_ev.update.symbol, book_ev.update.bids, book_ev.update.asks);
    }

    if (_strategy)
    {
      _strategy->onBookUpdate(book_ev);
    }

    for (auto* subscriber : _marketDataSubscribers)
    {
      subscriber->onBookUpdate(book_ev);
    }

    _pool->release();
  }
  else if (event.type == replay::EventType::OptionQuote ||
           event.type == replay::EventType::PoolState)
  {
    // Not dispatched: there is no strategy/subscriber handler for option-quote
    // or pool-state records. Count them and warn once so a tape that is mostly
    // these records does not look like it was fully replayed.
    if (_skippedRecordCount++ == 0)
    {
      LRVX_LOG_WARN(
          "[BacktestRunner] tape contains OptionQuote/PoolState records "
          "that this runner does not dispatch; they are counted in "
          "skippedRecordCount, not delivered to the strategy");
    }
  }

  // Invoke callback if set (interactive mode)
  if (_eventCallback)
  {
    _eventCallback(event, state());
  }
}

bool BacktestRunner::checkBreakpoints(const replay::ReplayEvent& event)
{
  if (!_interactiveMode)
  {
    return false;
  }

  // Check signal breakpoint
  if (_breakOnSignal && _signalEmitted)
  {
    _signalEmitted = false;
    return true;
  }

  for (const auto& bp : _breakpoints)
  {
    switch (bp.type)
    {
      case Breakpoint::Type::Time:
        if (event.timestamp_ns >= bp.timestampNs.raw())
        {
          return true;
        }
        break;
      case Breakpoint::Type::EventCount:
        if (_eventCount >= bp.count)
        {
          return true;
        }
        break;
      case Breakpoint::Type::TradeCount:
        if (_tradeCount >= bp.count)
        {
          return true;
        }
        break;
      case Breakpoint::Type::Signal:
        // Handled above with _signalEmitted
        break;
      case Breakpoint::Type::Custom:
        if (bp.predicate && bp.predicate(event))
        {
          return true;
        }
        break;
    }
  }
  return false;
}

void BacktestRunner::waitForResume()
{
  std::unique_lock lock(_mutex);
  _cv.wait(lock,
           [this]
           {
             return !_paused.load(std::memory_order_acquire) || _stepRequested.load(std::memory_order_acquire) ||
                    !_running.load(std::memory_order_acquire);
           });
}

void BacktestRunner::notifyPaused()
{
  if (_pauseCallback)
  {
    _pauseCallback(state());
  }
}

// The flag stores below happen under _mutex: waitForResume evaluates its
// predicate with the lock held, so a store+notify outside the lock can land
// between the predicate check and the sleep and the wakeup is lost forever.

void BacktestRunner::resume()
{
  {
    std::lock_guard lock(_mutex);
    _stepRequested.store(false, std::memory_order_release);
    _paused.store(false, std::memory_order_release);
  }
  _cv.notify_all();
}

void BacktestRunner::step()
{
  {
    std::lock_guard lock(_mutex);
    _stepMode.store(BacktestMode::Step, std::memory_order_release);
    _stepRequested.store(true, std::memory_order_release);
    _paused.store(false, std::memory_order_release);
  }
  _cv.notify_all();
}

void BacktestRunner::stepUntil(BacktestMode mode)
{
  {
    std::lock_guard lock(_mutex);
    _stepMode.store(mode, std::memory_order_release);
    _stepRequested.store(true, std::memory_order_release);
    _paused.store(false, std::memory_order_release);
  }
  _cv.notify_all();
}

void BacktestRunner::pause()
{
  std::lock_guard lock(_mutex);
  _paused.store(true, std::memory_order_release);
}

void BacktestRunner::stop()
{
  {
    std::lock_guard lock(_mutex);
    _running.store(false, std::memory_order_release);
    _paused.store(false, std::memory_order_release);
  }
  _cv.notify_all();
}

void BacktestRunner::addBreakpoint(Breakpoint bp)
{
  std::lock_guard lock(_mutex);
  _breakpoints.push_back(std::move(bp));
}

void BacktestRunner::clearBreakpoints()
{
  std::lock_guard lock(_mutex);
  _breakpoints.clear();
}

void BacktestRunner::setBreakOnSignal(bool enable)
{
  _breakOnSignal = enable;
}

BacktestState BacktestRunner::state() const
{
  return BacktestState{.currentTimeNs = static_cast<UnixNanos>(_clock.nowNs()),
                       .eventCount = _eventCount,
                       .tradeCount = _tradeCount,
                       .bookUpdateCount = _bookUpdateCount,
                       .signalCount = _signalCount,
                       .isRunning = _running.load(std::memory_order_acquire),
                       .isPaused = _paused.load(std::memory_order_acquire),
                       .isFinished = _finished.load(std::memory_order_acquire),
                       .lastEventType = _lastEventType};
}

// A result built off a venue executor prices its fills through that venue's
// fee ladder; a plain run has no schedule attached and keeps charging the
// flat BacktestConfig rate.
void BacktestRunner::attachFeeSchedule(BacktestResult& res) const
{
  if (const FeeSchedule* fees = sim().feeSchedule(); fees != nullptr)
  {
    res.setFeeSchedule(*fees);
  }
}

BacktestResult BacktestRunner::result() const
{
  BacktestResult res(_config, sim().fills().size());
  attachFeeSchedule(res);
  for (const auto& fill : sim().fills())
  {
    res.recordFill(fill);
  }
  return res;
}

BacktestResult BacktestRunner::extractResult()
{
  BacktestResult res(_config, sim().fills().size());
  attachFeeSchedule(res);
  auto fills = sim().extractFills();
  for (auto& fill : fills)
  {
    res.recordFill(fill);
  }
  return res;
}

// Pre-trade gate. Returns true if the order should pass through;
// false if any gate rejected it. Reduce-only orders bypass — when
// caps tighten you do not want to be stuck in a position; the live
// runner is conventionally wired this way and `lookup_symbol`
// surfaces a gotcha entry pointing at this contract.
bool BacktestRunner::passesPreTradeGate(const Order& order)
{
  if (order.flags.reduceOnly)
  {
    return true;
  }
  if (_killSwitch != nullptr)
  {
    // Engine contract: check(order) may flip state; isTriggered()
    // reads the current state. Call check first so a per-order rule
    // (e.g. "trip if cumulative loss > X") gets evaluated for this
    // order before we read the trigger.
    _killSwitch->check(order);
    if (_killSwitch->isTriggered())
    {
      return false;
    }
  }
  if (_orderValidator != nullptr)
  {
    std::string reason;
    if (!_orderValidator->validate(order, reason))
    {
      return false;
    }
  }
  if (_riskManager != nullptr && !_riskManager->allow(order))
  {
    return false;
  }
  return true;
}

void BacktestRunner::onSignal(const Signal& signal)
{
  ++_signalCount;
  _signalEmitted = true;

  // Route to custom executor if attached, else to the built-in simulator.
  IOrderExecutor& exec = (_customExecutor != nullptr)
                             ? *_customExecutor
                             : static_cast<IOrderExecutor&>(sim());

  switch (signal.type)
  {
    case SignalType::Market:
    case SignalType::Limit:
    case SignalType::StopMarket:
    case SignalType::StopLimit:
    case SignalType::TakeProfitMarket:
    case SignalType::TakeProfitLimit:
    case SignalType::TrailingStop:
    {
      Order order = signalToOrder(signal);
      if (!passesPreTradeGate(order))
      {
        return;
      }
      exec.submitOrder(order);
      break;
    }
    case SignalType::Cancel:
      // Cancel / CancelAll / Modify do not add exposure — same
      // pass-through semantics as the live runner.
      exec.cancelOrder(signal.orderId);
      break;
    case SignalType::CancelAll:
      exec.cancelAllOrders(signal.symbol);
      break;
    case SignalType::Modify:
    {
      Order newOrder{.id = _nextOrderId++, .price = signal.newPrice, .quantity = signal.newQuantity};
      exec.replaceOrder(signal.orderId, newOrder);
      break;
    }
    case SignalType::OCO:
    {
      // OCO creates two linked orders
      Order order1 = signalToOrder(signal);
      order1.type = OrderType::LIMIT;
      order1.price = signal.price;

      Order order2{.id = _nextOrderId++,
                   .side = signal.side,
                   .price = signal.triggerPrice,
                   .quantity = signal.quantity,
                   .type = OrderType::LIMIT,
                   .symbol = signal.symbol,
                   .timeInForce = signal.timeInForce,
                   .flags = {.reduceOnly = signal.reduceOnly, .postOnly = signal.postOnly}};

      // Both legs of an OCO need to pass; rejecting either drops the pair.
      if (!passesPreTradeGate(order1) || !passesPreTradeGate(order2))
      {
        return;
      }
      OCOParams params{.order1 = order1, .order2 = order2};
      exec.submitOCO(params);
      break;
    }
    case SignalType::ProvideLiquidity:
    case SignalType::WithdrawLiquidity:
      // DEX/AMM liquidity ops are executed by the on-chain connector, not
      // the backtest CEX execution path.
      break;
  }
}

Order BacktestRunner::signalToOrder(const Signal& sig)
{
  OrderId id = (sig.orderId != 0) ? sig.orderId : _nextOrderId++;

  OrderType orderType{};
  switch (sig.type)
  {
    case SignalType::Market:
      orderType = OrderType::MARKET;
      break;
    case SignalType::Limit:
      orderType = OrderType::LIMIT;
      break;
    case SignalType::StopMarket:
      orderType = OrderType::STOP_MARKET;
      break;
    case SignalType::StopLimit:
      orderType = OrderType::STOP_LIMIT;
      break;
    case SignalType::TakeProfitMarket:
      orderType = OrderType::TAKE_PROFIT_MARKET;
      break;
    case SignalType::TakeProfitLimit:
      orderType = OrderType::TAKE_PROFIT_LIMIT;
      break;
    case SignalType::TrailingStop:
      orderType = OrderType::TRAILING_STOP;
      break;
    default:
      orderType = OrderType::MARKET;
      break;
  }

  return Order{.id = id,
               .side = sig.side,
               .price = sig.price,
               .quantity = sig.quantity,
               .type = orderType,
               .symbol = sig.symbol,
               .timeInForce = sig.timeInForce,
               .flags = {.reduceOnly = sig.reduceOnly, .postOnly = sig.postOnly},
               .triggerPrice = sig.triggerPrice,
               .trailingOffset = sig.trailingOffset,
               .trailingCallbackRate = sig.trailingCallbackRate};
}

}  // namespace lrvx
