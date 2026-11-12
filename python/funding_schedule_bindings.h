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

#include "lrvx/backtest/funding_schedule.h"
#include "lrvx/capi/lrvx_capi.h"

namespace py = pybind11;

namespace lrvx
{

inline void bindFundingSchedule(py::module_& m)
{
  py::class_<lrvx::FundingPayment>(m, "FundingPayment")
      // UnixNanos has no pybind caster on purpose; Python keeps integer ns.
      .def_property_readonly("timestamp_ns", [](const lrvx::FundingPayment& p)
                             { return p.timestampNs.raw(); })
      .def_readonly("symbol", &lrvx::FundingPayment::symbol)
      .def_readonly("rate", &lrvx::FundingPayment::rate)
      .def_readonly("mark_price", &lrvx::FundingPayment::markPrice)
      .def_readonly("position_signed", &lrvx::FundingPayment::positionSigned)
      .def_readonly("amount", &lrvx::FundingPayment::amount);

  py::class_<lrvx::FundingTapeEntry>(m, "FundingTapeEntry")
      .def(py::init<>())
      .def_property(
          "timestamp_ns", [](const lrvx::FundingTapeEntry& e)
          { return e.timestampNs.raw(); },
          [](lrvx::FundingTapeEntry& e, int64_t v)
          { e.timestampNs = lrvx::UnixNanos::fromRaw(v); })
      .def_readwrite("symbol", &lrvx::FundingTapeEntry::symbol)
      .def_readwrite("rate", &lrvx::FundingTapeEntry::rate);

  py::class_<lrvx::FundingSchedule>(m, "FundingSchedule")
      .def(py::init<>())
      .def_static("constant", &lrvx::FundingSchedule::constant, py::arg("interval_ns"),
                  py::arg("rate"))
      .def_static("tape", &lrvx::FundingSchedule::tape, py::arg("events"))
      .def_static("tape_by_symbol", &lrvx::FundingSchedule::tapeBySymbol,
                  py::arg("entries"))
      .def_static("binance_um_futures", &lrvx::FundingSchedule::binance_um_futures)
      .def_static("bybit_linear", &lrvx::FundingSchedule::bybit_linear)
      .def_static("okx_swap", &lrvx::FundingSchedule::okx_swap)
      .def_static("bitget_hourly", &lrvx::FundingSchedule::bitget_hourly)
      // Through the C API so that the reason for a False is on record for
      // lrvx.last_error(); a LrvxFundingScheduleHandle is a FundingSchedule*.
      .def(
          "load_tape",
          [](lrvx::FundingSchedule& self, const std::string& path)
          {
            lrvx_clear_last_error();
            return lrvx_funding_schedule_load_tape(&self, path.c_str()) != 0;
          },
          py::arg("path"),
          "Read CSV with columns timestamp_ns, symbol, funding_rate. "
          "Returns True on success; after a False, lrvx.last_error() names "
          "the file that could not be opened.")
      .def("set_constant_rate", &lrvx::FundingSchedule::setConstantRate,
           py::arg("rate"))
      .def("reset", &lrvx::FundingSchedule::reset)
      .def("interval_ns", &lrvx::FundingSchedule::intervalNs)
      .def("constant_rate", &lrvx::FundingSchedule::constantRate)
      .def("last_tick_ns", &lrvx::FundingSchedule::lastTickNs)
      .def("settlement_timestamps", &lrvx::FundingSchedule::settlementTimestamps)
      .def("per_symbol_tape", &lrvx::FundingSchedule::perSymbolTape)
      .def("tick", &lrvx::FundingSchedule::tick, py::arg("now_ns"), py::arg("symbols"),
           py::arg("positions"), py::arg("mark_prices"));
}

}  // namespace lrvx
