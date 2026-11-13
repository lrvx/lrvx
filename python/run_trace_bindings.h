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

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "lrvx/run/trace_reader.h"
#include "lrvx/run/trace_recorder.h"

namespace py = pybind11;

namespace lrvx
{

inline void bindRunTrace(py::module_& m)
{
  using namespace lrvx::run;

  py::enum_<OrderEventKind>(m, "OrderEventKind")
      .value("SUBMIT", OrderEventKind::Submit)
      .value("CANCEL", OrderEventKind::Cancel)
      .value("MODIFY", OrderEventKind::Modify)
      .value("ACK", OrderEventKind::Ack)
      .value("REJECT", OrderEventKind::Reject)
      .value("EXPIRE", OrderEventKind::Expire);

  py::enum_<FillLiquidity>(m, "FillLiquidity")
      .value("UNKNOWN", FillLiquidity::Unknown)
      .value("MAKER", FillLiquidity::Maker)
      .value("TAKER", FillLiquidity::Taker);

  py::class_<TapeRef>(m, "TapeRef")
      .def(py::init<>())
      .def(py::init([](std::string path, std::string content_hash, int64_t first_event_ns,
                       int64_t last_event_ns)
                    {
             TapeRef r;
             r.path = std::move(path);
             r.content_hash = std::move(content_hash);
             r.first_event_ns = first_event_ns;
             r.last_event_ns = last_event_ns;
             return r; }),
           py::arg("path"), py::arg("content_hash") = std::string{},
           py::arg("first_event_ns") = 0, py::arg("last_event_ns") = 0)
      .def_readwrite("path", &TapeRef::path)
      .def_readwrite("content_hash", &TapeRef::content_hash)
      .def_readwrite("first_event_ns", &TapeRef::first_event_ns)
      .def_readwrite("last_event_ns", &TapeRef::last_event_ns);

  py::class_<TraceRecorder>(m, "TraceRecorder")
      .def(py::init([](const std::string& path, const std::string& strategy_id,
                       const std::string& strategy_hash, int64_t run_started_ns,
                       const std::vector<TapeRef>& tape_refs)
                    {
             TraceRecorderOptions opts;
             opts.strategy_id = strategy_id;
             opts.strategy_hash = strategy_hash;
             opts.run_started_ns = run_started_ns;
             opts.tape_refs = tape_refs;
             return std::make_unique<TraceRecorder>(path, std::move(opts)); }),
           py::arg("path"), py::arg("strategy_id") = std::string{},
           py::arg("strategy_hash") = std::string{},
           py::arg("run_started_ns") = int64_t{0},
           py::arg("tape_refs") = std::vector<TapeRef>{})
      .def(
          "write_signal",
          [](TraceRecorder& self, int64_t run_ts_ns, int64_t feed_ts_ns, uint32_t signal_id,
             uint32_t flags, int64_t strength_raw, const std::string& name,
             const std::vector<uint32_t>& symbol_ids, const py::bytes& payload)
          {
            SignalView s;
            s.run_ts_ns = run_ts_ns;
            s.feed_ts_ns = feed_ts_ns;
            s.signal_id = signal_id;
            s.flags = flags;
            s.strength_raw = strength_raw;
            s.name = name;
            s.symbol_ids = symbol_ids;
            std::string p = payload;
            s.payload = p;
            self.writeSignal(s);
          },
          py::arg("run_ts_ns"), py::arg("feed_ts_ns") = int64_t{0}, py::arg("signal_id") = uint32_t{0},
          py::arg("flags") = uint32_t{0}, py::arg("strength_raw") = int64_t{0},
          py::arg("name") = std::string{},
          py::arg("symbol_ids") = std::vector<uint32_t>{},
          py::arg("payload") = py::bytes{})
      .def(
          "write_order_event",
          [](TraceRecorder& self, int64_t run_ts_ns, int64_t feed_ts_ns, uint64_t order_id,
             uint64_t parent_signal_id, int64_t price_raw, int64_t qty_raw, uint32_t symbol_id,
             OrderEventKind event_kind, uint8_t side, uint8_t order_type, uint32_t flags,
             const std::string& reason)
          {
            OrderEventView e;
            e.run_ts_ns = run_ts_ns;
            e.feed_ts_ns = feed_ts_ns;
            e.order_id = order_id;
            e.parent_signal_id = parent_signal_id;
            e.price_raw = price_raw;
            e.qty_raw = qty_raw;
            e.symbol_id = symbol_id;
            e.event_kind = event_kind;
            e.side = side;
            e.order_type = order_type;
            e.flags = flags;
            e.reason = reason;
            self.writeOrderEvent(e);
          },
          py::arg("run_ts_ns"), py::arg("feed_ts_ns") = int64_t{0}, py::arg("order_id") = uint64_t{0},
          py::arg("parent_signal_id") = uint64_t{0}, py::arg("price_raw") = int64_t{0},
          py::arg("qty_raw") = int64_t{0}, py::arg("symbol_id") = uint32_t{0},
          py::arg("event_kind") = OrderEventKind::Submit, py::arg("side") = uint8_t{0},
          py::arg("order_type") = uint8_t{0}, py::arg("flags") = uint32_t{0},
          py::arg("reason") = std::string{})
      .def(
          "write_fill",
          [](TraceRecorder& self, int64_t run_ts_ns, int64_t feed_ts_ns, uint64_t order_id,
             uint64_t fill_id, int64_t price_raw, int64_t qty_raw, int64_t fee_raw,
             uint32_t symbol_id, uint8_t side, FillLiquidity liquidity)
          {
            FillView f;
            f.run_ts_ns = run_ts_ns;
            f.feed_ts_ns = feed_ts_ns;
            f.order_id = order_id;
            f.fill_id = fill_id;
            f.price_raw = price_raw;
            f.qty_raw = qty_raw;
            f.fee_raw = fee_raw;
            f.symbol_id = symbol_id;
            f.side = side;
            f.liquidity = liquidity;
            self.writeFill(f);
          },
          py::arg("run_ts_ns"), py::arg("feed_ts_ns") = int64_t{0}, py::arg("order_id") = uint64_t{0},
          py::arg("fill_id") = uint64_t{0}, py::arg("price_raw") = int64_t{0},
          py::arg("qty_raw") = int64_t{0}, py::arg("fee_raw") = int64_t{0},
          py::arg("symbol_id") = uint32_t{0}, py::arg("side") = uint8_t{0},
          py::arg("liquidity") = FillLiquidity::Unknown)
      .def("set_run_ended_ns", &TraceRecorder::setRunEndedNs)
      .def("close", &TraceRecorder::close);

  py::class_<TraceReader>(m, "TraceReader")
      .def(py::init<const std::string&>(), py::arg("path"))
      .def_property_readonly("strategy_id",
                             [](const TraceReader& self)
                             { return self.manifest().strategy_id; })
      .def_property_readonly("strategy_hash",
                             [](const TraceReader& self)
                             { return self.manifest().strategy_hash; })
      .def_property_readonly("run_started_ns",
                             [](const TraceReader& self)
                             { return self.manifest().run_started_ns; })
      .def_property_readonly("run_ended_ns",
                             [](const TraceReader& self)
                             { return self.manifest().run_ended_ns; })
      .def_property_readonly(
          "tape_refs",
          [](const TraceReader& self)
          {
            py::list out;
            for (const auto& r : self.manifest().tape_refs)
            {
              py::dict d;
              d["path"] = r.path;
              d["content_hash"] = r.content_hash;
              d["first_event_ns"] = r.first_event_ns;
              d["last_event_ns"] = r.last_event_ns;
              out.append(d);
            }
            return out;
          })
      .def("read_all_signals",
           [](TraceReader& self)
           {
             py::list out;
             for (auto& s : self.readAllSignals())
             {
               py::dict d;
               d["run_ts_ns"] = s.run_ts_ns;
               d["feed_ts_ns"] = s.feed_ts_ns;
               d["signal_id"] = s.signal_id;
               d["signal_id64"] = s.signalId64();
               d["flags"] = s.flags;
               d["strength_raw"] = s.strength_raw;
               d["name"] = s.name;
               d["symbol_ids"] = s.symbol_ids;
               d["payload"] = py::bytes(reinterpret_cast<const char*>(s.payload.data()),
                                        s.payload.size());
               out.append(d);
             }
             return out;
           })
      .def("read_all_order_events",
           [](TraceReader& self)
           {
             py::list out;
             for (auto& e : self.readAllOrderEvents())
             {
               py::dict d;
               d["run_ts_ns"] = e.run_ts_ns;
               d["feed_ts_ns"] = e.feed_ts_ns;
               d["order_id"] = e.order_id;
               d["parent_signal_id"] = e.parent_signal_id;
               d["price_raw"] = e.price_raw;
               d["qty_raw"] = e.qty_raw;
               d["symbol_id"] = e.symbol_id;
               d["event_kind"] = static_cast<int>(e.event_kind);
               d["side"] = e.side;
               d["order_type"] = e.order_type;
               d["flags"] = e.flags;
               d["reason"] = e.reason;
               out.append(d);
             }
             return out;
           })
      .def("read_all_fills", [](TraceReader& self)
           {
        py::list out;
        for (auto& f : self.readAllFills())
        {
          py::dict d;
          d["run_ts_ns"] = f.run_ts_ns;
          d["feed_ts_ns"] = f.feed_ts_ns;
          d["order_id"] = f.order_id;
          d["fill_id"] = f.fill_id;
          d["price_raw"] = f.price_raw;
          d["qty_raw"] = f.qty_raw;
          d["fee_raw"] = f.fee_raw;
          d["symbol_id"] = f.symbol_id;
          d["side"] = f.side;
          d["liquidity"] = static_cast<int>(f.liquidity);
          out.append(d);
        }
        return out; });

  m.attr("SIGNAL_FLAG_ENTER") = py::int_(SignalFlags::Enter);
  m.attr("SIGNAL_FLAG_EXIT") = py::int_(SignalFlags::Exit);
  m.attr("SIGNAL_FLAG_REBALANCE") = py::int_(SignalFlags::Rebalance);
  m.attr("ORDER_FLAG_POST_ONLY") = py::int_(OrderEventFlags::PostOnly);
  m.attr("ORDER_FLAG_REDUCE_ONLY") = py::int_(OrderEventFlags::ReduceOnly);
  m.attr("ORDER_FLAG_IOC") = py::int_(OrderEventFlags::Ioc);
}

}  // namespace lrvx
