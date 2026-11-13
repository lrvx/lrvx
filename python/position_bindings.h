// python/position_bindings.h

#pragma once

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "bindings_common.h"

#include "lrvx/common.h"
#include "lrvx/execution/order_tracker.h"
#include "lrvx/position/position_group.h"
#include "lrvx/position/position_tracker.h"

namespace py = pybind11;

namespace
{

class PyPositionTracker
{
  lrvx::PositionTracker _tracker;

 public:
  PyPositionTracker(const std::string& method)
      : _tracker(0, parseCostBasis(method))
  {
  }

  // Record a fill - create a minimal Order and call onOrderFilled
  void onFill(uint32_t symbol, const std::string& side, double price, double quantity)
  {
    lrvx::Order order;
    order.symbol = symbol;
    order.side = parseSide(side);
    order.price = lrvx::Price::fromDouble(price);
    order.quantity = lrvx::Quantity::fromDouble(quantity);
    order.id = ++_nextOrderId;
    _tracker.onOrderFilled(order);
  }

  double position(uint32_t symbol) const
  {
    return _tracker.getPosition(symbol).toDouble();
  }

  double avgEntryPrice(uint32_t symbol) const
  {
    return _tracker.getAvgEntryPrice(symbol).toDouble();
  }

  double realizedPnl(uint32_t symbol) const
  {
    return _tracker.getRealizedPnl(symbol).toDouble();
  }

  double totalRealizedPnl() const
  {
    return _tracker.getTotalRealizedPnl().toDouble();
  }

 private:
  uint64_t _nextOrderId = 0;

  static lrvx::CostBasisMethod parseCostBasis(const std::string& s)
  {
    return parseChoice<lrvx::CostBasisMethod>("cost basis", s,
                                              {{"fifo", lrvx::CostBasisMethod::FIFO},
                                               {"lifo", lrvx::CostBasisMethod::LIFO},
                                               {"average", lrvx::CostBasisMethod::AVERAGE}});
  }
};

class PyPositionGroupTracker
{
  lrvx::PositionGroupTracker _tracker;

 public:
  uint64_t openPosition(uint64_t orderId, uint32_t symbol, const std::string& side,
                        double price, double qty)
  {
    return _tracker.openPosition(orderId, symbol,
                                 parseSide(side),
                                 lrvx::Price::fromDouble(price),
                                 lrvx::Quantity::fromDouble(qty));
  }

  void closePosition(uint64_t pid, double exitPrice)
  {
    _tracker.closePosition(pid, lrvx::Price::fromDouble(exitPrice));
  }

  void partialClose(uint64_t pid, double qty, double exitPrice)
  {
    _tracker.partialClose(pid, lrvx::Quantity::fromDouble(qty),
                          lrvx::Price::fromDouble(exitPrice));
  }

  uint64_t createGroup(uint64_t parentId = 0)
  {
    return _tracker.createGroup(parentId);
  }

  bool assignToGroup(uint64_t pid, uint64_t gid)
  {
    return _tracker.assignToGroup(pid, gid);
  }

  py::object getPosition(uint64_t pid) const
  {
    auto* p = _tracker.getPosition(pid);
    if (!p)
    {
      return py::none();
    }
    py::dict d;
    d["position_id"] = p->positionId;
    d["order_id"] = p->originOrderId;
    d["symbol"] = p->symbol;
    d["side"] = (p->side == lrvx::Side::BUY) ? "buy" : "sell";
    d["entry_price"] = p->entryPrice.toDouble();
    d["quantity"] = p->quantity.toDouble();
    d["realized_pnl"] = p->realizedPnl.toDouble();
    d["closed"] = p->closed;
    d["group_id"] = p->groupId;
    return d;
  }

  double netPosition(uint32_t symbol) const
  {
    return _tracker.netPosition(symbol).toDouble();
  }

  double groupNetPosition(uint64_t gid) const
  {
    return _tracker.groupNetPosition(gid).toDouble();
  }

  double realizedPnl(uint32_t symbol) const
  {
    return _tracker.realizedPnl(symbol).toDouble();
  }

  double totalRealizedPnl() const
  {
    return _tracker.totalRealizedPnl().toDouble();
  }

  double groupRealizedPnl(uint64_t gid) const
  {
    return _tracker.groupRealizedPnl(gid).toDouble();
  }

  double groupUnrealizedPnl(uint64_t gid, double currentPrice) const
  {
    return _tracker.groupUnrealizedPnl(gid, lrvx::Price::fromDouble(currentPrice)).toDouble();
  }

  py::list openPositions(uint32_t symbol) const
  {
    auto positions = _tracker.getOpenPositions(symbol);
    py::list result;
    for (auto* p : positions)
    {
      py::dict d;
      d["position_id"] = p->positionId;
      d["order_id"] = p->originOrderId;
      d["symbol"] = p->symbol;
      d["side"] = (p->side == lrvx::Side::BUY) ? "buy" : "sell";
      d["entry_price"] = p->entryPrice.toDouble();
      d["quantity"] = p->quantity.toDouble();
      d["realized_pnl"] = p->realizedPnl.toDouble();
      d["closed"] = p->closed;
      d["group_id"] = p->groupId;
      result.append(d);
    }
    return result;
  }

  size_t openPositionCount(py::object symbol) const
  {
    if (symbol.is_none())
    {
      return _tracker.openPositionCount();
    }
    return _tracker.openPositionCount(symbol.cast<uint32_t>());
  }

  void pruneClosed()
  {
    _tracker.pruneClosedPositions();
  }
};

class PyOrderTracker
{
  lrvx::OrderTracker _tracker;

 public:
  bool onSubmitted(uint64_t orderId, const std::string& exchangeOrderId,
                   const std::string& clientOrderId = "")
  {
    lrvx::Order order;
    order.id = orderId;
    return _tracker.onSubmitted(order, exchangeOrderId, clientOrderId);
  }

  bool onFilled(uint64_t orderId, double fillQty)
  {
    return _tracker.onFilled(orderId, lrvx::Quantity::fromDouble(fillQty));
  }

  bool onCanceled(uint64_t orderId)
  {
    return _tracker.onCanceled(orderId);
  }

  bool onRejected(uint64_t orderId, const std::string& reason)
  {
    return _tracker.onRejected(orderId, reason);
  }

  py::object get(uint64_t orderId) const
  {
    auto state = _tracker.get(orderId);
    if (!state)
    {
      return py::none();
    }
    py::dict d;
    d["order_id"] = state->localOrder.id;
    d["exchange_order_id"] = state->exchangeOrderId;
    d["client_order_id"] = state->clientOrderId;
    d["status"] = statusToStr(state->status);
    d["filled"] = state->filled.toDouble();
    d["is_terminal"] = state->isTerminal();
    return d;
  }

  bool isActive(uint64_t orderId) const
  {
    return _tracker.isActive(orderId);
  }

  size_t activeCount() const
  {
    return _tracker.activeOrderCount();
  }

  size_t totalCount() const
  {
    return _tracker.totalOrderCount();
  }

  void pruneTerminal()
  {
    _tracker.pruneTerminal();
  }

 private:
  static std::string statusToStr(lrvx::OrderEventStatus s)
  {
    switch (s)
    {
      case lrvx::OrderEventStatus::NEW:
        return "new";
      case lrvx::OrderEventStatus::SUBMITTED:
        return "submitted";
      case lrvx::OrderEventStatus::ACCEPTED:
        return "accepted";
      case lrvx::OrderEventStatus::PARTIALLY_FILLED:
        return "partially_filled";
      case lrvx::OrderEventStatus::FILLED:
        return "filled";
      case lrvx::OrderEventStatus::PENDING_CANCEL:
        return "pending_cancel";
      case lrvx::OrderEventStatus::CANCELED:
        return "canceled";
      case lrvx::OrderEventStatus::EXPIRED:
        return "expired";
      case lrvx::OrderEventStatus::REJECTED:
        return "rejected";
      case lrvx::OrderEventStatus::REPLACED:
        return "replaced";
      case lrvx::OrderEventStatus::PENDING_TRIGGER:
        return "pending_trigger";
      case lrvx::OrderEventStatus::TRIGGERED:
        return "triggered";
      case lrvx::OrderEventStatus::TRAILING_UPDATED:
        return "trailing_updated";
      default:
        return "unknown";
    }
  }
};

}  // namespace

inline void bindPositions(py::module_& m)
{
  py::class_<PyPositionTracker>(m, "PositionTracker")
      .def(py::init<std::string>(), py::arg("cost_basis") = "fifo")
      .def("on_fill", &PyPositionTracker::onFill,
           "Record a fill",
           py::arg("symbol"), py::arg("side"), py::arg("price"), py::arg("quantity"))
      .def("position", &PyPositionTracker::position,
           "Signed position for symbol",
           py::arg("symbol"))
      .def("avg_entry_price", &PyPositionTracker::avgEntryPrice,
           "Average entry price for symbol",
           py::arg("symbol"))
      .def("realized_pnl", &PyPositionTracker::realizedPnl,
           "Realized PnL for symbol",
           py::arg("symbol"))
      .def("total_realized_pnl", &PyPositionTracker::totalRealizedPnl,
           "Total realized PnL across all symbols");

  py::class_<PyPositionGroupTracker>(m, "PositionGroupTracker")
      .def(py::init<>())
      .def("open_position", &PyPositionGroupTracker::openPosition,
           "Open a new individual position, returns position id",
           py::arg("order_id"), py::arg("symbol"), py::arg("side"),
           py::arg("price"), py::arg("quantity"))
      .def("close_position", &PyPositionGroupTracker::closePosition,
           "Close a position at exit price",
           py::arg("position_id"), py::arg("exit_price"))
      .def("partial_close", &PyPositionGroupTracker::partialClose,
           "Partially close a position",
           py::arg("position_id"), py::arg("quantity"), py::arg("exit_price"))
      .def("create_group", &PyPositionGroupTracker::createGroup,
           "Create a position group, returns group id",
           py::arg("parent_id") = 0)
      .def("assign_to_group", &PyPositionGroupTracker::assignToGroup,
           "Assign a position to a group",
           py::arg("position_id"), py::arg("group_id"))
      .def("get_position", &PyPositionGroupTracker::getPosition,
           "Get position dict or None",
           py::arg("position_id"))
      .def("net_position", &PyPositionGroupTracker::netPosition,
           "Net position for symbol",
           py::arg("symbol"))
      .def("group_net_position", &PyPositionGroupTracker::groupNetPosition,
           "Net position for a group",
           py::arg("group_id"))
      .def("realized_pnl", &PyPositionGroupTracker::realizedPnl,
           "Realized PnL for symbol",
           py::arg("symbol"))
      .def("total_realized_pnl", &PyPositionGroupTracker::totalRealizedPnl,
           "Total realized PnL across all symbols")
      .def("group_realized_pnl", &PyPositionGroupTracker::groupRealizedPnl,
           "Realized PnL for a group",
           py::arg("group_id"))
      .def("group_unrealized_pnl", &PyPositionGroupTracker::groupUnrealizedPnl,
           "Unrealized PnL for a group at current price",
           py::arg("group_id"), py::arg("current_price"))
      .def("open_positions", &PyPositionGroupTracker::openPositions,
           "List of open position dicts for symbol",
           py::arg("symbol"))
      .def("open_position_count", &PyPositionGroupTracker::openPositionCount,
           "Count of open positions (optionally filtered by symbol)",
           py::arg("symbol") = py::none())
      .def("prune_closed", &PyPositionGroupTracker::pruneClosed,
           "Remove closed positions from memory");

  py::class_<PyOrderTracker>(m, "OrderTracker")
      .def(py::init<>())
      .def("on_submitted", &PyOrderTracker::onSubmitted,
           "Record order submission",
           py::arg("order_id"), py::arg("exchange_order_id"),
           py::arg("client_order_id") = "")
      .def("on_filled", &PyOrderTracker::onFilled,
           "Record order fill",
           py::arg("order_id"), py::arg("fill_quantity"))
      .def("on_canceled", &PyOrderTracker::onCanceled,
           "Record order cancellation",
           py::arg("order_id"))
      .def("on_rejected", &PyOrderTracker::onRejected,
           "Record order rejection",
           py::arg("order_id"), py::arg("reason"))
      .def("get", &PyOrderTracker::get,
           "Get order state dict or None",
           py::arg("order_id"))
      .def("is_active", &PyOrderTracker::isActive,
           "Check if order is active",
           py::arg("order_id"))
      .def("active_count", &PyOrderTracker::activeCount,
           "Number of active orders")
      .def("total_count", &PyOrderTracker::totalCount,
           "Total tracked orders")
      .def("prune_terminal", &PyOrderTracker::pruneTerminal,
           "Remove terminal orders from memory");
}
