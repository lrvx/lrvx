/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <cassert>

#include "lrvx/common.h"
#include "lrvx/position/position_aggregation_mode.h"
#include "lrvx/position/position_group.h"
#include "lrvx/position/position_reconciler.h"
#include "lrvx/position/position_tracker.h"
#include "lrvx/strategy/symbol_state_map.h"
#include "lrvx/util/base/move_only_function.h"

#include <algorithm>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace lrvx
{

struct CachedPositionState
{
  std::deque<Lot> lots;
  int64_t cachedNetQty{0};
  Price realizedPnl{};

  Quantity position() const { return Quantity::fromRaw(cachedNetQty); }

  Price avgEntryPrice() const
  {
    if (lots.empty())
    {
      return Price{};
    }
    int64_t totalQty = 0;
    int64_t totalCost = 0;
    for (const auto& lot : lots)
    {
      int64_t absQty = std::abs(lot.quantity.raw());
      totalQty += absQty;
      totalCost += (Quantity::fromRaw(absQty) * lot.price).raw();
    }
    if (totalQty == 0)
    {
      return Price{};
    }
    return Volume::fromRaw(totalCost) / Quantity::fromRaw(totalQty);
  }
};

struct PerSidePositionState
{
  std::deque<Lot> longLots;
  std::deque<Lot> shortLots;
  int64_t cachedLongQty{0};
  int64_t cachedShortQty{0};
  Price realizedPnl{};

  Quantity longPosition() const { return Quantity::fromRaw(cachedLongQty); }
  Quantity shortPosition() const { return Quantity::fromRaw(cachedShortQty); }
  Quantity netPosition() const { return Quantity::fromRaw(cachedLongQty - cachedShortQty); }

  Price longAvgEntryPrice() const { return avgEntryForLots(longLots); }
  Price shortAvgEntryPrice() const { return avgEntryForLots(shortLots); }

 private:
  static Price avgEntryForLots(const std::deque<Lot>& lots)
  {
    if (lots.empty())
    {
      return Price{};
    }
    int64_t totalQty = 0;
    int64_t totalCost = 0;
    for (const auto& lot : lots)
    {
      int64_t absQty = std::abs(lot.quantity.raw());
      totalQty += absQty;
      totalCost += (Quantity::fromRaw(absQty) * lot.price).raw();
    }
    if (totalQty == 0)
    {
      return Price{};
    }
    return Volume::fromRaw(totalCost) / Quantity::fromRaw(totalQty);
  }
};

// Thread-safe proxy for accessing PositionGroupTracker.
// Holds the mutex for its lifetime -- use in limited scope.
template <typename T>
class LockedRef
{
 public:
  LockedRef(std::mutex& m, T& ref) : _lock(m), _ref(ref) {}
  T* operator->() { return &_ref; }
  T& operator*() { return _ref; }

 private:
  std::lock_guard<std::mutex> _lock;
  T& _ref;
};

template <typename T>
class LockedConstRef
{
 public:
  LockedConstRef(std::mutex& m, const T& ref) : _lock(m), _ref(ref) {}
  const T* operator->() const { return &_ref; }
  const T& operator*() const { return _ref; }

 private:
  std::lock_guard<std::mutex> _lock;
  const T& _ref;
};

class MultiModePositionTracker : public IPositionManager
{
 public:
  MultiModePositionTracker(SubscriberId id,
                           PositionAggregationMode mode = PositionAggregationMode::NET,
                           CostBasisMethod costBasis = CostBasisMethod::FIFO)
      : IPositionManager(id), _mode(mode), _costBasis(costBasis)
  {
    switch (_mode)
    {
      case PositionAggregationMode::NET:
        _netStates = std::make_unique<SymbolStateMap<CachedPositionState>>();
        break;
      case PositionAggregationMode::PER_SIDE:
        _perSideStates = std::make_unique<SymbolStateMap<PerSidePositionState>>();
        break;
      case PositionAggregationMode::GROUPED:
        _groups = std::make_unique<PositionGroupTracker>();
        break;
    }
  }

  void start() override {}
  void stop() override {}

  void openLong(SymbolId symbol, Price price, Quantity qty, uint16_t tag = 0)
  {
    applyExplicitFill(symbol, Side::BUY, price, qty, false, tag);
  }

  void closeLong(SymbolId symbol, Price price, Quantity qty, uint16_t tag = 0)
  {
    applyExplicitFill(symbol, Side::SELL, price, qty, true, tag);
  }

  void openShort(SymbolId symbol, Price price, Quantity qty, uint16_t tag = 0)
  {
    applyExplicitFill(symbol, Side::SELL, price, qty, false, tag);
  }

  void closeShort(SymbolId symbol, Price price, Quantity qty, uint16_t tag = 0)
  {
    applyExplicitFill(symbol, Side::BUY, price, qty, true, tag);
  }

  PositionAggregationMode mode() const { return _mode; }
  CostBasisMethod costBasisMethod() const { return _costBasis; }

  struct PositionSnapshot
  {
    Quantity longQty{};
    Quantity shortQty{};
    Price longAvgEntry{};
    Price shortAvgEntry{};
    Price realizedPnl{};

    Quantity netQty() const { return Quantity::fromRaw(longQty.raw() - shortQty.raw()); }

    Volume unrealizedPnl(Price currentPrice) const
    {
      int64_t pnl = 0;
      if (longQty.raw() > 0)
      {
        pnl += (longQty * (currentPrice - longAvgEntry)).raw();
      }
      if (shortQty.raw() > 0)
      {
        pnl += (shortQty * (shortAvgEntry - currentPrice)).raw();
      }
      return Volume::fromRaw(pnl);
    }
  };

  using PositionChangeCallback = MoveOnlyFunction<void(SymbolId, const PositionSnapshot&)>;

  void onPositionChange(PositionChangeCallback cb) { _onChange = std::move(cb); }

  PositionSnapshot snapshot(SymbolId symbol) const
  {
    std::lock_guard<std::mutex> lock(_mutex);
    return snapshotUnlocked(symbol);
  }

  Quantity getPosition(SymbolId symbol) const override
  {
    std::lock_guard<std::mutex> lock(_mutex);
    return getPositionUnlocked(symbol);
  }

  // Net entry price, or nothing when flat. In PER_SIDE mode a book that is
  // long and short at once has no single entry price, so the two sides are
  // blended by quantity the same way the net position is.
  std::optional<Price> getAverageEntryPrice(SymbolId symbol) const override
  {
    std::lock_guard<std::mutex> lock(_mutex);
    return blendedEntry(snapshotUnlocked(symbol));
  }

  // The net position and that entry price under one acquisition of the
  // mutex, which is what a market-data tick asks for. lrvx::PositionSnapshot,
  // not the per-side PositionSnapshot nested above: that one is this
  // tracker's own long/short breakdown and predates the interface.
  lrvx::PositionSnapshot positionSnapshot(SymbolId symbol) const override
  {
    std::lock_guard<std::mutex> lock(_mutex);
    const auto snap = snapshotUnlocked(symbol);
    return lrvx::PositionSnapshot{getPositionUnlocked(symbol), blendedEntry(snap)};
  }

  Quantity getLongPosition(SymbolId symbol) const
  {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_mode == PositionAggregationMode::PER_SIDE)
    {
      return (*_perSideStates)[symbol].longPosition();
    }
    auto pos = getPositionUnlocked(symbol);
    return pos.raw() > 0 ? pos : Quantity{};
  }

  Quantity getShortPosition(SymbolId symbol) const
  {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_mode == PositionAggregationMode::PER_SIDE)
    {
      return (*_perSideStates)[symbol].shortPosition();
    }
    auto pos = getPositionUnlocked(symbol);
    return pos.raw() < 0 ? Quantity::fromRaw(-pos.raw()) : Quantity{};
  }

  // Route through snapshotUnlocked, which handles all three modes. The old
  // code dereferenced _netStates unconditionally, which is nullptr in GROUPED
  // mode (the ctor allocates exactly one of the three state containers).
  Price getLongAvgEntry(SymbolId symbol) const
  {
    std::lock_guard<std::mutex> lock(_mutex);
    return snapshotUnlocked(symbol).longAvgEntry;
  }

  Price getShortAvgEntry(SymbolId symbol) const
  {
    std::lock_guard<std::mutex> lock(_mutex);
    return snapshotUnlocked(symbol).shortAvgEntry;
  }

  Volume getUnrealizedPnl(SymbolId symbol, Price currentPrice) const
  {
    return snapshot(symbol).unrealizedPnl(currentPrice);
  }

  // group accessors are only valid in GROUPED mode (_groups is nullptr in
  // NET/PER_SIDE). Assert rather than deref null so misuse fails loudly.
  LockedRef<PositionGroupTracker> lockedGroups()
  {
    assert(_groups && "lockedGroups() requires GROUPED aggregation mode");
    return {_mutex, *_groups};
  }

  LockedConstRef<PositionGroupTracker> lockedGroups() const
  {
    assert(_groups && "lockedGroups() requires GROUPED aggregation mode");
    return {_mutex, *_groups};
  }

  // Raw access (caller must ensure thread safety)
  PositionGroupTracker& groups()
  {
    assert(_groups && "groups() requires GROUPED aggregation mode");
    return *_groups;
  }
  const PositionGroupTracker& groups() const
  {
    assert(_groups && "groups() requires GROUPED aggregation mode");
    return *_groups;
  }

  Price getRealizedPnl(SymbolId symbol) const
  {
    std::lock_guard<std::mutex> lock(_mutex);
    switch (_mode)
    {
      case PositionAggregationMode::NET:
        return (*_netStates)[symbol].realizedPnl;
      case PositionAggregationMode::PER_SIDE:
        return (*_perSideStates)[symbol].realizedPnl;
      case PositionAggregationMode::GROUPED:
        return _groups->realizedPnl(symbol);
    }
    return Price{};
  }

  Price getTotalRealizedPnl() const
  {
    std::lock_guard<std::mutex> lock(_mutex);
    int64_t total = 0;
    switch (_mode)
    {
      case PositionAggregationMode::NET:
        _netStates->forEach([&total](SymbolId, const CachedPositionState& s)
                            { total += s.realizedPnl.raw(); });
        break;
      case PositionAggregationMode::PER_SIDE:
        _perSideStates->forEach([&total](SymbolId, const PerSidePositionState& s)
                                { total += s.realizedPnl.raw(); });
        break;
      case PositionAggregationMode::GROUPED:
        total = _groups->totalRealizedPnl().raw();
        break;
    }
    return Price::fromRaw(total);
  }

  void reset()
  {
    std::lock_guard<std::mutex> lock(_mutex);
    switch (_mode)
    {
      case PositionAggregationMode::NET:
        _netStates->clear();
        break;
      case PositionAggregationMode::PER_SIDE:
        _perSideStates->clear();
        break;
      case PositionAggregationMode::GROUPED:
        *_groups = PositionGroupTracker{};
        _tagToGroup.clear();
        break;
    }
    _nextExplicitId = kExplicitFillBaseId;
  }

  // --- Order execution callbacks ---
  // Convention: onOrderFilled is called ONCE with the full order quantity when the
  // entire order fills at once. If an order fills in parts, only
  // onOrderPartiallyFilled is called for each part -- onOrderFilled is NOT called
  // after partial fills complete. Calling both will double-count.

  void onOrderPartiallyFilled(const Order& order, Quantity fillQty) override
  {
    std::lock_guard<std::mutex> lock(_mutex);
    applyFillInternal(order, fillQty, precomputeFill(order, fillQty));
  }

  void onOrderFilled(const Order& order) override
  {
    std::lock_guard<std::mutex> lock(_mutex);
    applyFillInternal(order, order.quantity, precomputeFill(order, order.quantity));
  }

  // Preferred forms: they carry the price the fill actually happened at, which
  // a market order does not carry on the order itself.
  void onOrderPartiallyFilled(const Order& order, Quantity fillQty,
                              Price fillPrice) override
  {
    std::lock_guard<std::mutex> lock(_mutex);
    Order atFillPrice = order;
    atFillPrice.price = fillPrice;
    applyFillInternal(atFillPrice, fillQty, precomputeFill(atFillPrice, fillQty));
  }

  void onOrderFilled(const Order& order, Quantity fillQty, Price fillPrice) override
  {
    std::lock_guard<std::mutex> lock(_mutex);
    Order atFillPrice = order;
    atFillPrice.price = fillPrice;
    applyFillInternal(atFillPrice, fillQty, precomputeFill(atFillPrice, fillQty));
  }

  void onOrderCanceled(const Order& order) override
  {
    if (_mode == PositionAggregationMode::GROUPED)
    {
      std::lock_guard<std::mutex> lock(_mutex);
      auto* pos = _groups->getPositionByOrder(order.id);
      if (pos && pos->quantity.raw() == 0)
      {
        _groups->closePosition(pos->positionId, order.price);
      }
    }
  }

  std::vector<PositionMismatch> reconcile(
      const PositionReconciler& reconciler,
      const std::vector<ExchangePosition>& exchangePositions) const
  {
    std::lock_guard<std::mutex> lock(_mutex);
    return reconciler.reconcile(exchangePositions,
                                [this](SymbolId sym) -> std::pair<Quantity, Price>
                                {
                                  auto snap = snapshotUnlocked(sym);
                                  Quantity netQty = snap.netQty();
                                  Price avgEntry = (netQty.raw() >= 0) ? snap.longAvgEntry : snap.shortAvgEntry;
                                  return {netQty, avgEntry};
                                });
  }

 private:
  static constexpr OrderId kExplicitFillBaseId = kExplicitFillOrderIdBase;
  OrderId _nextExplicitId{kExplicitFillBaseId};

  struct PrecomputedFill
  {
    int64_t signedQty;
    bool isClose;
  };

  static PrecomputedFill precomputeFill(const Order& order, Quantity fillQty)
  {
    int64_t signedQty = (order.side == Side::BUY) ? fillQty.raw() : -fillQty.raw();
    bool isClose = order.flags.reduceOnly || order.flags.closePosition;
    return {signedQty, isClose};
  }

  void applyExplicitFill(SymbolId symbol, Side side, Price price, Quantity qty,
                         bool isClose, uint16_t tag)
  {
    Order order{};
    order.id = _nextExplicitId++;
    order.symbol = symbol;
    order.side = side;
    order.price = price;
    order.quantity = qty;
    order.flags.reduceOnly = isClose ? 1 : 0;
    order.orderTag = tag;

    int64_t signedQty = (side == Side::BUY) ? qty.raw() : -qty.raw();
    PrecomputedFill pc{signedQty, isClose};

    std::lock_guard<std::mutex> lock(_mutex);
    applyFillInternal(order, qty, pc);
  }

  // Blend the two sides into one entry price. Shared by
  // getAverageEntryPrice() and positionSnapshot() so there is one answer
  // rather than two that can drift apart.
  static std::optional<Price> blendedEntry(const PositionSnapshot& snap)
  {
    const int64_t longRaw = snap.longQty.raw();
    const int64_t shortRaw = snap.shortQty.raw();
    const int64_t totalRaw = longRaw + shortRaw;
    if (totalRaw == 0)
    {
      return std::nullopt;
    }
    if (shortRaw == 0)
    {
      return snap.longAvgEntry;
    }
    if (longRaw == 0)
    {
      return snap.shortAvgEntry;
    }
    const Volume blended = Volume::fromRaw((snap.longQty * snap.longAvgEntry).raw() +
                                           (snap.shortQty * snap.shortAvgEntry).raw());
    return blended / Quantity::fromRaw(totalRaw);
  }

  PositionSnapshot snapshotUnlocked(SymbolId symbol) const
  {
    PositionSnapshot snap;

    switch (_mode)
    {
      case PositionAggregationMode::NET:
      {
        const auto& s = (*_netStates)[symbol];
        int64_t pos = s.cachedNetQty;
        if (pos > 0)
        {
          snap.longQty = Quantity::fromRaw(pos);
          snap.longAvgEntry = s.avgEntryPrice();
        }
        else if (pos < 0)
        {
          snap.shortQty = Quantity::fromRaw(-pos);
          snap.shortAvgEntry = s.avgEntryPrice();
        }
        snap.realizedPnl = s.realizedPnl;
        break;
      }
      case PositionAggregationMode::PER_SIDE:
      {
        const auto& s = (*_perSideStates)[symbol];
        snap.longQty = s.longPosition();
        snap.shortQty = s.shortPosition();
        snap.longAvgEntry = s.longAvgEntryPrice();
        snap.shortAvgEntry = s.shortAvgEntryPrice();
        snap.realizedPnl = s.realizedPnl;
        break;
      }
      case PositionAggregationMode::GROUPED:
      {
        int64_t longRaw = 0, shortRaw = 0;
        int64_t longCost = 0, shortCost = 0;
        _groups->forEachOpen([&](const IndividualPosition& p)
                             {
          if (p.symbol != symbol){ return;
}
          if (p.side == Side::BUY)
          {
            longRaw += p.quantity.raw();
            longCost += (p.quantity * p.entryPrice).raw();
          }
          else
          {
            shortRaw += p.quantity.raw();
            shortCost += (p.quantity * p.entryPrice).raw();
          } });
        snap.longQty = Quantity::fromRaw(longRaw);
        snap.shortQty = Quantity::fromRaw(shortRaw);
        if (longRaw > 0)
        {
          snap.longAvgEntry = Volume::fromRaw(longCost) / Quantity::fromRaw(longRaw);
        }
        if (shortRaw > 0)
        {
          snap.shortAvgEntry = Volume::fromRaw(shortCost) / Quantity::fromRaw(shortRaw);
        }
        snap.realizedPnl = _groups->realizedPnl(symbol);
        break;
      }
    }
    return snap;
  }

  Quantity getPositionUnlocked(SymbolId symbol) const
  {
    switch (_mode)
    {
      case PositionAggregationMode::NET:
        return (*_netStates)[symbol].position();
      case PositionAggregationMode::PER_SIDE:
        return (*_perSideStates)[symbol].netPosition();
      case PositionAggregationMode::GROUPED:
        return _groups->netPosition(symbol);
    }
    return Quantity{};
  }

  void applyFillInternal(const Order& order, Quantity fillQty, const PrecomputedFill& pc)
  {
    bool changed = false;
    switch (_mode)
    {
      case PositionAggregationMode::NET:
        updateNet(order.symbol, pc.signedQty, order.price);
        changed = (pc.signedQty != 0);
        break;
      case PositionAggregationMode::PER_SIDE:
        changed = updatePerSide(order, fillQty, pc.isClose);
        break;
      case PositionAggregationMode::GROUPED:
        changed = updateGrouped(order, fillQty, pc.isClose);
        break;
    }

    // Only on a real change. Subscribers used to be told "position changed"
    // after a fill that moved nothing, and handed back the snapshot they
    // already had.
    if (_onChange && changed) [[unlikely]]
    {
      _onChange(order.symbol, snapshotUnlocked(order.symbol));
    }
  }

  void updateNet(SymbolId symbol, int64_t signedQty, Price price)
  {
    auto& s = (*_netStates)[symbol];
    int64_t currentPos = s.cachedNetQty;

    bool isClosing = (currentPos > 0 && signedQty < 0) || (currentPos < 0 && signedQty > 0);

    if (isClosing)
    {
      int64_t absSignedQty = std::abs(signedQty);
      int64_t absCurrentPos = std::abs(currentPos);
      int64_t qtyToClose = std::min(absSignedQty, absCurrentPos);
      Price pnl = closeLots(s.lots, Quantity::fromRaw(qtyToClose), price, currentPos > 0);
      s.realizedPnl = Price::fromRaw(s.realizedPnl.raw() + pnl.raw());
      s.cachedNetQty += signedQty;

      int64_t remaining = absSignedQty - qtyToClose;
      if (remaining > 0)
      {
        Quantity remQty = Quantity::fromRaw(signedQty > 0 ? remaining : -remaining);
        addLot(s.lots, remQty, price);
      }
    }
    else
    {
      addLot(s.lots, Quantity::fromRaw(signedQty), price);
      s.cachedNetQty += signedQty;
    }
  }

  bool updatePerSide(const Order& order, Quantity qty, bool isClose)
  {
    auto& s = (*_perSideStates)[order.symbol];

    if (order.side == Side::BUY)
    {
      if (isClose)
      {
        int64_t closeRaw = std::min(qty.raw(), s.cachedShortQty);
        if (closeRaw > 0)
        {
          Price pnl = closeLots(s.shortLots, Quantity::fromRaw(closeRaw), order.price, false);
          s.cachedShortQty -= closeRaw;
          s.realizedPnl = Price::fromRaw(s.realizedPnl.raw() + pnl.raw());
          return true;
        }
        return false;
      }
      addLot(s.longLots, qty, order.price);
      s.cachedLongQty += qty.raw();
      return qty.raw() != 0;
    }

    if (isClose)
    {
      int64_t closeRaw = std::min(qty.raw(), s.cachedLongQty);
      if (closeRaw > 0)
      {
        Price pnl = closeLots(s.longLots, Quantity::fromRaw(closeRaw), order.price, true);
        s.cachedLongQty -= closeRaw;
        s.realizedPnl = Price::fromRaw(s.realizedPnl.raw() + pnl.raw());
        return true;
      }
      return false;
    }
    addLot(s.shortLots, qty, order.price);
    s.cachedShortQty += qty.raw();
    return qty.raw() != 0;
  }

  // Close against the positions in one tag group, oldest first. Returns what
  // is left of the fill after the group is exhausted.
  int64_t closeWithinGroup(const Order& order, int64_t remaining)
  {
    GroupId gid = getOrCreateTagGroup(order.orderTag);
    const auto* group = _groups->getGroup(gid);
    if (!group)
    {
      return remaining;
    }
    for (auto pid : group->positionIds)
    {
      if (remaining <= 0)
      {
        break;
      }
      remaining -= closeOnePosition(order, pid, remaining);
    }
    return remaining;
  }

  // Close against every open position on this symbol, oldest first. Position
  // ids are handed out in order, so sorting them is the arrival order.
  int64_t closeAcrossSymbol(const Order& order, int64_t remaining)
  {
    std::vector<PositionId> ids;
    // order: sorted below, before use -- see the comment above this method
    for (const auto& [pid, pos] : _groups->positions())
    {
      if (!pos.closed && pos.symbol == order.symbol)
      {
        ids.push_back(pid);
      }
    }
    std::sort(ids.begin(), ids.end());
    for (auto pid : ids)
    {
      if (remaining <= 0)
      {
        break;
      }
      remaining -= closeOnePosition(order, pid, remaining);
    }
    return remaining;
  }

  // Returns how much of `remaining` this position absorbed.
  int64_t closeOnePosition(const Order& order, PositionId pid, int64_t remaining)
  {
    auto* pos = _groups->getPosition(pid);
    if (!pos || pos->closed || pos->symbol != order.symbol)
    {
      return 0;
    }
    const bool isOpposite = (order.side == Side::SELL && pos->side == Side::BUY) ||
                            (order.side == Side::BUY && pos->side == Side::SELL);
    if (!isOpposite)
    {
      return 0;
    }
    const int64_t closeQty = std::min(remaining, pos->quantity.raw());
    if (closeQty <= 0)
    {
      return 0;
    }
    _groups->partialClose(pid, Quantity::fromRaw(closeQty), order.price);
    return closeQty;
  }

  bool updateGrouped(const Order& order, Quantity fillQty, bool isClose)
  {
    if (isClose)
    {
      int64_t remaining = fillQty.raw();
      if (order.orderTag != 0)
      {
        remaining = closeWithinGroup(order, remaining);
      }
      else
      {
        // An untagged close covers the whole symbol. Without this the call
        // returned having done nothing: a position opened and closed through
        // the documented API stayed open, its realized PnL was lost, and
        // `Strategy::emitClosePosition` -- which sets reduceOnly and no tag --
        // could never close anything in this mode at all.
        remaining = closeAcrossSymbol(order, remaining);
      }
      return remaining < fillQty.raw();
    }

    auto* pos = _groups->getPositionByOrder(order.id);
    if (!pos)
    {
      PositionId pid = _groups->openPosition(order.id, order.symbol, order.side, order.price, fillQty);
      if (order.orderTag != 0)
      {
        GroupId gid = getOrCreateTagGroup(order.orderTag);
        _groups->assignToGroup(pid, gid);
      }
      return true;
    }

    // A later part of the same order tops the position up. Blend the entry
    // price by quantity: keeping the first part's price reported a 5@100 plus
    // 5@200 order as ten units held at 100, a cost basis off by the full
    // spread between the two fills.
    const int64_t signedFill = (pos->side == Side::SELL) ? -fillQty.raw() : fillQty.raw();
    _groups->addToNetCache(pos->symbol, signedFill);
    const int64_t newQtyRaw = pos->quantity.raw() + fillQty.raw();
    if (newQtyRaw != 0)
    {
      const Volume held = pos->quantity * pos->entryPrice;
      const Volume added = fillQty * order.price;
      pos->entryPrice =
          Volume::fromRaw(held.raw() + added.raw()) / Quantity::fromRaw(newQtyRaw);
    }
    pos->quantity = Quantity::fromRaw(newQtyRaw);
    return fillQty.raw() != 0;
  }

  GroupId getOrCreateTagGroup(uint16_t tag)
  {
    auto it = _tagToGroup.find(tag);
    if (it != _tagToGroup.end())
    {
      return it->second;
    }
    GroupId gid = _groups->createGroup();
    _tagToGroup[tag] = gid;
    return gid;
  }

  void addLot(std::deque<Lot>& lots, Quantity signedQty, Price price)
  {
    if (_costBasis == CostBasisMethod::AVERAGE && !lots.empty())
    {
      int64_t newQty = lots.front().quantity.raw() + signedQty.raw();
      if (newQty != 0)
      {
        Quantity absOld = Quantity::fromRaw(std::abs(lots.front().quantity.raw()));
        Quantity absNew = Quantity::fromRaw(std::abs(signedQty.raw()));
        Volume oldNotional = absOld * lots.front().price;
        Volume newNotional = absNew * price;
        Volume totalNotional = Volume::fromRaw(oldNotional.raw() + newNotional.raw());
        Quantity absTotal = Quantity::fromRaw(std::abs(newQty));
        lots.front().quantity = Quantity::fromRaw(newQty);
        lots.front().price = totalNotional / absTotal;
      }
      else
      {
        lots.clear();
      }
    }
    else
    {
      lots.push_back({signedQty, price});
    }
  }

  Price closeLots(std::deque<Lot>& lots, Quantity qtyToClose, Price closePrice, bool wasLong)
  {
    int64_t pnl = 0;
    int64_t remaining = qtyToClose.raw();

    while (remaining > 0 && !lots.empty())
    {
      Lot& lot = (_costBasis == CostBasisMethod::LIFO) ? lots.back() : lots.front();
      int64_t lotQty = std::abs(lot.quantity.raw());
      int64_t closeQty = std::min(remaining, lotQty);

      Price priceDiff = closePrice - lot.price;
      Volume lotPnl = Quantity::fromRaw(closeQty) * priceDiff;
      if (!wasLong)
      {
        lotPnl = Volume::fromRaw(-lotPnl.raw());
      }
      pnl += lotPnl.raw();
      remaining -= closeQty;

      if (closeQty >= lotQty)
      {
        if (_costBasis == CostBasisMethod::LIFO)
        {
          lots.pop_back();
        }
        else
        {
          lots.pop_front();
        }
      }
      else
      {
        int64_t newQty = (lot.quantity.raw() > 0) ? (lotQty - closeQty) : -(lotQty - closeQty);
        lot.quantity = Quantity::fromRaw(newQty);
      }
    }

    return Price::fromRaw(pnl);
  }

  PositionAggregationMode _mode;
  CostBasisMethod _costBasis;
  PositionChangeCallback _onChange;
  mutable std::mutex _mutex;

  // Only one is allocated (based on _mode)
  std::unique_ptr<SymbolStateMap<CachedPositionState>> _netStates;
  std::unique_ptr<SymbolStateMap<PerSidePositionState>> _perSideStates;
  std::unique_ptr<PositionGroupTracker> _groups;
  std::unordered_map<uint16_t, GroupId> _tagToGroup;
};

inline std::vector<PositionMismatch> reconcileWith(
    const PositionReconciler& reconciler,
    const MultiModePositionTracker& tracker,
    const std::vector<ExchangePosition>& exchangePositions)
{
  return tracker.reconcile(reconciler, exchangePositions);
}

}  // namespace lrvx
