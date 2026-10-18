/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/aggregator/events/bar_event.h"
#include "lrvx/book/events/book_update_event.h"
#include "lrvx/book/events/trade_event.h"
#include "lrvx/engine/symbol_registry.h"
#include "lrvx/execution/events/order_event.h"
#include "lrvx/execution/order_tracker.h"
#include "lrvx/position/abstract_position_manager.h"
#include "lrvx/strategy/abstract_signal_handler.h"
#include "lrvx/strategy/abstract_strategy.h"
#include "lrvx/strategy/signal.h"
#include "lrvx/strategy/symbol_context.h"
#include "lrvx/strategy/symbol_state_map.h"
#include "lrvx/util/performance/busy_backoff.h"

#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

namespace lrvx
{

// Base class for every strategy, and the layer every binding sits on:
// Python, Node, Codon and the JS engine all reach their user code through
// the hooks below.
//
// OWNERSHIP OF THE PER-SYMBOL STATE
//
// A strategy subscribes to three buses -- trades, book updates, bars -- and
// each bus runs its own consumer thread per subscriber. Two events for the
// SAME symbol therefore arrive at the same instant on two different
// threads, and everything they touch is shared: the symbol's context, the
// order book inside it, the last trade price, the last update timestamp.
// Nothing used to stand between those writers.
//
// The rule now is explicit. A symbol's state belongs to whoever holds that
// symbol's lock. Every dispatch below takes the lock before it touches the
// context and keeps it until the strategy hook has returned, because the
// hook is handed a reference to that context and reads it -- releasing
// earlier would hand user code a reference another thread is free to
// rewrite underneath it.
//
// Per symbol rather than per strategy, so two symbols still dispatch in
// parallel. Reentrant, because a hook may emit an order that an executor
// fills synchronously, which re-enters this same strategy for the same
// symbol on the same thread; a plain mutex would deadlock there.
//
// What the lock does NOT cover: the `ctx()` accessors hand out a bare
// reference, so they are for use from inside a hook (where this thread
// already holds the lock) or before the engine starts. Reading a context
// from an unrelated thread while the engine runs is outside the contract.
class Strategy : public IStrategy
{
 public:
  // Identifies a single timeframe instance: a `(BarType, param)` pair.
  // For Time bars the param is interval-in-nanoseconds; for Tick /
  // Volume bars it is the tick count or volume threshold the
  // aggregator was configured with.
  struct BarTfKey
  {
    BarType type;
    uint64_t param;
    bool operator==(const BarTfKey& o) const noexcept
    {
      return type == o.type && param == o.param;
    }
  };
  struct BarTfKeyHash
  {
    std::size_t operator()(const BarTfKey& k) const noexcept
    {
      return std::hash<uint8_t>{}(static_cast<uint8_t>(k.type)) ^
             (std::hash<uint64_t>{}(k.param) << 1);
    }
  };

  Strategy(SubscriberId id, std::vector<SymbolId> symbols, const SymbolRegistry& registry)
      : _id(id), _symbols(std::move(symbols))
  {
    _symbolSet.insert(_symbols.begin(), _symbols.end());
    for (SymbolId sym : _symbols)
    {
      auto info = registry.getSymbolInfo(sym);
      if (!info)
      {
        throw std::invalid_argument("Symbol " + std::to_string(sym) + " not found in registry");
      }
      _contexts[sym] = SymbolContext(info->tickSize);
      _contexts[sym].symbolId = sym;
    }
  }

  Strategy(SubscriberId id, SymbolId symbol, const SymbolRegistry& registry)
      : Strategy(id, std::vector<SymbolId>{symbol}, registry)
  {
  }

  SubscriberId id() const override { return _id; }

  void setSignalHandler(ISignalHandler* handler) override { _signalHandler = handler; }
  void setOrderTracker(OrderTracker* tracker) noexcept { _orderTracker = tracker; }
  void setPositionManager(IPositionManager* pm) noexcept override { _positionManager = pm; }

  void onTrade(const TradeEvent& ev) final
  {
    SymbolId sym = ev.trade.symbol;
    if (!isSubscribed(sym))
    {
      return;
    }

    std::lock_guard<SymbolLock> owned(lockFor(sym));
    auto& c = _contexts[sym];
    c.lastTradePrice = ev.trade.price;
    c.lastUpdateNs = ev.trade.exchangeTsNs.raw();
    refreshPosition(c, sym);

    onSymbolTrade(c, ev);
  }

  void onBookUpdate(const BookUpdateEvent& ev) final
  {
    SymbolId sym = ev.update.symbol;
    if (!isSubscribed(sym))
    {
      return;
    }

    // A TOP_OF_BOOK frame lands in the touch fields and nothing else: the
    // depth book ignores it (see BookUpdateType), and dispatching it would
    // hand the strategy the previous depth frame's book at touch rate,
    // stamped fresh.
    if (ev.update.type == BookUpdateType::TOP_OF_BOOK)
    {
      std::lock_guard<SymbolLock> owned(lockFor(sym));
      auto& c = _contexts[sym];
      c.touchBid = ev.update.bids.empty() ? std::nullopt
                                          : std::optional<BookLevel>(ev.update.bids.front());
      c.touchAsk = ev.update.asks.empty() ? std::nullopt
                                          : std::optional<BookLevel>(ev.update.asks.front());
      c.lastTouchNs = ev.update.exchangeTsNs.raw();
      return;
    }

    std::lock_guard<SymbolLock> owned(lockFor(sym));
    auto& c = _contexts[sym];
    c.book.applyBookUpdate(ev);
    c.lastUpdateNs = ev.update.exchangeTsNs.raw();
    refreshTouchFromBook(c, ev.update.exchangeTsNs.raw());
    refreshPosition(c, sym);

    onSymbolBook(c, ev);
  }

  void onBar(const BarEvent& ev) final
  {
    SymbolId sym = ev.symbol;
    if (!isSubscribed(sym))
    {
      return;
    }

    std::lock_guard<SymbolLock> owned(lockFor(sym));
    auto& c = _contexts[sym];
    c.lastTradePrice = ev.bar.close;
    c.lastUpdateNs = ev.bar.endTime.time_since_epoch().count();
    refreshPosition(c, sym);

    // Push into the per-(symbol, timeframe) ring so multi-TF strategies
    // can recall the last N closed bars without bookkeeping by hand.
    //
    // Its own lock, not the symbol's: inserting a new (symbol, timeframe)
    // key rehashes the whole map, which moves buckets belonging to every
    // other symbol, so per-symbol granularity would not be enough. Always
    // taken while the symbol lock is held, never the other way round.
    {
      std::lock_guard<std::mutex> rings(_barRingMutex);
      auto& ring = _barRings[sym][BarTfKey{ev.barType, ev.barTypeParam}];
      if (ring.size() >= _barRingCapacity.load(std::memory_order_relaxed))
      {
        ring.pop_front();
      }
      ring.push_back(ev.bar);
    }

    onSymbolBar(c, ev);
  }

  void onOrderEvent(const OrderEvent& ev) override
  {
    SymbolId sym = ev.order.symbol;
    if (!isSubscribed(sym))
    {
      return;
    }
    std::lock_guard<SymbolLock> owned(lockFor(sym));
    auto& c = _contexts[sym];
    if (ev.status == OrderEventStatus::FILLED ||
        ev.status == OrderEventStatus::PARTIALLY_FILLED)
    {
      onSymbolFill(c, ev);
    }
    else if (ev.status == OrderEventStatus::QUEUE_POSITION_UPDATED)
    {
      onSymbolQueuePositionChange(c, ev);
    }
    else if (ev.status == OrderEventStatus::MARKET_POSITION_CHANGED)
    {
      onSymbolMarketPositionChange(c, ev);
    }
    else
    {
      onSymbolOrderUpdate(c, ev);
    }
  }

 protected:
  virtual void onSymbolTrade(SymbolContext& ctx, const TradeEvent& ev) {}
  virtual void onSymbolBook(SymbolContext& ctx, const BookUpdateEvent& ev) {}
  virtual void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) {}

  // Order-event hooks. The runner forwards every executor event for an
  // order this strategy emitted (FILLED, PARTIALLY_FILLED, CANCELED,
  // REJECTED, etc) through `onOrderEvent`, which dispatches here.
  // Override `onSymbolFill` for fill notifications (the common case)
  // and `onSymbolOrderUpdate` for everything else (cancels, rejects,
  // pending-trigger transitions, etc). Without these hooks native
  // `stop_market` is unusable — there is no other path for the
  // strategy to learn its stop fired.
  virtual void onSymbolFill(SymbolContext& ctx, const OrderEvent& ev) {}
  virtual void onSymbolOrderUpdate(SymbolContext& ctx, const OrderEvent& ev) {}

  // Resting limit order's queue position moved without any other
  // lifecycle transition. `ev.queueAhead` is the volume in front of
  // the order at its price level at this moment; `ev.queueTotal` is
  // the level's total quantity. Backtest only — live exchanges do
  // not publish queue position.
  virtual void onSymbolQueuePositionChange(SymbolContext& ctx, const OrderEvent& ev) {}

  // Resting limit order's categorical market position transitioned.
  // `ev.marketPosition` is the new state (Best, BehindBest,
  // MidSpread, LevelEmpty, Crossed); `ev.distanceToBestTicks` is
  // signed ticks from best on our side. Backtest only.
  virtual void onSymbolMarketPositionChange(SymbolContext& ctx, const OrderEvent& ev) {}

  SymbolContext& ctx(SymbolId sym) noexcept { return _contexts[sym]; }
  const SymbolContext& ctx(SymbolId sym) const noexcept { return _contexts[sym]; }

  SymbolContext& ctx() noexcept { return _contexts[_symbols[0]]; }
  const SymbolContext& ctx() const noexcept { return _contexts[_symbols[0]]; }

  SymbolId symbol() const noexcept { return _symbols[0]; }

  bool isSubscribed(SymbolId sym) const noexcept { return _symbolSet.count(sym) > 0; }

  const std::vector<SymbolId>& symbols() const noexcept { return _symbols; }

  // Position and order status queries
  Quantity position(SymbolId sym) const
  {
    return _positionManager ? _positionManager->getPosition(sym) : Quantity{};
  }

  Quantity position() const { return position(_symbols[0]); }

  std::optional<OrderEventStatus> getOrderStatus(OrderId orderId) const
  {
    return _orderTracker ? _orderTracker->getStatus(orderId) : std::nullopt;
  }

  std::optional<OrderState> getOrder(OrderId orderId) const
  {
    return _orderTracker ? _orderTracker->get(orderId) : std::nullopt;
  }

 public:
  // Multi-TF alignment helpers.
  //
  // After a `BarAggregator` of the requested timeframe has emitted at
  // least one bar for the symbol, `lastClosedBar` returns it; before
  // that it returns `std::nullopt`. The ring stores up to
  // `barRingCapacity()` bars per (symbol, tf) and evicts the oldest
  // when full. `lastNClosedBars` returns the most recent `n` in
  // chronological order (oldest first).
  size_t barRingCapacity() const noexcept
  {
    return _barRingCapacity.load(std::memory_order_relaxed);
  }
  void setBarRingCapacity(size_t n) noexcept
  {
    _barRingCapacity.store(std::max<size_t>(n, 1), std::memory_order_relaxed);
  }

  std::optional<Bar> lastClosedBar(SymbolId sym, BarType type, uint64_t param) const
  {
    std::lock_guard<std::mutex> rings(_barRingMutex);
    auto symIt = _barRings.find(sym);
    if (symIt == _barRings.end())
    {
      return std::nullopt;
    }
    auto tfIt = symIt->second.find(BarTfKey{type, param});
    if (tfIt == symIt->second.end() || tfIt->second.empty())
    {
      return std::nullopt;
    }
    return tfIt->second.back();
  }

  std::vector<Bar> lastNClosedBars(SymbolId sym, BarType type, uint64_t param, size_t n) const
  {
    std::vector<Bar> out;
    std::lock_guard<std::mutex> rings(_barRingMutex);
    auto symIt = _barRings.find(sym);
    if (symIt == _barRings.end())
    {
      return out;
    }
    auto tfIt = symIt->second.find(BarTfKey{type, param});
    if (tfIt == symIt->second.end())
    {
      return out;
    }
    const auto& ring = tfIt->second;
    size_t take = std::min(n, ring.size());
    out.reserve(take);
    for (size_t i = ring.size() - take; i < ring.size(); ++i)
    {
      out.push_back(ring[i]);
    }
    return out;
  }

  // Signal emission
  void emit(const Signal& signal)
  {
    if (_signalHandler)
    {
      _signalHandler->onSignal(signal);
    }
  }

  // One tick's signals handed over together, so a handler that knows how to
  // coalesce (see ISignalHandler::onSignalBatch) can turn a two-sided quote
  // or a ladder requote into a single venue request. With the default
  // handler this is exactly a loop over emit().
  void emitBatch(std::span<const Signal> signals)
  {
    if (_signalHandler)
    {
      _signalHandler->onSignalBatch(signals);
    }
  }

  OrderId emitMarketBuy(SymbolId symbol, Quantity qty)
  {
    OrderId id = nextOrderId();
    emit(Signal::marketBuy(symbol, qty, id));
    return id;
  }

  OrderId emitMarketSell(SymbolId symbol, Quantity qty)
  {
    OrderId id = nextOrderId();
    emit(Signal::marketSell(symbol, qty, id));
    return id;
  }

  OrderId emitLimitBuy(SymbolId symbol, Price price, Quantity qty)
  {
    OrderId id = nextOrderId();
    emit(Signal::limitBuy(symbol, price, qty, id));
    return id;
  }

  OrderId emitLimitSell(SymbolId symbol, Price price, Quantity qty)
  {
    OrderId id = nextOrderId();
    emit(Signal::limitSell(symbol, price, qty, id));
    return id;
  }

  void emitCancel(OrderId orderId) { emit(Signal::cancel(orderId)); }
  void emitCancelAll(SymbolId symbol) { emit(Signal::cancelAll(symbol)); }

  void emitModify(OrderId orderId, Price newPrice, Quantity newQty)
  {
    emit(Signal::modify(orderId, newPrice, newQty));
  }

  // Stop orders
  OrderId emitStopMarket(SymbolId symbol, Side side, Price triggerPrice, Quantity qty)
  {
    OrderId id = nextOrderId();
    emit(Signal::stopMarket(symbol, side, triggerPrice, qty, id));
    return id;
  }

  OrderId emitStopLimit(SymbolId symbol, Side side, Price triggerPrice, Price limitPrice,
                        Quantity qty)
  {
    OrderId id = nextOrderId();
    emit(Signal::stopLimit(symbol, side, triggerPrice, limitPrice, qty, id));
    return id;
  }

  // Take profit orders
  OrderId emitTakeProfitMarket(SymbolId symbol, Side side, Price triggerPrice, Quantity qty)
  {
    OrderId id = nextOrderId();
    emit(Signal::takeProfitMarket(symbol, side, triggerPrice, qty, id));
    return id;
  }

  OrderId emitTakeProfitLimit(SymbolId symbol, Side side, Price triggerPrice, Price limitPrice,
                              Quantity qty)
  {
    OrderId id = nextOrderId();
    emit(Signal::takeProfitLimit(symbol, side, triggerPrice, limitPrice, qty, id));
    return id;
  }

  // Trailing stop
  OrderId emitTrailingStop(SymbolId symbol, Side side, Price offset, Quantity qty)
  {
    OrderId id = nextOrderId();
    emit(Signal::trailingStop(symbol, side, offset, qty, id));
    return id;
  }

  OrderId emitTrailingStopPercent(SymbolId symbol, Side side, int32_t callbackBps, Quantity qty)
  {
    OrderId id = nextOrderId();
    emit(Signal::trailingStopPercent(symbol, side, callbackBps, qty, id));
    return id;
  }

  // DEX/AMM liquidity provision. Executed by the on-chain connector.
  OrderId emitProvideLiquidity(SymbolId pool, Price priceLower, Price priceUpper, Quantity liquidity)
  {
    OrderId id = nextOrderId();
    emit(Signal::provideLiquidity(pool, priceLower, priceUpper, liquidity, id));
    return id;
  }

  OrderId emitWithdrawLiquidity(SymbolId pool, Quantity liquidity)
  {
    OrderId id = nextOrderId();
    emit(Signal::withdrawLiquidity(pool, liquidity, id));
    return id;
  }

  // Close position (reduce-only market order)
  OrderId emitClosePosition(SymbolId symbol)
  {
    Quantity pos = position(symbol);
    if (pos.raw() == 0)
    {
      return 0;
    }

    OrderId id = nextOrderId();
    Side side = (pos.raw() > 0) ? Side::SELL : Side::BUY;
    Quantity absQty = Quantity::fromRaw(pos.raw() > 0 ? pos.raw() : -pos.raw());

    auto signal = Signal::marketSell(symbol, absQty, id);
    signal.side = side;
    signal.reduceOnly = true;
    emit(signal);
    return id;
  }

  // Limit orders with TimeInForce
  OrderId emitLimitBuy(SymbolId symbol, Price price, Quantity qty, TimeInForce tif)
  {
    OrderId id = nextOrderId();
    auto signal = Signal::limitBuy(symbol, price, qty, id);
    signal.timeInForce = tif;
    emit(signal);
    return id;
  }

  OrderId emitLimitSell(SymbolId symbol, Price price, Quantity qty, TimeInForce tif)
  {
    OrderId id = nextOrderId();
    auto signal = Signal::limitSell(symbol, price, qty, id);
    signal.timeInForce = tif;
    emit(signal);
    return id;
  }

 private:
  // Order ids are per strategy instance, prefixed with the subscriber id so
  // that strategies sharing a process never collide.
  //
  // A single process-wide counter meant that a second run of the same tape in
  // the same process -- a grid search, a walk-forward, any batch runner --
  // handed out different order ids and different signal ids, so two traces of
  // an identical run did not compare equal byte for byte, and the numbering
  // also moved with however the strategies happened to interleave. A fresh
  // strategy now always starts its own numbering at 1.
  //
  // The namespace is the subscriber id, which is the only identity a strategy
  // has; two strategies holding the same one share an id space, and they are
  // already broken for every bus they sit on. Subscriber ids at or above
  // 2^32-1 would reach the range MultiModePositionTracker reserves for its
  // explicit fills.
  static constexpr int kOrderIdStrategyShift = 32;

  OrderId nextOrderId() noexcept
  {
    return (static_cast<OrderId>(_id) << kOrderIdStrategyShift) + (_nextOrderId++);
  }

  // Pull the latest position and cost basis from the attached
  // IPositionManager into the per-symbol context so `ctx.position` /
  // `ctx.is_long()` / `ctx.is_flat()` / `ctx.unrealizedPnl()` reflect fills
  // the executor has dispatched. Without this hook the SymbolContext.position
  // field is dead — initialised to zero and never updated — which silently
  // produces 0-trade backtests when a strategy guards entries on
  // `ctx.is_flat()` and exits on `ctx.is_long()`.
  //
  // One question per tick. This runs on every trade, every book update and
  // every bar while the symbol lock is held, and it used to ask getPosition()
  // and getAverageEntryPrice() separately: on the shipped trackers that is
  // two acquisitions of the manager's mutex -- the one the execution thread
  // also wants -- and two passes over the symbol's lots, on the market-data
  // thread, for a pair of values that come from the same state under the same
  // lock.
  void refreshPosition(SymbolContext& c, SymbolId sym) noexcept
  {
    if (_positionManager)
    {
      const PositionSnapshot snap = _positionManager->positionSnapshot(sym);
      c.position = snap.position;
      c.avgEntryPrice = snap.avgEntryPrice;
    }
  }

  // Caller holds the symbol lock and has just applied a depth frame: the
  // touch mirrors the book's new top.
  static void refreshTouchFromBook(SymbolContext& c, int64_t exchangeTsNs) noexcept
  {
    auto bid = c.book.bestBid();
    auto ask = c.book.bestAsk();
    c.touchBid = bid ? std::optional<BookLevel>(BookLevel(*bid, c.book.bidAtPrice(*bid)))
                     : std::nullopt;
    c.touchAsk = ask ? std::optional<BookLevel>(BookLevel(*ask, c.book.askAtPrice(*ask)))
                     : std::nullopt;
    c.lastTouchNs = exchangeTsNs;
  }

  // One lock per flat slot, padded to its own cache line so that locking
  // one symbol does not bounce a neighbour's line between bus threads.
  // Symbols past the flat table share a single lock, which also serialises
  // the map's overflow vector -- that one is a std::vector and two threads
  // appending different out-of-range symbols would corrupt it outright.
  //
  // Reentrant, because a hook can emit an order that an executor fills
  // synchronously and re-enters this strategy for the same symbol on the
  // same thread. A thread that already owns the slot skips the wait
  // entirely, so reentrancy costs one relaxed load rather than a
  // std::recursive_mutex, which is about twice a plain mutex uncontended.
  //
  // The wait is BusyBackoff, the same primitive the bus consumer loop
  // uses. Measured against a std::mutex at every contention point that
  // matters here: 1.6 ns against 5.0 uncontended, 1.8 against 7.6 with two
  // threads on one slot, and 221 against 380 with a 200 ns section and
  // three times more runnable threads than cores. A plain spin would be
  // the wrong answer for a section this long -- the lock is held across
  // the strategy hook, which is user code and may block -- but ADAPTIVE is
  // not a plain spin: it pauses 128 times, then yields, then sleeps, so a
  // waiter cedes the core instead of starving the holder it is waiting
  // for. AGGRESSIVE, which spins 2048 times before yielding, measures
  // three times worse under contention here, which is the degradation
  // rt_spin_guard.h already warns about.
  //
  // The owner is a std::thread::id rather than the address of a
  // function-local `thread_local`, which is what this used to be. The
  // address was 0.8 ns cheaper, but a `thread_local` in a header compiled
  // into a static archive gets a non-position-independent access model,
  // and linking that archive into a shared object fails outright on ELF
  // (`relocation R_X86_64_TPOFF32 against hidden symbol ... can not be
  // used when making a shared object`). The Python extension module is
  // exactly that link. Mach-O does not care, so the machine this was
  // written on could not see it. Fixing it with -ftls-model on the one
  // target that happened to fail would leave every future shared-object
  // consumer of liblrvx to rediscover it, so the thread-local is gone
  // instead.
  class SymbolLock
  {
   public:
    void lock()
    {
      const std::thread::id me = std::this_thread::get_id();
      if (_owner.load(std::memory_order_relaxed) == me)
      {
        ++_depth;
        return;
      }
      BusyBackoff backoff(BackoffMode::ADAPTIVE);
      for (;;)
      {
        bool expected = false;
        if (_held.compare_exchange_weak(expected, true, std::memory_order_acquire,
                                        std::memory_order_relaxed))
        {
          break;
        }
        backoff.pause();
      }
      _owner.store(me, std::memory_order_relaxed);
      _depth = 1;
    }

    void unlock()
    {
      if (--_depth != 0)
      {
        return;
      }
      _owner.store(std::thread::id{}, std::memory_order_relaxed);
      _held.store(false, std::memory_order_release);
    }

   private:
    // A non-lock-free atomic here would take a lock inside the fast path
    // that exists to avoid one, so fail the build rather than get slower
    // in silence.
    static_assert(std::atomic<std::thread::id>::is_always_lock_free,
                  "SymbolLock needs a lock-free atomic thread id");

    // The alignment sits on the member rather than on the class, so a
    // `SymbolLock` still occupies a whole cache line and the source-
    // scanning codegen does not read `alignas` as a member name.
    alignas(64) std::atomic<bool> _held{false};
    // A default-constructed id is "no thread", which is the free state.
    std::atomic<std::thread::id> _owner{};
    uint32_t _depth{0};
  };

  static constexpr size_t kLockSlots = SymbolStateMap<SymbolContext>::kMaxSymbols;
  using LockTable = std::array<SymbolLock, kLockSlots>;

  SymbolLock& lockFor(SymbolId sym) const noexcept
  {
    return sym < kLockSlots ? (*_ctxLocks)[sym] : _overflowLock;
  }

  // On the heap for the same reason the context table is: a strategy has no
  // business carrying a padded lock per symbol in its own footprint.
  std::unique_ptr<LockTable> _ctxLocks{std::make_unique<LockTable>()};
  mutable SymbolLock _overflowLock;

  std::atomic<OrderId> _nextOrderId{1};

  SubscriberId _id;
  ISignalHandler* _signalHandler{nullptr};
  OrderTracker* _orderTracker{nullptr};
  IPositionManager* _positionManager{nullptr};
  std::vector<SymbolId> _symbols;
  std::set<SymbolId> _symbolSet;
  // 256 SymbolContext slots, each carrying a full 512-level book. The table
  // lives in one heap block owned by the map (see symbol_state_map.h), so a
  // strategy object is small enough to sit on a stack again: it used to be
  // roughly 2 MB in a release build and 4 MB with LRVX_SCALE_CHECKS on, and
  // two of them in one frame overran a default 8 MB stack.
  mutable SymbolStateMap<SymbolContext> _contexts;

  // Per-(symbol, timeframe) ring of the most recent closed bars.
  // Capacity is the same for every (symbol, tf) slot; tune with
  // `setBarRingCapacity` when a strategy needs deeper history.
  std::atomic<size_t> _barRingCapacity{64};
  mutable std::mutex _barRingMutex;
  std::unordered_map<SymbolId,
                     std::unordered_map<BarTfKey, std::deque<Bar>, BarTfKeyHash>>
      _barRings;
};

}  // namespace lrvx
