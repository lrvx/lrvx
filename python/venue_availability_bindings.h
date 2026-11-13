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

#include "lrvx/backtest/venue_availability.h"

namespace py = pybind11;

namespace lrvx
{

inline lrvx::OnOutage parseOnOutage(const std::string& s)
{
  return parseChoice<lrvx::OnOutage>("open-order policy", s,
                                     {{"cancel_all", lrvx::OnOutage::CANCEL_ALL},
                                      {"hold", lrvx::OnOutage::HOLD},
                                      {"expire_gtc_after", lrvx::OnOutage::EXPIRE_GTC_AFTER}});
}

inline lrvx::OutageType parseOutageType(const std::string& s)
{
  return parseChoice<lrvx::OutageType>("outage type", s,
                                       {{"total", lrvx::OutageType::Total},
                                        {"submit_only_down", lrvx::OutageType::SubmitOnlyDown},
                                        {"submit_only", lrvx::OutageType::SubmitOnlyDown},
                                        {"cancel_only_down", lrvx::OutageType::CancelOnlyDown},
                                        {"cancel_only", lrvx::OutageType::CancelOnlyDown},
                                        {"slow_degradation", lrvx::OutageType::SlowDegradation},
                                        {"stale_book", lrvx::OutageType::StaleBook},
                                        {"wrong_side_recovery",
                                         lrvx::OutageType::WrongSideRecovery}});
}

inline void bindVenueAvailability(py::module_& m)
{
  py::class_<lrvx::VenueAvailability>(m, "VenueAvailability")
      .def(py::init<>())
      .def(
          "schedule_outage",
          [](lrvx::VenueAvailability& self, int64_t start_ns, int64_t duration_ns,
             const std::string& policy, int64_t gtc_ttl_ns)
          {
            self.scheduleOutage(start_ns, duration_ns, parseOnOutage(policy), gtc_ttl_ns);
            return &self;
          },
          py::arg("start_ns"), py::arg("duration_ns"),
          py::arg("on_open_orders") = std::string("cancel_all"),
          py::arg("gtc_ttl_ns") = 0,
          py::return_value_policy::reference_internal)
      .def(
          "auto_random_outages",
          [](lrvx::VenueAvailability& self, double per_day, int64_t mean_duration_ns,
             const std::string& policy, uint64_t seed)
          {
            self.autoRandomOutages(per_day, mean_duration_ns, parseOnOutage(policy), seed);
            return &self;
          },
          py::arg("per_day"), py::arg("mean_duration_ns"),
          py::arg("on_open_orders") = std::string("cancel_all"),
          py::arg("seed") = 0xC0FFEEULL,
          py::return_value_policy::reference_internal)
      .def(
          "schedule_outage_ex",
          [](lrvx::VenueAvailability& self, int64_t start_ns, int64_t duration_ns,
             const std::string& outage_type, const std::string& policy,
             int64_t gtc_ttl_ns, double degradation_latency_multiplier,
             double wrong_side_recovery_bps)
          {
            self.scheduleOutageEx(start_ns, duration_ns, parseOutageType(outage_type),
                                  parseOnOutage(policy), gtc_ttl_ns,
                                  degradation_latency_multiplier,
                                  wrong_side_recovery_bps);
            return &self;
          },
          py::arg("start_ns"), py::arg("duration_ns"),
          py::arg("outage_type") = std::string("total"),
          py::arg("on_open_orders") = std::string("cancel_all"),
          py::arg("gtc_ttl_ns") = 0,
          py::arg("degradation_latency_multiplier") = 1.0,
          py::arg("wrong_side_recovery_bps") = 0.0,
          py::return_value_policy::reference_internal)
      .def("is_up", &lrvx::VenueAvailability::isUp, py::arg("now_ns"))
      .def("submits_allowed", &lrvx::VenueAvailability::submitsAllowed,
           py::arg("now_ns"))
      .def("cancels_allowed", &lrvx::VenueAvailability::cancelsAllowed,
           py::arg("now_ns"))
      .def("book_updates_allowed", &lrvx::VenueAvailability::bookUpdatesAllowed,
           py::arg("now_ns"))
      .def("trades_allowed", &lrvx::VenueAvailability::tradesAllowed,
           py::arg("now_ns"))
      .def("latency_multiplier", &lrvx::VenueAvailability::latencyMultiplier,
           py::arg("now_ns"))
      .def("consume_wrong_side_recovery_bps",
           &lrvx::VenueAvailability::consumeWrongSideRecoveryBps);
}

}  // namespace lrvx
