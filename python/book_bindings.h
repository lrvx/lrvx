// python/book_bindings.h

#pragma once

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "bindings_common.h"
#include "lrvx/book/book_update.h"
#include "lrvx/book/events/book_update_event.h"
#include "lrvx/book/l3/l3_order_book.h"
#include "lrvx/book/nlevel_order_book.h"
#include "lrvx/common.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <memory_resource>

namespace py = pybind11;

namespace
{

using namespace lrvx;

// ─── NLevelOrderBook wrapper ────────────────────────────────────────────────

class PyOrderBook
{
 public:
  explicit PyOrderBook(double tickSize) : _book(Price::fromDouble(tickSize)) {}

  void applySnapshot(py::array_t<double, py::array::c_style | py::array::forcecast> bidPx,
                     py::array_t<double, py::array::c_style | py::array::forcecast> bidQty,
                     py::array_t<double, py::array::c_style | py::array::forcecast> askPx,
                     py::array_t<double, py::array::c_style | py::array::forcecast> askQty)
  {
    applyUpdate(bidPx, bidQty, askPx, askQty, BookUpdateType::SNAPSHOT);
  }

  void applyDelta(py::array_t<double, py::array::c_style | py::array::forcecast> bidPx,
                  py::array_t<double, py::array::c_style | py::array::forcecast> bidQty,
                  py::array_t<double, py::array::c_style | py::array::forcecast> askPx,
                  py::array_t<double, py::array::c_style | py::array::forcecast> askQty)
  {
    applyUpdate(bidPx, bidQty, askPx, askQty, BookUpdateType::DELTA);
  }

  py::object bestBid() const
  {
    auto v = _book.bestBid();
    if (!v)
    {
      return py::none();
    }
    return py::cast(v->toDouble());
  }

  py::object bestAsk() const
  {
    auto v = _book.bestAsk();
    if (!v)
    {
      return py::none();
    }
    return py::cast(v->toDouble());
  }

  py::object mid() const
  {
    auto v = _book.mid();
    if (!v)
    {
      return py::none();
    }
    return py::cast(v->toDouble());
  }

  py::object spread() const
  {
    auto v = _book.spread();
    if (!v)
    {
      return py::none();
    }
    return py::cast(v->toDouble());
  }

  double bidAtPrice(double price) const
  {
    return _book.bidAtPrice(Price::fromDouble(price)).toDouble();
  }

  double askAtPrice(double price) const
  {
    return _book.askAtPrice(Price::fromDouble(price)).toDouble();
  }

  py::array_t<double> getBids(size_t maxLevels) const
  {
    auto levels = _book.getBidLevels(maxLevels);
    py::array_t<double> result({static_cast<py::ssize_t>(levels.size()), static_cast<py::ssize_t>(2)});
    auto buf = result.mutable_unchecked<2>();
    for (size_t i = 0; i < levels.size(); ++i)
    {
      buf(i, 0) = levels[i].price.toDouble();
      buf(i, 1) = levels[i].quantity.toDouble();
    }
    return result;
  }

  py::array_t<double> getAsks(size_t maxLevels) const
  {
    auto levels = _book.getAskLevels(maxLevels);
    py::array_t<double> result({static_cast<py::ssize_t>(levels.size()), static_cast<py::ssize_t>(2)});
    auto buf = result.mutable_unchecked<2>();
    for (size_t i = 0; i < levels.size(); ++i)
    {
      buf(i, 0) = levels[i].price.toDouble();
      buf(i, 1) = levels[i].quantity.toDouble();
    }
    return result;
  }

  py::tuple consumeAsks(double quantity) const
  {
    auto [filledQty, totalCost] = _book.consumeAsks(Quantity::fromDouble(quantity));
    return py::make_tuple(filledQty.toDouble(), totalCost.toDouble());
  }

  py::tuple consumeBids(double quantity) const
  {
    auto [filledQty, totalCost] = _book.consumeBids(Quantity::fromDouble(quantity));
    return py::make_tuple(filledQty.toDouble(), totalCost.toDouble());
  }

  bool isCrossed() const { return _book.isCrossed(); }
  void clear() { _book.clear(); }

 private:
  void applyUpdate(py::array_t<double, py::array::c_style | py::array::forcecast> bidPx,
                   py::array_t<double, py::array::c_style | py::array::forcecast> bidQty,
                   py::array_t<double, py::array::c_style | py::array::forcecast> askPx,
                   py::array_t<double, py::array::c_style | py::array::forcecast> askQty,
                   BookUpdateType type)
  {
    size_t nb = bidPx.size();
    size_t na = askPx.size();
    // bidQty/askQty were read alongside bidPx/askPx up to nb/na with no
    // check that they actually have nb/na elements -- a caller passing a
    // shorter quantities array (a truncated column, a bad zip) read past
    // its end.
    checkSameSize(nb, static_cast<size_t>(bidQty.size()), "bid_px and bid_qty size");
    checkSameSize(na, static_cast<size_t>(askQty.size()), "ask_px and ask_qty size");
    auto* bp = bidPx.data();
    auto* bq = bidQty.data();
    auto* ap = askPx.data();
    auto* aq = askQty.data();

    std::byte buf[32768];
    std::pmr::monotonic_buffer_resource res(buf, sizeof(buf));
    BookUpdateEvent ev(&res);
    ev.update.type = type;

    ev.update.bids.reserve(nb);
    for (size_t i = 0; i < nb; ++i)
    {
      ev.update.bids.push_back({Price::fromDouble(bp[i]), Quantity::fromDouble(bq[i])});
    }
    ev.update.asks.reserve(na);
    for (size_t i = 0; i < na; ++i)
    {
      ev.update.asks.push_back({Price::fromDouble(ap[i]), Quantity::fromDouble(aq[i])});
    }

    _book.applyBookUpdate(ev);
  }

  NLevelOrderBook<8192> _book;
};

// ─── L3OrderBook wrapper ────────────────────────────────────────────────────

inline const char* orderStatusStr(OrderStatus s)
{
  switch (s)
  {
    case OrderStatus::Ok:
      return "ok";
    case OrderStatus::NoCapacity:
      return "no_capacity";
    case OrderStatus::NotFound:
      return "not_found";
    case OrderStatus::Extant:
      return "extant";
    default:
      return "unknown";
  }
}

class PyL3Book
{
 public:
  PyL3Book() = default;

  const char* addOrder(uint64_t orderId, double price, double quantity, const std::string& side)
  {
    const Side s = parseSide(side);
    auto status = _book.addOrder(orderId, Price::fromDouble(price), Quantity::fromDouble(quantity), s);
    return orderStatusStr(status);
  }

  const char* removeOrder(uint64_t orderId)
  {
    return orderStatusStr(_book.removeOrder(orderId));
  }

  const char* modifyOrder(uint64_t orderId, double newQuantity)
  {
    return orderStatusStr(_book.modifyOrder(orderId, Quantity::fromDouble(newQuantity)));
  }

  py::object bestBid()
  {
    auto v = _book.bestBid();
    if (!v)
    {
      return py::none();
    }
    return py::cast(v->toDouble());
  }

  py::object bestAsk()
  {
    auto v = _book.bestAsk();
    if (!v)
    {
      return py::none();
    }
    return py::cast(v->toDouble());
  }

  double bidAtPrice(double price) const
  {
    return _book.bidAtPrice(Price::fromDouble(price)).toDouble();
  }

  double askAtPrice(double price) const
  {
    return _book.askAtPrice(Price::fromDouble(price)).toDouble();
  }

  py::list exportSnapshot() const
  {
    auto snap = _book.exportSnapshot();
    py::list result;
    for (const auto& o : snap.orders_)
    {
      py::dict d;
      d["id"] = o.id;
      d["price"] = o.price.toDouble();
      d["quantity"] = o.quantity.toDouble();
      d["side"] = (o.side == Side::BUY) ? "buy" : "sell";
      result.append(d);
    }
    return result;
  }

  void buildFromSnapshot(py::list orders)
  {
    L3Snapshot snap;
    for (auto item : orders)
    {
      auto d = item.cast<py::dict>();
      OrderSnapshot o;
      o.id = d["id"].cast<uint64_t>();
      o.price = Price::fromDouble(d["price"].cast<double>());
      o.quantity = Quantity::fromDouble(d["quantity"].cast<double>());
      std::string side = d["side"].cast<std::string>();
      o.side = parseSide(side);
      snap.orders_.push_back(o);
    }
    _book.buildFromSnapshot(snap);
  }

 private:
  L3OrderBook<8192> _book;
};

}  // namespace

inline void bindBooks(py::module_& m)
{
  py::class_<PyOrderBook>(m, "OrderBook")
      .def(py::init<double>(), py::arg("tick_size"))
      .def("apply_snapshot", &PyOrderBook::applySnapshot,
           "Apply a full book snapshot",
           py::arg("bid_prices"), py::arg("bid_quantities"),
           py::arg("ask_prices"), py::arg("ask_quantities"))
      .def("apply_delta", &PyOrderBook::applyDelta,
           "Apply an incremental book update",
           py::arg("bid_prices"), py::arg("bid_quantities"),
           py::arg("ask_prices"), py::arg("ask_quantities"))
      .def("best_bid", &PyOrderBook::bestBid, "Best bid price or None")
      .def("best_ask", &PyOrderBook::bestAsk, "Best ask price or None")
      .def("mid", &PyOrderBook::mid, "Mid price or None")
      .def("spread", &PyOrderBook::spread, "Bid-ask spread or None")
      .def("bid_at_price", &PyOrderBook::bidAtPrice, py::arg("price"))
      .def("ask_at_price", &PyOrderBook::askAtPrice, py::arg("price"))
      .def("get_bids", &PyOrderBook::getBids, "Get bid levels as Nx2 array [price, qty]",
           py::arg("max_levels") = 20)
      .def("get_asks", &PyOrderBook::getAsks, "Get ask levels as Nx2 array [price, qty]",
           py::arg("max_levels") = 20)
      .def("consume_asks", &PyOrderBook::consumeAsks,
           "Simulate market buy: returns (filled_qty, total_cost)", py::arg("quantity"))
      .def("consume_bids", &PyOrderBook::consumeBids,
           "Simulate market sell: returns (filled_qty, total_cost)", py::arg("quantity"))
      .def("is_crossed", &PyOrderBook::isCrossed)
      .def("clear", &PyOrderBook::clear);

  py::class_<PyL3Book>(m, "L3Book")
      .def(py::init<>())
      .def("add_order", &PyL3Book::addOrder,
           py::arg("order_id"), py::arg("price"), py::arg("quantity"), py::arg("side"))
      .def("remove_order", &PyL3Book::removeOrder, py::arg("order_id"))
      .def("modify_order", &PyL3Book::modifyOrder, py::arg("order_id"), py::arg("new_quantity"))
      .def("best_bid", &PyL3Book::bestBid)
      .def("best_ask", &PyL3Book::bestAsk)
      .def("bid_at_price", &PyL3Book::bidAtPrice, py::arg("price"))
      .def("ask_at_price", &PyL3Book::askAtPrice, py::arg("price"))
      .def("export_snapshot", &PyL3Book::exportSnapshot)
      .def("build_from_snapshot", &PyL3Book::buildFromSnapshot, py::arg("orders"));
}
