/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/backtest/simulated_executor.h"
#include "lrvx/execution/order_price_rule.h"

#include "lrvx/backtest/latency_profiles.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace lrvx
{

// FIXED_TICKS uses SlippageProfile::tickSize (raw) as the size of one tick.
// If the caller left tickSize zero, a single raw price unit is used as a
// safe fallback.

namespace
{

// A conditional order that has fired fills like the order it became: the
// market-style trio walks the book, the limit-style pair rests at its
// price. It keeps its own type while it does so, because every event it
// emits has to name the order the caller submitted -- a take-profit that
// reported MARKET on its fill told the strategy it had placed something
// it never placed.
bool fillsAsMarket(OrderType type)
{
  return type == OrderType::MARKET || type == OrderType::STOP_MARKET ||
         type == OrderType::TAKE_PROFIT_MARKET || type == OrderType::TRAILING_STOP;
}

bool fillsAsLimit(OrderType type)
{
  return type == OrderType::LIMIT || type == OrderType::STOP_LIMIT ||
         type == OrderType::TAKE_PROFIT_LIMIT;
}

}  // namespace

SimulatedExecutor::SimulatedExecutor(IClock& clock) : _clock(clock)
{
  _pending_orders.reserve(kDefaultOrderCapacity);
  _conditional_orders.reserve(kDefaultOrderCapacity);
  _fills.reserve(kDefaultFillCapacity);
  _compositeLogic.setExecutor(this);
  _slippageSetFlat.fill(false);
  _orderTickFlat.fill(-1);
  _orderLotFlat.fill(-1);
}

void SimulatedExecutor::setSymbolRegistry(const SymbolRegistry* registry)
{
  _registry = registry;
  _venueTicks = registry != nullptr ? registry->venueTickLog() : nullptr;
  _venueLogOverlay = false;
  _venueLogSeen = _venueTicks ? _venueTicks->revision() : 0;
  _orderTickFlat.fill(-1);
  _orderLotFlat.fill(-1);
}

void SimulatedExecutor::setVenueTickLog(std::shared_ptr<const VenueTickLog> log)
{
  _venueTicks = std::move(log);
  _venueLogOverlay = _venueTicks != nullptr;
  _venueLogSeen = _venueTicks ? _venueTicks->revision() : 0;
  _orderTickFlat.fill(-1);
  _orderLotFlat.fill(-1);
}

// Drops both caches when a tick or lot was applied since they were filled.
// The revision is read before the entries the caches are refilled from, so a
// value applied in between moves it again and is read on the next call.
void SimulatedExecutor::followVenueLog() const
{
  if (!_venueTicks)
  {
    return;
  }
  const uint64_t revision = _venueTicks->revision();
  if (revision == _venueLogSeen)
  {
    return;
  }
  _venueLogSeen = revision;
  _orderTickFlat.fill(-1);
  _orderLotFlat.fill(-1);
}

int64_t SimulatedExecutor::orderTickRaw(SymbolId symbol) const
{
  if (_registry == nullptr)
  {
    return 0;
  }
  followVenueLog();
  if (symbol < kMaxSymbols && _orderTickFlat[symbol] >= 0)
  {
    return _orderTickFlat[symbol];
  }
  const auto info = _registry->getSymbolInfo(symbol);
  if (!info)
  {
    // Not registered yet: answer "unknown" without caching, so a symbol added
    // later is still read.
    return 0;
  }
  int64_t tick = lrvx::orderTickRaw(*info);
  if (_venueLogOverlay)
  {
    if (const auto applied = _venueTicks->tickFor(symbol); applied && applied->raw() > 0)
    {
      tick = applied->raw();
    }
  }
  if (symbol < kMaxSymbols)
  {
    _orderTickFlat[symbol] = tick;
  }
  return tick;
}

int64_t SimulatedExecutor::orderLotRaw(SymbolId symbol) const
{
  if (_registry == nullptr)
  {
    return 0;
  }
  followVenueLog();
  if (symbol < kMaxSymbols && _orderLotFlat[symbol] >= 0)
  {
    return _orderLotFlat[symbol];
  }
  const auto info = _registry->getSymbolInfo(symbol);
  if (!info)
  {
    return 0;
  }
  int64_t lot = lrvx::orderLotRaw(*info);
  if (_venueLogOverlay)
  {
    if (const auto applied = _venueTicks->lotFor(symbol); applied && applied->raw() > 0)
    {
      lot = applied->raw();
    }
  }
  if (symbol < kMaxSymbols)
  {
    _orderLotFlat[symbol] = lot;
  }
  return lot;
}

void SimulatedExecutor::emitRejected(const Order& order, const char* reason)
{
  OrderEvent ev;
  ev.status = OrderEventStatus::REJECTED;
  ev.order = order;
  ev.exchangeTsNs = _clock.nowNs();
  ev.timestamps = timestampsFor(order.id);
  ev.timestamps.rejectedAtNs = ev.exchangeTsNs;
  ev.rejectReason = reason;
  if (_callback)
  {
    _callback(ev);
  }
}

void SimulatedExecutor::emitReplaceRejected(const Order& order, const Order& newOrder,
                                            const char* reason)
{
  if (!_callback)
  {
    return;
  }
  OrderEvent ev;
  ev.status = OrderEventStatus::REPLACE_REJECTED;
  ev.order = order;
  ev.newOrder = newOrder;
  ev.rejectReason = reason;
  ev.exchangeTsNs = _clock.nowNs();
  ev.timestamps = timestampsFor(order.id);
  _callback(ev);
}

void SimulatedExecutor::setOrderEventCallback(OrderEventCallback cb)
{
  _callback = std::move(cb);
}

void SimulatedExecutor::applyConfig(const BacktestConfig& config)
{
  _defaultSlippage = config.defaultSlippage;
  _slippageSetFlat.fill(false);
  _slippageOverflow.clear();
  for (const auto& [sym, prof] : config.perSymbolSlippage)
  {
    setSymbolSlippage(sym, prof);
  }
  setQueueModel(config.queueModel, config.queueDepth);
  _queuePosMinFraction = config.queuePositionMinChangeFraction;
  setCancelAckLatency(config.cancelAckLatencyNs, config.cancelAckJitterNs);
  _cancelAckSeed = config.cancelAckSeed;
  _cancelAckRng.seed(config.cancelAckSeed);
  setReplaceAckLatency(config.replaceAckLatencyNs, config.replaceAckJitterNs);
  setSubmitAckLatency(config.submitAckLatencyNs, config.submitAckJitterNs);
  setLatencyModel(config.latency);
}

void SimulatedExecutor::reset()
{
  _pending_orders.clear();
  _conditional_orders.clear();
  _heldBarOrders.clear();
  _barCallbackDepth = 0;
  _nextEventDepth = 0;
  _fills.clear();
  _trailing_states.clear();
  _fillScanIds.clear();
  _netPositionRaw.clear();
  _ladders.clear();
  _marketStatesFlat.fill(MarketState{});
  _marketStatesOverflow.clear();
  _pendingCancels.clear();
  _pendingReplaces.clear();
  _pendingSubmissions.clear();
  _outageBuffer.clear();
  _venueWasUp = true;
  _orderTimestamps.clear();
  _lastEmittedQueueAheadRaw.clear();
  _lastEmittedMarketPosition.clear();
  _brackets.clear();
  _legToBracket.clear();
  _bracketTemplates.clear();
  _iceberg.clear();
  _queueFillBuffer.clear();
  _queueSnapshotBuffer.clear();
  _queueTracker.clear();
  _compositeLogic = CompositeOrderLogic{0};
  _compositeLogic.setExecutor(this);
  _cancelAckRng.seed(_cancelAckSeed);
  _icebergJitterRng.seed(_icebergJitterSeed);
  if (_hasRateLimit)
  {
    _rateLimit = _rateLimitAsInstalled;
  }
}

void SimulatedExecutor::setDefaultSlippage(const SlippageProfile& profile)
{
  _defaultSlippage = profile;
}

void SimulatedExecutor::setSymbolSlippage(SymbolId symbol, const SlippageProfile& profile)
{
  if (symbol < kMaxSymbols)
  {
    _slippageFlat[symbol] = profile;
    _slippageSetFlat[symbol] = true;
    return;
  }
  for (auto& [id, prof] : _slippageOverflow)
  {
    if (id == symbol)
    {
      prof = profile;
      return;
    }
  }
  _slippageOverflow.emplace_back(symbol, profile);
}

void SimulatedExecutor::setQueueModel(QueueModel model, size_t depth)
{
  _queueModel = model;
  _queueTracker.setModel(model, depth);
}

void SimulatedExecutor::setQueueFifoTopN(size_t topN)
{
  _queueTracker.setFifoTopN(topN);
}

void SimulatedExecutor::setTopPriorityShare(double share)
{
  _queueTracker.setTopPriorityShare(share);
}

void SimulatedExecutor::setLmmOrders(const std::vector<OrderId>& ids)
{
  _queueTracker.setLmmOrders(ids);
}

void SimulatedExecutor::setLmmBonusMultiplier(double multiplier)
{
  _queueTracker.setLmmBonusMultiplier(multiplier);
}

void SimulatedExecutor::setOrderPriorityMultiplier(OrderId id, double multiplier)
{
  _queueTracker.setOrderPriorityMultiplier(id, multiplier);
}

void SimulatedExecutor::setQueuePositionMinChangeFraction(double fraction)
{
  _queuePosMinFraction = fraction;
}

static LatencyDistribution makeLegacyDist(int64_t latencyNs, int64_t jitterNs)
{
  if (jitterNs <= 0)
  {
    return LatencyDistribution::constant(latencyNs);
  }
  return LatencyDistribution::uniform(std::max<int64_t>(0, latencyNs - jitterNs),
                                      latencyNs + jitterNs);
}

void SimulatedExecutor::setSubmitAckLatency(int64_t latencyNs, int64_t jitterNs)
{
  _submitAckDist = makeLegacyDist(latencyNs, jitterNs);
}

void SimulatedExecutor::setCancelAckLatency(int64_t latencyNs, int64_t jitterNs)
{
  _cancelAckDist = makeLegacyDist(latencyNs, jitterNs);
}

void SimulatedExecutor::setReplaceAckLatency(int64_t latencyNs, int64_t jitterNs)
{
  _replaceAckDist = makeLegacyDist(latencyNs, jitterNs);
}

void SimulatedExecutor::setSubmitAckLatencyDistribution(const LatencyDistribution& dist)
{
  _submitAckDist = dist;
}

void SimulatedExecutor::setCancelAckLatencyDistribution(const LatencyDistribution& dist)
{
  _cancelAckDist = dist;
}

void SimulatedExecutor::setReplaceAckLatencyDistribution(const LatencyDistribution& dist)
{
  _replaceAckDist = dist;
}

bool SimulatedExecutor::applyLatencyProfile(const char* name)
{
  if (name == nullptr)
  {
    return false;
  }
  std::string n(name);
  BacktestConfig cfg{};
  if (n == "binance_um_futures")
  {
    LatencyProfiles::binance_um_futures(cfg);
  }
  else if (n == "bybit_linear")
  {
    LatencyProfiles::bybit_linear(cfg);
  }
  else if (n == "okx_swap")
  {
    LatencyProfiles::okx_swap(cfg);
  }
  else if (n == "deribit")
  {
    LatencyProfiles::deribit(cfg);
  }
  else if (n == "idealized")
  {
    LatencyProfiles::idealized(cfg);
  }
  else if (n == "adversarial")
  {
    LatencyProfiles::adversarial(cfg);
  }
  else
  {
    return false;
  }
  setSubmitAckLatency(cfg.submitAckLatencyNs, cfg.submitAckJitterNs);
  setCancelAckLatency(cfg.cancelAckLatencyNs, cfg.cancelAckJitterNs);
  setReplaceAckLatency(cfg.replaceAckLatencyNs, cfg.replaceAckJitterNs);
  return true;
}

void SimulatedExecutor::setRateLimitPolicy(const RateLimitPolicy& policy)
{
  _rateLimitAsInstalled = policy;
  _rateLimit = policy;
  _hasRateLimit = _rateLimit.bucketCount() > 0;
}

void SimulatedExecutor::clearRateLimitPolicy()
{
  _rateLimit = RateLimitPolicy{};
  _hasRateLimit = false;
}

void SimulatedExecutor::setSTPGroupMembership(uint64_t accountId, uint64_t groupId)
{
  if (groupId == 0)
  {
    _stpGroupMembership.erase(accountId);
  }
  else
  {
    _stpGroupMembership[accountId] = groupId;
  }
}

uint64_t SimulatedExecutor::stpGroupFor(uint64_t accountId) const
{
  auto it = _stpGroupMembership.find(accountId);
  return it == _stpGroupMembership.end() ? 0 : it->second;
}

bool SimulatedExecutor::sameStpScope(uint64_t a, uint64_t b) const
{
  if (a == b)
  {
    return true;
  }
  const uint64_t ga = stpGroupFor(a);
  const uint64_t gb = stpGroupFor(b);
  return ga != 0 && ga == gb;
}

void SimulatedExecutor::setFokModeByName(const std::string& name)
{
  std::string lower;
  lower.reserve(name.size());
  for (char c : name)
  {
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (lower == "any_price" || lower == "anyprice")
  {
    _fokMode = FokMode::AnyPrice;
  }
  else if (lower == "single_price" || lower == "singleprice")
  {
    _fokMode = FokMode::SinglePrice;
  }
}

const SlippageProfile& SimulatedExecutor::slippageFor(SymbolId symbol) const
{
  if (symbol < kMaxSymbols && _slippageSetFlat[symbol])
  {
    return _slippageFlat[symbol];
  }
  for (const auto& [id, prof] : _slippageOverflow)
  {
    if (id == symbol)
    {
      return prof;
    }
  }
  return _defaultSlippage;
}

int64_t SimulatedExecutor::applySlippage(int64_t priceRaw, Side side, SymbolId symbol,
                                         Quantity qty, int64_t levelQtyRaw) const
{
  const SlippageProfile& prof = slippageFor(symbol);
  if (prof.model == SlippageModel::NONE)
  {
    return priceRaw;
  }

  int64_t offsetRaw = 0;
  switch (prof.model)
  {
    case SlippageModel::FIXED_TICKS:
    {
      const int64_t tickRaw = (prof.tickSize.raw() > 0) ? prof.tickSize.raw() : 1;
      offsetRaw = static_cast<int64_t>(prof.ticks) * tickRaw;
      break;
    }
    case SlippageModel::FIXED_BPS:
      offsetRaw = static_cast<int64_t>(std::llround(
          static_cast<double>(priceRaw) * (prof.bps * 1e-4)));
      break;
    case SlippageModel::VOLUME_IMPACT:
    {
      // No visible depth (bar/trade-only tapes never set level qty): the model
      // has no denominator, so it cannot say anything about impact. Falling back
      // to "1 raw unit" made ratio ~1e8 and produced a garbage fill price;
      // disabling the model (zero offset) is the honest behaviour.
      if (levelQtyRaw > 0)
      {
        const double ratio = static_cast<double>(qty.raw()) / static_cast<double>(levelQtyRaw);
        offsetRaw = static_cast<int64_t>(
            std::llround(static_cast<double>(priceRaw) * prof.impactCoeff * ratio));
      }
      break;
    }
    case SlippageModel::NONE:
      break;
  }

  return (side == Side::BUY) ? priceRaw + offsetRaw : priceRaw - offsetRaw;
}

void SimulatedExecutor::submitOrder(const Order& order)
{
  if (_barCallbackDepth > 0)
  {
    // The strategy is inside a bar callback, so every price of that bar --
    // open, high, low and close -- is already in the market state. Matching
    // here would hand the order a price it only exists to react to. Hold it;
    // the next bar's open releases it through this same path.
    _heldBarOrders.push_back(order);
    return;
  }

  Order accepted = order;
  accepted.createdAt = fromUnixNs(_clock.nowNs());

  if (_venue && !_venue->submitsAllowed(_clock.nowNs().raw()))
  {
    _outageBuffer.push_back(BufferedRequest{.action = BufferedAction::SUBMIT,
                                            .order = accepted});
    return;
  }

  // Native iceberg: replace order.quantity with the visible tranche
  // and stash the hidden remainder. Subsequent fills against the
  // visible portion trigger refreshes via maybeRefreshIceberg.
  if (accepted.type == OrderType::ICEBERG &&
      accepted.visibleQuantity.raw() > 0 &&
      accepted.visibleQuantity.raw() < accepted.quantity.raw())
  {
    const int64_t totalRaw = accepted.quantity.raw();
    const int64_t visibleRaw = accepted.visibleQuantity.raw();
    _iceberg[accepted.id] = IcebergState{
        .hiddenRaw = totalRaw - visibleRaw,
        .visibleRaw = visibleRaw,
        .refreshLatencyNs = _icebergRefreshLatencyNs,
        .refreshDueNs = 0};
    accepted.quantity = Quantity::fromRaw(visibleRaw);
    // Treat as a LIMIT for matching; iceberg type stays on the order
    // for diagnostics and event payloads.
    accepted.type = OrderType::LIMIT;
  }

  if (_hasRateLimit &&
      !_rateLimit.tryConsume(RateLimitPolicy::ActionKind::Submit,
                             _clock.nowNs().raw()))
  {
    emitEvent(OrderEventStatus::REJECTED_RATE_LIMIT, accepted);
    return;
  }

  // A venue refuses a price that is not a whole number of ticks.
  if (const char* why = offTickReason(order, orderTickRaw(accepted.symbol)))
  {
    emitRejected(accepted, why);
    return;
  }
  // And a quantity that is not a whole number of lots.
  if (const char* why = offLotReason(order, orderLotRaw(accepted.symbol)))
  {
    emitRejected(accepted, why);
    return;
  }

  // reduce_only: order may only reduce existing position. Computed
  // against the simulator-side net position (updated in executeFill).
  // If the order would open or grow the position it is rejected; if
  // it would overshoot zero, the size is truncated to flat.
  //
  // This runs before self-trade prevention, as the guide describes. An order
  // the engine is going to reject on its own terms must not get to cancel or
  // shrink something already resting on the way out.
  if (accepted.flags.reduceOnly)
  {
    const int64_t netRaw = netPositionRaw(accepted.symbol);
    const int64_t qtyRaw = accepted.quantity.raw();
    const int64_t signedQtyRaw = (accepted.side == Side::BUY) ? qtyRaw : -qtyRaw;

    // No open position → reduce-only can't reduce anything.
    if (netRaw == 0 || (netRaw > 0 && signedQtyRaw > 0) ||
        (netRaw < 0 && signedQtyRaw < 0))
    {
      OrderEvent ev;
      ev.status = OrderEventStatus::REJECTED;
      ev.order = accepted;
      ev.exchangeTsNs = _clock.nowNs();
      ev.timestamps = timestampsFor(accepted.id);
      ev.timestamps.rejectedAtNs = ev.exchangeTsNs;
      ev.rejectReason = "reduce_only";
      if (_callback)
      {
        _callback(ev);
      }
      return;
    }

    // Truncate so we don't flip sign.
    const int64_t maxReductionRaw = (netRaw > 0) ? netRaw : -netRaw;
    if (qtyRaw > maxReductionRaw)
    {
      accepted.quantity = Quantity::fromRaw(maxReductionRaw);
    }
  }

  // Self-trade prevention. Walk pending orders for an opposite-side
  // crossing match on the same symbol; apply the configured mode.
  // Empty / None mode is a no-op.
  if (_stpMode != STPMode::None && !_pending_orders.empty())
  {
    for (size_t i = 0; i < _pending_orders.size(); ++i)
    {
      Order& existing = _pending_orders[i];
      if (existing.symbol != accepted.symbol || existing.side == accepted.side)
      {
        continue;
      }
      // Multi-account STP scope: STP only fires when the two orders
      // share an STP scope (same accountId, or both accounts in the
      // same explicit STP group). Default accountId=0 keeps the
      // original single-account self-trade-prevention semantics.
      if (!sameStpScope(existing.accountId, accepted.accountId))
      {
        continue;
      }
      const int64_t aPrice = accepted.price.raw();
      const int64_t ePrice = existing.price.raw();
      const bool crosses =
          (accepted.side == Side::BUY && aPrice >= ePrice) ||
          (accepted.side == Side::SELL && aPrice <= ePrice);
      if (!crosses)
      {
        continue;
      }

      auto rejectAccepted = [&](const char* reason)
      {
        OrderEvent ev;
        ev.status = OrderEventStatus::REJECTED;
        ev.order = accepted;
        ev.exchangeTsNs = _clock.nowNs();
        ev.timestamps = timestampsFor(accepted.id);
        ev.timestamps.rejectedAtNs = ev.exchangeTsNs;
        ev.rejectReason = reason;
        if (_callback)
        {
          _callback(ev);
        }
      };

      auto cancelExisting = [&]()
      {
        Order cancelled = existing;
        _queueTracker.removeOrder(cancelled.id);
        forgetQueuePosition(cancelled.id);
        forgetMarketPosition(cancelled.id);
        _pending_orders.erase(_pending_orders.begin() + static_cast<long>(i));
        emitEvent(OrderEventStatus::CANCELED, cancelled);
      };

      switch (_stpMode)
      {
        case STPMode::CancelNewest:
          rejectAccepted("stp_cancel_newest");
          return;
        case STPMode::CancelOldest:
          cancelExisting();
          // Fall through to normal submit path for the accepted order.
          goto stp_done;
        case STPMode::CancelBoth:
          cancelExisting();
          rejectAccepted("stp_cancel_both");
          return;
        case STPMode::Decrement:
        {
          const int64_t aQty = accepted.quantity.raw();
          const int64_t eQty = existing.quantity.raw() - existing.filledQuantity.raw();
          if (aQty <= eQty)
          {
            // Smaller side is the new order. Cancel it; shrink existing by aQty.
            // The queue tracker carries its own copy of what the resting order
            // can still trade, so it has to shrink with it. When nothing is
            // left to trade the order is pulled, the way a venue pulls an
            // order decremented to zero.
            const OrderId existingId = existing.id;
            const int64_t newQtyRaw = existing.quantity.raw() - aQty;
            const int64_t newRemainingRaw = newQtyRaw - existing.filledQuantity.raw();
            if (newRemainingRaw <= 0)
            {
              cancelExisting();
            }
            else
            {
              existing.quantity = Quantity::fromRaw(newQtyRaw);
              _queueTracker.resizeOrder(existingId,
                                        Quantity::fromRaw(newRemainingRaw));
            }
            rejectAccepted("stp_decrement_newest");
            return;
          }
          else
          {
            // Smaller side is the existing order. Cancel it; shrink new by eQty.
            cancelExisting();
            accepted.quantity = Quantity::fromRaw(aQty - eQty);
            goto stp_done;
          }
        }
        case STPMode::None:
          break;
      }
    }
  }
stp_done:

  emitEvent(OrderEventStatus::SUBMITTED, accepted);

  // How long this order takes to reach the matching engine: the wire delay
  // the latency model draws, plus whatever the venue's ack model adds. Both
  // are sampled once, here, so a model and an ack profile compose instead of
  // overriding each other.
  //
  // The branch is on the sampled total, not on whether a model is attached:
  // a model that draws zero must reproduce the instant baseline to the
  // nanosecond, and the synchronous path is the only thing that fills inside
  // this call.
  const int64_t deferNs =
      sampleOrderLatency() +
      (_submitAckDist.medianNs() > 0 ? sampleSubmitAckLatency() : 0);
  if (deferNs > 0 || _nextEventDepth > 0)
  {
    // Async path: ACCEPTED defers until the deadline. The order is held
    // aside; finishSubmission runs the existing book-add / queue-tracker /
    // try-fill logic when it arrives. Inside a next-event window a zero
    // deadline takes this path too, so the order meets the next event and
    // not the state that is already here.
    enqueuePendingSubmission(accepted, deferNs);
    return;
  }

  finishSubmission(std::move(accepted), /*fromAck=*/false);
}

void SimulatedExecutor::finishSubmission(Order accepted, bool fromAck)
{
  emitEvent(OrderEventStatus::ACCEPTED, accepted);

  // POST_ONLY: reject any limit that would cross the book (taker semantics
  // are not allowed for post-only). Real exchanges reject these on submit;
  // the simulator must do the same so backtests don't silently fill them
  // as makers via the queue tracker on subsequent ticks.
  if (accepted.type == OrderType::LIMIT && accepted.timeInForce == TimeInForce::POST_ONLY)
  {
    const MarketState& state = getMarketState(accepted.symbol);
    const int64_t orderPriceRaw = accepted.price.raw();
    const bool crosses =
        (accepted.side == Side::BUY && state.hasAsk && orderPriceRaw >= state.bestAskRaw) ||
        (accepted.side == Side::SELL && state.hasBid && orderPriceRaw <= state.bestBidRaw);
    if (crosses)
    {
      if (_callback)
      {
        OrderEvent ev;
        ev.status = OrderEventStatus::REJECTED;
        ev.order = accepted;
        ev.exchangeTsNs = _clock.nowNs();
        ev.timestamps = timestampsFor(accepted.id);
        ev.timestamps.rejectedAtNs = ev.exchangeTsNs;
        if (fromAck)
        {
          ev.rejectReason = "late_post_only_crossed";
        }
        _callback(ev);
      }
      return;
    }
  }

  // FOK: must fully fill or reject. The simulator only tracks top-of-
  // book qty, so multi-level walking is approximated.
  //   AnyPrice    — accept if TOB crosses the order's limit and TOB
  //                 qty >= order qty. Deeper walks are a follow-up.
  //   SinglePrice — accept only if TOB price exactly matches the
  //                 order's limit AND TOB qty >= order qty; otherwise
  //                 reject with "fok_unfillable".
  if (accepted.type == OrderType::LIMIT &&
      accepted.timeInForce == TimeInForce::FOK)
  {
    const MarketState& state = getMarketState(accepted.symbol);
    int64_t availRaw = 0;
    bool canCross = false;
    bool exactLevel = false;
    if (accepted.side == Side::BUY && state.hasAsk &&
        accepted.price.raw() >= state.bestAskRaw)
    {
      availRaw = state.bestAskQtyRaw;
      canCross = true;
      exactLevel = (accepted.price.raw() == state.bestAskRaw);
    }
    else if (accepted.side == Side::SELL && state.hasBid &&
             accepted.price.raw() <= state.bestBidRaw)
    {
      availRaw = state.bestBidQtyRaw;
      canCross = true;
      exactLevel = (accepted.price.raw() == state.bestBidRaw);
    }
    const bool sizeOk = availRaw >= accepted.quantity.raw();
    const bool fillable =
        canCross && sizeOk &&
        (_fokMode == FokMode::AnyPrice || exactLevel);
    if (!fillable)
    {
      OrderEvent ev;
      ev.status = OrderEventStatus::REJECTED;
      ev.order = accepted;
      ev.exchangeTsNs = _clock.nowNs();
      ev.timestamps = timestampsFor(accepted.id);
      ev.timestamps.rejectedAtNs = ev.exchangeTsNs;
      ev.rejectReason = (_fokMode == FokMode::SinglePrice && canCross && !exactLevel)
                            ? "fok_unfillable"
                            : "fok_not_fillable";
      if (_callback)
      {
        _callback(ev);
      }
      return;
    }
    // Fall through to tryFillOrder which will take the crossing
    // liquidity at the limit price (or better).
  }

  // IOC: take whatever crosses now; cancel any remainder. We tag it
  // so the post-fill path skips resting the order.
  bool isIOC = (accepted.type == OrderType::LIMIT &&
                accepted.timeInForce == TimeInForce::IOC);

  // Handle conditional orders (stop, TP, trailing)
  if (isConditionalOrder(accepted.type))
  {
    _conditional_orders.push_back(accepted);
    emitEvent(OrderEventStatus::PENDING_TRIGGER, accepted);

    if (accepted.type == OrderType::TRAILING_STOP)
    {
      const MarketState& state = getMarketState(accepted.symbol);
      Price currentPrice =
          state.hasTrade ? Price::fromRaw(state.lastTradeRaw) : Price::fromRaw(state.bestBidRaw);

      TrailingState trailing;
      trailing.activationPrice = currentPrice;

      if (accepted.trailingOffset.raw() > 0)
      {
        trailing.currentTrigger = (accepted.side == Side::SELL)
                                      ? Price::fromRaw(currentPrice.raw() - accepted.trailingOffset.raw())
                                      : Price::fromRaw(currentPrice.raw() + accepted.trailingOffset.raw());
      }
      else if (accepted.trailingCallbackRate > 0)
      {
        int64_t offsetRaw = (currentPrice.raw() * accepted.trailingCallbackRate) / 10000;
        const int64_t triggerRaw = (accepted.side == Side::SELL)
                                       ? currentPrice.raw() - offsetRaw
                                       : currentPrice.raw() + offsetRaw;
        trailing.currentTrigger = Price::fromRaw(
            snapAgainstOwner(triggerRaw, orderTickRaw(accepted.symbol), accepted.side));
      }

      _trailing_states.emplace_back(accepted.id, trailing);
    }
    return;
  }

  // When queue simulation is enabled, LIMIT orders that do not cross the book
  // get registered in the queue tracker. Orders that cross fill immediately.
  if (accepted.type == OrderType::LIMIT && _queueTracker.enabled())
  {
    const MarketState& state = getMarketState(accepted.symbol);
    const int64_t orderPriceRaw = accepted.price.raw();
    const bool crosses =
        (accepted.side == Side::BUY && state.hasAsk && orderPriceRaw >= state.bestAskRaw) ||
        (accepted.side == Side::SELL && state.hasBid && orderPriceRaw <= state.bestBidRaw);

    if (!crosses)
    {
      if (isIOC)
      {
        // IOC limit that does not cross — nothing to fill now and
        // nothing to rest. Cancel immediately.
        emitEvent(OrderEventStatus::CANCELED, accepted);
        return;
      }
      Quantity levelQty = Quantity::fromRaw(0);
      if (accepted.side == Side::BUY && state.hasBid && orderPriceRaw == state.bestBidRaw)
      {
        levelQty = Quantity::fromRaw(state.bestBidQtyRaw);
      }
      else if (accepted.side == Side::SELL && state.hasAsk && orderPriceRaw == state.bestAskRaw)
      {
        levelQty = Quantity::fromRaw(state.bestAskQtyRaw);
      }
      _queueTracker.addOrder(accepted.symbol, accepted.side, accepted.price,
                             accepted.id, accepted.quantity, levelQty);
      // Seed last-emitted with the arrival ahead so the first
      // queue-position event fires only after a real movement.
      _lastEmittedQueueAheadRaw[accepted.id] = levelQty.raw();
      _pending_orders.push_back(accepted);
      return;
    }
  }

  // The order is arriving, not resting: if it crosses, it is the aggressor and
  // it pays the touch.
  const bool filled = tryFillOrder(accepted, /*resting=*/false);
  if (!filled && isIOC)
  {
    // IOC: cancel the unfilled remainder instead of resting. The
    // CANCELED event carries the IOC tag on the order itself; no
    // free-text reason field on cancel today (rejectReason would
    // be misleading).
    emitEvent(OrderEventStatus::CANCELED, accepted);
    return;
  }
  if (!filled)
  {
    _pending_orders.push_back(accepted);
  }
}

void SimulatedExecutor::cancelOrder(OrderId orderId)
{
  // An order submitted and pulled inside the same bar callback never reached
  // the venue. Drop it where it waits, or the next bar's open would release
  // an order the strategy has already cancelled.
  for (size_t i = 0; i < _heldBarOrders.size(); ++i)
  {
    if (_heldBarOrders[i].id == orderId)
    {
      const Order cancelled = _heldBarOrders[i];
      _heldBarOrders.erase(_heldBarOrders.begin() + static_cast<std::ptrdiff_t>(i));
      emitEvent(OrderEventStatus::CANCELED, cancelled);
      return;
    }
  }

  if (_venue && !_venue->cancelsAllowed(_clock.nowNs().raw()))
  {
    _outageBuffer.push_back(BufferedRequest{.action = BufferedAction::CANCEL,
                                            .oldOrderId = orderId});
    return;
  }
  if (_hasRateLimit &&
      !_rateLimit.tryConsume(RateLimitPolicy::ActionKind::Cancel,
                             _clock.nowNs().raw()))
  {
    for (const auto& o : _pending_orders)
    {
      if (o.id == orderId)
      {
        emitEvent(OrderEventStatus::REJECTED_RATE_LIMIT, o);
        return;
      }
    }
    return;
  }

  // Async path: emit PENDING_CANCEL and defer CANCELED until the
  // sampled ack deadline. Leave the order in the book so it can
  // still fill in the race window.
  if (_cancelAckDist.medianNs() > 0)
  {
    for (const auto& o : _pending_orders)
    {
      if (o.id == orderId)
      {
        emitEvent(OrderEventStatus::PENDING_CANCEL, o);
        enqueuePendingCancel(o);
        return;
      }
    }
    for (const auto& o : _conditional_orders)
    {
      if (o.id == orderId)
      {
        emitEvent(OrderEventStatus::PENDING_CANCEL, o);
        enqueuePendingCancel(o);
        return;
      }
    }
    return;
  }

  // Detach the order from every book the executor keeps before announcing the
  // cancel. The announcement re-enters the executor -- a strategy submits its
  // replacement right there -- and anything still holding a position in
  // _pending_orders at that point is holding a stale one.
  for (size_t i = 0; i < _pending_orders.size(); ++i)
  {
    if (_pending_orders[i].id != orderId)
    {
      continue;
    }
    const Order canceled = _pending_orders[i];
    _pending_orders[i] = _pending_orders.back();
    _pending_orders.pop_back();
    _queueTracker.removeOrder(orderId);
    forgetQueuePosition(orderId);
    forgetMarketPosition(orderId);
    forgetPendingReplace(orderId);
    _iceberg.erase(orderId);
    // The event carries the order's timestamps, so they are dropped after it
    // has been built and delivered.
    emitEvent(OrderEventStatus::CANCELED, canceled);
    forgetTimestamps(orderId);
    _compositeLogic.onOrderCanceled(canceled);
    return;
  }

  for (size_t i = 0; i < _conditional_orders.size(); ++i)
  {
    if (_conditional_orders[i].id != orderId)
    {
      continue;
    }
    const Order canceled = _conditional_orders[i];

    for (auto trailingIt = _trailing_states.begin(); trailingIt != _trailing_states.end();
         ++trailingIt)
    {
      if (trailingIt->first == orderId)
      {
        *trailingIt = _trailing_states.back();
        _trailing_states.pop_back();
        break;
      }
    }

    _conditional_orders[i] = _conditional_orders.back();
    _conditional_orders.pop_back();
    emitEvent(OrderEventStatus::CANCELED, canceled);
    _compositeLogic.onOrderCanceled(canceled);
    return;
  }
}

void SimulatedExecutor::cancelAllOrders(SymbolId symbol)
{
  if (_venue && !_venue->cancelsAllowed(_clock.nowNs().raw()))
  {
    _outageBuffer.push_back(BufferedRequest{.action = BufferedAction::CANCEL_ALL_SYMBOL,
                                            .cancelAllSymbol = symbol});
    return;
  }
  if (_cancelAckDist.medianNs() > 0)
  {
    cancelAllOrdersAsync(symbol);
    return;
  }
  // Detach everything first, announce afterwards: each CANCELED event re-enters
  // the executor and can add or remove orders while the sweep is still running.
  std::vector<Order> canceled;
  size_t i = 0;
  // Orders still held for the next bar's open are part of "all orders" as far
  // as the strategy is concerned -- it submitted them and never got them back.
  while (i < _heldBarOrders.size())
  {
    if (_heldBarOrders[i].symbol == symbol)
    {
      canceled.push_back(_heldBarOrders[i]);
      _heldBarOrders.erase(_heldBarOrders.begin() + static_cast<std::ptrdiff_t>(i));
    }
    else
    {
      ++i;
    }
  }

  i = 0;
  while (i < _pending_orders.size())
  {
    if (_pending_orders[i].symbol == symbol)
    {
      canceled.push_back(_pending_orders[i]);
      _queueTracker.removeOrder(_pending_orders[i].id);
      forgetQueuePosition(_pending_orders[i].id);
      forgetMarketPosition(_pending_orders[i].id);
      forgetPendingReplace(_pending_orders[i].id);
      _iceberg.erase(_pending_orders[i].id);
      _pending_orders[i] = _pending_orders.back();
      _pending_orders.pop_back();
    }
    else
    {
      ++i;
    }
  }

  i = 0;
  while (i < _conditional_orders.size())
  {
    if (_conditional_orders[i].symbol == symbol)
    {
      const OrderId orderId = _conditional_orders[i].id;
      canceled.push_back(_conditional_orders[i]);

      for (auto trailingIt = _trailing_states.begin(); trailingIt != _trailing_states.end();
           ++trailingIt)
      {
        if (trailingIt->first == orderId)
        {
          *trailingIt = _trailing_states.back();
          _trailing_states.pop_back();
          break;
        }
      }

      _conditional_orders[i] = _conditional_orders.back();
      _conditional_orders.pop_back();
    }
    else
    {
      ++i;
    }
  }

  for (const Order& order : canceled)
  {
    // The event carries the order's timestamps, so they are dropped after it
    // has been built and delivered.
    emitEvent(OrderEventStatus::CANCELED, order);
    forgetTimestamps(order.id);
  }
}

void SimulatedExecutor::cancelAllOrdersAsync(SymbolId symbol)
{
  // The same request as cancelOrder() on every order of the symbol, in the
  // order the synchronous sweep announces them: held orders first (they never
  // reached the venue, so they cancel at once), then resting, then
  // conditional orders, each going PENDING_CANCEL and staying live until its
  // sampled ack. The ids are taken up front; an order an earlier event's
  // callback has already removed is skipped, as cancelOrder() would.
  std::vector<OrderId> ids;
  for (const Order& o : _heldBarOrders)
  {
    if (o.symbol == symbol)
    {
      ids.push_back(o.id);
    }
  }
  const size_t heldCount = ids.size();
  // The synchronous sweep removes by swap-and-pop, which decides the order it
  // announces in; walk a copy the same way so a latency changes only when the
  // cancels land, never their order.
  const auto sweepOrder = [&](std::vector<Order> book)
  {
    size_t i = 0;
    while (i < book.size())
    {
      if (book[i].symbol == symbol)
      {
        ids.push_back(book[i].id);
        book[i] = book.back();
        book.pop_back();
      }
      else
      {
        ++i;
      }
    }
  };
  sweepOrder(_pending_orders);
  sweepOrder(_conditional_orders);

  for (size_t k = 0; k < ids.size(); ++k)
  {
    const OrderId id = ids[k];
    if (k < heldCount)
    {
      for (size_t i = 0; i < _heldBarOrders.size(); ++i)
      {
        if (_heldBarOrders[i].id == id)
        {
          const Order cancelled = _heldBarOrders[i];
          _heldBarOrders.erase(_heldBarOrders.begin() + static_cast<std::ptrdiff_t>(i));
          emitEvent(OrderEventStatus::CANCELED, cancelled);
          forgetTimestamps(id);
          break;
        }
      }
      continue;
    }
    const Order* live = findPendingOrder(id);
    if (live == nullptr)
    {
      for (const Order& o : _conditional_orders)
      {
        if (o.id == id)
        {
          live = &o;
          break;
        }
      }
    }
    if (live != nullptr)
    {
      const Order snapshot = *live;
      emitEvent(OrderEventStatus::PENDING_CANCEL, snapshot);
      enqueuePendingCancel(snapshot);
    }
  }
}

void SimulatedExecutor::replaceOrder(OrderId oldOrderId, const Order& newOrder)
{
  // An off-tick or off-lot replacement is refused and the original order stays
  // as it was.
  const int64_t tickRaw = orderTickRaw(newOrder.symbol);
  const char* why = offTickReason(newOrder, tickRaw);
  // A trailing stop is placed with an offset and no trigger; a replacement
  // may name one, and it has to sit on the grid like any other trigger.
  if (why == nullptr && newOrder.type == OrderType::TRAILING_STOP && tickRaw > 0 &&
      newOrder.triggerPrice.raw() % tickRaw != 0)
  {
    why = kRejectTriggerOffTick;
  }
  if (why == nullptr)
  {
    why = offLotReason(newOrder, orderLotRaw(newOrder.symbol));
  }
  if (why != nullptr)
  {
    const Order* old = findOrder(oldOrderId);
    emitReplaceRejected(old != nullptr ? *old : newOrder, newOrder, why);
    return;
  }

  // Replacing an order that is still held for the next bar's open rewrites it
  // in place: the venue never saw the original, so there is nothing to cancel
  // and the replacement inherits its place in the hold list.
  for (Order& held : _heldBarOrders)
  {
    if (held.id == oldOrderId)
    {
      const Order oldOrder = held;
      held = newOrder;
      if (_callback)
      {
        OrderEvent ev;
        ev.status = OrderEventStatus::REPLACED;
        ev.order = oldOrder;
        ev.newOrder = newOrder;
        ev.exchangeTsNs = _clock.nowNs();
        _callback(ev);
      }
      return;
    }
  }

  // Replace is a cancel + submit pair; if either action is currently
  // gated we have to buffer the whole request until both are open.
  const int64_t nowNs = _clock.nowNs().raw();
  if (_venue && (!_venue->cancelsAllowed(nowNs) || !_venue->submitsAllowed(nowNs)))
  {
    _outageBuffer.push_back(BufferedRequest{.action = BufferedAction::REPLACE,
                                            .order = newOrder,
                                            .oldOrderId = oldOrderId});
    return;
  }
  // The order rests in the book, or still waits on its trigger. Anything
  // else -- an id never seen, an order already filled or cancelled -- is
  // refused, so the caller hears that nothing changed.
  Order* resting = findPendingOrder(oldOrderId);
  Order* untriggered = resting == nullptr ? findConditionalOrder(oldOrderId) : nullptr;
  if (resting == nullptr && untriggered == nullptr)
  {
    emitReplaceRejected(newOrder, newOrder, kRejectUnknownOrder);
    return;
  }
  Order& live = resting != nullptr ? *resting : *untriggered;
  // A stop stays a stop: its trigger bookkeeping follows its type.
  if (untriggered != nullptr && newOrder.type != untriggered->type)
  {
    emitReplaceRejected(live, newOrder, kRejectReplaceChangesType);
    return;
  }

  if (_hasRateLimit &&
      !_rateLimit.tryConsume(RateLimitPolicy::ActionKind::Replace,
                             _clock.nowNs().raw()))
  {
    emitEvent(OrderEventStatus::REJECTED_RATE_LIMIT, live);
    return;
  }

  // Async path: emit REPLACE_SUBMITTED immediately and defer the
  // ACCEPTED + REPLACED transition until the ack deadline. The
  // original order stays in force and can still fill or trigger in the
  // race window.
  if (_replaceAckDist.medianNs() > 0)
  {
    OrderEvent ev;
    ev.status = OrderEventStatus::REPLACE_SUBMITTED;
    ev.order = live;
    ev.newOrder = newOrder;
    ev.exchangeTsNs = _clock.nowNs();
    ev.timestamps = timestampsFor(live.id);
    // The callback may re-enter and move the order books under `live`.
    const Order snapshot = live;
    if (_callback)
    {
      _callback(ev);
    }
    enqueuePendingReplace(snapshot, newOrder, untriggered != nullptr);
    return;
  }

  const Order oldOrder = live;
  if (untriggered != nullptr)
  {
    replaceConditionalOrder(*untriggered, newOrder);
  }
  else
  {
    live = newOrder;
  }

  OrderEvent ev;
  ev.status = OrderEventStatus::REPLACED;
  ev.order = oldOrder;
  ev.newOrder = newOrder;
  ev.exchangeTsNs = _clock.nowNs();
  if (_callback)
  {
    _callback(ev);
  }
}

void SimulatedExecutor::replaceConditionalOrder(Order& conditional, const Order& newOrder)
{
  conditional = newOrder;
  if (newOrder.type != OrderType::TRAILING_STOP || newOrder.triggerPrice.raw() == 0)
  {
    return;
  }
  for (auto& [id, trailing] : _trailing_states)
  {
    if (id == newOrder.id)
    {
      trailing.currentTrigger = newOrder.triggerPrice;
      return;
    }
  }
}

void SimulatedExecutor::submitOCO(const OCOParams& params)
{
  _compositeLogic.registerOCO(params.order1.id, params.order2.id);
  submitOrder(params.order1);
  submitOrder(params.order2);
}

namespace
{
Order legToOrder(const BracketLeg& leg, OrderId id, SymbolId symbol, Quantity qty)
{
  Order o;
  o.id = id;
  o.side = leg.side;
  o.type = leg.type;
  o.price = leg.price;
  o.triggerPrice = leg.triggerPrice;
  o.quantity = qty;
  o.symbol = symbol;
  return o;
}
}  // namespace

void SimulatedExecutor::submitBracket(const BracketOrder& bracket)
{
  const OrderId entryId = bracket.bracketId * 3 + 0;
  const OrderId tpId = bracket.bracketId * 3 + 1;
  const OrderId stopId = bracket.bracketId * 3 + 2;

  BracketStatus st;
  st.bracketId = bracket.bracketId;
  st.state = BracketState::PENDING_ENTRY;
  st.entryOrderId = entryId;
  st.tpOrderId = tpId;
  st.stopOrderId = stopId;
  _brackets[bracket.bracketId] = st;
  _bracketTemplates[bracket.bracketId] = bracket;
  _legToBracket[entryId] = bracket.bracketId;

  submitOrder(legToOrder(bracket.entry, entryId, bracket.symbol,
                         bracket.entry.quantity));
}

void SimulatedExecutor::cancelBracket(uint64_t bracketId)
{
  auto it = _brackets.find(bracketId);
  if (it == _brackets.end())
  {
    return;
  }
  const auto& st = it->second;
  // Cancel every leg that is still resting in the book.
  if (st.state == BracketState::PENDING_ENTRY)
  {
    cancelOrder(st.entryOrderId);
  }
  if (st.state == BracketState::ENTRY_FILLED)
  {
    cancelOrder(st.tpOrderId);
    cancelOrder(st.stopOrderId);
  }
  // ENTRY_FILLED → TP_FILLED/STOP_FILLED transitions already cancel the sibling.
  it->second.state = BracketState::CANCELED;
  _legToBracket.erase(st.entryOrderId);
  _legToBracket.erase(st.tpOrderId);
  _legToBracket.erase(st.stopOrderId);
  _bracketTemplates.erase(bracketId);
}

BracketStatus SimulatedExecutor::bracketStatus(uint64_t bracketId) const
{
  auto it = _brackets.find(bracketId);
  return it == _brackets.end() ? BracketStatus{} : it->second;
}

void SimulatedExecutor::onBracketFillEvent(const Order& filledOrder)
{
  auto legIt = _legToBracket.find(filledOrder.id);
  if (legIt == _legToBracket.end())
  {
    return;
  }
  const uint64_t bracketId = legIt->second;
  auto stIt = _brackets.find(bracketId);
  auto tplIt = _bracketTemplates.find(bracketId);
  if (stIt == _brackets.end() || tplIt == _bracketTemplates.end())
  {
    return;
  }
  auto& st = stIt->second;
  const BracketOrder& tpl = tplIt->second;

  if (filledOrder.id == st.entryOrderId)
  {
    const bool entryFullyFilled =
        filledOrder.filledQuantity.raw() >= filledOrder.quantity.raw();

    if (_bracketArmMode == BracketArmMode::OnFullFill)
    {
      if (st.state != BracketState::PENDING_ENTRY || !entryFullyFilled)
      {
        return;
      }
      // Entry fully filled: submit TP + stop scaled to the actual
      // entry fill quantity.
      st.entryFilled = filledOrder.filledQuantity;
      st.state = BracketState::ENTRY_FILLED;
      _legToBracket[st.tpOrderId] = bracketId;
      _legToBracket[st.stopOrderId] = bracketId;
      submitOrder(legToOrder(tpl.takeProfit, st.tpOrderId, tpl.symbol,
                             st.entryFilled));
      submitOrder(legToOrder(tpl.stop, st.stopOrderId, tpl.symbol,
                             st.entryFilled));
      return;
    }

    // OnPartialFill mode: arm / resize children incrementally
    // on every partial fill.
    if (st.state == BracketState::PENDING_ENTRY)
    {
      // First fill (partial or full): arm both children at the
      // current entry-fill quantity.
      st.entryFilled = filledOrder.filledQuantity;
      st.state = BracketState::ENTRY_FILLED;
      _legToBracket[st.tpOrderId] = bracketId;
      _legToBracket[st.stopOrderId] = bracketId;
      submitOrder(legToOrder(tpl.takeProfit, st.tpOrderId, tpl.symbol,
                             st.entryFilled));
      submitOrder(legToOrder(tpl.stop, st.stopOrderId, tpl.symbol,
                             st.entryFilled));
      return;
    }
    if (st.state == BracketState::ENTRY_FILLED &&
        filledOrder.filledQuantity.raw() > st.entryFilled.raw())
    {
      // Subsequent partial / final fill: resize children to the new
      // entry-fill quantity via replace.
      st.entryFilled = filledOrder.filledQuantity;
      Order tpReplace = legToOrder(tpl.takeProfit, st.tpOrderId,
                                   tpl.symbol, st.entryFilled);
      Order stopReplace = legToOrder(tpl.stop, st.stopOrderId,
                                     tpl.symbol, st.entryFilled);
      replaceOrder(st.tpOrderId, tpReplace);
      replaceOrder(st.stopOrderId, stopReplace);
      return;
    }
    return;
  }

  if (filledOrder.id == st.tpOrderId && st.state == BracketState::ENTRY_FILLED)
  {
    st.state = BracketState::TP_FILLED;
    cancelOrder(st.stopOrderId);
    return;
  }
  if (filledOrder.id == st.stopOrderId && st.state == BracketState::ENTRY_FILLED)
  {
    st.state = BracketState::STOP_FILLED;
    cancelOrder(st.tpOrderId);
    return;
  }
}

bool SimulatedExecutor::bookFeedAllowed()
{
  if (!_venue)
  {
    return true;
  }
  const int64_t now = _clock.nowNs().raw();
  const bool up = _venue->isUp(now);
  if (!up && _venueWasUp)
  {
    applyOutagePolicy();
    _venueWasUp = false;
  }
  else if (up && !_venueWasUp)
  {
    _venueWasUp = true;
    flushOutageBuffer();
  }
  // False during a Total or StaleBook outage: the caller drops the frame.
  return _venue->bookUpdatesAllowed(now);
}

void SimulatedExecutor::onBookUpdate(SymbolId symbol, const std::pmr::vector<BookLevel>& bids,
                                     const std::pmr::vector<BookLevel>& asks)
{
  if (!bookFeedAllowed())
  {
    return;
  }
  MarketState& state = getMarketState(symbol);

  state.hasBid = !bids.empty();
  state.hasAsk = !asks.empty();
  if (state.hasBid)
  {
    state.bestBidRaw = bids[0].price.raw();
    state.bestBidQtyRaw = bids[0].quantity.raw();
  }
  if (state.hasAsk)
  {
    state.bestAskRaw = asks[0].price.raw();
    state.bestAskQtyRaw = asks[0].quantity.raw();
  }

  // Snapshot the visible ladder. Takers eat it, so it is replaced wholesale on
  // every update: what an order consumed is restored by the next book event,
  // which is the venue reporting the level again.
  {
    DepthLadder& ladder = _ladders[symbol];
    ladder.bids.clear();
    ladder.asks.clear();
    ladder.bids.reserve(bids.size());
    ladder.asks.reserve(asks.size());
    for (const auto& lvl : bids)
    {
      if (lvl.quantity.raw() > 0)
      {
        ladder.bids.emplace_back(lvl.price.raw(), lvl.quantity.raw());
      }
    }
    for (const auto& lvl : asks)
    {
      if (lvl.quantity.raw() > 0)
      {
        ladder.asks.emplace_back(lvl.price.raw(), lvl.quantity.raw());
      }
    }
  }

  if (_queueTracker.enabled())
  {
    // First update every level present in the snapshot.
    for (const auto& lvl : bids)
    {
      _queueTracker.onLevelUpdate(symbol, Side::BUY, lvl.price, lvl.quantity);
    }
    for (const auto& lvl : asks)
    {
      _queueTracker.onLevelUpdate(symbol, Side::SELL, lvl.price, lvl.quantity);
    }
    // Then zero-out any tracked level that disappeared from the snapshot.
    // Book providers usually drop empty levels rather than reporting qty=0;
    // without this pass, our queue tracker would keep stale `level.totalQty`
    // values for those levels and LevelEmpty would never fire.
    std::vector<Price> trackedBids;
    _queueTracker.trackedPrices(symbol, Side::BUY, trackedBids);
    for (const Price& p : trackedBids)
    {
      bool present = false;
      for (const auto& lvl : bids)
      {
        if (lvl.price.raw() == p.raw())
        {
          present = true;
          break;
        }
      }
      if (!present)
      {
        _queueTracker.onLevelUpdate(symbol, Side::BUY, p, Quantity::fromRaw(0));
      }
    }
    std::vector<Price> trackedAsks;
    _queueTracker.trackedPrices(symbol, Side::SELL, trackedAsks);
    for (const Price& p : trackedAsks)
    {
      bool present = false;
      for (const auto& lvl : asks)
      {
        if (lvl.price.raw() == p.raw())
        {
          present = true;
          break;
        }
      }
      if (!present)
      {
        _queueTracker.onLevelUpdate(symbol, Side::SELL, p, Quantity::fromRaw(0));
      }
    }
    maybeEmitQueuePositionChanges();
  }
  maybeEmitMarketPositionChanges();

  finalizePendingSubmissions();
  finalizePendingCancels();
  finalizePendingReplaces();
  processExpiredOrders();
  processPendingOrders(symbol, state);
  processConditionalOrders(symbol, state);
}

void SimulatedExecutor::onTopOfBook(SymbolId symbol, const std::pmr::vector<BookLevel>& bids,
                                    const std::pmr::vector<BookLevel>& asks)
{
  if (!bookFeedAllowed())
  {
    return;
  }
  MarketState& state = getMarketState(symbol);

  state.hasBid = !bids.empty();
  state.hasAsk = !asks.empty();
  if (state.hasBid)
  {
    state.bestBidRaw = bids[0].price.raw();
    state.bestBidQtyRaw = bids[0].quantity.raw();
  }
  if (state.hasAsk)
  {
    state.bestAskRaw = asks[0].price.raw();
    state.bestAskQtyRaw = asks[0].quantity.raw();
  }

  // Levels more aggressive than the touch are gone, the touch level takes
  // the reported size, deeper ones are not the frame's to update.
  {
    DepthLadder& ladder = _ladders[symbol];
    auto reconcile = [](std::vector<std::pair<int64_t, int64_t>>& levels, bool has,
                        int64_t touchRaw, int64_t touchQtyRaw, bool isBid)
    {
      if (!has)
      {
        levels.clear();
        return;
      }
      size_t gone = 0;
      while (gone < levels.size() && (isBid ? levels[gone].first > touchRaw
                                            : levels[gone].first < touchRaw))
      {
        ++gone;
      }
      if (gone > 0)
      {
        levels.erase(levels.begin(), levels.begin() + static_cast<std::ptrdiff_t>(gone));
      }
      if (!levels.empty() && levels.front().first == touchRaw)
      {
        levels.front().second = touchQtyRaw;
        if (touchQtyRaw == 0)
        {
          levels.erase(levels.begin());
        }
      }
      else if (touchQtyRaw > 0)
      {
        levels.insert(levels.begin(), {touchRaw, touchQtyRaw});
      }
    };
    reconcile(ladder.bids, state.hasBid, state.bestBidRaw, state.bestBidQtyRaw,
              /*isBid=*/true);
    reconcile(ladder.asks, state.hasAsk, state.bestAskRaw, state.bestAskQtyRaw,
              /*isBid=*/false);
  }

  if (_queueTracker.enabled())
  {
    // Same assertion against the tracked queue levels.
    auto sweep = [&](Side side, bool has, int64_t touchRaw, int64_t touchQtyRaw)
    {
      std::vector<Price> tracked;
      _queueTracker.trackedPrices(symbol, side, tracked);
      for (const Price& p : tracked)
      {
        const bool emptied = !has || (side == Side::BUY ? p.raw() > touchRaw
                                                        : p.raw() < touchRaw);
        if (emptied)
        {
          _queueTracker.onLevelUpdate(symbol, side, p, Quantity::fromRaw(0));
        }
      }
      if (has)
      {
        _queueTracker.onLevelUpdate(symbol, side, Price::fromRaw(touchRaw),
                                    Quantity::fromRaw(touchQtyRaw));
      }
    };
    sweep(Side::BUY, state.hasBid, state.bestBidRaw, state.bestBidQtyRaw);
    sweep(Side::SELL, state.hasAsk, state.bestAskRaw, state.bestAskQtyRaw);
    maybeEmitQueuePositionChanges();
  }
  maybeEmitMarketPositionChanges();

  finalizePendingSubmissions();
  finalizePendingCancels();
  finalizePendingReplaces();
  processExpiredOrders();
  processPendingOrders(symbol, state);
  processConditionalOrders(symbol, state);
}

void SimulatedExecutor::settleQueuedFills()
{
  // Native iceberg: top up the visible tranche from the hidden remainder
  // before the fully-filled-orders cleanup pass. The refresh reads the level's
  // remaining depth to place the new tranche, so the tracker has not compacted
  // its levels yet.
  for (auto& o : _pending_orders)
  {
    maybeRefreshIceberg(o);
  }
  _queueTracker.compact();
  // Remove fully-filled queued orders and drop their queue position +
  // timestamp tracking entries. Iceberg orders with hidden remainder or a
  // pending refresh deadline are kept alive so the next tick can finalize the
  // refresh.
  auto icebergPending = [this](const Order& o)
  {
    auto it = _iceberg.find(o.id);
    return it != _iceberg.end() &&
           (it->second.hiddenRaw > 0 || it->second.refreshDueNs > 0);
  };
  for (const auto& o : _pending_orders)
  {
    if (o.filledQuantity.raw() >= o.quantity.raw() && !icebergPending(o))
    {
      forgetQueuePosition(o.id);
      forgetTimestamps(o.id);
      forgetMarketPosition(o.id);
      forgetPendingReplace(o.id);
      _iceberg.erase(o.id);
    }
  }
  _pending_orders.erase(
      std::remove_if(_pending_orders.begin(), _pending_orders.end(),
                     [&](const Order& o)
                     {
                       return o.filledQuantity.raw() >= o.quantity.raw() &&
                              !icebergPending(o);
                     }),
      _pending_orders.end());
}

void SimulatedExecutor::onTrade(SymbolId symbol, Price price, bool isBuy)
{
  onTrade(symbol, price, Quantity::fromRaw(0), isBuy);
}

void SimulatedExecutor::onTrade(SymbolId symbol, Price price, Quantity qty, bool isBuy)
{
  if (_venue)
  {
    const int64_t now = _clock.nowNs().raw();
    const bool up = _venue->isUp(now);
    if (!up && _venueWasUp)
    {
      applyOutagePolicy();
      _venueWasUp = false;
    }
    else if (up && !_venueWasUp)
    {
      _venueWasUp = true;
      flushOutageBuffer();
    }
    if (!_venue->tradesAllowed(now))
    {
      return;
    }
  }
  MarketState& state = getMarketState(symbol);
  state.lastTradeRaw = price.raw();
  state.hasTrade = true;

  if (_queueTracker.enabled() && qty.raw() > 0)
  {
    _queueFillBuffer.clear();
    _queueTracker.onTrade(symbol, price, qty, isBuy ? Side::BUY : Side::SELL,
                          _queueFillBuffer);
    for (const auto& [orderId, fillQty] : _queueFillBuffer)
    {
      // Re-resolve on every iteration: a fill callback can submit, cancel or
      // clear orders, which moves the pending-order storage under us.
      Order* ord = findPendingOrder(orderId);
      if (!ord)
      {
        continue;
      }
      // Fill came from queue consumption — our order rested and an
      // aggressive opposite trade walked into it.
      executeFill(*ord, price, fillQty, /*isMaker=*/true);
    }
    settleQueuedFills();
  }

  maybeEmitQueuePositionChanges();
  maybeEmitMarketPositionChanges();

  finalizePendingSubmissions();
  finalizePendingCancels();
  finalizePendingReplaces();
  processExpiredOrders();
  processPendingOrders(symbol, state);
  processConditionalOrders(symbol, state);
  updateTrailingStops(symbol, price);
}

bool SimulatedExecutor::barFeedAllowed()
{
  if (!_venue)
  {
    return true;
  }
  const int64_t now = _clock.nowNs().raw();
  const bool up = _venue->isUp(now);
  if (!up && _venueWasUp)
  {
    applyOutagePolicy();
    _venueWasUp = false;
  }
  else if (up && !_venueWasUp)
  {
    _venueWasUp = true;
    flushOutageBuffer();
  }
  return up;
}

void SimulatedExecutor::onBar(SymbolId symbol, Price price)
{
  if (!barFeedAllowed())
  {
    return;
  }
  stepBarPrice(symbol, price);
}

void SimulatedExecutor::onBar(SymbolId symbol, Price high, Price low, Price close)
{
  if (!barFeedAllowed())
  {
    return;
  }
  // low first (a long's protective stop is tested before its target), then the
  // opposite extreme, then settle at close. Each step runs the full matching
  // pass so wick-triggered stops/TPs fill.
  stepBarPrice(symbol, low);
  stepBarPrice(symbol, high);
  stepBarPrice(symbol, close);
}

void SimulatedExecutor::onBar(SymbolId symbol, Price open, Price high, Price low,
                              Price close)
{
  if (!barFeedAllowed())
  {
    return;
  }
  // The open is in the market state before the held orders are released, so an
  // order the previous bar's callback produced trades at this open -- the
  // first price that exists after the strategy could have decided to send it.
  setBarMarketState(symbol, open);
  releaseHeldOrders(symbol);
  stepBarPrice(symbol, open);
  stepBarPrice(symbol, low);
  stepBarPrice(symbol, high);
  stepBarPrice(symbol, close);
}

void SimulatedExecutor::releaseHeldOrders(SymbolId symbol)
{
  if (_heldBarOrders.empty() || _barCallbackDepth > 0)
  {
    return;
  }
  // Submitting runs order-event callbacks, which can submit or cancel more
  // orders. Lift this symbol's orders out of the hold list first so the list
  // stays consistent whatever the callbacks do with it.
  std::vector<Order> released;
  for (size_t i = 0; i < _heldBarOrders.size();)
  {
    if (_heldBarOrders[i].symbol == symbol)
    {
      released.push_back(std::move(_heldBarOrders[i]));
      _heldBarOrders.erase(_heldBarOrders.begin() + static_cast<std::ptrdiff_t>(i));
    }
    else
    {
      ++i;
    }
  }
  for (const Order& order : released)
  {
    // Full submit path: the order reaches the venue now, so rate limits,
    // reduce-only and self-trade prevention are all evaluated against the
    // state at the open rather than the state the strategy saw.
    submitOrder(order);
  }
}

void SimulatedExecutor::setBarMarketState(SymbolId symbol, Price price)
{
  // A bar reports no depth. Dropping the ladder keeps a stale book from being
  // walked at prices this bar has already left behind: on bar data every size
  // trades at the step price, which is the only thing the bar says.
  _ladders.erase(symbol);

  MarketState& state = getMarketState(symbol);
  const int64_t priceRaw = price.raw();
  state.bestBidRaw = priceRaw;
  state.bestAskRaw = priceRaw;
  state.lastTradeRaw = priceRaw;
  state.hasBid = true;
  state.hasAsk = true;
  state.hasTrade = true;
}

void SimulatedExecutor::driveQueueFromBarStep(SymbolId symbol, Price stepPrice)
{
  if (!_queueTracker.enabled() || _pending_orders.empty())
  {
    return;
  }
  const int64_t stepRaw = stepPrice.raw();

  // One synthetic print per level, not per order: two of our orders resting at
  // the same price are behind the same queue, and printing twice would trade
  // that queue twice. The print is sized to reach the order standing furthest
  // back, so the model still decides how much of it each order gets.
  struct LevelPrint
  {
    Side side{};
    int64_t priceRaw{0};
    int64_t qtyRaw{0};
  };
  std::vector<LevelPrint> prints;

  for (const auto& order : _pending_orders)
  {
    if (order.symbol != symbol || !fillsAsLimit(order.type))
    {
      continue;
    }
    // Strictly through, not merely touched: for the bar to print past the
    // level, everything resting there had to trade. A bar that only reaches
    // the price says nothing about whether our turn in the queue came up.
    const bool tradedThrough = (order.side == Side::BUY) ? (stepRaw < order.price.raw())
                                                         : (stepRaw > order.price.raw());
    if (!tradedThrough)
    {
      continue;
    }
    const int64_t remainingRaw = order.quantity.raw() - order.filledQuantity.raw();
    if (remainingRaw <= 0)
    {
      continue;
    }
    const auto snap = _queueTracker.snapshot(order.id);
    if (!snap)
    {
      continue;
    }
    const int64_t reachRaw = snap->ahead.raw() + remainingRaw;

    LevelPrint* existing = nullptr;
    for (auto& lp : prints)
    {
      if (lp.side == order.side && lp.priceRaw == order.price.raw())
      {
        existing = &lp;
        break;
      }
    }
    if (existing == nullptr)
    {
      prints.push_back(LevelPrint{order.side, order.price.raw(), reachRaw});
    }
    else if (reachRaw > existing->qtyRaw)
    {
      existing->qtyRaw = reachRaw;
    }
  }

  for (const LevelPrint& lp : prints)
  {
    // Our resting bid is taken out by a seller, our resting offer by a buyer.
    const Side aggressor = (lp.side == Side::BUY) ? Side::SELL : Side::BUY;
    const Price levelPrice = Price::fromRaw(lp.priceRaw);
    _queueFillBuffer.clear();
    _queueTracker.onTrade(symbol, levelPrice, Quantity::fromRaw(lp.qtyRaw), aggressor,
                          _queueFillBuffer);
    for (const auto& [orderId, fillQty] : _queueFillBuffer)
    {
      // Re-resolve every time: a fill callback can submit or cancel orders,
      // which moves the pending-order storage under us.
      Order* ord = findPendingOrder(orderId);
      if (ord == nullptr || fillQty.raw() <= 0)
      {
        continue;
      }
      executeFill(*ord, levelPrice, fillQty, /*isMaker=*/true);
    }
  }

  if (!prints.empty())
  {
    settleQueuedFills();
    maybeEmitQueuePositionChanges();
  }
}

void SimulatedExecutor::stepBarPrice(SymbolId symbol, Price price)
{
  setBarMarketState(symbol, price);
  MarketState& state = getMarketState(symbol);
  finalizePendingSubmissions();
  finalizePendingCancels();
  finalizePendingReplaces();
  processExpiredOrders();
  // Bar data carries no trade stream, so without this the queue tracker never
  // hears a print and every resting limit sits untouched for the whole run.
  driveQueueFromBarStep(symbol, price);
  processPendingOrders(symbol, state);
  processConditionalOrders(symbol, state);
  updateTrailingStops(symbol, price);
}

SimulatedExecutor::MarketState& SimulatedExecutor::getMarketState(SymbolId symbol)
{
  if (symbol < kMaxSymbols) [[likely]]
  {
    return _marketStatesFlat[symbol];
  }

  for (auto& [id, state] : _marketStatesOverflow)
  {
    if (id == symbol)
    {
      return state;
    }
  }
  _marketStatesOverflow.emplace_back(symbol, MarketState{});
  return _marketStatesOverflow.back().second;
}

Price SimulatedExecutor::bestBidPrice(SymbolId symbol) const
{
  if (symbol < kMaxSymbols)
  {
    const auto& s = _marketStatesFlat[symbol];
    return s.hasBid ? Price::fromRaw(s.bestBidRaw) : Price::fromRaw(0);
  }
  for (const auto& [id, s] : _marketStatesOverflow)
  {
    if (id == symbol)
    {
      return s.hasBid ? Price::fromRaw(s.bestBidRaw) : Price::fromRaw(0);
    }
  }
  return Price::fromRaw(0);
}

Price SimulatedExecutor::bestAskPrice(SymbolId symbol) const
{
  if (symbol < kMaxSymbols)
  {
    const auto& s = _marketStatesFlat[symbol];
    return s.hasAsk ? Price::fromRaw(s.bestAskRaw) : Price::fromRaw(0);
  }
  for (const auto& [id, s] : _marketStatesOverflow)
  {
    if (id == symbol)
    {
      return s.hasAsk ? Price::fromRaw(s.bestAskRaw) : Price::fromRaw(0);
    }
  }
  return Price::fromRaw(0);
}

Price SimulatedExecutor::bookMidPrice(SymbolId symbol) const
{
  const Price bid = bestBidPrice(symbol);
  const Price ask = bestAskPrice(symbol);
  if (bid.raw() <= 0 || ask.raw() <= 0)
  {
    return Price::fromRaw(0);
  }
  return Price::fromRaw((bid.raw() + ask.raw()) / 2);
}

Order* SimulatedExecutor::findPendingOrder(OrderId orderId)
{
  for (auto& order : _pending_orders)
  {
    if (order.id == orderId)
    {
      return &order;
    }
  }
  return nullptr;
}

Order* SimulatedExecutor::findConditionalOrder(OrderId orderId)
{
  for (auto& order : _conditional_orders)
  {
    if (order.id == orderId)
    {
      return &order;
    }
  }
  return nullptr;
}

const Order* SimulatedExecutor::findOrder(OrderId orderId) const
{
  for (const auto* book : {&_heldBarOrders, &_pending_orders, &_conditional_orders})
  {
    for (const Order& order : *book)
    {
      if (order.id == orderId)
      {
        return &order;
      }
    }
  }
  return nullptr;
}

SimulatedExecutor::DepthLadder* SimulatedExecutor::findLadder(SymbolId symbol)
{
  auto it = _ladders.find(symbol);
  return it == _ladders.end() ? nullptr : &it->second;
}

SimulatedExecutor::LadderWalk SimulatedExecutor::consumeLadder(SymbolId symbol, Side side,
                                                               int64_t qtyRaw,
                                                               int64_t limitPriceRaw)
{
  LadderWalk walk;
  DepthLadder* ladder = findLadder(symbol);
  if (ladder == nullptr)
  {
    return walk;
  }
  auto& levels = (side == Side::BUY) ? ladder->asks : ladder->bids;
  if (levels.empty())
  {
    return walk;
  }
  walk.walked = true;

  int64_t left = qtyRaw;
  size_t emptied = 0;
  for (auto& [priceRaw, levelQtyRaw] : levels)
  {
    if (left <= 0)
    {
      break;
    }
    // A limit order stops where its own price stops; a market order (limit 0)
    // keeps walking down the ladder.
    if (limitPriceRaw != 0)
    {
      const bool eligible = (side == Side::BUY) ? (priceRaw <= limitPriceRaw)
                                                : (priceRaw >= limitPriceRaw);
      if (!eligible)
      {
        break;
      }
    }
    const int64_t take = std::min(left, levelQtyRaw);
    if (take > 0)
    {
      // Notional through the fixed-point helpers, not doubles: a raw price
      // times a raw quantity is a 1e16-scaled product that only the widened
      // Quantity * Price -> Volume path carries without wrapping.
      walk.notional = walk.notional + (Quantity::fromRaw(take) * Price::fromRaw(priceRaw));
      walk.takenRaw += take;
      walk.worstPriceRaw = priceRaw;
      ++walk.levels;
      left -= take;
      levelQtyRaw -= take;
    }
    if (levelQtyRaw == 0)
    {
      ++emptied;
    }
  }

  if (emptied > 0)
  {
    levels.erase(levels.begin(), levels.begin() + static_cast<std::ptrdiff_t>(emptied));
  }

  // Republish the touch the walk left behind. The market state and the ladder
  // have to agree, or the next order's crossing check would be answered by a
  // level that is no longer there.
  MarketState& state = getMarketState(symbol);
  const bool empty = levels.empty();
  if (side == Side::BUY)
  {
    state.hasAsk = !empty;
    state.bestAskRaw = empty ? 0 : levels.front().first;
    state.bestAskQtyRaw = empty ? 0 : levels.front().second;
  }
  else
  {
    state.hasBid = !empty;
    state.bestBidRaw = empty ? 0 : levels.front().first;
    state.bestBidQtyRaw = empty ? 0 : levels.front().second;
  }
  return walk;
}

bool SimulatedExecutor::tryFillOrder(Order& order, bool resting, int64_t triggerBoundRaw)
{
  const MarketState& state = getMarketState(order.symbol);
  int64_t fillPriceRaw = 0;
  int64_t levelQtyRaw = 0;
  bool canFill = false;

  if (fillsAsMarket(order.type))
  {
    if (order.side == Side::BUY && state.hasAsk)
    {
      fillPriceRaw = state.bestAskRaw;
      levelQtyRaw = state.bestAskQtyRaw;
      canFill = true;
    }
    else if (order.side == Side::SELL && state.hasBid)
    {
      fillPriceRaw = state.bestBidRaw;
      levelQtyRaw = state.bestBidQtyRaw;
      canFill = true;
    }
    else if (state.hasTrade)
    {
      fillPriceRaw = state.lastTradeRaw;
      canFill = true;
    }
  }
  else
  {
    const int64_t orderPriceRaw = order.price.raw();
    if (order.side == Side::BUY && state.hasAsk && orderPriceRaw >= state.bestAskRaw)
    {
      fillPriceRaw = state.bestAskRaw;
      levelQtyRaw = state.bestAskQtyRaw;
      canFill = true;
    }
    else if (order.side == Side::SELL && state.hasBid && orderPriceRaw <= state.bestBidRaw)
    {
      fillPriceRaw = state.bestBidRaw;
      levelQtyRaw = state.bestBidQtyRaw;
      canFill = true;
    }
  }

  if (!canFill)
  {
    return false;
  }

  const Quantity remainingQty =
      Quantity::fromRaw(order.quantity.raw() - order.filledQuantity.raw());

  // A limit order that was not marketable when it arrived is sitting in the
  // book. Whoever crossed it had to trade through its price to get where the
  // touch now is, so it trades at the price it posted and it provided the
  // liquidity. The gap between its price and the far touch is not a price
  // improvement the resting side collects -- no venue hands that out, and on
  // coarse data the gap is the whole bar.
  bool isMaker = false;
  if (resting && !fillsAsMarket(order.type))
  {
    fillPriceRaw = order.price.raw();
    isMaker = true;
  }

  // A stop-limit armed at a price past its own limit cannot trade there: it
  // rests instead, which is what a venue does with a stop-limit triggered on a
  // gap. Decided before the walk, so a refused fill does not eat depth on its
  // way out.
  if (triggerBoundRaw != 0 && !fillsAsMarket(order.type))
  {
    const bool beyondLimit =
        (order.side == Side::BUY && triggerBoundRaw > order.price.raw()) ||
        (order.side == Side::SELL && triggerBoundRaw < order.price.raw());
    if (beyondLimit)
    {
      return false;
    }
  }

  // A taker walks the visible ladder instead of printing its whole size at the
  // touch, and what it ate is gone for the next order in this same step. The
  // fill is reported once, at the volume-weighted price of the walk.
  //
  // Size the ladder cannot cover is priced at the deepest level the walk
  // reached -- for the whole order, not just the excess. Past that level the
  // feed said nothing, and averaging the unknown remainder in at the touch is
  // the one answer that is certainly wrong: it makes size free, which is the
  // bug this replaces. The order still fills in full, so a thin book slows
  // nothing down; it just stops being cheap.
  bool averaged = false;
  if (!isMaker)
  {
    const int64_t limitRaw = fillsAsMarket(order.type) ? 0 : order.price.raw();
    const LadderWalk walk =
        consumeLadder(order.symbol, order.side, remainingQty.raw(), limitRaw);
    if (walk.walked && walk.takenRaw > 0)
    {
      averaged = walk.takenRaw >= remainingQty.raw() && walk.levels > 1;
      fillPriceRaw = (walk.takenRaw >= remainingQty.raw())
                         ? (walk.notional / Quantity::fromRaw(walk.takenRaw)).raw()
                         : walk.worstPriceRaw;
    }
  }

  // A conditional order may not print better than the price that armed it. On
  // bar data the trigger is tested against a synthetic bid = ask = extreme, so
  // without this bound a take-profit sell at 105 books the bar's 110 high -- a
  // price the order had no claim on, since it only existed once the bar was
  // over. The bound is one-sided: a stop armed at 95 on a bar that traded to
  // 90 still pays 90, because that is the adverse side and the order really
  // was in the market for it.
  if (triggerBoundRaw != 0)
  {
    fillPriceRaw = (order.side == Side::BUY) ? std::max(fillPriceRaw, triggerBoundRaw)
                                             : std::min(fillPriceRaw, triggerBoundRaw);
  }

  // Apply slippage only to market-style fills (limit makers trade at posted price).
  // A price the model moved off the tick is snapped back onto it against the
  // order's owner, since a single level only trades on the tick. An average
  // over several levels is not a traded price and stays as it is.
  if (fillsAsMarket(order.type))
  {
    const int64_t slippedRaw = applySlippage(fillPriceRaw, order.side, order.symbol,
                                             remainingQty, levelQtyRaw);
    fillPriceRaw = (slippedRaw != fillPriceRaw && !averaged)
                       ? snapAgainstOwner(slippedRaw, orderTickRaw(order.symbol), order.side)
                       : slippedRaw;
  }
  executeFill(order, Price::fromRaw(fillPriceRaw), remainingQty, isMaker);
  return true;
}

void SimulatedExecutor::processPendingOrders(SymbolId symbol, const MarketState&)
{
  // A fill callback is a strategy callback: it can submit, cancel, replace or
  // clear orders, and every one of those reshapes _pending_orders. Collect the
  // candidates up front and resolve each one by id so no reference or index
  // has to survive a callback.
  _fillScanIds.clear();
  for (const auto& order : _pending_orders)
  {
    if (order.symbol != symbol)
    {
      continue;
    }
    // Orders the queue tracker holds are its business: it fills them from
    // prints once their queue is gone. The test is registration, not the
    // model being on -- a limit the tracker never took (a stop-limit that
    // rested when it triggered, or an order submitted before the model was
    // configured) has no queue to wait behind, and skipping it on the model
    // alone left it resting for the whole run.
    if (fillsAsLimit(order.type) && _queueTracker.enabled() &&
        _queueTracker.snapshot(order.id).has_value())
    {
      continue;
    }
    _fillScanIds.push_back(order.id);
  }

  for (const OrderId id : _fillScanIds)
  {
    Order* order = findPendingOrder(id);
    if (!order || !tryFillOrder(*order, /*resting=*/true))
    {
      continue;
    }
    removePendingOrder(id);
  }
}

void SimulatedExecutor::removePendingOrder(OrderId orderId)
{
  for (size_t i = 0; i < _pending_orders.size(); ++i)
  {
    if (_pending_orders[i].id == orderId)
    {
      _pending_orders[i] = _pending_orders.back();
      _pending_orders.pop_back();
      return;
    }
  }
}

void SimulatedExecutor::maybeRefreshIceberg(Order& order)
{
  auto it = _iceberg.find(order.id);
  if (it == _iceberg.end())
  {
    return;
  }
  auto& st = it->second;
  // Refresh only when the visible tranche is fully consumed and
  // there is hidden remainder.
  if (order.filledQuantity.raw() < order.quantity.raw() || st.hiddenRaw <= 0)
  {
    return;
  }
  const int64_t now = _clock.nowNs().raw();
  if (st.refreshDueNs == 0)
  {
    st.refreshDueNs = now + st.refreshLatencyNs;
  }
  if (now < st.refreshDueNs)
  {
    return;
  }
  int64_t slice = std::min<int64_t>(st.visibleRaw, st.hiddenRaw);
  // Size randomisation: jitter the visible-slice size by a
  // uniform fraction in [-pct, +pct] of st.visibleRaw, then clamp to
  // [1, hiddenRaw]. Mean is preserved so a long sample sequence
  // averages back to st.visibleRaw.
  if (_icebergSizeRandomisationPct > 0.0)
  {
    std::uniform_real_distribution<double> u(-_icebergSizeRandomisationPct,
                                             _icebergSizeRandomisationPct);
    const double frac = u(_icebergJitterRng);
    int64_t jittered = static_cast<int64_t>(
        static_cast<double>(st.visibleRaw) * (1.0 + frac));
    if (jittered < 1)
    {
      jittered = 1;
    }
    if (jittered > st.hiddenRaw)
    {
      jittered = st.hiddenRaw;
    }
    slice = jittered;
  }
  st.hiddenRaw -= slice;
  st.refreshDueNs = 0;
  order.quantity = Quantity::fromRaw(order.quantity.raw() + slice);
  // Priority mode. Back (the default) queues the refreshed tranche behind
  // whatever is still resting at the level, the way most crypto venues treat a
  // refreshed slice. Retain keeps the queue position the consumed tranche held.
  // Either way the tranche has to go back into the tracker: the consumed entry
  // was already removed when it filled, and an order with no entry never
  // trades again.
  if (_queueTracker.enabled())
  {
    _queueTracker.refreshOrder(
        order.symbol, order.side, order.price, order.id,
        Quantity::fromRaw(slice),
        _icebergPriorityMode == IcebergPriorityMode::Back);
  }
}

void SimulatedExecutor::setIcebergPriorityModeByName(const std::string& name)
{
  std::string lower;
  lower.reserve(name.size());
  for (char c : name)
  {
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (lower == "retain")
  {
    _icebergPriorityMode = IcebergPriorityMode::Retain;
  }
  else if (lower == "back")
  {
    _icebergPriorityMode = IcebergPriorityMode::Back;
  }
}

void SimulatedExecutor::executeFill(Order& order, Price price, Quantity qty, bool isMaker)
{
  // An order never trades more than it has left. The queue tracker holds its
  // own copy of that remainder, and anything that shrinks the order after
  // registration has to shrink both; this clamp keeps a mismatch from turning
  // into size the strategy never had.
  const int64_t remainingRaw = order.quantity.raw() - order.filledQuantity.raw();
  const int64_t fillRaw = std::min(qty.raw(), remainingRaw);
  if (fillRaw <= 0)
  {
    return;
  }
  const Quantity fillQty = Quantity::fromRaw(fillRaw);

  const UnixNanos now = _clock.nowNs();

  _fills.push_back({.orderId = order.id,
                    .symbol = order.symbol,
                    .side = order.side,
                    .price = price,
                    .quantity = fillQty,
                    .timestampNs = now,
                    .isMaker = isMaker});

  // Update simulator-side net position for reduce_only enforcement.
  const int64_t signedRaw = (order.side == Side::BUY) ? fillRaw : -fillRaw;
  _netPositionRaw[order.symbol] += signedRaw;

  order.filledQuantity = Quantity::fromRaw(order.filledQuantity.raw() + fillRaw);

  OrderEvent ev;
  ev.order = order;
  ev.fillQty = fillQty;
  ev.fillPrice = price;
  ev.isMaker = isMaker;
  ev.exchangeTsNs = now;
  ev.status =
      (order.filledQuantity >= order.quantity) ? OrderEventStatus::FILLED : OrderEventStatus::PARTIALLY_FILLED;
  if (auto snap = _queueTracker.snapshot(order.id))
  {
    ev.queueAhead = snap->ahead;
    ev.queueTotal = snap->total;
  }
  auto& ts = timestampsFor(order.id);
  if (!ts.firstFillAtNs)
  {
    ts.firstFillAtNs = now;
  }
  ts.lastFillAtNs = now;
  ev.timestamps = ts;

  // Past this point `order` may no longer be there to read: the callback and
  // the handlers below re-enter the executor, and a submit or a cancel from a
  // strategy moves the pending-order storage. Everything that follows works
  // off this snapshot, which is complete because the order is fully mutated
  // above.
  const Order filled = order;

  if (_callback)
  {
    _callback(ev);
  }

  if (ev.status == OrderEventStatus::FILLED)
  {
    resolveLateCancelOnFill(filled);
    resolveLateReplaceOnFill(filled);
    _compositeLogic.onOrderFilled(filled);
    onBracketFillEvent(filled);
  }
  else if (ev.status == OrderEventStatus::PARTIALLY_FILLED)
  {
    // Bracket child-arm mode "on_partial_fill" arms / resizes
    // children on every partial entry fill; OnFullFill mode
    // returns early in the handler.
    onBracketFillEvent(filled);
  }
}

void SimulatedExecutor::emitEvent(OrderEventStatus status, const Order& order)
{
  if (!_callback)
  {
    return;
  }
  const UnixNanos nowT = _clock.nowNs();
  const int64_t nowNs = nowT.raw();
  auto& ts = timestampsFor(order.id);
  switch (status)
  {
    case OrderEventStatus::SUBMITTED:
      ts.submittedAtNs = nowT;
      break;
    case OrderEventStatus::ACCEPTED:
      ts.acceptedAtNs = nowT;
      break;
    case OrderEventStatus::CANCELED:
      ts.canceledAtNs = nowT;
      break;
    case OrderEventStatus::REJECTED:
      ts.rejectedAtNs = nowT;
      break;
    case OrderEventStatus::TRIGGERED:
      ts.triggeredAtNs = nowT;
      break;
    case OrderEventStatus::EXPIRED:
      ts.expiredAtNs = nowT;
      break;
    default:
      break;
  }
  OrderEvent ev;
  ev.status = status;
  ev.order = order;
  ev.exchangeTsNs = nowT;
  ev.timestamps = ts;
  _callback(ev);
}

void SimulatedExecutor::maybeEmitQueuePositionChanges()
{
  if (!_queueTracker.enabled() || !_callback || _queuePosMinFraction >= 1.0)
  {
    return;
  }
  _queueTracker.snapshotAll(_queueSnapshotBuffer);
  for (const auto& snap : _queueSnapshotBuffer)
  {
    auto it = _lastEmittedQueueAheadRaw.find(snap.orderId);
    const int64_t lastRaw = (it == _lastEmittedQueueAheadRaw.end())
                                ? snap.aheadAtArrival.raw()
                                : it->second;
    const int64_t deltaRaw =
        (snap.ahead.raw() > lastRaw) ? (snap.ahead.raw() - lastRaw) : (lastRaw - snap.ahead.raw());
    const int64_t denomRaw = (snap.aheadAtArrival.raw() > 0) ? snap.aheadAtArrival.raw() : 1;
    if (_queuePosMinFraction > 0.0 &&
        static_cast<double>(deltaRaw) <
            _queuePosMinFraction * static_cast<double>(denomRaw))
    {
      continue;
    }
    Order* ord = findPendingOrder(snap.orderId);
    if (!ord)
    {
      continue;
    }
    OrderEvent ev;
    ev.status = OrderEventStatus::QUEUE_POSITION_UPDATED;
    ev.order = *ord;
    ev.queueAhead = snap.ahead;
    ev.queueTotal = snap.total;
    ev.exchangeTsNs = _clock.nowNs();
    ev.timestamps = timestampsFor(snap.orderId);
    _callback(ev);
    _lastEmittedQueueAheadRaw[snap.orderId] = snap.ahead.raw();
  }
}

void SimulatedExecutor::forgetQueuePosition(OrderId orderId)
{
  _lastEmittedQueueAheadRaw.erase(orderId);
}

void SimulatedExecutor::forgetMarketPosition(OrderId orderId)
{
  _lastEmittedMarketPosition.erase(orderId);
}

int64_t SimulatedExecutor::sampleReplaceAckLatency()
{
  int64_t base = _replaceAckDist.sample(_cancelAckRng);
  if (_venue)
  {
    const double mult = _venue->latencyMultiplier(_clock.nowNs().raw());
    if (mult > 1.0)
    {
      base = static_cast<int64_t>(static_cast<double>(base) * mult);
    }
  }
  return base;
}

void SimulatedExecutor::enqueuePendingReplace(const Order& oldOrder, const Order& newOrder,
                                              bool untriggered)
{
  const int64_t now = _clock.nowNs().raw();
  _pendingReplaces.push_back(
      PendingReplace{.orderId = oldOrder.id,
                     .ackAtNs = now + sampleReplaceAckLatency(),
                     .oldSnapshot = oldOrder,
                     .newOrder = newOrder,
                     .untriggered = untriggered});
}

void SimulatedExecutor::forgetPendingReplace(OrderId orderId)
{
  for (auto it = _pendingReplaces.begin(); it != _pendingReplaces.end();)
  {
    if (it->orderId == orderId)
    {
      it = _pendingReplaces.erase(it);
    }
    else
    {
      ++it;
    }
  }
}

void SimulatedExecutor::finalizePendingReplaces()
{
  // A replacement applies whether or not anyone listens; only the events
  // need a callback.
  if (_pendingReplaces.empty())
  {
    return;
  }
  const int64_t now = _clock.nowNs().raw();
  size_t i = 0;
  while (i < _pendingReplaces.size())
  {
    if (_pendingReplaces[i].ackAtNs > now)
    {
      ++i;
      continue;
    }
    const OrderId orderId = _pendingReplaces[i].orderId;
    Order oldSnapshot = _pendingReplaces[i].oldSnapshot;
    Order newOrder = _pendingReplaces[i].newOrder;
    const bool untriggered = _pendingReplaces[i].untriggered;
    _pendingReplaces[i] = _pendingReplaces.back();
    _pendingReplaces.pop_back();

    Order* live = findPendingOrder(orderId);
    Order* conditional = nullptr;
    if (untriggered)
    {
      conditional = findConditionalOrder(orderId);
      if (conditional == nullptr)
      {
        // It triggered inside the window. The replacement was written for
        // an order still waiting on its trigger, so it is refused and the
        // triggered order works on as placed. An order that also filled, or
        // was cancelled, is gone and has been answered already.
        if (live != nullptr)
        {
          emitReplaceRejected(*live, newOrder, kRejectLateReplaceAfterTrigger);
        }
        continue;
      }
    }
    else if (!live)
    {
      // Original order is gone (typically filled mid-window); the
      // fill path already emitted REPLACE_REJECTED.
      continue;
    }

    OrderEvent accepted;
    accepted.status = OrderEventStatus::REPLACE_ACCEPTED;
    accepted.order = oldSnapshot;
    accepted.newOrder = newOrder;
    accepted.exchangeTsNs = UnixNanos::fromRaw(now);
    accepted.timestamps = timestampsFor(orderId);
    if (_callback)
    {
      _callback(accepted);
    }

    // The callback may have re-entered and moved the books: look again.
    if (untriggered)
    {
      conditional = findConditionalOrder(orderId);
      if (conditional == nullptr)
      {
        continue;
      }
      replaceConditionalOrder(*conditional, newOrder);
    }
    else
    {
      live = findPendingOrder(orderId);
      if (live == nullptr)
      {
        continue;
      }
      *live = newOrder;
    }

    OrderEvent replaced;
    replaced.status = OrderEventStatus::REPLACED;
    replaced.order = oldSnapshot;
    replaced.newOrder = newOrder;
    replaced.exchangeTsNs = UnixNanos::fromRaw(now);
    replaced.timestamps = timestampsFor(orderId);
    if (_callback)
    {
      _callback(replaced);
    }
  }
}

int64_t SimulatedExecutor::sampleSubmitAckLatency()
{
  int64_t base = _submitAckDist.sample(_cancelAckRng);
  if (_venue)
  {
    const double mult = _venue->latencyMultiplier(_clock.nowNs().raw());
    if (mult > 1.0)
    {
      base = static_cast<int64_t>(static_cast<double>(base) * mult);
    }
  }
  return base;
}

void SimulatedExecutor::enqueuePendingSubmission(const Order& order, int64_t delayNs)
{
  const int64_t now = _clock.nowNs().raw();
  _pendingSubmissions.push_back(
      PendingSubmission{.ackAtNs = now + delayNs, .order = order});
}

int64_t SimulatedExecutor::sampleOrderLatency()
{
  if (!_latency)
  {
    return 0;
  }
  return std::max<int64_t>(0, _latency->orderDelay());
}

void SimulatedExecutor::finalizePendingSubmissions()
{
  if (_pendingSubmissions.empty())
  {
    return;
  }
  const int64_t now = _clock.nowNs().raw();
  size_t i = 0;
  while (i < _pendingSubmissions.size())
  {
    if (_pendingSubmissions[i].ackAtNs > now)
    {
      ++i;
      continue;
    }
    Order accepted = _pendingSubmissions[i].order;
    _pendingSubmissions[i] = _pendingSubmissions.back();
    _pendingSubmissions.pop_back();
    finishSubmission(std::move(accepted), /*fromAck=*/true);
  }
}

void SimulatedExecutor::resolveLateReplaceOnFill(const Order& order)
{
  if (_pendingReplaces.empty())
  {
    return;
  }
  for (auto it = _pendingReplaces.begin(); it != _pendingReplaces.end(); ++it)
  {
    if (it->orderId != order.id)
    {
      continue;
    }
    OrderEvent ev;
    ev.status = OrderEventStatus::REPLACE_REJECTED;
    ev.order = it->oldSnapshot;
    ev.newOrder = it->newOrder;
    ev.rejectReason = "late_replace_after_fill";
    ev.exchangeTsNs = _clock.nowNs();
    ev.timestamps = timestampsFor(order.id);
    if (_callback)
    {
      _callback(ev);
    }
    *it = _pendingReplaces.back();
    _pendingReplaces.pop_back();
    return;
  }
}

MarketPosition SimulatedExecutor::computeMarketPosition(
    const Order& order, const MarketState& state, Quantity /*ourRemaining*/,
    Quantity nonOurLevelQty) const
{
  const int64_t p = order.price.raw();
  const bool isBuy = (order.side == Side::BUY);
  const bool hasBid = state.hasBid;
  const bool hasAsk = state.hasAsk;
  const int64_t bid = state.bestBidRaw;
  const int64_t ask = state.bestAskRaw;
  // nonOurLevelQty is the queue tracker's view of "others' quantity at this
  // order's price level". Meaningful only when the queue tracker is enabled
  // and our order is registered with it. Otherwise it is zero and cannot
  // distinguish "no others at level" from "no tracker info".
  const bool queueInfo = _queueTracker.enabled();

  if (isBuy)
  {
    if (hasAsk && p >= ask)
    {
      return MarketPosition::Crossed;
    }
    // No bid level on the book: we are the only resting buy at our
    // price (or a worse one). If the queue tracker confirms no
    // others share our exact level, we are LevelEmpty.
    if (!hasBid)
    {
      if (queueInfo && nonOurLevelQty.raw() == 0)
      {
        return MarketPosition::LevelEmpty;
      }
      if (hasAsk && p < ask)
      {
        return MarketPosition::MidSpread;
      }
      return MarketPosition::Unknown;
    }
    if (p == bid)
    {
      if (queueInfo && nonOurLevelQty.raw() == 0)
      {
        return MarketPosition::LevelEmpty;
      }
      return MarketPosition::Best;
    }
    if (p < bid)
    {
      return MarketPosition::BehindBest;
    }
    // p > bid and p < ask → mid-spread (our order is bid-side but
    // above the current best bid yet below best ask — improving the
    // book without crossing).
    if (hasAsk && p < ask)
    {
      return MarketPosition::MidSpread;
    }
    return MarketPosition::Unknown;
  }
  // SELL side.
  if (hasBid && p <= bid)
  {
    return MarketPosition::Crossed;
  }
  if (!hasAsk)
  {
    if (queueInfo && nonOurLevelQty.raw() == 0)
    {
      return MarketPosition::LevelEmpty;
    }
    if (hasBid && p > bid)
    {
      return MarketPosition::MidSpread;
    }
    return MarketPosition::Unknown;
  }
  if (p == ask)
  {
    if (queueInfo && nonOurLevelQty.raw() == 0)
    {
      return MarketPosition::LevelEmpty;
    }
    return MarketPosition::Best;
  }
  if (p > ask)
  {
    return MarketPosition::BehindBest;
  }
  if (hasBid && p > bid)
  {
    return MarketPosition::MidSpread;
  }
  return MarketPosition::Unknown;
}

int32_t SimulatedExecutor::computeDistanceToBestTicks(
    const Order& order, const MarketState& state) const
{
  // One raw price unit per "tick" is the safe fallback if the engine
  // has no symbol tick-size handy. Strategies that care about this
  // value typically know their own tick size.
  const int64_t p = order.price.raw();
  if (order.side == Side::BUY)
  {
    if (!state.hasBid)
    {
      return 0;
    }
    return static_cast<int32_t>(state.bestBidRaw - p);  // positive = behind
  }
  if (!state.hasAsk)
  {
    return 0;
  }
  return static_cast<int32_t>(p - state.bestAskRaw);
}

void SimulatedExecutor::maybeEmitMarketPositionChanges()
{
  if (!_callback || _pending_orders.empty())
  {
    return;
  }
  for (const Order& o : _pending_orders)
  {
    if (!fillsAsLimit(o.type))
    {
      continue;
    }
    const MarketState& state = getMarketState(o.symbol);
    Quantity queueTotal{0};
    if (auto snap = _queueTracker.snapshot(o.id))
    {
      queueTotal = snap->total;
    }
    const Quantity remaining =
        Quantity::fromRaw(o.quantity.raw() - o.filledQuantity.raw());
    const MarketPosition pos = computeMarketPosition(o, state, remaining, queueTotal);
    const int32_t dist = computeDistanceToBestTicks(o, state);

    auto it = _lastEmittedMarketPosition.find(o.id);
    if (it != _lastEmittedMarketPosition.end() && it->second == pos)
    {
      continue;
    }
    _lastEmittedMarketPosition[o.id] = pos;

    OrderEvent ev;
    ev.status = OrderEventStatus::MARKET_POSITION_CHANGED;
    ev.order = o;
    ev.marketPosition = pos;
    ev.distanceToBestTicks = dist;
    ev.exchangeTsNs = _clock.nowNs();
    ev.timestamps = timestampsFor(o.id);
    _callback(ev);
  }
}

int64_t SimulatedExecutor::sampleCancelAckLatency()
{
  int64_t base = _cancelAckDist.sample(_cancelAckRng);
  if (_venue)
  {
    const double mult = _venue->latencyMultiplier(_clock.nowNs().raw());
    if (mult > 1.0)
    {
      base = static_cast<int64_t>(static_cast<double>(base) * mult);
    }
  }
  return base;
}

void SimulatedExecutor::enqueuePendingCancel(const Order& order)
{
  const int64_t now = _clock.nowNs().raw();
  _pendingCancels.push_back(
      PendingCancel{.orderId = order.id,
                    .ackAtNs = now + sampleCancelAckLatency(),
                    .orderSnapshot = order});
}

void SimulatedExecutor::finalizePendingCancels()
{
  if (_pendingCancels.empty())
  {
    return;
  }
  const int64_t now = _clock.nowNs().raw();
  size_t i = 0;
  while (i < _pendingCancels.size())
  {
    if (_pendingCancels[i].ackAtNs > now)
    {
      ++i;
      continue;
    }
    const OrderId orderId = _pendingCancels[i].orderId;
    Order* live = findPendingOrder(orderId);
    Order snapshot = _pendingCancels[i].orderSnapshot;
    _pendingCancels[i] = _pendingCancels.back();
    _pendingCancels.pop_back();

    if (live)
    {
      Order canceled = *live;
      emitEvent(OrderEventStatus::CANCELED, canceled);
      for (auto it = _pending_orders.begin(); it != _pending_orders.end(); ++it)
      {
        if (it->id == orderId)
        {
          *it = _pending_orders.back();
          _pending_orders.pop_back();
          break;
        }
      }
      _queueTracker.removeOrder(orderId);
      forgetQueuePosition(orderId);
      forgetTimestamps(orderId);
      forgetMarketPosition(orderId);
      forgetPendingReplace(orderId);
      _compositeLogic.onOrderCanceled(canceled);
      continue;
    }
    // Order not in book any more — typically it filled mid-window.
    // The fill path already emitted REJECTED on the cancel attempt.
    // Conditional orders cancel synchronously below if still present.
    for (auto it = _conditional_orders.begin(); it != _conditional_orders.end();
         ++it)
    {
      if (it->id == orderId)
      {
        Order canceled = *it;
        *it = _conditional_orders.back();
        _conditional_orders.pop_back();
        // As the synchronous cancel does: a trailing reference left here would
        // arm a later trailing stop with the same id at this one's trigger.
        for (auto trailingIt = _trailing_states.begin(); trailingIt != _trailing_states.end();
             ++trailingIt)
        {
          if (trailingIt->first == orderId)
          {
            *trailingIt = _trailing_states.back();
            _trailing_states.pop_back();
            break;
          }
        }
        emitEvent(OrderEventStatus::CANCELED, canceled);
        _compositeLogic.onOrderCanceled(canceled);
        break;
      }
    }
  }
}

void SimulatedExecutor::resolveLateCancelOnFill(const Order& order)
{
  if (_pendingCancels.empty())
  {
    return;
  }
  for (auto it = _pendingCancels.begin(); it != _pendingCancels.end(); ++it)
  {
    if (it->orderId != order.id)
    {
      continue;
    }
    OrderEvent ev;
    ev.status = OrderEventStatus::REJECTED;
    ev.order = order;
    ev.rejectReason = "late_cancel_after_fill";
    ev.exchangeTsNs = _clock.nowNs();
    ev.timestamps = timestampsFor(order.id);
    if (_callback)
    {
      _callback(ev);
    }
    *it = _pendingCancels.back();
    _pendingCancels.pop_back();
    return;
  }
}

OrderTimestamps& SimulatedExecutor::timestampsFor(OrderId id)
{
  return _orderTimestamps[id];
}

void SimulatedExecutor::forgetTimestamps(OrderId id)
{
  _orderTimestamps.erase(id);
}

void SimulatedExecutor::emitTrailingUpdate(const Order& order, Price newTrigger)
{
  if (_callback)
  {
    OrderEvent ev;
    ev.status = OrderEventStatus::TRAILING_UPDATED;
    ev.order = order;
    ev.newTrailingPrice = newTrigger;
    ev.exchangeTsNs = _clock.nowNs();
    _callback(ev);
  }
}

bool SimulatedExecutor::isConditionalOrder(OrderType type) const
{
  return type == OrderType::STOP_MARKET || type == OrderType::STOP_LIMIT ||
         type == OrderType::TAKE_PROFIT_MARKET || type == OrderType::TAKE_PROFIT_LIMIT ||
         type == OrderType::TRAILING_STOP;
}

bool SimulatedExecutor::checkStopTrigger(const Order& order, const MarketState& state) const
{
  if (!state.hasTrade)
  {
    return false;
  }

  const int64_t triggerRaw = order.triggerPrice.raw();
  const int64_t priceRaw = state.lastTradeRaw;

  if (order.side == Side::SELL)
  {
    return priceRaw <= triggerRaw;
  }
  else
  {
    return priceRaw >= triggerRaw;
  }
}

bool SimulatedExecutor::checkTakeProfitTrigger(const Order& order, const MarketState& state) const
{
  if (!state.hasTrade)
  {
    return false;
  }

  const int64_t triggerRaw = order.triggerPrice.raw();
  const int64_t priceRaw = state.lastTradeRaw;

  if (order.side == Side::SELL)
  {
    return priceRaw >= triggerRaw;
  }
  else
  {
    return priceRaw <= triggerRaw;
  }
}

bool SimulatedExecutor::checkTrailingStopTrigger(const Order& order, const TrailingState& trailing,
                                                 const MarketState& state) const
{
  if (!state.hasTrade)
  {
    return false;
  }

  const int64_t priceRaw = state.lastTradeRaw;
  const int64_t triggerRaw = trailing.currentTrigger.raw();

  if (order.side == Side::SELL)
  {
    return priceRaw <= triggerRaw;
  }
  else
  {
    return priceRaw >= triggerRaw;
  }
}

void SimulatedExecutor::updateTrailingStops(SymbolId symbol, Price currentPrice)
{
  for (auto& [orderId, trailing] : _trailing_states)
  {
    for (auto& order : _conditional_orders)
    {
      if (order.id != orderId || order.symbol != symbol)
      {
        continue;
      }

      const int64_t priceRaw = currentPrice.raw();
      int64_t newTriggerRaw = trailing.currentTrigger.raw();
      bool updated = false;

      if (order.side == Side::SELL)
      {
        int64_t offsetRaw = 0;
        if (order.trailingOffset.raw() > 0)
        {
          offsetRaw = order.trailingOffset.raw();
        }
        else if (order.trailingCallbackRate > 0)
        {
          offsetRaw = (priceRaw * order.trailingCallbackRate) / 10000;
        }

        const int64_t newCandidate =
            snapAgainstOwner(priceRaw - offsetRaw, orderTickRaw(symbol), Side::SELL);
        if (newCandidate > newTriggerRaw)
        {
          newTriggerRaw = newCandidate;
          updated = true;
        }
      }
      else
      {
        int64_t offsetRaw = 0;
        if (order.trailingOffset.raw() > 0)
        {
          offsetRaw = order.trailingOffset.raw();
        }
        else if (order.trailingCallbackRate > 0)
        {
          offsetRaw = (priceRaw * order.trailingCallbackRate) / 10000;
        }

        const int64_t newCandidate =
            snapAgainstOwner(priceRaw + offsetRaw, orderTickRaw(symbol), Side::BUY);
        if (newCandidate < newTriggerRaw)
        {
          newTriggerRaw = newCandidate;
          updated = true;
        }
      }

      if (updated)
      {
        trailing.currentTrigger = Price::fromRaw(newTriggerRaw);
        emitTrailingUpdate(order, trailing.currentTrigger);
      }

      break;
    }
  }
}

void SimulatedExecutor::triggerConditionalOrder(Order& order)
{
  emitEvent(OrderEventStatus::TRIGGERED, order);

  // The price that armed the order bounds how good its fill may be. A trailing
  // stop carries its moving trigger in the trailing state, which the caller
  // has already copied onto the order by the time it fires; an order with no
  // trigger price at all carries no bound.
  const int64_t triggerBoundRaw = order.triggerPrice.raw();

  // A triggered conditional order enters the book now, so it is the aggressor
  // if it crosses.
  if (!tryFillOrder(order, /*resting=*/false, triggerBoundRaw))
  {
    // A limit that rests after firing joins the queue at its level exactly as
    // a plain limit does on submit. Without this the queue tracker never knew
    // the order: it reported LevelEmpty however much depth rested there, was
    // never advanced by prints, and its position events were never emitted.
    if (fillsAsLimit(order.type) && _queueTracker.enabled())
    {
      const MarketState& state = getMarketState(order.symbol);
      const int64_t orderPriceRaw = order.price.raw();
      Quantity levelQty = Quantity::fromRaw(0);
      if (order.side == Side::BUY && state.hasBid && orderPriceRaw == state.bestBidRaw)
      {
        levelQty = Quantity::fromRaw(state.bestBidQtyRaw);
      }
      else if (order.side == Side::SELL && state.hasAsk && orderPriceRaw == state.bestAskRaw)
      {
        levelQty = Quantity::fromRaw(state.bestAskQtyRaw);
      }
      _queueTracker.addOrder(order.symbol, order.side, order.price, order.id, order.quantity,
                             levelQty);
      _lastEmittedQueueAheadRaw[order.id] = levelQty.raw();
    }
    _pending_orders.push_back(order);
  }
}

void SimulatedExecutor::processConditionalOrders(SymbolId symbol, const MarketState& state)
{
  size_t i = 0;
  while (i < _conditional_orders.size())
  {
    Order& order = _conditional_orders[i];
    if (order.symbol != symbol)
    {
      ++i;
      continue;
    }

    bool triggered = false;

    switch (order.type)
    {
      case OrderType::STOP_MARKET:
      case OrderType::STOP_LIMIT:
        triggered = checkStopTrigger(order, state);
        break;

      case OrderType::TAKE_PROFIT_MARKET:
      case OrderType::TAKE_PROFIT_LIMIT:
        triggered = checkTakeProfitTrigger(order, state);
        break;

      case OrderType::TRAILING_STOP:
      {
        for (const auto& [id, trailing] : _trailing_states)
        {
          if (id == order.id)
          {
            triggered = checkTrailingStopTrigger(order, trailing, state);
            if (triggered && trailing.currentTrigger.raw() != 0)
            {
              // The trailing trigger lives in the trailing state, not on the
              // order. Copy it across so the TRIGGERED event reports the price
              // that armed the order and the fill is bounded by it like any
              // other stop's. A trailing stop configured with neither an offset
              // nor a callback rate has no trigger to copy, and whatever the
              // caller put on the order is left alone.
              order.triggerPrice = trailing.currentTrigger;
            }
            break;
          }
        }
        break;
      }

      default:
        break;
    }

    if (triggered)
    {
      Order triggeredOrder = order;

      _conditional_orders[i] = _conditional_orders.back();
      _conditional_orders.pop_back();

      for (auto trailingIt = _trailing_states.begin(); trailingIt != _trailing_states.end();
           ++trailingIt)
      {
        if (trailingIt->first == triggeredOrder.id)
        {
          *trailingIt = _trailing_states.back();
          _trailing_states.pop_back();
          break;
        }
      }

      triggerConditionalOrder(triggeredOrder);
    }
    else
    {
      ++i;
    }
  }
}

void SimulatedExecutor::drainQueueFills(SymbolId /*symbol*/)
{
  // Reserved for future heartbeat-driven draining. Queue fills currently drain
  // inside onTrade(symbol, price, qty, isBuy).
}

int64_t SimulatedExecutor::netPositionRaw(SymbolId symbol) const
{
  auto it = _netPositionRaw.find(symbol);
  return it == _netPositionRaw.end() ? 0 : it->second;
}

void SimulatedExecutor::processExpiredOrders()
{
  const int64_t nowNs = _clock.nowNs().raw();
  for (auto it = _pending_orders.begin(); it != _pending_orders.end();)
  {
    if (it->timeInForce == TimeInForce::GTD && it->expiresAfter.has_value())
    {
      // expiresAfter is a TimePoint (nanoseconds since epoch). Compare
      // against the simulator clock; expired orders are dropped with
      // EXPIRED, removed from the book, and forgotten from trackers.
      const int64_t expiresNs = it->expiresAfter->time_since_epoch().count();
      if (nowNs >= expiresNs)
      {
        Order expired = *it;
        _queueTracker.removeOrder(expired.id);
        forgetQueuePosition(expired.id);
        forgetMarketPosition(expired.id);
        emitEvent(OrderEventStatus::EXPIRED, expired);
        it = _pending_orders.erase(it);
        continue;
      }
    }
    ++it;
  }
}

void SimulatedExecutor::applyOutagePolicy()
{
  if (!_venue)
  {
    return;
  }
  const int64_t now = _clock.nowNs().raw();
  const auto* outage = _venue->activeOutage(now);
  if (!outage)
  {
    return;
  }
  switch (outage->policy)
  {
    case OnOutage::HOLD:
      // Orders stay; nothing to do.
      break;
    case OnOutage::CANCEL_ALL:
    {
      // Cancel every resting order at outage start.
      for (auto& o : _pending_orders)
      {
        emitEvent(OrderEventStatus::CANCELED, o);
        _queueTracker.removeOrder(o.id);
        forgetQueuePosition(o.id);
        forgetTimestamps(o.id);
        forgetMarketPosition(o.id);
        forgetPendingReplace(o.id);
      }
      _pending_orders.clear();
      break;
    }
    case OnOutage::EXPIRE_GTC_AFTER:
    {
      // Drop orders whose age in flight exceeds the venue TTL.
      const int64_t ttlNs = outage->gtcTtlNs;
      if (ttlNs <= 0)
      {
        return;
      }
      size_t i = 0;
      while (i < _pending_orders.size())
      {
        auto& o = _pending_orders[i];
        const int64_t createdNs = o.createdAt.time_since_epoch().count();
        const int64_t ageNs = now - createdNs;
        if (ageNs >= ttlNs)
        {
          emitEvent(OrderEventStatus::CANCELED, o);
          _queueTracker.removeOrder(o.id);
          forgetQueuePosition(o.id);
          forgetTimestamps(o.id);
          forgetMarketPosition(o.id);
          forgetPendingReplace(o.id);
          _pending_orders[i] = _pending_orders.back();
          _pending_orders.pop_back();
          continue;
        }
        ++i;
      }
      break;
    }
  }
}

void SimulatedExecutor::flushOutageBuffer()
{
  if (_outageBuffer.empty())
  {
    return;
  }
  std::vector<BufferedRequest> drained;
  drained.swap(_outageBuffer);
  for (const auto& req : drained)
  {
    switch (req.action)
    {
      case BufferedAction::SUBMIT:
        submitOrder(req.order);
        break;
      case BufferedAction::CANCEL:
        cancelOrder(req.oldOrderId);
        break;
      case BufferedAction::REPLACE:
        replaceOrder(req.oldOrderId, req.order);
        break;
      case BufferedAction::CANCEL_ALL_SYMBOL:
        cancelAllOrders(req.cancelAllSymbol);
        break;
    }
  }
}

}  // namespace lrvx
