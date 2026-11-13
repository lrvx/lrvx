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

#include "lrvx/execution/order_group.h"

namespace py = pybind11;

namespace lrvx
{

inline void bindOrderGroup(py::module_& m)
{
  py::enum_<lrvx::OrderGroupPolicy>(m, "OrderGroupPolicy")
      .value("BEST_EFFORT", lrvx::OrderGroupPolicy::BestEffort)
      .value("ALL_OR_NOTHING", lrvx::OrderGroupPolicy::AllOrNothing)
      .value("ONE_SIDED", lrvx::OrderGroupPolicy::OneSided)
      .export_values();

  py::enum_<lrvx::LegState>(m, "LegState")
      .value("PENDING", lrvx::LegState::Pending)
      .value("SUBMITTED", lrvx::LegState::Submitted)
      .value("PARTIALLY_FILLED", lrvx::LegState::PartiallyFilled)
      .value("FILLED", lrvx::LegState::Filled)
      .value("CANCELLED", lrvx::LegState::Cancelled)
      .value("FAILED", lrvx::LegState::Failed)
      .export_values();

  py::enum_<lrvx::OrderGroupState>(m, "OrderGroupState")
      .value("PENDING", lrvx::OrderGroupState::Pending)
      .value("SUBMITTED", lrvx::OrderGroupState::Submitted)
      .value("PARTIALLY_FILLED", lrvx::OrderGroupState::PartiallyFilled)
      .value("FILLED", lrvx::OrderGroupState::Filled)
      .value("CANCELLED", lrvx::OrderGroupState::Cancelled)
      .value("REVERTING", lrvx::OrderGroupState::Reverting)
      .value("FAILED", lrvx::OrderGroupState::Failed)
      .export_values();

  py::class_<lrvx::OrderGroup>(m, "OrderGroup")
      .def(py::init([](uint64_t parent_signal_id, lrvx::OrderGroupPolicy policy)
                    { return new lrvx::OrderGroup(parent_signal_id, policy); }),
           py::arg("parent_signal_id") = 0,
           py::arg("policy") = lrvx::OrderGroupPolicy::BestEffort)
      .def("parent_signal_id", &lrvx::OrderGroup::parentSignalId)
      .def("policy", &lrvx::OrderGroup::policy)
      .def("add_market_leg", [](lrvx::OrderGroup& g, uint32_t symbol, uint8_t side, double qty)
           { return g.addMarketLeg(symbol, side, lrvx::Quantity::fromDouble(qty)); }, py::arg("symbol"), py::arg("side"), py::arg("qty"))
      .def("add_limit_leg", [](lrvx::OrderGroup& g, uint32_t symbol, uint8_t side, double price, double qty)
           { return g.addLimitLeg(symbol, side, lrvx::Price::fromDouble(price),
                                  lrvx::Quantity::fromDouble(qty)); }, py::arg("symbol"), py::arg("side"), py::arg("price"), py::arg("qty"))
      .def("leg_count", &lrvx::OrderGroup::legCount)
      .def("leg_state", [](const lrvx::OrderGroup& g, size_t idx)
           { return g.leg(idx).state; }, py::arg("leg_index"))
      .def("leg_filled", [](const lrvx::OrderGroup& g, size_t idx)
           { return g.leg(idx).filledQty.toDouble(); }, py::arg("leg_index"))
      .def("leg_order_id", [](const lrvx::OrderGroup& g, size_t idx)
           { return g.leg(idx).orderId; }, py::arg("leg_index"))
      .def("record_submit", &lrvx::OrderGroup::recordSubmit, py::arg("leg_index"), py::arg("order_id"))
      .def("record_fill", [](lrvx::OrderGroup& g, size_t idx, double cumulative_qty)
           { g.recordFill(idx, lrvx::Quantity::fromDouble(cumulative_qty)); }, py::arg("leg_index"), py::arg("cumulative_qty"))
      .def("record_cancel", &lrvx::OrderGroup::recordCancel, py::arg("leg_index"))
      .def("record_failure", &lrvx::OrderGroup::recordFailure, py::arg("leg_index"))
      .def("record_replace_accepted", &lrvx::OrderGroup::recordReplaceAccepted, py::arg("leg_index"), py::arg("new_order_id"))
      .def("record_replace_rejected", &lrvx::OrderGroup::recordReplaceRejected, py::arg("leg_index"))
      .def("find_leg_by_order_id", [](const lrvx::OrderGroup& g, uint64_t order_id) -> py::object
           {
            auto idx = g.findLegByOrderId(order_id);
            if (idx.has_value()){ return py::cast(*idx);
}
            return py::none(); }, py::arg("order_id"))
      .def("mark_action_dispatched", [](lrvx::OrderGroup& g, size_t idx, const std::string& kind)
           {
            const auto k = parseChoice<lrvx::OrderGroupAction::Kind>(
                "action kind", kind,
                {{"cancel", lrvx::OrderGroupAction::Kind::CancelLeg},
                 {"revert", lrvx::OrderGroupAction::Kind::RevertLeg}});
            g.markActionDispatched(idx, k); }, py::arg("leg_index"), py::arg("kind"))
      .def("auto_dispatch", [](lrvx::OrderGroup& g, py::object strategy)
           {
            // Dispatch every not-yet-dispatched recommended action
            // through the strategy's emit helpers. Marks each action
            // as dispatched so subsequent calls don't re-fire it.
            size_t fired = 0;
            for (const auto& a : g.recommendedActions())
            {
              if (a.kind == lrvx::OrderGroupAction::Kind::CancelLeg)
              {
                strategy.attr("emit_cancel")(a.orderId);
              }
              else
              {
                if (a.side == 0)
                {
                  strategy.attr("emit_market_buy")(a.symbol, a.qty.toDouble());
                }
                else
                {
                  strategy.attr("emit_market_sell")(a.symbol, a.qty.toDouble());
                }
              }
              g.markActionDispatched(a.legIndex, a.kind);
              ++fired;
            }
            return fired; }, py::arg("strategy"))
      .def("set_risk_limits", [](lrvx::OrderGroup& g, double max_gross_notional, double max_concentration_pct, double max_leg_qty)
           {
            lrvx::GroupRiskLimits limits;
            limits.maxGrossNotional = lrvx::Quantity::fromDouble(max_gross_notional);
            limits.maxConcentrationPct = max_concentration_pct;
            limits.maxLegQty = lrvx::Quantity::fromDouble(max_leg_qty);
            g.setRiskLimits(limits); }, py::arg("max_gross_notional") = 0.0, py::arg("max_concentration_pct") = 0.0, py::arg("max_leg_qty") = 0.0)
      .def("precheck_submission", [](const lrvx::OrderGroup& g, double equity, const std::vector<double>& market_ref_prices)
           {
            std::vector<lrvx::Price> prices;
            prices.reserve(market_ref_prices.size());
            for (double p : market_ref_prices){
              prices.push_back(lrvx::Price::fromDouble(p));
}
            auto breach = g.precheckSubmission(equity, prices);
            py::dict d;
            d["denied"] = breach.denied;
            d["rule"] = breach.rule;
            d["detail"] = breach.detail;
            return d; }, py::arg("equity") = 0.0, py::arg("market_ref_prices") = std::vector<double>{})
      .def("set_pair_latency_budget_ns", &lrvx::OrderGroup::setPairLatencyBudgetNs, py::arg("budget_ns"))
      .def("pair_latency_decision", [](const lrvx::OrderGroup& g, int64_t leader_submit_ts_ns, int64_t leader_ack_ts_ns, bool ack_received)
           {
            auto d = g.pairLatencyDecision(leader_submit_ts_ns, leader_ack_ts_ns,
                                            ack_received);
            switch (d)
            {
              case lrvx::PairLatencyDecision::Wait: return std::string("wait");
              case lrvx::PairLatencyDecision::SubmitFollower:
                return std::string("submit_follower");
              case lrvx::PairLatencyDecision::CancelLeader:
                return std::string("cancel_leader");
            }
            return std::string("wait"); }, py::arg("leader_submit_ts_ns"), py::arg("leader_ack_ts_ns"), py::arg("ack_received") = false)
      .def("state", &lrvx::OrderGroup::state)
      .def("recommended_actions", [](const lrvx::OrderGroup& g)
           {
        py::list out;
        for (const auto& a : g.recommendedActions())
        {
          py::dict d;
          d["kind"] = a.kind == lrvx::OrderGroupAction::Kind::CancelLeg ? "cancel" : "revert";
          d["leg_index"] = a.legIndex;
          if (a.kind == lrvx::OrderGroupAction::Kind::CancelLeg)
          {
            d["order_id"] = a.orderId;
          }
          else
          {
            d["symbol"] = a.symbol;
            d["side"] = a.side;
            d["qty"] = a.qty.toDouble();
          }
          out.append(d);
        }
        return out; });
}

}  // namespace lrvx
