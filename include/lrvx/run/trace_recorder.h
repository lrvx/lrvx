/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/run/run_format_v1.h"

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lrvx::run
{

struct TapeRef
{
  std::string path;
  std::string content_hash;
  int64_t first_event_ns{0};
  int64_t last_event_ns{0};
};

struct TraceRecorderOptions
{
  std::string strategy_id;
  std::string strategy_hash;
  int64_t run_started_ns{0};
  std::vector<TapeRef> tape_refs;
};

struct SignalView
{
  int64_t run_ts_ns{0};
  int64_t feed_ts_ns{0};
  // All 64 bits are written (see SignalRecord).
  uint64_t signal_id{0};
  uint32_t flags{0};
  int64_t strength_raw{0};
  std::string_view name;
  std::vector<uint32_t> symbol_ids;
  std::string_view payload;
};

struct OrderEventView
{
  int64_t run_ts_ns{0};
  int64_t feed_ts_ns{0};
  uint64_t order_id{0};
  uint64_t parent_signal_id{0};
  int64_t price_raw{0};
  int64_t qty_raw{0};
  uint32_t symbol_id{0};
  OrderEventKind event_kind{OrderEventKind::Submit};
  uint8_t side{0};
  uint8_t order_type{0};
  uint32_t flags{0};
  std::string_view reason;
};

struct FillView
{
  int64_t run_ts_ns{0};
  int64_t feed_ts_ns{0};
  uint64_t order_id{0};
  uint64_t fill_id{0};
  int64_t price_raw{0};
  int64_t qty_raw{0};
  int64_t fee_raw{0};
  uint32_t symbol_id{0};
  uint8_t side{0};
  FillLiquidity liquidity{FillLiquidity::Unknown};
};

class TraceRecorder
{
 public:
  TraceRecorder(const std::string& path, TraceRecorderOptions options);
  ~TraceRecorder();

  TraceRecorder(const TraceRecorder&) = delete;
  TraceRecorder& operator=(const TraceRecorder&) = delete;

  void writeSignal(const SignalView& s);
  void writeOrderEvent(const OrderEventView& e);
  void writeFill(const FillView& f);

  // Add a tape reference to the run manifest. Safe to call any time before close().
  void addTapeRef(TapeRef ref);

  // Optional: caller-provided run-end timestamp. Defaults to last seen ts.
  void setRunEndedNs(int64_t ns) noexcept { _run_ended_ns = ns; }

  // Flush and finalize segment files + manifest.json. Idempotent.
  void close();

 private:
  struct SegmentWriter
  {
    std::string filename;
    std::ofstream stream;
    RunSegmentHeader header{};
    bool header_written{false};
    int64_t first_event_ns{0};
    int64_t last_event_ns{0};
    uint32_t event_count{0};
    uint64_t size_bytes{0};
  };

  SegmentWriter& openSegment(RecordKind kind);
  void writeFrameBlob(SegmentWriter& seg, FrameType type, const void* payload, uint32_t size, int64_t ts);
  void finalizeSegment(SegmentWriter& seg);
  void writeManifest();

  std::string _root;
  TraceRecorderOptions _options;
  int64_t _run_ended_ns{0};
  std::unique_ptr<SegmentWriter> _signals;
  std::unique_ptr<SegmentWriter> _orders;
  std::unique_ptr<SegmentWriter> _fills;
  bool _closed{false};
};

}  // namespace lrvx::run
