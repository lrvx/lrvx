/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/backtest/abstract_clock.h"
#include "lrvx/backtest/backtest_config.h"
#include "lrvx/backtest/bracket_order.h"
#include "lrvx/backtest/latency_distribution.h"
#include "lrvx/backtest/latency_model.h"
#include "lrvx/backtest/order_queue_tracker.h"
#include "lrvx/backtest/rate_limit_policy.h"
#include "lrvx/backtest/venue_availability.h"
#include "lrvx/book/book_update.h"
#include "lrvx/clearing/fee_schedule.h"
#include "lrvx/engine/symbol_registry.h"
#include "lrvx/execution/abstract_executor.h"
#include "lrvx/execution/composite_order_logic.h"
#include "lrvx/execution/events/order_event.h"

#include <array>
#include <functional>
#include <memory>
#include <random>
#include <unordered_map>
#include <vector>

namespace lrvx
{

struct Fill
{
  OrderId orderId{};
  SymbolId symbol{};
  Side side{};
  Price price{};
  Quantity quantity{};
  UnixNanos timestampNs{};
  // Whether this fill provided liquidity. The executor already knows -- it
  // decides maker vs taker to pick the fill price, and reports it on
  // OrderEvent -- but Fill used to drop it, which left every consumer of the
  // fill stream unable to select a tier from a FeeSchedule and forced flat
  // fee rates on results built from fills.
  bool isMaker{false};
};

struct TrailingState
{
  Price activationPrice{};  // price when trailing stop was activated
  Price currentTrigger{};   // current trigger price (moves with price)
};

class SimulatedExecutor : public IOrderExecutor
{
 public:
  static constexpr size_t kMaxSymbols = 256;
  static constexpr size_t kDefaultOrderCapacity = 64;
  static constexpr size_t kDefaultFillCapacity = 4096;

  using OrderEventCallback = std::function<void(const OrderEvent&)>;

  explicit SimulatedExecutor(IClock& clock);

  void setOrderEventCallback(OrderEventCallback cb);

  // Where the executor reads each symbol's tick. An order whose limit price,
  // trigger price or trailing distance is off a known tick is rejected at
  // submit (OrderEventStatus::REJECTED) and on replace (REPLACE_REJECTED),
  // with the reason from execution/order_price_rule.h. Fills moved by a
  // slippage model and percentage trailing triggers are snapped onto the tick
  // against the order's owner. Without a registry nothing is checked. The
  // registry must outlive the executor or be detached with nullptr.
  //
  // The tick and lot are cached per symbol and follow the registry: a value
  // a connector applies later (SymbolRegistry::applyVenueTick /
  // applyVenueLot) is read on the next order, at the cost of one atomic load
  // per check while nothing changes.
  void setSymbolRegistry(const SymbolRegistry* registry);
  // For a registry that is a copy of the entries another registry held (the
  // one BacktestRunner builds from Strategy::symbolInfos()): the venue tick
  // log of the registry the copy was taken from. A tick or lot that log holds
  // is used in place of the copied entry's, so values applied to the source
  // after the copy was taken are still followed. Call after
  // setSymbolRegistry, which resets it to the registry's own log.
  void setVenueTickLog(std::shared_ptr<const VenueTickLog> log);
  // The tick orders on `symbol` are checked against, as a raw price; 0 when
  // none is known.
  int64_t orderTickRaw(SymbolId symbol) const;
  // The lot order quantities on `symbol` are checked against, as a raw
  // quantity; 0 when the symbol declares none. An order whose quantity (or
  // iceberg visible quantity) is off a declared lot is rejected at submit
  // and on replace like an off-tick price, with reason quantity_off_lot.
  int64_t orderLotRaw(SymbolId symbol) const;

  // Apply slippage and queue-simulation settings from a BacktestConfig. May be
  // called after construction; existing orders keep their prior behavior.
  void applyConfig(const BacktestConfig& config);

  // Convenience setters for bindings without a full BacktestConfig.
  void setDefaultSlippage(const SlippageProfile& profile);
  void setSymbolSlippage(SymbolId symbol, const SlippageProfile& profile);
  void setQueueModel(QueueModel model, size_t depth);
  // For QueueModel::PRO_RATA_WITH_FIFO: the first N orders at a
  // level consume the trade FIFO; the remainder is split pro-rata
  // across the rest. Ignored in other queue models.
  void setQueueFifoTopN(size_t topN);
  // TOP_PRO_LMM: fraction of each incoming trade reserved for the
  // queue-front order. Default 0.40. Ignored in other queue models.
  void setTopPriorityShare(double share);
  // TOP_PRO_LMM: identify these orders as Lead Market Makers. LMM
  // orders get an implicit bonus multiplier during the pro-rata
  // remainder distribution.
  void setLmmOrders(const std::vector<OrderId>& ids);
  void setLmmBonusMultiplier(double multiplier);
  // TOP_PRO_LMM / PRO_RATA_WITH_PRIORITY: per-order priority weight.
  // Defaults to 1.0. Effective allocation weight in pro-rata
  // distribution = remaining × priorityMultiplier.
  void setOrderPriorityMultiplier(OrderId id, double multiplier);
  void setQueuePositionMinChangeFraction(double fraction);
  void setSubmitAckLatency(int64_t latencyNs, int64_t jitterNs);
  void setCancelAckLatency(int64_t latencyNs, int64_t jitterNs);
  void setReplaceAckLatency(int64_t latencyNs, int64_t jitterNs);

  // Distribution-based variants. The scalar setters above delegate
  // to these: zero jitter → Constant, non-zero → Uniform with the
  // legacy [base-jitter, base+jitter] window.
  void setSubmitAckLatencyDistribution(const LatencyDistribution& dist);
  void setCancelAckLatencyDistribution(const LatencyDistribution& dist);
  void setReplaceAckLatencyDistribution(const LatencyDistribution& dist);

  // Returns false if `name` is not a known profile, so callers can
  // surface a typo instead of silently running default latencies.
  bool applyLatencyProfile(const char* name);

  // Wire latency model (BacktestConfig::latency). Its orderDelay() is drawn
  // once per submitted order and holds that order out of matching until the
  // delay has elapsed on the simulated clock; the fill is then stamped at
  // the time it matched. Adds to the venue's submit-ack latency rather than
  // replacing it. Passing nullptr removes the model.
  void setLatencyModel(std::shared_ptr<LatencyModel> model) noexcept
  {
    _latency = std::move(model);
  }
  const std::shared_ptr<LatencyModel>& latencyModel() const noexcept { return _latency; }

  // Self-trade prevention. When set, submitOrder consults pending
  // resting orders and applies the configured mode if the incoming
  // order would cross one of the same account's resting orders.
  // Multi-account workflows: STP keys on (Order::accountId, optional
  // STP group). Two orders share an STP scope when either their
  // accountIds match, or their accounts belong to the same explicit
  // STP group (configure via setSTPGroupMembership).
  void setSTPMode(STPMode mode) noexcept { _stpMode = mode; }
  STPMode stpMode() const noexcept { return _stpMode; }

  // Opt an account into an STP group. Group id 0 removes any prior
  // mapping. Two orders belong to the same STP scope when either
  // their accountIds match (and are equal), or both accounts map to
  // the same non-zero group.
  void setSTPGroupMembership(uint64_t accountId, uint64_t groupId);
  uint64_t stpGroupFor(uint64_t accountId) const;
  // True when the simulator should treat two orders as candidates for
  // STP (same direct account, or both in the same non-zero group).
  bool sameStpScope(uint64_t a, uint64_t b) const;

  // FOK fill semantics. Real venues split:
  //   AnyPrice    — fill if cumulative liquidity along the book (at
  //                 prices crossing the order's limit) >= order qty.
  //                 Walks through multiple price levels.
  //   SinglePrice — fill if the level at the order's limit price
  //                 holds >= order qty in one trade. Reject otherwise.
  // Default is AnyPrice (matches crypto venues). CME / Eurex / most
  // US equities use SinglePrice. The simulator currently consults
  // top-of-book qty only; deeper book walks are a follow-up.
  enum class FokMode : uint8_t
  {
    AnyPrice = 0,
    SinglePrice = 1,
  };
  void setFokMode(FokMode mode) noexcept { _fokMode = mode; }
  FokMode fokMode() const noexcept { return _fokMode; }
  // String form for codegen / bindings. Accepts "any_price" or
  // "single_price" (case-insensitive). Unknown values are ignored.
  void setFokModeByName(const std::string& name);

  // Attach a venue-availability model. Submit / cancel / replace
  // issued while the venue is down are buffered and flushed at the
  // recovery edge in FIFO order. Market-data callbacks (onTrade /
  // onBookUpdate / onBar) are silently dropped during an outage so
  // the strategy sees a feed gap. Passing nullptr disables outages.
  void setVenueAvailability(VenueAvailability* availability) { _venue = availability; }
  VenueAvailability* venueAvailability() noexcept { return _venue; }

  // Attach the venue's fee ladder. The executor does not charge fees itself
  // -- it is the fill stream's owner, so this is where a result built from
  // that stream finds the schedule that prices it. BacktestRunner::result
  // hands it to BacktestResult, which then bills each fill at the tier the
  // account's 30-day notional resolves to instead of the flat
  // BacktestConfig::feeRate. Passing nullptr reverts to the flat rate.
  void setFeeSchedule(FeeSchedule* fees) noexcept { _fees = fees; }
  const FeeSchedule* feeSchedule() const noexcept { return _fees; }

  // Attach a rate-limit policy. Submit / cancel / replace consult the
  // policy first; an overflow emits OrderEventStatus::REJECTED_RATE_LIMIT
  // and the action is not committed to the simulator. Passing an
  // empty policy disables enforcement.
  void setRateLimitPolicy(const RateLimitPolicy& policy);
  void clearRateLimitPolicy();
  bool hasRateLimitPolicy() const noexcept { return _hasRateLimit; }
  RateLimitPolicy& rateLimitPolicy() { return _rateLimit; }
  const RateLimitPolicy& rateLimitPolicy() const { return _rateLimit; }

  void start() override {}
  void stop() override {}

  void submitOrder(const Order& order) override;
  void cancelOrder(OrderId orderId) override;
  // Without a cancel ack latency every order of the symbol is CANCELED before
  // this returns. With one it is the same request as cancelOrder() on each of
  // them: held orders cancel at once, the rest go PENDING_CANCEL and stay live
  // until their own sampled ack. Like today, cancel-all spends no rate-limit
  // token per order.
  void cancelAllOrders(SymbolId symbol) override;
  void replaceOrder(OrderId oldOrderId, const Order& newOrder) override;

  // OCO: one-cancels-other
  void submitOCO(const OCOParams& params) override;

  // Native bracket primitive: a single call that submits the entry
  // leg and arms a take-profit + stop pair on entry fill. The
  // simulator manages the state machine: the first child to fill
  // cancels the other; cancelling the bracket cancels every still-
  // live leg. Returns the bracketId for later lookup / cancel.
  // The bracket assumes order_ids for the entry / TP / stop are
  // chosen by the caller: entry_id = bracketId * 3 + 0,
  // tp_id = bracketId * 3 + 1, stop_id = bracketId * 3 + 2.
  void submitBracket(const BracketOrder& bracket);
  void cancelBracket(uint64_t bracketId);
  BracketStatus bracketStatus(uint64_t bracketId) const;

  ExchangeCapabilities capabilities() const override { return ExchangeCapabilities::simulated(); }

  void onBookUpdate(SymbolId symbol, const std::pmr::vector<BookLevel>& bids,
                    const std::pmr::vector<BookLevel>& asks);
  // Touch-only frame (BookUpdateType::TOP_OF_BOOK): the venue's complete
  // word on the touch and nothing deeper, read with the same contract as
  // CompositeBookMatrix, so an empty side means an empty side. Levels more
  // aggressive than the touch are gone, deeper ones stay as the last depth
  // frame reported them; then the same matching pass as onBookUpdate.
  void onTopOfBook(SymbolId symbol, const std::pmr::vector<BookLevel>& bids,
                   const std::pmr::vector<BookLevel>& asks);
  void onTrade(SymbolId symbol, Price price, bool isBuy);
  void onTrade(SymbolId symbol, Price price, Quantity qty, bool isBuy);
  // Close-only: sets bid=ask=last=close and matches at close. Intrabar SL/TP
  // that would have triggered against the wick are missed. Prefer the
  // high/low/close overload for realistic stop/target fills.
  void onBar(SymbolId symbol, Price close);

  // Walks the bar low -> high -> close so resting stops and take-profits match
  // against the intrabar extremes, not just the close. low-then-high is the
  // pessimistic order for a long (its protective stop is tested before its
  // target). Bars must be in non-decreasing time order.
  void onBar(SymbolId symbol, Price high, Price low, Price close);

  // Open-aware form. The open is the first price of the bar, so it is also the
  // first price an order held back from the previous bar's callback is allowed
  // to trade at: this overload moves the market to the open, releases the held
  // orders of `symbol` there, and only then walks low -> high -> close.
  // Bar-driven runs should prefer it -- the high/low/close form carries no
  // open, so it leaves held orders waiting for the next call that does.
  void onBar(SymbolId symbol, Price open, Price high, Price low, Price close);

  // Bar-callback window. A bar reaches the strategy only after the simulator
  // has walked it, so open, high, low and close are all in the market state by
  // the time the callback runs. Matching an order submitted from there is
  // look-ahead: it prints at a price that exists only because the bar has
  // already happened. While the window is open every arriving order is held
  // instead of submitted, and the next bar's open releases it. The depth is
  // counted so a re-entrant callback cannot close a window it did not open.
  void beginBarCallbackWindow() noexcept { ++_barCallbackDepth; }
  void endBarCallbackWindow() noexcept
  {
    if (_barCallbackDepth > 0)
    {
      --_barCallbackDepth;
    }
  }
  bool barCallbackWindowOpen() const noexcept { return _barCallbackDepth > 0; }

  // RAII pairing for the window. Hold one for the span of the callback rather
  // than calling begin/end by hand: an early return or a callback that throws
  // would otherwise leave the counter up, and a window that never closes holds
  // every order submitted after it for the rest of the run -- silently, since
  // a held order reports nothing. The raw calls stay public for drivers that
  // cannot wrap the callback in a scope.
  class BarCallbackScope
  {
   public:
    explicit BarCallbackScope(SimulatedExecutor& exec) noexcept : _exec(exec)
    {
      _exec.beginBarCallbackWindow();
    }
    ~BarCallbackScope() { _exec.endBarCallbackWindow(); }

    BarCallbackScope(const BarCallbackScope&) = delete;
    BarCallbackScope& operator=(const BarCallbackScope&) = delete;
    BarCallbackScope(BarCallbackScope&&) = delete;
    BarCallbackScope& operator=(BarCallbackScope&&) = delete;

   private:
    SimulatedExecutor& _exec;
  };
  // Next-event window, the tick-matched counterpart of the bar-callback
  // window. A driver that shows the strategy a bar built from trades and then
  // keeps feeding the executor those trades (BacktestRunner::runTapeBarsOnTicks)
  // opens it around the bar callback. An order submitted while it is open goes
  // through the full submit path at the current clock -- SUBMITTED, rate
  // limits, reduce-only, the sampled wire and ack latency -- but is never
  // matched inside the call: it waits in the pending-submission queue for the
  // first market event (onTrade / onBookUpdate / onTopOfBook) at or after
  // now + latency, and is matched against the state that event leaves. With
  // zero latency that is the very next event, so the market state the bar was
  // built from can never fill it. Counted like the bar-callback window.
  void beginNextEventWindow() noexcept { ++_nextEventDepth; }
  void endNextEventWindow() noexcept
  {
    if (_nextEventDepth > 0)
    {
      --_nextEventDepth;
    }
  }
  bool nextEventWindowOpen() const noexcept { return _nextEventDepth > 0; }

  class NextEventScope
  {
   public:
    explicit NextEventScope(SimulatedExecutor& exec) noexcept : _exec(exec)
    {
      _exec.beginNextEventWindow();
    }
    ~NextEventScope() { _exec.endNextEventWindow(); }

    NextEventScope(const NextEventScope&) = delete;
    NextEventScope& operator=(const NextEventScope&) = delete;
    NextEventScope(NextEventScope&&) = delete;
    NextEventScope& operator=(NextEventScope&&) = delete;

   private:
    SimulatedExecutor& _exec;
  };

  // Orders still held, over every symbol. Orders whose symbol never gets
  // another bar stay held to the end of the run: they never reached the venue,
  // so they neither fill nor cancel.
  size_t heldOrderCount() const noexcept { return _heldBarOrders.size(); }
  // Submit everything held for `symbol` into the current market state. Called
  // from the open step of that symbol's next bar; a no-op while a window is
  // open.
  void releaseHeldOrders(SymbolId symbol);

  // Drop every trace of a finished run -- fills, live, held and conditional
  // orders, queue positions, net positions, brackets, in-flight acks, the
  // market state and the visible ladder -- while keeping the configuration the
  // caller installed: slippage, queue model, latency distributions, rate
  // limits, STP, callbacks and the attached venue-availability model. Seeded
  // RNGs go back to their configured seeds, so a repeated run repeats. A
  // runner calls this before each run, which is what makes a second run report
  // that run instead of the sum of every run so far.
  void reset();

  const std::vector<Fill>& fills() const { return _fills; }
  std::vector<Fill> extractFills() { return std::move(_fills); }
  const std::vector<Order>& conditionalOrders() const { return _conditional_orders; }
  // The order as the executor holds it right now -- held for the next bar's
  // open, resting, or waiting on its trigger. nullptr when no such order is
  // live. The pointer is good until the next call that changes the order set.
  const Order* findOrder(OrderId orderId) const;

  // Top-of-book accessors. Return 0 when the side is empty.
  Price bestBidPrice(SymbolId symbol) const;
  Price bestAskPrice(SymbolId symbol) const;
  // Mid of best bid and best ask. Returns 0 when either side is empty.
  Price bookMidPrice(SymbolId symbol) const;

  CompositeOrderLogic& compositeLogic() { return _compositeLogic; }

 private:
  struct MarketState
  {
    int64_t bestBidRaw{0};
    int64_t bestAskRaw{0};
    int64_t lastTradeRaw{0};
    int64_t bestBidQtyRaw{0};
    int64_t bestAskQtyRaw{0};
    bool hasBid{false};
    bool hasAsk{false};
    bool hasTrade{false};
  };

  // The visible ladder as of the last book update, most aggressive level
  // first. A taker consumes it, so its size decides the price the taker pays
  // and the next order in the same step sees what the previous one left
  // behind. onBookUpdate replaces it wholesale and onTopOfBook reconciles
  // its front with the touch; a bar or a trade-only feed reports no depth,
  // and those paths keep the single-price behaviour.
  struct DepthLadder
  {
    std::vector<std::pair<int64_t, int64_t>> bids;  // (priceRaw, qtyRaw)
    std::vector<std::pair<int64_t, int64_t>> asks;
  };

  struct LadderWalk
  {
    int64_t takenRaw{0};       // size the ladder could actually supply
    int64_t worstPriceRaw{0};  // deepest level the walk reached
    Volume notional{};         // sum(price * qty) over the levels consumed
    bool walked{false};        // false when the symbol has no visible ladder
    size_t levels{0};          // price levels the walk took size from
  };

  MarketState& getMarketState(SymbolId symbol);
  DepthLadder* findLadder(SymbolId symbol);
  // Takes up to `qtyRaw` off `side` of the ladder, stopping at `limitPriceRaw`
  // (0 = no price bound), removes what it took and republishes the touch that
  // is left. Returns what the walk found.
  LadderWalk consumeLadder(SymbolId symbol, Side side, int64_t qtyRaw,
                           int64_t limitPriceRaw);
  const SlippageProfile& slippageFor(SymbolId symbol) const;
  int64_t applySlippage(int64_t priceRaw, Side side, SymbolId symbol,
                        Quantity qty, int64_t levelQtyRaw) const;

  // `resting` says the order is already sitting in the book rather than
  // arriving now. A resting limit order trades at the price it posted and
  // provides the liquidity; an arriving one crosses and pays the touch.
  // `triggerBoundRaw` (0 = none) is the price that armed a conditional order:
  // its fill may be worse than that price but never better, whatever the
  // market state the trigger happened to be evaluated against says.
  bool tryFillOrder(Order& order, bool resting, int64_t triggerBoundRaw = 0);
  void processPendingOrders(SymbolId symbol, const MarketState& state);
  void processConditionalOrders(SymbolId symbol, const MarketState& state);
  void updateTrailingStops(SymbolId symbol, Price currentPrice);
  bool checkStopTrigger(const Order& order, const MarketState& state) const;
  bool checkTakeProfitTrigger(const Order& order, const MarketState& state) const;
  bool checkTrailingStopTrigger(const Order& order, const TrailingState& trailing,
                                const MarketState& state) const;
  void triggerConditionalOrder(Order& order);
  bool isConditionalOrder(OrderType type) const;
  void executeFill(Order& order, Price price, Quantity qty, bool isMaker = false);
  void emitEvent(OrderEventStatus status, const Order& order);
  void emitRejected(const Order& order, const char* reason);
  // A refused replace: `order` is the order it named, left as it was.
  void emitReplaceRejected(const Order& order, const Order& newOrder, const char* reason);
  void emitTrailingUpdate(const Order& order, Price newTrigger);

  // Emit QUEUE_POSITION_UPDATED for every resting order whose
  // `queueAhead` has shifted by at least the configured fractional
  // threshold since the last emission. Call after any operation
  // that may shift queue positions (onTrade, onBookUpdate, addOrder).
  void maybeEmitQueuePositionChanges();
  void forgetQueuePosition(OrderId orderId);

  // Native iceberg refresh. If `order` has an iceberg state, the
  // visible tranche is fully filled, the refresh deadline has
  // elapsed, and there is hidden remainder, expose another visible
  // tranche by increasing `order.quantity` by the lesser of the
  // hidden remainder and the configured visible slice. Re-adds the
  // refreshed slice to the queue tracker. No-op for non-iceberg
  // orders or when the refresh isn't due yet.
  void maybeRefreshIceberg(Order& order);

  // Asynchronous cancel ack support. enqueuePendingCancel records the
  // ack deadline; finalizePendingCancels walks the queue and CANCELs
  // any whose deadline has passed. resolveLateCancelOnFill drops a
  // pending cancel when its order has just filled and emits the
  // late-cancel-after-fill REJECTED notification.
  void enqueuePendingCancel(const Order& order);
  void cancelAllOrdersAsync(SymbolId symbol);
  void finalizePendingCancels();
  void resolveLateCancelOnFill(const Order& order);
  int64_t sampleCancelAckLatency();

  // Market position tracking. Recomputes MarketPosition for every
  // resting limit order after a book or trade event and emits
  // MARKET_POSITION_CHANGED on categorical transitions.
  void maybeEmitMarketPositionChanges();
  MarketPosition computeMarketPosition(const Order& order,
                                       const MarketState& state,
                                       Quantity ourRemaining,
                                       Quantity queueTotal) const;
  int32_t computeDistanceToBestTicks(const Order& order,
                                     const MarketState& state) const;
  void forgetMarketPosition(OrderId orderId);

  // Replace-in-flight ack: enqueue pending replace, finalize when
  // ack deadline passes, reject the replace when the original
  // order fills first.
  void enqueuePendingReplace(const Order& oldOrder, const Order& newOrder, bool untriggered);
  void finalizePendingReplaces();
  void resolveLateReplaceOnFill(const Order& order);
  int64_t sampleReplaceAckLatency();
  void forgetPendingReplace(OrderId orderId);

  // Submit ack: when the order's total deferral (wire latency plus
  // cfg.submitAckLatencyNs) is positive, SUBMITTED fires immediately and
  // ACCEPTED defers by `delayNs`. The order is held aside until ACCEPTED,
  // then runs through the existing book-add / queue-tracker / try-fill path.
  void enqueuePendingSubmission(const Order& order, int64_t delayNs);
  void finalizePendingSubmissions();
  int64_t sampleSubmitAckLatency();
  // Order-side wire delay from the attached LatencyModel; 0 when none is
  // attached. Negative draws are clamped -- an order cannot arrive before it
  // was sent.
  int64_t sampleOrderLatency();
  void finishSubmission(Order accepted, bool fromAck);

  Order* findPendingOrder(OrderId orderId);
  // The order while it still waits on its trigger, or nullptr.
  Order* findConditionalOrder(OrderId orderId);
  // Rewrites an untriggered conditional order in place. A trailing stop's
  // trigger lives in its trailing state, so a replacement that names a
  // trigger moves that.
  void replaceConditionalOrder(Order& conditional, const Order& newOrder);
  void removePendingOrder(OrderId orderId);
  void drainQueueFills(SymbolId symbol);

  // Ids collected before a fill sweep. Fill callbacks re-enter the executor
  // and reshape _pending_orders, so the sweep resolves each order by id
  // instead of holding an index or a reference across a callback.
  std::vector<OrderId> _fillScanIds;

  // GTD: expire any pending order whose expiresAfter has passed. Called
  // alongside the ack finalizers from onBookUpdate / onTrade so the
  // expiry fires deterministically at the next event boundary.
  void processExpiredOrders();
  // Moves bid = ask = last to `price` without running a matching pass. The bar
  // open needs the state in place before the held orders are released, so that
  // they are matched at the open and not at the previous bar's close.
  void setBarMarketState(SymbolId symbol, Price price);
  void stepBarPrice(SymbolId symbol, Price price);
  // Bar data carries no trade stream, so the queue tracker would never hear a
  // print and every resting limit would sit forever. A bar step that trades
  // strictly through a resting order's price proves the queue ahead of it
  // traded, so the step is handed to the tracker as a synthetic print at that
  // level, sized to reach the order standing furthest back. How much of it
  // each order gets is then the queue model's decision, not a hardcoded fill.
  void driveQueueFromBarStep(SymbolId symbol, Price stepPrice);
  // Iceberg refresh, level compaction and removal of fully-filled queued
  // orders. Shared by every path that lets the queue tracker fill orders.
  void settleQueuedFills();
  // Shared prologue of every onBar overload: runs the outage state machine and
  // says whether the venue is up enough to take the bar at all.
  bool barFeedAllowed();
  // Same for the book-frame paths (onBookUpdate / onTopOfBook), gating on
  // bookUpdatesAllowed instead.
  bool bookFeedAllowed();

  // reduce_only enforcement: simulator-side net position per symbol,
  // updated in executeFill. A reduce-only submit that would open or
  // grow the position is rejected; one that overshoots is truncated.
  std::unordered_map<SymbolId, int64_t> _netPositionRaw;
  int64_t netPositionRaw(SymbolId symbol) const;

  IClock& _clock;
  OrderEventCallback _callback;

  std::vector<Order> _pending_orders;
  std::vector<Order> _conditional_orders;
  std::vector<Fill> _fills;

  // Trailing stop state tracking (indexed by order id)
  std::vector<std::pair<OrderId, TrailingState>> _trailing_states;

  // OCO order logic
  CompositeOrderLogic _compositeLogic{0};

  std::array<MarketState, kMaxSymbols> _marketStatesFlat{};
  std::vector<std::pair<SymbolId, MarketState>> _marketStatesOverflow;

  std::unordered_map<SymbolId, DepthLadder> _ladders;

  // Orders submitted while a bar-callback window was open, in arrival order,
  // waiting for their symbol's next bar open.
  std::vector<Order> _heldBarOrders;
  size_t _barCallbackDepth{0};
  size_t _nextEventDepth{0};

  // Slippage config: default + per-symbol overrides
  std::array<SlippageProfile, kMaxSymbols> _slippageFlat{};
  std::array<bool, kMaxSymbols> _slippageSetFlat{};
  std::vector<std::pair<SymbolId, SlippageProfile>> _slippageOverflow;
  SlippageProfile _defaultSlippage{};

  // Per-symbol order tick, read from the registry on first use. -1 = not read.
  const SymbolRegistry* _registry{nullptr};
  mutable std::array<int64_t, kMaxSymbols> _orderTickFlat{};
  // Per-symbol order lot, read the same way.
  mutable std::array<int64_t, kMaxSymbols> _orderLotFlat{};
  // The log whose revision the two caches follow, and the revision they were
  // filled at. _venueLogOverlay: the log belongs to another registry than
  // _registry and its values replace the entry's (setVenueTickLog).
  std::shared_ptr<const VenueTickLog> _venueTicks;
  mutable uint64_t _venueLogSeen{0};
  bool _venueLogOverlay{false};
  void followVenueLog() const;

  // Seeds as configured, kept so reset() can put the generators back where a
  // fresh executor would have them.
  uint64_t _cancelAckSeed{42};
  uint64_t _icebergJitterSeed{0xC0FFEEC0FFEEULL};

  OrderQueueTracker _queueTracker;
  QueueModel _queueModel{};
  std::vector<std::pair<OrderId, Quantity>> _queueFillBuffer;  // reused scratch
  std::vector<QueueSnapshot> _queueSnapshotBuffer;             // reused scratch
  std::unordered_map<OrderId, int64_t> _lastEmittedQueueAheadRaw;
  double _queuePosMinFraction{0.05};

  // Per-order lifecycle-stage timestamps. Populated as statuses fire
  // (SUBMITTED, ACCEPTED, fills, CANCELED, REJECTED, TRIGGERED,
  // EXPIRED). Each emitted OrderEvent receives the current snapshot.
  std::unordered_map<OrderId, OrderTimestamps> _orderTimestamps;

  OrderTimestamps& timestampsFor(OrderId id);
  void forgetTimestamps(OrderId id);

  struct PendingCancel
  {
    OrderId orderId;
    int64_t ackAtNs;
    Order orderSnapshot;  // copy at the moment of cancelOrder()
  };
  std::vector<PendingCancel> _pendingCancels;
  LatencyDistribution _cancelAckDist;
  std::mt19937_64 _cancelAckRng{42};

  // Last emitted MarketPosition per resting order. Used to debounce
  // MARKET_POSITION_CHANGED emission so it fires only on categorical
  // transitions, not on every tick.
  std::unordered_map<OrderId, MarketPosition> _lastEmittedMarketPosition;

  struct PendingReplace
  {
    OrderId orderId;
    int64_t ackAtNs;
    Order oldSnapshot;
    Order newOrder;
    // The order was still waiting on its trigger when the replace was sent.
    bool untriggered{false};
  };
  std::vector<PendingReplace> _pendingReplaces;
  LatencyDistribution _replaceAckDist;

  struct PendingSubmission
  {
    int64_t ackAtNs;
    Order order;
  };
  std::vector<PendingSubmission> _pendingSubmissions;
  LatencyDistribution _submitAckDist;

  RateLimitPolicy _rateLimit;
  // The policy exactly as setRateLimitPolicy() installed it. reset() restores
  // it: a policy's consumed-token log is timestamped, and a clock rewound to
  // the start of the next run never expires entries the previous run wrote.
  // Tuning applied through the mutable rateLimitPolicy() accessor after
  // installation is not part of the snapshot and has to be reapplied.
  RateLimitPolicy _rateLimitAsInstalled;
  bool _hasRateLimit{false};

  STPMode _stpMode{STPMode::None};
  std::unordered_map<uint64_t, uint64_t> _stpGroupMembership;
  FokMode _fokMode{FokMode::AnyPrice};

  VenueAvailability* _venue{nullptr};
  // Non-owning; the VenueStack (or the caller) outlives the executor.
  FeeSchedule* _fees{nullptr};
  std::shared_ptr<LatencyModel> _latency{};
  enum class BufferedAction : uint8_t
  {
    SUBMIT = 0,
    CANCEL = 1,
    REPLACE = 2,
    CANCEL_ALL_SYMBOL = 3,
  };
  struct BufferedRequest
  {
    BufferedAction action{BufferedAction::SUBMIT};
    Order order{};
    OrderId oldOrderId{0};
    SymbolId cancelAllSymbol{0};
  };
  std::vector<BufferedRequest> _outageBuffer;
  void applyOutagePolicy();
  void flushOutageBuffer();
  bool _venueWasUp{true};

  // Native bracket state. Keyed by user-provided bracketId.
  std::unordered_map<uint64_t, BracketStatus> _brackets;
  std::unordered_map<OrderId, uint64_t> _legToBracket;
  std::unordered_map<uint64_t, BracketOrder> _bracketTemplates;
  void onBracketFillEvent(const Order& filledOrder);

  // Native iceberg state. Keyed by parent OrderId. `hiddenRaw` is
  // the remaining hidden quantity that has not yet been exposed to
  // the book. `visibleRaw` is the slice size used on each refresh.
  // `refreshLatencyNs` is the configured delay between a visible
  // tranche filling and the next one being exposed (0 = instant).
  struct IcebergState
  {
    int64_t hiddenRaw{0};
    int64_t visibleRaw{0};
    int64_t refreshLatencyNs{0};
    int64_t refreshDueNs{0};  // when the next refresh becomes eligible
  };
  std::unordered_map<OrderId, IcebergState> _iceberg;
  int64_t _icebergRefreshLatencyNs{0};

 public:
  // Queue priority on refresh. "back" (default) sends
  // the refreshed slice to the back of the queue. "retain" keeps the
  // refresh in the same queue position as the prior slice (CME-style).
  enum class IcebergPriorityMode : uint8_t
  {
    Back = 0,
    Retain = 1,
  };

 private:
  // Per-refresh size jitter as a fraction (0.0 disables;
  // 0.10 = ±10% uniform). Default 0 keeps refresh slicing deterministic.
  double _icebergSizeRandomisationPct{0.0};
  IcebergPriorityMode _icebergPriorityMode{IcebergPriorityMode::Back};
  std::mt19937_64 _icebergJitterRng{0xC0FFEEC0FFEEULL};

 public:
  // Child-arm policy. "on_full_fill" (default) arms TP+stop
  // once at the moment of full entry fill. "on_partial_fill" arms
  // children at the running entry-fill quantity on every partial,
  // resizing via replace as more entry quantity fills.
  enum class BracketArmMode : uint8_t
  {
    OnFullFill = 0,
    OnPartialFill = 1,
  };
  void setBracketChildArmMode(BracketArmMode mode) noexcept { _bracketArmMode = mode; }
  BracketArmMode bracketChildArmMode() const noexcept { return _bracketArmMode; }

  // Configure the default native-iceberg refresh latency (the delay
  // between a visible tranche fully filling and the next one being
  // exposed to the book). Applies to orders of type
  // OrderType::ICEBERG submitted after this call.
  void setIcebergRefreshLatency(int64_t latencyNs) noexcept
  {
    _icebergRefreshLatencyNs = latencyNs;
  }
  int64_t icebergRefreshLatencyNs() const noexcept { return _icebergRefreshLatencyNs; }

  // Per-refresh visible-slice size jitter as a fraction
  // (0.0 = deterministic; 0.10 = ±10% uniform).
  // The jitter is sampled from an internal RNG seeded deterministically;
  // call setIcebergJitterSeed to reproduce a specific draw sequence.
  void setIcebergSizeRandomisationPct(double pct) noexcept
  {
    _icebergSizeRandomisationPct = pct;
  }
  double icebergSizeRandomisationPct() const noexcept
  {
    return _icebergSizeRandomisationPct;
  }
  void setIcebergJitterSeed(uint64_t seed) noexcept
  {
    _icebergJitterSeed = seed;
    _icebergJitterRng.seed(seed);
  }
  // Queue priority on refresh.
  //   Back    — refreshed slice goes to the back of the queue (the
  //             default; matches most crypto venues).
  //   Retain  — refreshed slice keeps the same queue position as the
  //             prior slice (CME options + some Eurex contracts).
  void setIcebergPriorityMode(IcebergPriorityMode mode) noexcept
  {
    _icebergPriorityMode = mode;
  }
  IcebergPriorityMode icebergPriorityMode() const noexcept
  {
    return _icebergPriorityMode;
  }
  // Builds a lowercased copy of the name, which allocates for anything past
  // the small-string buffer.
  void setIcebergPriorityModeByName(const std::string& name);
  // Diagnostic: remaining hidden quantity for an iceberg order, or 0
  // if none.
  int64_t icebergHiddenRemainingRaw(OrderId id) const
  {
    auto it = _iceberg.find(id);
    return it == _iceberg.end() ? 0 : it->second.hiddenRaw;
  }

 private:
  BracketArmMode _bracketArmMode{BracketArmMode::OnFullFill};
};

}  // namespace lrvx
