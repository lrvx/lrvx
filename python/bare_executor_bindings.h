/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// python/bare_executor_bindings.h
//
// Direct py::class_<lrvx::SimulatedExecutor> bindings, so the executor
// instance owned by a VenueStack can be driven from Python via
// stack.executor(). The standalone PySimulatedExecutor wrapper in
// backtest_bindings.h owns its own SimulatedExecutor and is unrelated;
// VenueStack hands out a reference to its internal one, which without
// this binding has no Python-visible methods.
//
// Initial method surface is the slim subset needed by RL env
// venue-stack backing, venue-stack broker plumbing (PaperBroker / CcxtBroker),
// and docs/examples/python_rate_limit_aware_market_maker.py:
// submit / cancel / book-and-trade updates / fills inspection / rate
// limit + venue availability setters. Iceberg, brackets, slippage, and
// queue-model accessors land in follow-up PRs as Phase 2 tasks need
// them.

#pragma once

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "bindings_common.h"
#include "lrvx/backtest/rate_limit_policy.h"
#include "lrvx/backtest/simulated_executor.h"
#include "lrvx/backtest/venue_availability.h"
#include "lrvx/common.h"
#include "lrvx/execution/order.h"
#include "order_type_bindings.h"

#include <cstdint>
#include <string>

namespace py = pybind11;

namespace lrvx
{

inline void bindBareExecutor(py::module_& m)
{
  using namespace lrvx;

  auto parseSide = [](const std::string& s) -> Side
  { return ::parseSide(s); };

  // The canonical table, not a copy of it: see
  // python/order_type_bindings.h. An unrecognised name raises instead of
  // quietly becoming a market order.
  auto parseOrderType = [](const std::string& s) -> OrderType
  { return lrvx::parseOrderTypeStrict(s); };

  auto parseTif = [](const std::string& s) -> TimeInForce
  { return parseTimeInForceWithExpiry(s); };

  py::class_<lrvx::SimulatedExecutor>(
      m, "VenueExecutor",
      "Executor handed out by VenueStack.executor(). Same simulated\n"
      "engine as standalone SimulatedExecutor, but the instance is\n"
      "owned by the venue stack and shares its account, fees, funding,\n"
      "liquidation, and rate-limit policy. Use this when you want to\n"
      "drive the venue-realistic stack from Python — RL envs, paper\n"
      "brokers, custom Strategy harnesses.")
      .def(
          "submit_order",
          [parseSide, parseOrderType, parseTif](
              lrvx::SimulatedExecutor& self, uint64_t id,
              const std::string& side, double price, double qty,
              const std::string& type, uint32_t symbol,
              const std::string& tif, bool reduce_only,
              int64_t expires_at_ns, uint64_t account_id, double trigger)
          {
            Order order;
            order.id = id;
            order.side = parseSide(side);
            order.price = Price::fromDouble(price);
            order.quantity = Quantity::fromDouble(qty);
            order.symbol = symbol;
            order.type = parseOrderType(type);
            order.timeInForce = parseTif(tif);
            order.flags.reduceOnly = reduce_only ? 1 : 0;
            order.accountId = account_id;
            if (expires_at_ns > 0)
            {
              order.expiresAfter = TimePoint(
                  std::chrono::nanoseconds(expires_at_ns));
            }

            // The simulator arms a conditional order off
            // Order::triggerPrice, which this binding never set: every
            // stop and take-profit submitted here carried a zero trigger
            // and fired on the first print. Same rule as the standalone
            // SimulatedExecutor binding -- `price` doubles as the trigger
            // for the market-style conditionals when the caller left
            // `trigger` unset, and the limit-style pair needs both.
            const bool isConditional = order.type == OrderType::STOP_MARKET ||
                                       order.type == OrderType::STOP_LIMIT ||
                                       order.type == OrderType::TAKE_PROFIT_MARKET ||
                                       order.type == OrderType::TAKE_PROFIT_LIMIT;
            if (isConditional)
            {
              order.triggerPrice =
                  Price::fromDouble(trigger > 0.0 ? trigger : price);
            }
            self.submitOrder(order);
          },
          "Submit an order. side: buy|sell. type: one of "
          "lrvx.ORDER_TYPE_NAMES (market|limit|stop_market|stop_limit|"
          "tp_market|tp_limit|trailing_stop|iceberg); anything else raises "
          "ValueError. tif: gtc|ioc|fok|gtd|post_only. trigger: arming "
          "price for the conditional types, defaulting to `price`.",
          py::arg("id"), py::arg("side"), py::arg("price"), py::arg("quantity"),
          py::arg("type") = "market", py::arg("symbol") = 1,
          py::arg("tif") = "gtc", py::arg("reduce_only") = false,
          py::arg("expires_at_ns") = 0, py::arg("account_id") = 0,
          py::arg("trigger") = 0.0)
      .def(
          "cancel_order",
          [](lrvx::SimulatedExecutor& self, uint64_t order_id)
          { self.cancelOrder(order_id); },
          py::arg("order_id"))
      .def(
          "cancel_all",
          [](lrvx::SimulatedExecutor& self, uint32_t symbol)
          { self.cancelAllOrders(symbol); },
          py::arg("symbol"))
      .def(
          "on_trade",
          [](lrvx::SimulatedExecutor& self, uint32_t symbol, double price,
             bool is_buy)
          { self.onTrade(symbol, Price::fromDouble(price), is_buy); },
          "Feed a trade tick for order matching.",
          py::arg("symbol"), py::arg("price"), py::arg("is_buy"))
      .def(
          "on_trade_qty",
          [](lrvx::SimulatedExecutor& self, uint32_t symbol, double price,
             double quantity, bool is_buy)
          {
            self.onTrade(symbol, Price::fromDouble(price),
                         Quantity::fromDouble(quantity), is_buy);
          },
          "Feed a trade tick with quantity (enables queue-fill simulation).",
          py::arg("symbol"), py::arg("price"), py::arg("quantity"),
          py::arg("is_buy"))
      .def(
          "on_bar",
          [](lrvx::SimulatedExecutor& self, uint32_t symbol, double close_price)
          { self.onBar(symbol, Price::fromDouble(close_price)); },
          "Feed a bar close for order matching.",
          py::arg("symbol"), py::arg("close_price"))
      .def(
          "fills_list",
          [](const lrvx::SimulatedExecutor& self)
          {
            py::list out;
            for (const auto& f : self.fills())
            {
              py::dict d;
              d["order_id"] = f.orderId;
              d["symbol"] = f.symbol;
              d["side"] = (f.side == Side::BUY) ? "buy" : "sell";
              d["price"] = f.price.toDouble();
              d["quantity"] = f.quantity.toDouble();
              d["timestamp_ns"] = f.timestampNs.raw();
              d["is_maker"] = f.isMaker;
              out.append(d);
            }
            return out;
          },
          "Every fill recorded on this executor as a list of dicts.")
      .def_property_readonly(
          "fill_count",
          [](const lrvx::SimulatedExecutor& self)
          { return self.fills().size(); })
      .def(
          "clear_fills",
          [](lrvx::SimulatedExecutor& self)
          { self.extractFills(); },
          "Discard every fill recorded so far. `fills_list()` and "
          "`fill_count` read the same accumulating buffer as the plain "
          "PySimulatedExecutor wrapper; nothing resets it between runs "
          "on its own, so a caller that reuses one executor across "
          "repeated episodes (an RL training loop, a walk-forward fold) "
          "must call this explicitly or fills keep growing forever.")
      .def(
          "set_rate_limit_policy",
          [](lrvx::SimulatedExecutor& self, const lrvx::RateLimitPolicy& p)
          { self.setRateLimitPolicy(p); },
          py::arg("policy"))
      .def(
          "clear_rate_limit_policy",
          [](lrvx::SimulatedExecutor& self)
          { self.clearRateLimitPolicy(); })
      .def(
          "set_venue_availability",
          [](lrvx::SimulatedExecutor& self, lrvx::VenueAvailability* va)
          { self.setVenueAvailability(va); },
          py::arg("venue_availability"));
}

}  // namespace lrvx
