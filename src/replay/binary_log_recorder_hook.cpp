/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/replay/binary_log_recorder_hook.h"

#include "lrvx/replay/binary_format_v1.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <span>
#include <sstream>

namespace lrvx::replay
{

namespace
{

std::string isoNow()
{
  auto now = std::chrono::system_clock::now();
  auto t = std::chrono::system_clock::to_time_t(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch())
                .count() %
            1000;

  std::ostringstream oss;
  oss << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%S");
  oss << '.' << std::setfill('0') << std::setw(3) << ms << 'Z';
  return oss.str();
}

}  // namespace

BinaryLogRecorderHook::BinaryLogRecorderHook(BinaryLogRecorderHookConfig config)
    : _config(std::move(config))
{
}

BinaryLogRecorderHook::~BinaryLogRecorderHook() { stop(); }

void BinaryLogRecorderHook::start()
{
  if (_recording.exchange(true, std::memory_order_acq_rel))
  {
    return;
  }

  RecordingMetadata meta = _config.metadata.value_or(RecordingMetadata{});
  if (meta.recording_start.empty())
  {
    meta.recording_start = isoNow();
  }

  WriterConfig wcfg{
      .output_dir = _config.output_dir,
      .max_segment_bytes = _config.max_segment_bytes,
      .exchange_id = _config.exchange_id,
      .compression = _config.compression,
      .metadata = std::move(meta),
  };

  _writer = std::make_unique<BinaryLogWriter>(std::move(wcfg));
}

void BinaryLogRecorderHook::stop()
{
  if (!_recording.exchange(false, std::memory_order_acq_rel))
  {
    return;
  }
  if (_writer)
  {
    _writer->close();
    _writer.reset();
  }
}

void BinaryLogRecorderHook::onTrade(uint32_t symbol_id, int64_t price_raw,
                                    int64_t qty_raw, bool is_buy,
                                    int64_t exchange_ts_ns,
                                    int64_t recv_ts_ns) noexcept
{
  if (!_recording.load(std::memory_order_relaxed) || !_writer)
  {
    return;
  }

  TradeRecord rec{};
  rec.exchange_ts_ns = exchange_ts_ns;
  rec.recv_ts_ns = recv_ts_ns;
  rec.price_raw = price_raw;
  rec.qty_raw = qty_raw;
  rec.symbol_id = symbol_id;
  // Side convention: 0 = buy, 1 = sell, matching lrvx::Side, the TradeRecord
  // row of docs/spec/lrvx.md and every other writer of the format.
  rec.side = is_buy ? 0 : 1;
  rec.exchange_id = _config.exchange_id;

  if (_writer->writeTrade(rec))
  {
    ++_stats.trades_written;
    // Lazy metadata flag so downstream consumers (e.g. MergedTapeReader's
    // overlap detector) can tell which content kinds this tape carries.
    _writer->setHasTrades(true);
  }
  else
  {
    ++_stats.errors;
  }
}

void BinaryLogRecorderHook::onBookUpdate(uint32_t symbol_id, uint8_t update_type,
                                         const BookLevel* bids, uint32_t n_bids,
                                         const BookLevel* asks, uint32_t n_asks,
                                         int64_t exchange_ts_ns,
                                         int64_t recv_ts_ns) noexcept
{
  if (!_recording.load(std::memory_order_relaxed) || !_writer)
  {
    return;
  }

  BookRecordHeader header{};
  header.exchange_ts_ns = exchange_ts_ns;
  header.recv_ts_ns = recv_ts_ns;
  header.symbol_id = symbol_id;
  header.bid_count = static_cast<uint16_t>(n_bids);
  header.ask_count = static_cast<uint16_t>(n_asks);
  header.type = (update_type == 1) ? 0 : (update_type == 2) ? 2
                                                            : 1;
  header.exchange_id = _config.exchange_id;

  std::span<const BookLevel> bid_span(bids, n_bids);
  std::span<const BookLevel> ask_span(asks, n_asks);

  if (_writer->writeBook(header, bid_span, ask_span))
  {
    ++_stats.book_updates_written;
    if (update_type == 1)
    {
      _writer->setHasBookSnapshots(true);
    }
    else if (update_type == 2)
    {
      _writer->setHasBookTops(true);
    }
    else
    {
      _writer->setHasBookDeltas(true);
    }
  }
  else
  {
    ++_stats.errors;
  }
}

void BinaryLogRecorderHook::addSymbol(const SymbolInfo& info)
{
  if (_writer)
  {
    _writer->addSymbol(info);
  }
  else
  {
    // Stash for next start(): mutate the metadata config.
    if (!_config.metadata)
    {
      _config.metadata.emplace();
    }
    _config.metadata->symbols.push_back(info);
  }
}

void BinaryLogRecorderHook::flush()
{
  if (_writer)
  {
    _writer->flush();
  }
}

RecorderStats BinaryLogRecorderHook::stats() const noexcept
{
  if (_writer)
  {
    auto ws = _writer->stats();
    _stats.bytes_written = ws.bytes_written;
    _stats.segments_created = ws.segments_created;
    _stats.trades_written = ws.trades_written;
    _stats.book_updates_written = ws.book_updates_written;
  }
  return _stats;
}

std::filesystem::path BinaryLogRecorderHook::currentSegmentPath() const
{
  return _writer ? _writer->currentSegmentPath() : std::filesystem::path{};
}

}  // namespace lrvx::replay
