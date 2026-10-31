// python/replay_bindings.h

#pragma once

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>

#include <pybind11/stl.h>
#include "lrvx/error/lrvx_error.h"

#include "lrvx/common.h"
#include "lrvx/replay/binary_format_v1.h"
#include "lrvx/replay/binary_log_recorder_hook.h"
#include "lrvx/replay/merged_tape_reader.h"
#include "lrvx/replay/readers/binary_log_reader.h"
#include "lrvx/replay/recording_metadata.h"
#include "lrvx/replay/writers/binary_log_writer.h"
#include "tape_aggregator_bindings.h"

#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace py = pybind11;

namespace lrvx
{

// Thin pybind wrapper over lrvx::replay::BinaryLogRecorderHook. Lives in
// lrvx namespace so strategy_bindings.h can take a shared_ptr to it in
// `runner.set_market_data_recorder(...)` overloads.
class PyBinaryLogRecorderHook
{
  std::unique_ptr<lrvx::replay::BinaryLogRecorderHook> _hook;

 public:
  PyBinaryLogRecorderHook(const std::string& outputDir, uint64_t maxSegmentMb,
                          uint8_t exchangeId, const std::string& compression,
                          const std::string& exchangeName,
                          const std::string& instrumentType)
  {
    lrvx::replay::BinaryLogRecorderHookConfig cfg{};
    cfg.output_dir = outputDir;
    cfg.max_segment_bytes = maxSegmentMb * (1024ull * 1024ull);
    cfg.exchange_id = exchangeId;
    if (compression == "none" || compression.empty())
    {
      cfg.compression = lrvx::replay::CompressionType::None;
    }
    else if (compression == "lz4")
    {
      cfg.compression = lrvx::replay::CompressionType::LZ4;
    }
    else
    {
      throw lrvx::LrvxError(
          "E_INPUT_001",
          "Unknown compression: '" + compression + "'. Expected 'none' or 'lz4'.");
    }
    if (!exchangeName.empty() || !instrumentType.empty())
    {
      lrvx::replay::RecordingMetadata meta{};
      meta.exchange = exchangeName;
      meta.instrument_type = instrumentType;
      cfg.metadata = std::move(meta);
    }
    _hook = std::make_unique<lrvx::replay::BinaryLogRecorderHook>(std::move(cfg));
  }

  lrvx::replay::BinaryLogRecorderHook* raw() noexcept { return _hook.get(); }

  void addSymbol(uint32_t symbolId, const std::string& name, const std::string& base,
                 const std::string& quote, int8_t pricePrecision, int8_t qtyPrecision)
  {
    lrvx::replay::SymbolInfo info;
    info.symbol_id = symbolId;
    info.name = name;
    info.base_asset = base;
    info.quote_asset = quote;
    info.price_precision = pricePrecision;
    info.qty_precision = qtyPrecision;
    _hook->addSymbol(info);
  }

  void flush() { _hook->flush(); }

  // Idempotent — safe to call from user code OR via the engine's on_stop
  // lifecycle callback. Just stops the underlying writer.
  void close() { _hook->stop(); }

  pybind11::dict stats()
  {
    auto s = _hook->stats();
    pybind11::dict d;
    d["trades_written"] = s.trades_written;
    d["book_updates_written"] = s.book_updates_written;
    d["bytes_written"] = s.bytes_written;
    d["segments_created"] = s.segments_created;
    d["errors"] = s.errors;
    return d;
  }

  std::string currentSegmentPath() const { return _hook->currentSegmentPath().string(); }
};

}  // namespace lrvx

namespace
{

using namespace lrvx::replay;

// ─── Numpy-compatible trade struct ─────────────────────────────────────────────

#pragma pack(push, 1)
struct PyTrade
{
  int64_t exchange_ts_ns;
  int64_t recv_ts_ns;
  int64_t price_raw;
  int64_t qty_raw;
  uint64_t trade_id;
  uint32_t symbol_id;
  uint8_t side;
  uint8_t _pad[3];
};
#pragma pack(pop)
static_assert(sizeof(PyTrade) == 48);

// ─── Numpy-compatible book structs ─────────────────────────────────────────────

// Best bid/ask from a single book update event.
#pragma pack(push, 1)
struct PyBBO
{
  int64_t exchange_ts_ns;
  int64_t recv_ts_ns;
  int64_t seq;
  uint32_t symbol_id;
  uint8_t event_type;  // record byte: 0=snapshot, 1=delta, 2=top-of-book
  uint8_t _pad[3];
  int64_t bid_price_raw;
  int64_t bid_qty_raw;
  int64_t ask_price_raw;
  int64_t ask_qty_raw;
};
#pragma pack(pop)
static_assert(sizeof(PyBBO) == 64);

// Numpy-compatible option quote. Raw fixed-point fields mirror
// OptionQuoteRecord: mark/index use PRICE_SCALE, iv uses kIvScale (1e8),
// open_interest uses QUANTITY_SCALE. Consumers convert raw -> double.
#pragma pack(push, 1)
struct PyOptionQuote
{
  int64_t exchange_ts_ns;
  int64_t recv_ts_ns;
  int64_t mark_price_raw;
  int64_t index_price_raw;
  int64_t underlying_price_raw;
  int64_t iv_raw;
  int64_t bid_price_raw;
  int64_t ask_price_raw;
  int64_t bid_size_raw;
  int64_t ask_size_raw;
  int64_t bid_iv_raw;
  int64_t ask_iv_raw;
  int64_t open_interest_raw;
  uint32_t symbol_id;
  uint8_t instrument;
  uint8_t _pad[3];
};
#pragma pack(pop)
static_assert(sizeof(PyOptionQuote) == 112);

// Per-event header for read_book_updates(); levels are in a separate flat array.
#pragma pack(push, 1)
struct PyBookUpdateHeader
{
  int64_t exchange_ts_ns;
  int64_t recv_ts_ns;
  int64_t seq;
  uint32_t symbol_id;
  uint16_t bid_count;
  uint16_t ask_count;
  uint32_t level_offset;  // index into the flat levels array for this event
  uint8_t event_type;     // record byte: 0=snapshot, 1=delta, 2=top-of-book
  uint8_t _pad[3];
};
#pragma pack(pop)
static_assert(sizeof(PyBookUpdateHeader) == 40);

// One price level in the flat levels array.
#pragma pack(push, 1)
struct PyLevel
{
  int64_t price_raw;
  int64_t qty_raw;
  uint8_t side;  // 0=bid, 1=ask
  uint8_t _pad[7];
};
#pragma pack(pop)
static_assert(sizeof(PyLevel) == 24);

// ─── Helpers ───────────────────────────────────────────────────────────────────

inline PyTrade tradeRecordToPyTrade(const lrvx::replay::TradeRecord& tr)
{
  return {tr.exchange_ts_ns, tr.recv_ts_ns, tr.price_raw, tr.qty_raw, tr.trade_id, tr.symbol_id, tr.side, {}};
}

inline ReaderConfig makeReaderConfig(const std::string& dataDir, py::object fromNs,
                                     py::object toNs, py::object symbols,
                                     py::object reorderWindowNs, bool strictOrdering)
{
  ReaderConfig cfg;
  cfg.data_dir = dataDir;

  if (!fromNs.is_none())
  {
    cfg.from_ns = fromNs.cast<int64_t>();
  }
  if (!toNs.is_none())
  {
    cfg.to_ns = toNs.cast<int64_t>();
  }
  if (!symbols.is_none())
  {
    auto symList = symbols.cast<std::vector<uint32_t>>();
    cfg.symbols.insert(symList.begin(), symList.end());
  }
  if (!reorderWindowNs.is_none())
  {
    cfg.reorder_window_ns = reorderWindowNs.cast<int64_t>();
  }
  cfg.strict_ordering = strictOrdering;

  return cfg;
}

inline WriterConfig makeWriterConfig(const std::string& outputDir, uint64_t maxSegmentMb,
                                     uint8_t exchangeId, const std::string& compression)
{
  WriterConfig cfg;
  cfg.output_dir = outputDir;
  cfg.max_segment_bytes = maxSegmentMb * (1024ull * 1024ull);
  cfg.exchange_id = exchangeId;

  if (compression == "lz4")
  {
    cfg.compression = CompressionType::LZ4;
  }
  else if (compression == "none" || compression.empty())
  {
    cfg.compression = CompressionType::None;
  }
  else
  {
    throw lrvx::LrvxError(
        "E_INPUT_001",
        "Unknown compression type: '" + compression +
            "'. Expected 'none' or 'lz4'.");
  }

  return cfg;
}

// ─── PyDataReader ──────────────────────────────────────────────────────────────

class PyDataReader
{
  BinaryLogReader _reader;
  // Upper time bound of the reader config. The forEachFrom walk itself
  // runs to the end of the tape; bounded readers (option quotes) use
  // this to collect only in-range rows and abort the walk early.
  std::optional<int64_t> _to_ns;

 public:
  PyDataReader(const std::string& dataDir, py::object fromNs, py::object toNs,
               py::object symbols, py::object reorderWindowNs, bool strictOrdering)
      : _reader(makeReaderConfig(dataDir, fromNs, toNs, symbols, reorderWindowNs,
                                 strictOrdering))
  {
    if (!toNs.is_none())
    {
      _to_ns = toNs.cast<int64_t>();
    }
  }

  py::dict summary()
  {
    DatasetSummary s;
    {
      py::gil_scoped_release release;
      s = _reader.summary();
    }

    py::dict d;
    d["data_dir"] = s.data_dir.string();
    d["first_event_ns"] = s.first_event_ns;
    d["last_event_ns"] = s.last_event_ns;
    d["total_events"] = s.total_events;
    d["segment_count"] = s.segment_count;
    d["total_bytes"] = s.total_bytes;
    d["duration_seconds"] = s.durationSeconds();
    d["fully_indexed"] = s.fullyIndexed();

    py::set syms;
    for (auto id : s.symbols)
    {
      syms.add(py::int_(id));
    }
    d["symbols"] = syms;

    return d;
  }

  uint64_t count()
  {
    uint64_t n;
    {
      py::gil_scoped_release release;
      n = _reader.count();
    }
    return n;
  }

  py::set symbols()
  {
    std::set<uint32_t> syms;
    {
      py::gil_scoped_release release;
      syms = _reader.availableSymbols();
    }
    py::set result;
    for (auto id : syms)
    {
      result.add(py::int_(id));
    }
    return result;
  }

  py::object timeRange()
  {
    auto range = _reader.timeRange();
    if (!range.has_value())
    {
      return py::none();
    }
    return py::make_tuple(range->first, range->second);
  }

  py::array_t<PyTrade> readTrades()
  {
    std::vector<PyTrade> trades;
    {
      py::gil_scoped_release release;
      _reader.forEach(
          [&](const ReplayEvent& ev) -> bool
          {
            if (ev.type == EventType::Trade)
            {
              trades.push_back(tradeRecordToPyTrade(ev.trade));
            }
            return true;
          });
    }

    py::array_t<PyTrade> result(trades.size());
    if (!trades.empty())
    {
      std::memcpy(result.mutable_data(), trades.data(), trades.size() * sizeof(PyTrade));
    }
    return result;
  }

  py::array_t<PyTrade> readTradesFrom(int64_t startTsNs)
  {
    std::vector<PyTrade> trades;
    {
      py::gil_scoped_release release;
      _reader.forEachFrom(startTsNs,
                          [&](const ReplayEvent& ev) -> bool
                          {
                            if (ev.type == EventType::Trade)
                            {
                              trades.push_back(tradeRecordToPyTrade(ev.trade));
                            }
                            return true;
                          });
    }

    py::array_t<PyTrade> result(trades.size());
    if (!trades.empty())
    {
      std::memcpy(result.mutable_data(), trades.data(), trades.size() * sizeof(PyTrade));
    }
    return result;
  }

  // Read option quotes (mark/iv/index/open-interest) as a structured numpy array.
  // Honors the reader's to_ns: quotes at/after the bound are not collected and
  // the walk aborts once events run past the bound (with a reorder-slack margin),
  // so a bounded window costs O(prefix), not O(tape), in both time and memory.
  //
  // The bound here is HALF-OPEN: a quote exactly at to_ns belongs to the next
  // window, not this one. That is deliberate -- slicing a month by day through
  // this reader must not return the boundary quote twice -- and it differs from
  // the trade path, where ReaderConfig::to_ns is inclusive. Both are pinned by
  // tests; the difference is documented on the DataReader constructor and in
  // docs/reference/python/replay.md.
  py::array_t<PyOptionQuote> readOptionQuotesFrom(int64_t startTsNs)
  {
    const int64_t stop_ns = _to_ns.value_or(std::numeric_limits<int64_t>::max());
    // Events can be locally out of order (bounded reorder on write);
    // keep walking this margin past the bound before aborting.
    constexpr int64_t kReorderSlackNs = 600'000'000'000;  // 10 min
    const int64_t hard_stop =
        stop_ns > std::numeric_limits<int64_t>::max() - kReorderSlackNs
            ? std::numeric_limits<int64_t>::max()
            : stop_ns + kReorderSlackNs;
    std::vector<PyOptionQuote> quotes;
    {
      py::gil_scoped_release release;
      _reader.forEachFrom(startTsNs,
                          [&](const ReplayEvent& ev) -> bool
                          {
                            if (ev.timestamp_ns >= hard_stop)
                            {
                              return false;
                            }
                            if (ev.type == EventType::OptionQuote)
                            {
                              const auto& q = ev.option_quote;
                              if (q.exchange_ts_ns >= stop_ns)
                              {
                                return true;
                              }
                              PyOptionQuote p{};
                              p.exchange_ts_ns = q.exchange_ts_ns;
                              p.recv_ts_ns = q.recv_ts_ns;
                              p.mark_price_raw = q.mark_price_raw;
                              p.index_price_raw = q.index_price_raw;
                              p.underlying_price_raw = q.underlying_price_raw;
                              p.iv_raw = q.iv_raw;
                              p.bid_price_raw = q.bid_price_raw;
                              p.ask_price_raw = q.ask_price_raw;
                              p.bid_size_raw = q.bid_size_raw;
                              p.ask_size_raw = q.ask_size_raw;
                              p.bid_iv_raw = q.bid_iv_raw;
                              p.ask_iv_raw = q.ask_iv_raw;
                              p.open_interest_raw = q.open_interest_raw;
                              p.symbol_id = q.symbol_id;
                              p.instrument = q.instrument;
                              quotes.push_back(p);
                            }
                            return true;
                          });
    }

    py::array_t<PyOptionQuote> result(quotes.size());
    if (!quotes.empty())
    {
      std::memcpy(result.mutable_data(), quotes.data(),
                  quotes.size() * sizeof(PyOptionQuote));
    }
    return result;
  }

  // Read best bid/ask from every book update as a structured numpy array.
  py::array_t<PyBBO> readBBO() { return readBBOInternal(/*from=*/std::nullopt); }

  py::array_t<PyBBO> readBBOFrom(int64_t startTsNs)
  {
    return readBBOInternal(startTsNs);
  }

 private:
  py::array_t<PyBBO> readBBOInternal(std::optional<int64_t> fromTsNs)
  {
    std::vector<PyBBO> bbos;
    auto cb = [&](const ReplayEvent& ev) -> bool
    {
      if (ev.type != EventType::BookSnapshot && ev.type != EventType::BookDelta &&
          ev.type != EventType::BookTop)
      {
        return true;
      }

      PyBBO b{};
      b.exchange_ts_ns = ev.book_header.exchange_ts_ns;
      b.recv_ts_ns = ev.book_header.recv_ts_ns;
      b.seq = ev.book_header.seq;
      b.symbol_id = ev.book_header.symbol_id;
      b.event_type = ev.book_header.type;
      if (!ev.bids.empty())
      {
        b.bid_price_raw = ev.bids[0].price_raw;
        b.bid_qty_raw = ev.bids[0].qty_raw;
      }
      if (!ev.asks.empty())
      {
        b.ask_price_raw = ev.asks[0].price_raw;
        b.ask_qty_raw = ev.asks[0].qty_raw;
      }
      bbos.push_back(b);
      return true;
    };

    {
      py::gil_scoped_release release;
      if (fromTsNs)
      {
        _reader.forEachFrom(*fromTsNs, cb);
      }
      else
      {
        _reader.forEach(cb);
      }
    }

    py::array_t<PyBBO> result(bbos.size());
    if (!bbos.empty())
    {
      std::memcpy(result.mutable_data(), bbos.data(), bbos.size() * sizeof(PyBBO));
    }
    return result;
  }

 public:
  // Read all book updates.
  // Returns (headers, levels):
  //   headers  — structured array of PyBookUpdateHeader
  //   levels   — flat structured array of PyLevel (bids then asks per event)
  // To slice levels for event i:
  //   offset = headers[i]['level_offset']
  //   bids   = levels[offset : offset + headers[i]['bid_count']]
  //   asks   = levels[offset + headers[i]['bid_count'] : offset + headers[i]['bid_count'] + headers[i]['ask_count']]
  py::tuple readBookUpdates() { return readBookUpdatesInternal(/*from=*/std::nullopt); }

  py::tuple readBookUpdatesFrom(int64_t startTsNs)
  {
    return readBookUpdatesInternal(startTsNs);
  }

 private:
  py::tuple readBookUpdatesInternal(std::optional<int64_t> fromTsNs)
  {
    std::vector<PyBookUpdateHeader> headers;
    std::vector<PyLevel> levels;
    auto cb = [&](const ReplayEvent& ev) -> bool
    {
      if (ev.type != EventType::BookSnapshot && ev.type != EventType::BookDelta &&
          ev.type != EventType::BookTop)
      {
        return true;
      }

      PyBookUpdateHeader h{};
      h.exchange_ts_ns = ev.book_header.exchange_ts_ns;
      h.recv_ts_ns = ev.book_header.recv_ts_ns;
      h.seq = ev.book_header.seq;
      h.symbol_id = ev.book_header.symbol_id;
      h.bid_count = static_cast<uint16_t>(ev.bids.size());
      h.ask_count = static_cast<uint16_t>(ev.asks.size());
      h.level_offset = static_cast<uint32_t>(levels.size());
      h.event_type = ev.book_header.type;
      headers.push_back(h);

      for (const auto& bl : ev.bids)
      {
        levels.push_back({bl.price_raw, bl.qty_raw, 0, {}});
      }
      for (const auto& bl : ev.asks)
      {
        levels.push_back({bl.price_raw, bl.qty_raw, 1, {}});
      }

      return true;
    };

    {
      py::gil_scoped_release release;
      if (fromTsNs)
      {
        _reader.forEachFrom(*fromTsNs, cb);
      }
      else
      {
        _reader.forEach(cb);
      }
    }

    py::array_t<PyBookUpdateHeader> h_arr(headers.size());
    if (!headers.empty())
    {
      std::memcpy(h_arr.mutable_data(), headers.data(), headers.size() * sizeof(PyBookUpdateHeader));
    }

    py::array_t<PyLevel> l_arr(levels.size());
    if (!levels.empty())
    {
      std::memcpy(l_arr.mutable_data(), levels.data(), levels.size() * sizeof(PyLevel));
    }

    return py::make_tuple(h_arr, l_arr);
  }

 public:
  py::dict stats()
  {
    auto s = _reader.stats();
    py::dict d;
    d["files_read"] = s.files_read;
    d["events_read"] = s.events_read;
    d["trades_read"] = s.trades_read;
    d["book_updates_read"] = s.book_updates_read;
    d["bytes_read"] = s.bytes_read;
    d["crc_errors"] = s.crc_errors;
    d["late_dropped"] = s.late_dropped;
    d["unknown_frames_skipped"] = s.unknown_frames_skipped;
    return d;
  }

  // Streaming aggregator dispatch. Walks the tape once, forwarding each
  // event to every IPyAggregator wrapper's underlying IAggregator, then
  // calls finalize() on each. Empty list is a no-op (no decompression).
  // GIL released for the whole walk — concrete aggregators must be
  // self-contained (no Python crossings) for this to be safe.
  //
  // n_threads=1 (default) preserves single-threaded semantics.
  // n_threads>1 partitions the segment list across workers; each
  // worker clones the panel via cloneEmpty(), then results merge
  // into the user-visible instances before finalize().
  bool run(py::list py_aggregators, std::size_t n_threads,
           py::object progress_callback, uint32_t progress_interval_ms)
  {
    auto raw = lrvx::collectAggregators(py_aggregators);
    const bool has_callback = !progress_callback.is_none();
    if (has_callback)
    {
      auto cb = progress_callback;  // capture by value (Python ref)
      std::chrono::milliseconds interval{
          progress_interval_ms == 0 ? 1000u : progress_interval_ms};
      _reader.setProgressCallback(
          [cb](double pct, int64_t cursor_ts_ns) -> bool
          {
            py::gil_scoped_acquire acquire;
            try
            {
              py::object result = cb(pct, cursor_ts_ns);
              // Pythonic convention: returning None / no value is
              // treated as "keep running". An explicit False cancels.
              if (result.is_none())
              {
                return true;
              }
              return py::cast<bool>(result);
            }
            catch (py::error_already_set& e)
            {
              // Surface the Python exception by cancelling. The
              // exception is restored after run() returns so the
              // caller sees it instead of a silent partial result.
              e.restore();
              return false;
            }
          },
          interval);
    }
    else
    {
      _reader.clearProgressCallback();
    }
    bool ok;
    {
      py::gil_scoped_release release;
      ok = _reader.run(raw, n_threads);
    }
    // Clear so a re-run without callback does not inherit the previous
    // one — common foot-gun when a user toggles progress on / off.
    if (has_callback)
    {
      _reader.clearProgressCallback();
      // If the Python callback threw, propagate that exception now
      // (we restored it before returning false above).
      if (PyErr_Occurred() != nullptr)
      {
        throw py::error_already_set();
      }
    }
    return ok;
  }

  py::list segmentFiles()
  {
    auto files = _reader.segmentFiles();
    py::list result;
    for (const auto& f : files)
    {
      result.append(f.string());
    }
    return result;
  }

  py::list segments()
  {
    const auto& segs = _reader.segments();
    py::list result;
    for (const auto& seg : segs)
    {
      py::dict d;
      d["path"] = seg.path.string();
      d["first_event_ns"] = seg.first_event_ns;
      d["last_event_ns"] = seg.last_event_ns;
      d["event_count"] = seg.event_count;
      d["has_index"] = seg.has_index;
      result.append(d);
    }
    return result;
  }
};

// ─── PyDataWriter ──────────────────────────────────────────────────────────────

class PyDataWriter
{
  BinaryLogWriter _writer;

 public:
  PyDataWriter(const std::string& outputDir, uint64_t maxSegmentMb, uint8_t exchangeId,
               const std::string& compression)
      : _writer(makeWriterConfig(outputDir, maxSegmentMb, exchangeId, compression))
  {
  }

  bool writeTrade(int64_t exchangeTsNs, int64_t recvTsNs, double price, double qty,
                  uint64_t tradeId, uint32_t symbolId, uint8_t side)
  {
    lrvx::replay::TradeRecord tr{};
    tr.exchange_ts_ns = exchangeTsNs;
    tr.recv_ts_ns = recvTsNs;
    tr.price_raw = lrvx::Price::fromDouble(price).raw();
    tr.qty_raw = lrvx::Quantity::fromDouble(qty).raw();
    tr.trade_id = tradeId;
    tr.symbol_id = symbolId;
    tr.side = side;
    return _writer.writeTrade(tr);
  }

  // c_style | forcecast on every array below: without it a strided view
  // (a `[::2]` slice, a structured-array field) is accepted silently and
  // read at stride 8 bytes regardless of its real element spacing, which
  // writes the wrong trade data to disk -- a durable corruption, worse
  // than a one-off bad read, since the tape this produces looks fine
  // until someone replays it. See rawToDouble a little further down in
  // this file, which already guards against the same thing for reads.
  uint64_t writeTrades(py::array_t<int64_t, py::array::c_style | py::array::forcecast> exchangeTsNs,
                       py::array_t<int64_t, py::array::c_style | py::array::forcecast> recvTsNs,
                       py::array_t<double, py::array::c_style | py::array::forcecast> prices,
                       py::array_t<double, py::array::c_style | py::array::forcecast> quantities,
                       py::array_t<uint64_t, py::array::c_style | py::array::forcecast> tradeIds,
                       py::array_t<uint32_t, py::array::c_style | py::array::forcecast> symbolIds,
                       py::array_t<uint8_t, py::array::c_style | py::array::forcecast> sides)
  {
    size_t n = exchangeTsNs.size();
    if (recvTsNs.size() != static_cast<py::ssize_t>(n) ||
        prices.size() != static_cast<py::ssize_t>(n) ||
        quantities.size() != static_cast<py::ssize_t>(n) ||
        tradeIds.size() != static_cast<py::ssize_t>(n) ||
        symbolIds.size() != static_cast<py::ssize_t>(n) ||
        sides.size() != static_cast<py::ssize_t>(n))
    {
      throw lrvx::LrvxError(
          "E_LEN_001",
          "All input arrays must have the same length.");
    }

    const auto* ets = exchangeTsNs.data();
    const auto* rts = recvTsNs.data();
    const auto* px = prices.data();
    const auto* qt = quantities.data();
    const auto* tid = tradeIds.data();
    const auto* sid = symbolIds.data();
    const auto* sd = sides.data();

    uint64_t written = 0;
    {
      py::gil_scoped_release release;
      for (size_t i = 0; i < n; ++i)
      {
        lrvx::replay::TradeRecord tr{};
        tr.exchange_ts_ns = ets[i];
        tr.recv_ts_ns = rts[i];
        tr.price_raw = lrvx::Price::fromDouble(px[i]).raw();
        tr.qty_raw = lrvx::Quantity::fromDouble(qt[i]).raw();
        tr.trade_id = tid[i];
        tr.symbol_id = sid[i];
        tr.side = sd[i];
        if (_writer.writeTrade(tr))
        {
          ++written;
        }
      }
    }
    return written;
  }

  // Bulk option-quote write. mark_price / index_price are doubles converted via
  // PRICE_SCALE; iv is a double (e.g. 0.65) scaled by kIvScale; open_interest is
  // a double converted via QUANTITY_SCALE.
  uint64_t writeOptionQuotes(py::array_t<int64_t, py::array::c_style | py::array::forcecast> exchangeTsNs,
                             py::array_t<int64_t, py::array::c_style | py::array::forcecast> recvTsNs,
                             py::array_t<double, py::array::c_style | py::array::forcecast> markPrices,
                             py::array_t<double, py::array::c_style | py::array::forcecast> indexPrices,
                             py::array_t<double, py::array::c_style | py::array::forcecast> ivs,
                             py::array_t<double, py::array::c_style | py::array::forcecast> openInterest,
                             py::array_t<uint32_t, py::array::c_style | py::array::forcecast> symbolIds,
                             py::object bidPrices,
                             py::object askPrices, py::object underlyingPrices, py::object bidSizes,
                             py::object askSizes, py::object bidIvs, py::object askIvs)
  {
    size_t n = exchangeTsNs.size();
    if (recvTsNs.size() != static_cast<py::ssize_t>(n) ||
        markPrices.size() != static_cast<py::ssize_t>(n) ||
        indexPrices.size() != static_cast<py::ssize_t>(n) ||
        ivs.size() != static_cast<py::ssize_t>(n) ||
        openInterest.size() != static_cast<py::ssize_t>(n) ||
        symbolIds.size() != static_cast<py::ssize_t>(n))
    {
      throw lrvx::LrvxError("E_LEN_001", "All input arrays must have the same length.");
    }

    // Optional channels: pass None to record 0. Each must match length if
    // given. c_style | forcecast on the cast target, same reason as the
    // required arrays above: without it a strided optional channel writes
    // silently wrong data instead of failing loudly or copying correctly.
    auto optArr = [&](py::object o) -> py::array_t<double, py::array::c_style | py::array::forcecast>
    {
      if (o.is_none())
      {
        return py::array_t<double, py::array::c_style | py::array::forcecast>();
      }
      auto a = o.cast<py::array_t<double, py::array::c_style | py::array::forcecast>>();
      if (a.size() != static_cast<py::ssize_t>(n))
      {
        throw lrvx::LrvxError("E_LEN_001", "All input arrays must have the same length.");
      }
      return a;
    };
    py::array_t<double, py::array::c_style | py::array::forcecast> bidArr = optArr(bidPrices),
                                                                   askArr = optArr(askPrices), undArr = optArr(underlyingPrices), bidSzArr = optArr(bidSizes),
                                                                   askSzArr = optArr(askSizes), bidIvArr = optArr(bidIvs), askIvArr = optArr(askIvs);

    const auto* ets = exchangeTsNs.data();
    const auto* rts = recvTsNs.data();
    const auto* mk = markPrices.data();
    const auto* ix = indexPrices.data();
    const auto* iv = ivs.data();
    const auto* oi = openInterest.data();
    const auto* sid = symbolIds.data();
    const auto* bd = bidArr.size() ? bidArr.data() : nullptr;
    const auto* ak = askArr.size() ? askArr.data() : nullptr;
    const auto* un = undArr.size() ? undArr.data() : nullptr;
    const auto* bsz = bidSzArr.size() ? bidSzArr.data() : nullptr;
    const auto* asz = askSzArr.size() ? askSzArr.data() : nullptr;
    const auto* biv = bidIvArr.size() ? bidIvArr.data() : nullptr;
    const auto* aiv = askIvArr.size() ? askIvArr.data() : nullptr;

    uint64_t written = 0;
    {
      py::gil_scoped_release release;
      for (size_t i = 0; i < n; ++i)
      {
        lrvx::replay::OptionQuoteRecord q{};
        q.exchange_ts_ns = ets[i];
        q.recv_ts_ns = rts[i];
        q.mark_price_raw = lrvx::Price::fromDouble(mk[i]).raw();
        q.index_price_raw = lrvx::Price::fromDouble(ix[i]).raw();
        q.underlying_price_raw = un ? lrvx::Price::fromDouble(un[i]).raw() : 0;
        q.iv_raw = static_cast<int64_t>(iv[i] * lrvx::replay::kIvScale);
        q.open_interest_raw = lrvx::Quantity::fromDouble(oi[i]).raw();
        q.bid_price_raw = bd ? lrvx::Price::fromDouble(bd[i]).raw() : 0;
        q.ask_price_raw = ak ? lrvx::Price::fromDouble(ak[i]).raw() : 0;
        q.bid_size_raw = bsz ? lrvx::Quantity::fromDouble(bsz[i]).raw() : 0;
        q.ask_size_raw = asz ? lrvx::Quantity::fromDouble(asz[i]).raw() : 0;
        q.bid_iv_raw = biv ? static_cast<int64_t>(biv[i] * lrvx::replay::kIvScale) : 0;
        q.ask_iv_raw = aiv ? static_cast<int64_t>(aiv[i] * lrvx::replay::kIvScale) : 0;
        q.symbol_id = sid[i];
        q.instrument = static_cast<uint8_t>(lrvx::InstrumentType::Option);
        if (_writer.writeOptionQuote(q))
        {
          ++written;
        }
      }
    }
    return written;
  }

  // Single book write. `bids`/`asks` are structured arrays with PyLevel
  // dtype (price_raw, qty_raw, side). `side` is ignored on write — bid/
  // ask is determined by which array a level is in. Returns True iff the
  // write succeeded.
  bool writeBook(int64_t exchangeTsNs, int64_t recvTsNs, int64_t seq,
                 uint32_t symbolId, bool isSnapshot,
                 py::array_t<PyLevel, py::array::c_style | py::array::forcecast> bids,
                 py::array_t<PyLevel, py::array::c_style | py::array::forcecast> asks)
  {
    auto n_bids = static_cast<uint32_t>(bids.size());
    auto n_asks = static_cast<uint32_t>(asks.size());

    BookRecordHeader header{};
    header.exchange_ts_ns = exchangeTsNs;
    header.recv_ts_ns = recvTsNs;
    header.seq = seq;
    header.symbol_id = symbolId;
    header.bid_count = static_cast<uint16_t>(n_bids);
    header.ask_count = static_cast<uint16_t>(n_asks);
    header.type = isSnapshot ? 0 : 1;

    // PyLevel includes a `side` byte; copy into contiguous BookLevel.
    std::vector<lrvx::replay::BookLevel> bid_levels;
    std::vector<lrvx::replay::BookLevel> ask_levels;
    bid_levels.reserve(n_bids);
    ask_levels.reserve(n_asks);
    const auto* b = bids.data();
    for (uint32_t i = 0; i < n_bids; ++i)
    {
      bid_levels.push_back({b[i].price_raw, b[i].qty_raw});
    }
    const auto* a = asks.data();
    for (uint32_t i = 0; i < n_asks; ++i)
    {
      ask_levels.push_back({a[i].price_raw, a[i].qty_raw});
    }
    return _writer.writeBook(header, bid_levels, ask_levels);
  }

  // Batched book writer. `headers` is a structured array with
  // PyBookUpdateHeader dtype; `levels` is a flat PyLevel array sliced
  // per event by header.level_offset / bid_count / ask_count. Round-trip
  // with DataReader.read_book_updates(). Returns count successfully written.
  uint64_t writeBooks(py::array_t<PyBookUpdateHeader, py::array::c_style | py::array::forcecast> headers,
                      py::array_t<PyLevel, py::array::c_style | py::array::forcecast> levels)
  {
    const auto n_events = static_cast<uint64_t>(headers.size());
    const auto* h = headers.data();
    const auto* lv = levels.data();
    std::vector<lrvx::replay::BookLevel> scratch;
    uint64_t written = 0;
    {
      py::gil_scoped_release release;
      for (uint64_t i = 0; i < n_events; ++i)
      {
        const auto& fh = h[i];
        const uint64_t total = static_cast<uint64_t>(fh.bid_count) + fh.ask_count;
        scratch.clear();
        scratch.reserve(total);
        for (uint64_t k = 0; k < total; ++k)
        {
          const auto& el = lv[fh.level_offset + k];
          scratch.push_back({el.price_raw, el.qty_raw});
        }

        BookRecordHeader rh{};
        rh.exchange_ts_ns = fh.exchange_ts_ns;
        rh.recv_ts_ns = fh.recv_ts_ns;
        rh.seq = fh.seq;
        rh.symbol_id = fh.symbol_id;
        rh.bid_count = fh.bid_count;
        rh.ask_count = fh.ask_count;
        rh.type = (fh.event_type == 2) ? 0 : 1;

        std::span<const lrvx::replay::BookLevel> bid_span(scratch.data(), fh.bid_count);
        std::span<const lrvx::replay::BookLevel> ask_span(scratch.data() + fh.bid_count, fh.ask_count);
        if (_writer.writeBook(rh, bid_span, ask_span))
        {
          ++written;
        }
      }
    }
    return written;
  }

  void flush() { _writer.flush(); }

  void close() { _writer.close(); }

  py::dict stats()
  {
    auto s = _writer.stats();
    py::dict d;
    d["bytes_written"] = s.bytes_written;
    d["events_written"] = s.events_written;
    d["segments_created"] = s.segments_created;
    d["trades_written"] = s.trades_written;
    d["book_updates_written"] = s.book_updates_written;
    d["blocks_written"] = s.blocks_written;
    d["uncompressed_bytes"] = s.uncompressed_bytes;
    d["compressed_bytes"] = s.compressed_bytes;
    return d;
  }

  std::string currentSegmentPath() { return _writer.currentSegmentPath().string(); }
};

// ─── PyMergedTapeReader ────────────────────────────────────────────────────────

class PyMergedTapeReader
{
  lrvx::replay::MergedTapeReader _reader;

  static lrvx::replay::MergedTapeReaderConfig
  buildConfig(const std::vector<std::string>& paths, py::object from_ns,
              py::object to_ns, py::object symbols)
  {
    lrvx::replay::MergedTapeReaderConfig cfg{};
    cfg.tape_dirs.reserve(paths.size());
    for (const auto& p : paths)
    {
      cfg.tape_dirs.emplace_back(p);
    }
    if (!from_ns.is_none())
    {
      cfg.from_ns = from_ns.cast<int64_t>();
    }
    if (!to_ns.is_none())
    {
      cfg.to_ns = to_ns.cast<int64_t>();
    }
    if (!symbols.is_none())
    {
      for (auto h : symbols.cast<py::list>())
      {
        cfg.symbol_filter.push_back(h.cast<uint32_t>());
      }
    }
    return cfg;
  }

 public:
  PyMergedTapeReader(const std::vector<std::string>& paths, py::object from_ns,
                     py::object to_ns, py::object symbols)
      : _reader(buildConfig(paths, std::move(from_ns), std::move(to_ns),
                            std::move(symbols)))
  {
  }

  py::list symbolTable() const
  {
    py::list out;
    for (const auto& s : _reader.symbols())
    {
      py::dict d;
      d["global_id"] = s.global_id;
      d["exchange"] = s.exchange;
      d["name"] = s.name;
      d["price_precision"] = s.price_precision;
      d["qty_precision"] = s.qty_precision;
      out.append(d);
    }
    return out;
  }

  py::tuple timeRange() const
  {
    auto [a, b] = _reader.timeRange();
    return py::make_tuple(a, b);
  }

  py::dict summary() const
  {
    auto s = _reader.summary();
    py::dict d;
    d["first_event_ns"] = s.first_event_ns;
    d["last_event_ns"] = s.last_event_ns;
    d["total_events"] = s.total_events;
    d["tape_count"] = s.tape_count;
    d["symbol_count"] = s.symbol_count;
    return d;
  }

  py::array readTrades()
  {
    auto rows = _reader.readTrades();
    py::array_t<PyTrade> arr(static_cast<py::ssize_t>(rows.size()));
    auto* out = arr.mutable_data();
    for (size_t i = 0; i < rows.size(); ++i)
    {
      const auto& r = rows[i];
      out[i].exchange_ts_ns = r.exchange_ts_ns;
      out[i].recv_ts_ns = r.recv_ts_ns;
      out[i].price_raw = r.price_raw;
      out[i].qty_raw = r.qty_raw;
      out[i].trade_id = r.trade_id;
      out[i].symbol_id = r.global_symbol_id;
      out[i].side = r.side;
      out[i]._pad[0] = out[i]._pad[1] = out[i]._pad[2] = 0;
    }
    return std::move(arr);
  }

  py::tuple readBooks()
  {
    auto [rows, levels] = _reader.readBooks();
    py::array_t<PyBookUpdateHeader> headers(static_cast<py::ssize_t>(rows.size()));
    auto* hout = headers.mutable_data();
    for (size_t i = 0; i < rows.size(); ++i)
    {
      const auto& r = rows[i];
      hout[i].exchange_ts_ns = r.exchange_ts_ns;
      hout[i].recv_ts_ns = r.recv_ts_ns;
      hout[i].seq = r.seq;
      hout[i].symbol_id = r.global_symbol_id;
      hout[i].bid_count = r.bid_count;
      hout[i].ask_count = r.ask_count;
      hout[i].level_offset = r.level_offset;
      hout[i].event_type = r.event_type;
    }

    py::array_t<PyLevel> lvls(static_cast<py::ssize_t>(levels.size()));
    auto* lout = lvls.mutable_data();
    size_t k = 0;
    for (size_t i = 0; i < rows.size(); ++i)
    {
      const auto& r = rows[i];
      for (uint16_t b = 0; b < r.bid_count; ++b, ++k)
      {
        lout[k].price_raw = levels[k].price_raw;
        lout[k].qty_raw = levels[k].qty_raw;
        lout[k].side = 0;
      }
      for (uint16_t a = 0; a < r.ask_count; ++a, ++k)
      {
        lout[k].price_raw = levels[k].price_raw;
        lout[k].qty_raw = levels[k].qty_raw;
        lout[k].side = 1;
      }
    }
    return py::make_tuple(std::move(headers), std::move(lvls));
  }

  py::list perTapeStats() const
  {
    py::list out;
    for (const auto& s : _reader.perTapeStats())
    {
      py::dict d;
      d["path"] = s.path.string();
      d["trades"] = s.trades;
      d["books"] = s.books;
      d["first_event_ns"] = s.first_event_ns;
      d["last_event_ns"] = s.last_event_ns;
      out.append(d);
    }
    return out;
  }

  // Streaming walk via the C++ N-way heap merge. O(N tapes) peak
  // memory regardless of total event count. on_trade / on_book are
  // optional Python callables; returning False from either aborts.
  // Lossless raw int64 prices/qty preserved (no float conversion).
  void streamEvents(py::object on_trade, py::object on_book)
  {
    _reader.streamEvents(
        [&](uint32_t tape_index, const lrvx::replay::ReplayEvent& ev) -> bool
        {
          if (ev.type == lrvx::replay::EventType::Trade)
          {
            if (on_trade.is_none())
            {
              return true;
            }
            py::object rv = on_trade(int64_t{ev.trade.exchange_ts_ns},
                                     int64_t{ev.trade.recv_ts_ns},
                                     int64_t{ev.trade.price_raw},
                                     int64_t{ev.trade.qty_raw},
                                     uint32_t{ev.trade.symbol_id},
                                     uint32_t{tape_index},
                                     uint8_t{ev.trade.side});
            if (!rv.is_none() && !rv.cast<bool>())
            {
              return false;
            }
            return true;
          }
          if (ev.type == lrvx::replay::EventType::BookSnapshot ||
              ev.type == lrvx::replay::EventType::BookDelta)
          {
            if (on_book.is_none())
            {
              return true;
            }
            const bool is_snap = ev.type == lrvx::replay::EventType::BookSnapshot;
            py::list bids;
            for (const auto& b : ev.bids)
            {
              bids.append(py::make_tuple(int64_t{b.price_raw}, int64_t{b.qty_raw}));
            }
            py::list asks;
            for (const auto& a : ev.asks)
            {
              asks.append(py::make_tuple(int64_t{a.price_raw}, int64_t{a.qty_raw}));
            }
            py::object rv = on_book(int64_t{ev.book_header.exchange_ts_ns},
                                    int64_t{ev.book_header.recv_ts_ns},
                                    uint32_t{ev.book_header.symbol_id},
                                    uint32_t{tape_index},
                                    is_snap,
                                    bids, asks);
            if (!rv.is_none() && !rv.cast<bool>())
            {
              return false;
            }
            return true;
          }
          return true;
        });
  }

  // Streaming aggregator dispatch over the merged stream. Same
  // contract as PyDataReader::run — empty list is a no-op, otherwise
  // every aggregator's onEvent fires once per merged event and
  // finalize() once at the end. GIL released around the whole walk.
  bool run(py::list py_aggregators)
  {
    auto raw = lrvx::collectAggregators(py_aggregators);
    bool ok;
    {
      py::gil_scoped_release release;
      ok = _reader.run(raw);
    }
    return ok;
  }
};

}  // namespace

// ─── Module binding ────────────────────────────────────────────────────────────

inline void bindReplay(py::module_& m)
{
  PYBIND11_NUMPY_DTYPE(PyTrade, exchange_ts_ns, recv_ts_ns, price_raw, qty_raw, trade_id,
                       symbol_id, side);
  PYBIND11_NUMPY_DTYPE(PyBBO, exchange_ts_ns, recv_ts_ns, seq, symbol_id, event_type,
                       bid_price_raw, bid_qty_raw, ask_price_raw, ask_qty_raw);
  PYBIND11_NUMPY_DTYPE(PyBookUpdateHeader, exchange_ts_ns, recv_ts_ns, seq, symbol_id,
                       bid_count, ask_count, level_offset, event_type);
  PYBIND11_NUMPY_DTYPE(PyLevel, price_raw, qty_raw, side);
  PYBIND11_NUMPY_DTYPE(PyOptionQuote, exchange_ts_ns, recv_ts_ns, mark_price_raw,
                       index_price_raw, underlying_price_raw, iv_raw, bid_price_raw, ask_price_raw,
                       bid_size_raw, ask_size_raw, bid_iv_raw, ask_iv_raw, open_interest_raw,
                       symbol_id, instrument);

  // Fixed-point scales — match lrvx::Price/Quantity/Volume in lrvx/common.h.
  // Use these instead of hardcoding 1e8 in client code.
  m.attr("PRICE_SCALE") = py::int_(lrvx::Price::Scale);
  m.attr("QUANTITY_SCALE") = py::int_(lrvx::Quantity::Scale);
  m.attr("VOLUME_SCALE") = py::int_(lrvx::Volume::Scale);

  // Vectorized raw → double converters. Operate on numpy int64 arrays of any
  // shape, including non-contiguous views (e.g. `bars["close_raw"]` taken from
  // a structured array). Output is always a fresh contiguous array of the same
  // shape as the input.
  auto rawToDouble = [](py::array raw, int64_t scale) -> py::array_t<double>
  {
    // Force int64 dtype + contiguous layout. If the caller passed a strided
    // view (e.g. a structured-array field), this materialises a contiguous
    // copy; if it was already contiguous int64, no copy.
    py::array_t<int64_t, py::array::c_style | py::array::forcecast> in(raw);
    const int64_t* in_ptr = in.data();

    std::vector<py::ssize_t> shape(in.shape(), in.shape() + in.ndim());
    py::array_t<double> out(shape);
    double* out_ptr = out.mutable_data();

    const double inv_scale = 1.0 / static_cast<double>(scale);
    const py::ssize_t n = in.size();
    for (py::ssize_t i = 0; i < n; ++i)
    {
      out_ptr[i] = static_cast<double>(in_ptr[i]) * inv_scale;
    }
    return out;
  };

  m.def(
      "prices_to_double",
      [rawToDouble](py::array_t<int64_t> raw)
      { return rawToDouble(raw, lrvx::Price::Scale); },
      "Convert raw int64 price array to float64 array (divides by PRICE_SCALE).",
      py::arg("raw"));

  m.def(
      "quantities_to_double",
      [rawToDouble](py::array_t<int64_t> raw)
      { return rawToDouble(raw, lrvx::Quantity::Scale); },
      "Convert raw int64 quantity array to float64 array (divides by QUANTITY_SCALE).",
      py::arg("raw"));

  m.def(
      "volumes_to_double",
      [rawToDouble](py::array_t<int64_t> raw)
      { return rawToDouble(raw, lrvx::Volume::Scale); },
      "Convert raw int64 volume array to float64 array (divides by VOLUME_SCALE).",
      py::arg("raw"));

  // Module-level inspect function
  m.def(
      "inspect",
      [](const std::string& dataDir) -> py::dict
      {
        lrvx::replay::DatasetSummary s;
        {
          py::gil_scoped_release release;
          s = lrvx::replay::BinaryLogReader::inspect(dataDir);
        }

        py::dict d;
        d["first_event_ns"] = s.first_event_ns;
        d["last_event_ns"] = s.last_event_ns;
        d["total_events"] = s.total_events;
        d["segment_count"] = s.segment_count;
        d["total_bytes"] = s.total_bytes;
        d["duration_seconds"] = s.durationSeconds();
        d["fully_indexed"] = s.fullyIndexed();
        return d;
      },
      "Inspect a data directory and return summary statistics",
      py::arg("data_dir"));

  // DataReader
  py::class_<PyDataReader>(m, "DataReader")
      .def(py::init<const std::string&, py::object, py::object, py::object,
                    py::object, bool>(),
           "Create a DataReader for a binary log data directory. "
           "`reorder_window_ns` controls the bounded reorder buffer applied "
           "to segments without the Sorted flag (default 10s). An event that "
           "arrives past that window is dropped and counted in "
           "stats()['late_dropped'], with one warning logged per segment; set "
           "`strict_ordering=True` to raise LrvxError E_DATA_002 on the first "
           "such event instead. `to_ns` is inclusive for trade reads and "
           "exclusive for option-quote reads -- see read_option_quotes.",
           py::arg("data_dir"),
           py::arg("from_ns") = py::none(),
           py::arg("to_ns") = py::none(),
           py::arg("symbols") = py::none(),
           py::arg("reorder_window_ns") = py::none(),
           py::arg("strict_ordering") = false)
      .def("summary", &PyDataReader::summary,
           "Return a dict summarizing the dataset")
      .def("count", &PyDataReader::count,
           "Return the total number of events")
      .def("symbols", &PyDataReader::symbols,
           "Return the set of available symbol IDs")
      .def("time_range", &PyDataReader::timeRange,
           "Return (start_ns, end_ns) tuple or None")
      .def("read_trades", &PyDataReader::readTrades,
           "Read all trades as a numpy structured array (PyTrade dtype). The "
           "reader's to_ns bound is INCLUSIVE here: a trade stamped exactly "
           "to_ns is returned.")
      .def("read_trades_from", &PyDataReader::readTradesFrom,
           "Read trades starting from a given timestamp (nanoseconds). The "
           "reader's to_ns bound is inclusive.",
           py::arg("start_ts_ns"))
      .def("read_option_quotes_from", &PyDataReader::readOptionQuotesFrom,
           "Read option quotes (mark/iv/index/open-interest) from a given timestamp "
           "(nanoseconds) as a numpy structured array (PyOptionQuote dtype). Raw "
           "fixed-point: mark/index use PRICE_SCALE, iv uses 1e8, oi uses QUANTITY_SCALE. "
           "The reader's to_ns bound is EXCLUSIVE here, unlike read_trades: a quote "
           "stamped exactly to_ns belongs to the next window, so day-by-day slicing "
           "of a month returns each quote once.",
           py::arg("start_ts_ns"))
      .def("read_bbo", &PyDataReader::readBBO,
           "Read best bid/ask from every book update as a numpy structured array (PyBBO dtype)")
      .def("read_bbo_from", &PyDataReader::readBBOFrom,
           "Read BBO starting from a given timestamp (nanoseconds)",
           py::arg("start_ts_ns"))
      .def("read_book_updates", &PyDataReader::readBookUpdates,
           "Read all book updates. Returns (headers, levels) tuple of numpy structured arrays. "
           "headers dtype: PyBookUpdateHeader; levels dtype: PyLevel. "
           "For event i: bids=levels[h['level_offset']:h['level_offset']+h['bid_count']], "
           "asks=levels[h['level_offset']+h['bid_count']:h['level_offset']+h['bid_count']+h['ask_count']]")
      .def("read_book_updates_from", &PyDataReader::readBookUpdatesFrom,
           "Read book updates starting from a given timestamp (nanoseconds). "
           "Same return shape as read_book_updates().",
           py::arg("start_ts_ns"))
      .def("stats", &PyDataReader::stats,
           "Return reader statistics as a dict")
      .def("segment_files", &PyDataReader::segmentFiles,
           "Return list of segment file paths")
      .def("segments", &PyDataReader::segments,
           "Return list of segment info dicts")
      .def("run", &PyDataReader::run,
           "Run a panel of streaming aggregators over the tape in a single "
           "decompression pass. Each aggregator's onEvent fires once per "
           "event, then finalize() fires once at the end. An empty list is "
           "a no-op (no decompression). GIL released for the whole walk. "
           "n_threads=0 (default) is auto, resolved to "
           "min(blocks_per_segment/2, hardware_concurrency); n_threads=1 "
           "forces explicit single-thread; n_threads>1 partitions each "
           "segment at the compressed-block level via "
           "IAggregator::cloneEmpty()/merge() and is capped to the "
           "effective block count. "
           "progress_callback, when set, fires inside the run loop at most "
           "once per progress_interval_ms with (pct: float in [0,1], "
           "cursor_ts_ns: int). Return False to cancel; raising stops the "
           "run and re-raises after finalize. Progress is reported only on "
           "the single-thread path (n_threads=1); multi-thread runs ignore "
           "the callback because per-event GIL re-acquisition would defeat "
           "the parallelism.",
           py::arg("aggregators"), py::arg("n_threads") = std::size_t{0},
           py::arg("progress_callback") = py::none(),
           py::arg("progress_interval_ms") = uint32_t{1000});

  // DataWriter
  py::class_<PyDataWriter>(m, "DataWriter")
      .def(py::init<const std::string&, uint64_t, uint8_t, const std::string&>(),
           "Create a DataWriter for binary log output",
           py::arg("output_dir"),
           py::arg("max_segment_mb") = 256,
           py::arg("exchange_id") = 0,
           py::arg("compression") = "none")
      .def("write_trade", &PyDataWriter::writeTrade,
           "Write a single trade record",
           py::arg("exchange_ts_ns"), py::arg("recv_ts_ns"),
           py::arg("price"), py::arg("qty"),
           py::arg("trade_id"), py::arg("symbol_id"), py::arg("side"))
      .def("write_book", &PyDataWriter::writeBook,
           "Write a single book update. bids/asks are PyLevel structured arrays.",
           py::arg("exchange_ts_ns"), py::arg("recv_ts_ns"),
           py::arg("seq"), py::arg("symbol_id"), py::arg("is_snapshot"),
           py::arg("bids"), py::arg("asks"))
      .def("write_books", &PyDataWriter::writeBooks,
           "Batched book writer. Round-trip with DataReader.read_book_updates.",
           py::arg("headers"), py::arg("levels"))
      .def("write_trades", &PyDataWriter::writeTrades,
           "Write trades from numpy arrays (vectorized). Returns number of trades written.",
           py::arg("exchange_ts_ns"), py::arg("recv_ts_ns"),
           py::arg("prices"), py::arg("quantities"),
           py::arg("trade_ids"), py::arg("symbol_ids"), py::arg("sides"))
      .def("write_option_quotes", &PyDataWriter::writeOptionQuotes,
           "Bulk-write option quotes. mark_prices/index_prices are doubles "
           "(PRICE_SCALE), ivs are doubles (e.g. 0.65), open_interest is a double "
           "(QUANTITY_SCALE). The rest are optional (None -> 0): bid_prices/"
           "ask_prices and bid_sizes/ask_sizes give the touch the fill model "
           "crosses; underlying_prices is the per-expiry forward (pricing/"
           "moneyness) vs index_prices (spot index); bid_ivs/ask_ivs are the vol "
           "at each touch. Returns the count written.",
           py::arg("exchange_ts_ns"), py::arg("recv_ts_ns"), py::arg("mark_prices"),
           py::arg("index_prices"), py::arg("ivs"), py::arg("open_interest"),
           py::arg("symbol_ids"), py::arg("bid_prices") = py::none(),
           py::arg("ask_prices") = py::none(), py::arg("underlying_prices") = py::none(),
           py::arg("bid_sizes") = py::none(), py::arg("ask_sizes") = py::none(),
           py::arg("bid_ivs") = py::none(), py::arg("ask_ivs") = py::none())
      .def("flush", &PyDataWriter::flush,
           "Flush buffered data to disk")
      .def("close", &PyDataWriter::close,
           "Close the writer and finalize all segments")
      .def("stats", &PyDataWriter::stats,
           "Return writer statistics as a dict")
      .def("current_segment_path", &PyDataWriter::currentSegmentPath,
           "Return the path of the current segment being written");

  // BinaryLogRecorderHook — built-in `.lrvx` sink. Plug into a runner
  // via `runner.set_market_data_recorder(hook)`; lifecycle (start/stop) is
  // driven automatically by the engine.
  py::class_<lrvx::PyBinaryLogRecorderHook, std::shared_ptr<lrvx::PyBinaryLogRecorderHook>>(
      m, "BinaryLogRecorderHook")
      .def(py::init<const std::string&, uint64_t, uint8_t, const std::string&,
                    const std::string&, const std::string&>(),
           "Create a binary-log recorder hook. Trades and books are written "
           "via BinaryLogWriter on the engine thread — no Python callback "
           "per event. exchange_name + instrument_type are stamped into "
           "the recording's metadata.json so MergedTapeReader can key by "
           "(exchange, name) across multiple tapes.",
           py::arg("output_dir"),
           py::arg("max_segment_mb") = 256,
           py::arg("exchange_id") = 0,
           py::arg("compression") = "none",
           py::arg("exchange_name") = "",
           py::arg("instrument_type") = "")
      .def("add_symbol", &lrvx::PyBinaryLogRecorderHook::addSymbol,
           "Register a symbol for recording metadata",
           py::arg("symbol_id"), py::arg("name"),
           py::arg("base") = "", py::arg("quote") = "",
           py::arg("price_precision") = 8, py::arg("qty_precision") = 8)
      .def("flush", &lrvx::PyBinaryLogRecorderHook::flush,
           "Flush buffered data to disk")
      .def("close", &lrvx::PyBinaryLogRecorderHook::close,
           "Stop recording (idempotent; engine on_stop also calls this)")
      .def("stats", &lrvx::PyBinaryLogRecorderHook::stats,
           "Return recorder statistics as a dict")
      .def("current_segment_path", &lrvx::PyBinaryLogRecorderHook::currentSegmentPath,
           "Return the path of the segment currently being written");

  // MergedTapeReader — k-tape consumption primitive.
  py::class_<PyMergedTapeReader>(m, "MergedTapeReader")
      .def(py::init<const std::vector<std::string>&, py::object, py::object,
                    py::object>(),
           "Open N `.lrvx` directories and expose a merged trade / book "
           "stream. Symbols are rekeyed into a global id space, keyed by "
           "(metadata.exchange, name). Ties on exchange_ts_ns are broken by "
           "tape order in `paths`. read_trades / read_books are eager — "
           "they materialise the merged arrays.",
           py::arg("paths"), py::arg("from_ns") = py::none(),
           py::arg("to_ns") = py::none(), py::arg("symbols") = py::none())
      .def("symbol_table", &PyMergedTapeReader::symbolTable,
           "List of dicts: global_id, exchange, name, price_precision, "
           "qty_precision.")
      .def("time_range", &PyMergedTapeReader::timeRange,
           "(min first_event_ns, max last_event_ns) across all tapes.")
      .def("summary", &PyMergedTapeReader::summary,
           "Aggregate stats: first_event_ns, last_event_ns, "
           "total_events (populated after readTrades/readBooks), "
           "tape_count, symbol_count.")
      .def("read_trades", &PyMergedTapeReader::readTrades,
           "Merged trades as PyTrade structured numpy array, sorted by "
           "exchange_ts_ns; tie-break by tape order.")
      .def("read_books", &PyMergedTapeReader::readBooks,
           "(headers, levels) tuple. Headers carry global symbol_id; "
           "level_offset slices the levels array per event.")
      .def("per_tape_stats", &PyMergedTapeReader::perTapeStats,
           "Per-tape breakdown — useful for debugging an empty input.")
      .def("stream_events", &PyMergedTapeReader::streamEvents,
           "Walk the merged stream via N-way heap merge (O(N tapes) "
           "peak memory). Calls on_trade(exchange_ts_ns, recv_ts_ns, "
           "price_raw, qty_raw, symbol_id, tape_index, side) and "
           "on_book(exchange_ts_ns, recv_ts_ns, symbol_id, "
           "tape_index, is_snapshot, bids, asks). bids/asks are "
           "lists of (price_raw, qty_raw) tuples. Returning False "
           "from either aborts the walk.",
           py::arg("on_trade") = py::none(),
           py::arg("on_book") = py::none())
      .def("run", &PyMergedTapeReader::run,
           "Run a panel of streaming aggregators over the merged tape "
           "stream in a single decompression pass. Same semantics as "
           "DataReader.run — events are seen with global-rewritten "
           "symbol ids; per-tape provenance is not surfaced to "
           "aggregators (reach for stream_events directly when needed).",
           py::arg("aggregators"));
}
