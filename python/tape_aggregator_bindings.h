// python/tape_aggregator_bindings.h
//
// Pybind11 surface for the streaming tape aggregator framework
// (lrvx::replay::IAggregator + 5 native aggregators).
//
// Wiring: each concrete aggregator is wrapped in a `PyXxxAggregator`
// holder that owns the C++ value and exposes a `native()` accessor
// returning the underlying `IAggregator*`. All holders inherit from
// `IPyAggregator` so `DataReader.run([...])` / `MergedTapeReader.run(...)`
// can iterate the Python list as a span of base pointers without
// per-type dispatch.

#pragma once

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "lrvx/replay/aggregator.h"
#include "lrvx/replay/aggregators/bin_count.h"
#include "lrvx/replay/aggregators/book_snapshot_bin.h"
#include "lrvx/replay/aggregators/event_type_stats.h"
#include "lrvx/replay/aggregators/ohlc_bin.h"
#include "lrvx/replay/aggregators/peak.h"
#include "lrvx/replay/aggregators/quantile.h"
#include "lrvx/replay/aggregators/volume_bin.h"

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace py = pybind11;

namespace lrvx
{

// Zeroes the bytes of a structured array that no dtype field covers: the
// explicit `_pad` members of the record structs and the implicit padding of
// the aggregator rows. Those bytes are copied from, or left as, whatever the
// allocator held, so without this two reads of one tape give buffers that
// hash differently although every field agrees.
template <typename Row>
inline py::array_t<Row> zeroPadding(py::array_t<Row> arr)
{
  const py::dtype dt = py::dtype::of<Row>();
  const auto itemsize = static_cast<size_t>(dt.itemsize());
  std::vector<bool> covered(itemsize, false);
  for (auto item : dt.attr("fields").cast<py::dict>())
  {
    auto spec = item.second.cast<py::tuple>();
    const auto field = spec[0].cast<py::dtype>();
    const auto offset = spec[1].cast<size_t>();
    for (size_t b = 0; b < static_cast<size_t>(field.itemsize()) && offset + b < itemsize; ++b)
    {
      covered[offset + b] = true;
    }
  }
  std::vector<std::pair<size_t, size_t>> gaps;  // [start, length)
  for (size_t b = 0; b < itemsize;)
  {
    if (covered[b])
    {
      ++b;
      continue;
    }
    const size_t start = b;
    while (b < itemsize && !covered[b])
    {
      ++b;
    }
    gaps.emplace_back(start, b - start);
  }
  if (gaps.empty() || arr.size() == 0)
  {
    return arr;
  }
  auto* base = reinterpret_cast<std::byte*>(arr.mutable_data());
  for (py::ssize_t i = 0; i < arr.size(); ++i)
  {
    for (const auto& [start, len] : gaps)
    {
      std::memset(base + static_cast<size_t>(i) * itemsize + start, 0, len);
    }
  }
  return arr;
}

// Common base over every Python-bound aggregator. Lets DataReader.run
// take a py::list and extract IAggregator* without knowing every
// concrete subclass.
class IPyAggregator
{
 public:
  virtual ~IPyAggregator() = default;
  virtual lrvx::replay::IAggregator* native() = 0;
};

class PyEventTypeStatsAggregator : public IPyAggregator
{
 public:
  PyEventTypeStatsAggregator(lrvx::replay::AggregatorEventFilter event_filter,
                             std::vector<uint32_t> symbol_filter)
      : _impl(event_filter, std::move(symbol_filter))
  {
  }

  lrvx::replay::IAggregator* native() override { return &_impl; }

  py::array_t<lrvx::replay::EventTypeStatsAggregator::PerSymbolRow> result()
  {
    const auto& rows = _impl.result();
    py::array_t<lrvx::replay::EventTypeStatsAggregator::PerSymbolRow> out(rows.size());
    if (!rows.empty())
    {
      std::memcpy(out.mutable_data(), rows.data(),
                  rows.size() * sizeof(lrvx::replay::EventTypeStatsAggregator::PerSymbolRow));
    }
    return zeroPadding(std::move(out));
  }

 private:
  lrvx::replay::EventTypeStatsAggregator _impl;
};

class PyBinCountAggregator : public IPyAggregator
{
 public:
  PyBinCountAggregator(int64_t bucket_ns, bool by_side, bool by_symbol,
                       lrvx::replay::AggregatorEventFilter event_filter,
                       std::vector<uint32_t> symbol_filter)
      : _impl(bucket_ns, by_side, by_symbol, event_filter, std::move(symbol_filter))
  {
  }

  lrvx::replay::IAggregator* native() override { return &_impl; }

  py::array_t<lrvx::replay::BinCountAggregator::Row> result()
  {
    const auto& rows = _impl.result();
    py::array_t<lrvx::replay::BinCountAggregator::Row> out(rows.size());
    if (!rows.empty())
    {
      std::memcpy(out.mutable_data(), rows.data(),
                  rows.size() * sizeof(lrvx::replay::BinCountAggregator::Row));
    }
    return zeroPadding(std::move(out));
  }

 private:
  lrvx::replay::BinCountAggregator _impl;
};

class PyVolumeBinAggregator : public IPyAggregator
{
 public:
  PyVolumeBinAggregator(int64_t bucket_ns, bool by_side, bool by_symbol,
                        lrvx::replay::AggregatorEventFilter event_filter,
                        std::vector<uint32_t> symbol_filter)
      : _impl(bucket_ns, by_side, by_symbol, event_filter, std::move(symbol_filter))
  {
  }

  lrvx::replay::IAggregator* native() override { return &_impl; }

  py::array_t<lrvx::replay::VolumeBinAggregator::Row> result()
  {
    const auto& rows = _impl.result();
    py::array_t<lrvx::replay::VolumeBinAggregator::Row> out(rows.size());
    if (!rows.empty())
    {
      std::memcpy(out.mutable_data(), rows.data(),
                  rows.size() * sizeof(lrvx::replay::VolumeBinAggregator::Row));
    }
    return zeroPadding(std::move(out));
  }

 private:
  lrvx::replay::VolumeBinAggregator _impl;
};

class PyOHLCBinAggregator : public IPyAggregator
{
 public:
  PyOHLCBinAggregator(int64_t bucket_ns, bool by_symbol,
                      lrvx::replay::AggregatorEventFilter event_filter,
                      std::vector<uint32_t> symbol_filter)
      : _impl(bucket_ns, by_symbol, event_filter, std::move(symbol_filter))
  {
  }

  lrvx::replay::IAggregator* native() override { return &_impl; }

  py::array_t<lrvx::replay::OHLCBinAggregator::Row> result()
  {
    const auto& rows = _impl.result();
    py::array_t<lrvx::replay::OHLCBinAggregator::Row> out(rows.size());
    if (!rows.empty())
    {
      std::memcpy(out.mutable_data(), rows.data(),
                  rows.size() * sizeof(lrvx::replay::OHLCBinAggregator::Row));
    }
    return zeroPadding(std::move(out));
  }

 private:
  lrvx::replay::OHLCBinAggregator _impl;
};

class PyBookSnapshotBinAggregator : public IPyAggregator
{
 public:
  PyBookSnapshotBinAggregator(int64_t bucket_ns, uint16_t levels,
                              lrvx::replay::AggregatorEventFilter event_filter,
                              std::vector<uint32_t> symbol_filter)
      : _impl(bucket_ns, levels, event_filter, std::move(symbol_filter))
  {
  }

  lrvx::replay::IAggregator* native() override { return &_impl; }

  py::array_t<lrvx::replay::BookSnapshotBinAggregator::Row> result()
  {
    const auto& rows = _impl.result();
    py::array_t<lrvx::replay::BookSnapshotBinAggregator::Row> out(rows.size());
    if (!rows.empty())
    {
      std::memcpy(out.mutable_data(), rows.data(),
                  rows.size() * sizeof(lrvx::replay::BookSnapshotBinAggregator::Row));
    }
    return zeroPadding(std::move(out));
  }

 private:
  lrvx::replay::BookSnapshotBinAggregator _impl;
};

class PyPeakAggregator : public IPyAggregator
{
 public:
  PyPeakAggregator(std::vector<int64_t> window_ns_list, std::size_t top_n,
                   std::size_t oversample_factor,
                   lrvx::replay::AggregatorEventFilter event_filter,
                   std::vector<uint32_t> symbol_filter)
      : _impl(std::move(window_ns_list), top_n, oversample_factor, event_filter,
              std::move(symbol_filter))
  {
  }

  lrvx::replay::IAggregator* native() override { return &_impl; }

  // Result shape: dict[window_ns -> list[tuple(count, start_ns)]].
  // Matches the tracker spec — bindings convert away from the flat
  // (window, count, start) C++ rows because grouping by scale is
  // what consumers actually want.
  py::dict result()
  {
    py::dict out;
    for (const auto& row : _impl.result())
    {
      py::int_ key(row.window_ns);
      if (!out.contains(key))
      {
        out[key] = py::list();
      }
      out[key].cast<py::list>().append(py::make_tuple(row.count, row.start_ns));
    }
    return out;
  }

 private:
  lrvx::replay::PeakAggregator _impl;
};

class PyQuantileAggregator : public IPyAggregator
{
 public:
  PyQuantileAggregator(std::vector<int64_t> window_ns_list,
                       std::vector<double> quantiles,
                       lrvx::replay::AggregatorEventFilter event_filter,
                       std::vector<uint32_t> symbol_filter)
      : _impl(std::move(window_ns_list), std::move(quantiles), event_filter,
              std::move(symbol_filter))
  {
  }

  lrvx::replay::IAggregator* native() override { return &_impl; }

  // Result shape: dict[window_ns -> dict[quantile -> count]] —
  // matches tracker spec.
  py::dict result()
  {
    py::dict out;
    for (const auto& row : _impl.result())
    {
      py::int_ window_key(row.window_ns);
      if (!out.contains(window_key))
      {
        out[window_key] = py::dict();
      }
      out[window_key].cast<py::dict>()[py::float_(row.quantile)] = row.count;
    }
    return out;
  }

 private:
  lrvx::replay::QuantileAggregator _impl;
};

// Helper used by PyDataReader::run / PyMergedTapeReader::run to
// convert a Python list of aggregator wrappers into a flat
// std::vector<IAggregator*>. Returns empty vector when the list is
// empty so callers can short-circuit on the no-op path.
inline std::vector<lrvx::replay::IAggregator*> collectAggregators(py::list py_aggregators)
{
  std::vector<lrvx::replay::IAggregator*> out;
  out.reserve(py::len(py_aggregators));
  for (auto item : py_aggregators)
  {
    auto* handle = item.cast<IPyAggregator*>();
    out.push_back(handle->native());
  }
  return out;
}

inline void bindTapeAggregators(py::module_& m)
{
  // Numpy-dtype registrations for the row types returned by the tabular
  // aggregators. Registered once per module init; the structured arrays
  // returned by .result() use these dtypes so consumers can do
  // `arr["count"]` / `arr["symbol_id"]` directly.
  PYBIND11_NUMPY_DTYPE(lrvx::replay::EventTypeStatsAggregator::PerSymbolRow,
                       symbol_id, trades, book_snapshots, book_deltas, book_tops);
  PYBIND11_NUMPY_DTYPE(lrvx::replay::BinCountAggregator::Row, bucket_ts_ns,
                       symbol_id, side, count);
  PYBIND11_NUMPY_DTYPE(lrvx::replay::VolumeBinAggregator::Row, bucket_ts_ns,
                       symbol_id, side, qty_raw);
  PYBIND11_NUMPY_DTYPE(lrvx::replay::OHLCBinAggregator::Row, bucket_ts_ns,
                       symbol_id, open_raw, high_raw, low_raw, close_raw);
  PYBIND11_NUMPY_DTYPE(lrvx::replay::BookSnapshotBinAggregator::Row, bucket_ts_ns,
                       symbol_id, level, flags, bid_price_raw, bid_qty_raw,
                       ask_price_raw, ask_qty_raw);

  py::enum_<lrvx::replay::AggregatorEventFilter>(m, "AggregatorEventFilter",
                                                 "Which event kinds an aggregator counts. Trades = TradeRecord only, "
                                                 "BooksOnly = book events only (snapshot / delta / top), Both = every event.")
      .value("Trades", lrvx::replay::AggregatorEventFilter::Trades)
      .value("BooksOnly", lrvx::replay::AggregatorEventFilter::BooksOnly)
      .value("Both", lrvx::replay::AggregatorEventFilter::Both);

  // Internal base. Not directly constructible from Python — only
  // surfaced so isinstance() checks work and so the bound concrete
  // classes can declare their inheritance.
  py::class_<IPyAggregator>(m, "_AggregatorHandle");

  py::class_<PyEventTypeStatsAggregator, IPyAggregator>(m, "EventTypeStatsAggregator",
                                                        "Per-symbol counters split by event kind (trade / book_snapshot / "
                                                        "book_delta / book_top). Cheapest of the native aggregators; useful as the "
                                                        "'what's in this tape' overview.")
      .def(py::init<lrvx::replay::AggregatorEventFilter, std::vector<uint32_t>>(),
           py::arg("event_filter") = lrvx::replay::AggregatorEventFilter::Both,
           py::arg("symbol_filter") = std::vector<uint32_t>{})
      .def("result", &PyEventTypeStatsAggregator::result,
           "Structured numpy array with fields "
           "(symbol_id u4, trades u8, book_snapshots u8, book_deltas u8, "
           "book_tops u8). "
           "Sorted by symbol_id ascending. Empty until DataReader.run() has "
           "called finalize().");

  py::class_<PyBinCountAggregator, IPyAggregator>(m, "BinCountAggregator",
                                                  "Time-bucketed event count. Each event floors to its bucket "
                                                  "(floor(exchange_ts_ns / bucket_ns) * bucket_ns) and increments the "
                                                  "(bucket, optional symbol, optional side) cell. Result is a flat "
                                                  "structured numpy array of (bucket, symbol_id, side, count) rows.")
      .def(py::init<int64_t, bool, bool, lrvx::replay::AggregatorEventFilter,
                    std::vector<uint32_t>>(),
           py::arg("bucket_ns"),
           py::arg("by_side") = false,
           py::arg("by_symbol") = false,
           py::arg("event_filter") = lrvx::replay::AggregatorEventFilter::Both,
           py::arg("symbol_filter") = std::vector<uint32_t>{})
      .def("result", &PyBinCountAggregator::result,
           "Structured numpy: (bucket_ts_ns i8, symbol_id u4, side u1, "
           "count u8). side encoding: 0 = aggregate, 1 = BUY, 2 = SELL. "
           "symbol_id is 0 when by_symbol=False.");

  py::class_<PyVolumeBinAggregator, IPyAggregator>(m, "VolumeBinAggregator",
                                                   "Time-bucketed trade quantity sum. Same bucketing structure as "
                                                   "BinCountAggregator but sums trade.qty_raw per cell instead of "
                                                   "counting. Books are ignored (no scalar qty per book event).")
      .def(py::init<int64_t, bool, bool, lrvx::replay::AggregatorEventFilter,
                    std::vector<uint32_t>>(),
           py::arg("bucket_ns"),
           py::arg("by_side") = false,
           py::arg("by_symbol") = false,
           py::arg("event_filter") = lrvx::replay::AggregatorEventFilter::Trades,
           py::arg("symbol_filter") = std::vector<uint32_t>{})
      .def("result", &PyVolumeBinAggregator::result,
           "Structured numpy: (bucket_ts_ns i8, symbol_id u4, side u1, "
           "qty_raw i8). qty_raw is fixed-point — divide by Quantity::SCALE "
           "to get floats.");

  py::class_<PyOHLCBinAggregator, IPyAggregator>(m, "OHLCBinAggregator",
                                                 "Time-bucketed price OHLC over trade events. For each cell, "
                                                 "records open (earliest ts), close (latest ts), high (max), and "
                                                 "low (min) of trade.price_raw. Empty buckets produce no row; "
                                                 "forward-fill on the caller side if a dense series is needed. "
                                                 "Books are ignored.")
      .def(py::init<int64_t, bool, lrvx::replay::AggregatorEventFilter,
                    std::vector<uint32_t>>(),
           py::arg("bucket_ns"),
           py::arg("by_symbol") = false,
           py::arg("event_filter") = lrvx::replay::AggregatorEventFilter::Trades,
           py::arg("symbol_filter") = std::vector<uint32_t>{})
      .def("result", &PyOHLCBinAggregator::result,
           "Structured numpy: (bucket_ts_ns i8, symbol_id u4, "
           "open_raw i8, high_raw i8, low_raw i8, close_raw i8). "
           "*_raw fields are fixed-point. Divide by Price::SCALE to get "
           "floats. symbol_id is 0 when by_symbol=False.");

  py::class_<PyBookSnapshotBinAggregator, IPyAggregator>(m, "BookSnapshotBinAggregator",
                                                         "Time-bucketed order-book snapshots. Maintains the full ladder per "
                                                         "symbol from book snapshot/delta events and at each bucket boundary "
                                                         "emits the latest state observed inside the closed bucket as up to "
                                                         "`levels` rows per side (row = bid+ask paired at the same depth, "
                                                         "shorter side zero-padded). Buckets without book events produce no "
                                                         "rows; the last observed state is emitted as the trailing bucket. "
                                                         "Book reconstruction is order-dependent across the whole tape, so "
                                                         "this aggregator REQUIRES DataReader.run(..., n_threads=1) — "
                                                         "parallel runs raise.")
      .def(py::init<int64_t, uint16_t, lrvx::replay::AggregatorEventFilter,
                    std::vector<uint32_t>>(),
           py::arg("bucket_ns"),
           py::arg("levels") = uint16_t{20},
           py::arg("event_filter") = lrvx::replay::AggregatorEventFilter::BooksOnly,
           py::arg("symbol_filter") = std::vector<uint32_t>{})
      .def("result", &PyBookSnapshotBinAggregator::result,
           "Structured numpy: (bucket_ts_ns i8, symbol_id u4, level u2, "
           "flags u2, bid_price_raw i8, bid_qty_raw i8, ask_price_raw i8, "
           "ask_qty_raw i8). level 0 = top of book; zero price+qty on a "
           "side = no level at that depth. flags bit 0 = book crossed "
           "(best bid >= best ask) at this bucket. *_raw fields are "
           "fixed-point — divide by Price/Quantity SCALE. Sorted by "
           "(bucket_ts_ns, symbol_id, level).");

  py::class_<PyPeakAggregator, IPyAggregator>(m, "PeakAggregator",
                                              "Streaming top-N busiest fixed-width windows per scale. For each "
                                              "window_ns in the constructor list, finds the top_n time intervals "
                                              "that pack the most events into a window of that width. Peaks within "
                                              "3*window_ns of a stronger one are deduped at finalize().")
      .def(py::init<std::vector<int64_t>, std::size_t, std::size_t,
                    lrvx::replay::AggregatorEventFilter,
                    std::vector<uint32_t>>(),
           py::arg("window_ns_list"),
           py::arg("top_n") = std::size_t{10},
           py::arg("oversample_factor") = std::size_t{100},
           py::arg("event_filter") = lrvx::replay::AggregatorEventFilter::Trades,
           py::arg("symbol_filter") = std::vector<uint32_t>{})
      .def("result", &PyPeakAggregator::result,
           "dict[window_ns -> list[(count, start_ns)]] — peaks per scale, "
           "sorted by count descending after dedup.");

  py::class_<PyQuantileAggregator, IPyAggregator>(m, "QuantileAggregator",
                                                  "Window-count distribution + quantile lookup. For each window_ns, "
                                                  "observes the count of events inside a sliding window of that width "
                                                  "at every event arrival, builds a histogram, and at finalize() "
                                                  "resolves each requested quantile to the count threshold below which "
                                                  "that fraction of observations lies.")
      .def(py::init<std::vector<int64_t>, std::vector<double>,
                    lrvx::replay::AggregatorEventFilter,
                    std::vector<uint32_t>>(),
           py::arg("window_ns_list"),
           py::arg("quantiles"),
           py::arg("event_filter") = lrvx::replay::AggregatorEventFilter::Trades,
           py::arg("symbol_filter") = std::vector<uint32_t>{})
      .def("result", &PyQuantileAggregator::result,
           "dict[window_ns -> dict[quantile -> count]].");
}

}  // namespace lrvx
