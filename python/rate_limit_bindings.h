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

#include "lrvx/backtest/rate_limit_policy.h"

namespace py = pybind11;

namespace lrvx
{

inline void bindRateLimitPolicy(py::module_& m)
{
  // Note: no .export_values() here — `Account` collides with the
  // top-level `lrvx.Account` class. Access via
  // `lrvx.RateLimitEndpointFamily.{Trading,MarketData,Account}`.
  py::enum_<lrvx::RateLimitPolicy::EndpointFamily>(m, "RateLimitEndpointFamily")
      .value("Trading", lrvx::RateLimitPolicy::EndpointFamily::Trading)
      .value("MarketData", lrvx::RateLimitPolicy::EndpointFamily::MarketData)
      .value("Account", lrvx::RateLimitPolicy::EndpointFamily::Account);
  py::class_<lrvx::RateLimitPolicy>(m, "RateLimitPolicy")
      .def(py::init<>())
      .def("add_bucket", &lrvx::RateLimitPolicy::addBucket, py::arg("name"),
           py::arg("window_ns"), py::arg("capacity"), py::arg("submit_weight") = 1,
           py::arg("cancel_weight") = 1, py::arg("replace_weight") = 2,
           py::arg("family") = lrvx::RateLimitPolicy::EndpointFamily::Trading,
           py::arg("query_weight") = 1)
      .def("add_family_bucket", [](lrvx::RateLimitPolicy& p, lrvx::RateLimitPolicy::EndpointFamily family, const std::string& name, int64_t window_ns, uint32_t capacity, uint32_t query_weight)
           { p.addFamilyBucket(family, name, window_ns, capacity, query_weight); }, py::arg("family"), py::arg("name"), py::arg("window_ns"), py::arg("capacity"), py::arg("query_weight") = 1)
      .def("set_ban", &lrvx::RateLimitPolicy::setBan, py::arg("after_consecutive_rejects"), py::arg("ban_duration_ns"))
      .def("try_consume", [](lrvx::RateLimitPolicy& p, const std::string& action, int64_t now_ns)
           {
             using Kind = lrvx::RateLimitPolicy::ActionKind;
             const Kind a = parseChoice<Kind>("rate limit action", action,
                                              {{"submit", Kind::Submit},
                                               {"cancel", Kind::Cancel},
                                               {"replace", Kind::Replace},
                                               {"query_account", Kind::QueryAccount},
                                               {"query_market_data", Kind::QueryMarketData}});
             return p.tryConsume(a, now_ns); }, py::arg("action"), py::arg("now_ns"))
      .def("bucket_states", [](lrvx::RateLimitPolicy& p, int64_t now_ns)
           {
             py::list out;
             for (const auto& s : p.bucketStates(now_ns))
             {
               py::dict d;
               d["name"] = s.name;
               d["endpoint_family"] = s.endpointFamily;
               d["window_ns"] = s.windowNs;
               d["used"] = s.used;
               d["capacity"] = s.capacity;
               out.append(d);
             }
             return out; }, py::arg("now_ns"))
      .def("ban_until_ns", &lrvx::RateLimitPolicy::banUntilNs)
      .def("consecutive_rejects", &lrvx::RateLimitPolicy::consecutiveRejects)
      .def("bucket_count", &lrvx::RateLimitPolicy::bucketCount)
      .def_static("binance_um_futures", &lrvx::RateLimitPolicy::binance_um_futures)
      .def_static("bybit_linear", &lrvx::RateLimitPolicy::bybit_linear)
      .def_static("okx_swap", &lrvx::RateLimitPolicy::okx_swap)
      .def_static("deribit", &lrvx::RateLimitPolicy::deribit);
}

}  // namespace lrvx
