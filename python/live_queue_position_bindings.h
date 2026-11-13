/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "bindings_common.h"

#include "lrvx/execution/live_queue_position_estimator.h"

namespace py = pybind11;

namespace lrvx
{

inline void bindLiveQueuePositionEstimator(py::module_& m)
{
  py::class_<lrvx::LiveQueuePositionEstimator>(m, "LiveQueuePositionEstimator")
      .def(py::init<>())
      .def("set_confidence_half_life_ns",
           &lrvx::LiveQueuePositionEstimator::setConfidenceHalfLifeNs,
           py::arg("half_life_ns"))
      .def("set_shrink_attribution_factor",
           &lrvx::LiveQueuePositionEstimator::setShrinkAttributionFactor,
           py::arg("factor"))
      .def(
          "on_order_placed",
          [](lrvx::LiveQueuePositionEstimator& e, uint32_t symbol, uint8_t side,
             double price, uint64_t order_id, double order_qty, double level_qty,
             int64_t ts_ns)
          {
            e.onOrderPlaced(symbol, static_cast<lrvx::Side>(side),
                            lrvx::Price::fromDouble(price), order_id,
                            lrvx::Quantity::fromDouble(order_qty),
                            lrvx::Quantity::fromDouble(level_qty), ts_ns);
          },
          py::arg("symbol"), py::arg("side"), py::arg("price"), py::arg("order_id"),
          py::arg("order_qty"), py::arg("level_qty_now"), py::arg("ts_ns") = 0)
      .def("on_order_cancelled", &lrvx::LiveQueuePositionEstimator::onOrderCancelled,
           py::arg("order_id"), py::arg("ts_ns") = 0)
      .def(
          "on_order_filled",
          [](lrvx::LiveQueuePositionEstimator& e, uint64_t order_id,
             double cumulative_fill, int64_t ts_ns)
          {
            e.onOrderFilled(order_id, lrvx::Quantity::fromDouble(cumulative_fill), ts_ns);
          },
          py::arg("order_id"), py::arg("cumulative_fill"), py::arg("ts_ns") = 0)
      .def(
          "on_trade",
          [](lrvx::LiveQueuePositionEstimator& e, uint32_t symbol, double price,
             double qty, int64_t ts_ns)
          {
            e.onTrade(symbol, lrvx::Price::fromDouble(price),
                      lrvx::Quantity::fromDouble(qty), ts_ns);
          },
          py::arg("symbol"), py::arg("price"), py::arg("qty"), py::arg("ts_ns") = 0)
      .def(
          "on_trade_with_flag",
          [](lrvx::LiveQueuePositionEstimator& e, uint32_t symbol, double price,
             double qty, int64_t ts_ns, bool is_hidden)
          {
            e.onTradeWithFlag(symbol, lrvx::Price::fromDouble(price),
                              lrvx::Quantity::fromDouble(qty), ts_ns, is_hidden);
          },
          py::arg("symbol"), py::arg("price"), py::arg("qty"), py::arg("ts_ns") = 0,
          py::arg("is_hidden") = false)
      .def("set_hidden_order_policy", [](lrvx::LiveQueuePositionEstimator& e, const std::string& policy)
           {
             const auto p = parseChoice<lrvx::HiddenOrderPolicy>(
                 "hidden order policy", policy,
                 {{"ignore", lrvx::HiddenOrderPolicy::Ignore},
                  {"trust_trade_flag", lrvx::HiddenOrderPolicy::TrustTradeFlag},
                  {"infer_if_trade_exceeds_visible",
                   lrvx::HiddenOrderPolicy::InferIfTradeExceedsVisible}});
             e.setHiddenOrderPolicy(p); }, py::arg("policy"))
      .def("on_level_update", [](lrvx::LiveQueuePositionEstimator& e, uint32_t symbol, uint8_t side, double price, double new_qty, int64_t ts_ns)
           { e.onLevelUpdate(symbol, static_cast<lrvx::Side>(side),
                             lrvx::Price::fromDouble(price),
                             lrvx::Quantity::fromDouble(new_qty), ts_ns); }, py::arg("symbol"), py::arg("side"), py::arg("price"), py::arg("new_qty"), py::arg("ts_ns") = 0)
      .def("snapshot", [](const lrvx::LiveQueuePositionEstimator& e, uint64_t order_id, int64_t now_ns) -> py::object
           {
            auto s = e.snapshot(order_id, now_ns);
            if (!s.has_value())
            {
              return py::none();
            }
            py::dict d;
            d["order_id"] = s->orderId;
            d["queue_ahead_est"] = s->queueAheadEst.toDouble();
            d["total"] = s->total.toDouble();
            d["confidence"] = s->confidence;
            d["last_update_ns"] = s->lastUpdateNs;
            d["hidden_volume_seen"] = s->hiddenVolumeSeen.toDouble();
            return d; }, py::arg("order_id"), py::arg("now_ns") = 0)
      .def("tracked_order_count", &lrvx::LiveQueuePositionEstimator::trackedOrderCount);
}

}  // namespace lrvx
