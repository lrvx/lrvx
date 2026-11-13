/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/run/trace_recorder.h"

#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lrvx::run
{

struct TraceManifest
{
  uint32_t schema_version{1};
  uint32_t format_version{1};
  std::string strategy_id;
  std::string strategy_hash;
  int64_t run_started_ns{0};
  int64_t run_ended_ns{0};
  std::vector<TapeRef> tape_refs;

  struct Segment
  {
    std::string name;
    RecordKind record_kind{RecordKind::Unknown};
    uint64_t size_bytes{0};
    int64_t first_event_ns{0};
    int64_t last_event_ns{0};
    uint32_t event_count{0};
  };
  std::vector<Segment> segments;
};

// Owns its own buffers for variable-length payloads (name / symbol_ids / reason / payload).
struct OwnedSignal
{
  int64_t run_ts_ns{0};
  int64_t feed_ts_ns{0};
  // The low 32 bits of the id, as this field has always read. signalId64()
  // is the whole id; the two differ for a captured signal, whose id is its
  // order id.
  uint32_t signal_id{0};
  uint32_t signal_id_hi{0};
  uint64_t signalId64() const noexcept
  {
    return (static_cast<uint64_t>(signal_id_hi) << 32) | signal_id;
  }
  uint32_t flags{0};
  int64_t strength_raw{0};
  std::string name;
  std::vector<uint32_t> symbol_ids;
  std::vector<uint8_t> payload;
};

struct OwnedOrderEvent
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
  std::string reason;
};

class TraceReader
{
 public:
  explicit TraceReader(const std::string& path);
  ~TraceReader();

  TraceReader(const TraceReader&) = delete;
  TraceReader& operator=(const TraceReader&) = delete;

  const TraceManifest& manifest() const noexcept { return _manifest; }

  // Read all signals / orders / fills in-order from their respective segments.
  // Returns empty vector if no segment of that kind was recorded.
  std::vector<OwnedSignal> readAllSignals();
  std::vector<OwnedOrderEvent> readAllOrderEvents();
  std::vector<FillRecord> readAllFills();

 private:
  void loadManifest();
  std::optional<TraceManifest::Segment> findSegment(RecordKind kind) const;
  std::vector<uint8_t> readSegmentBytes(const std::string& name) const;

  std::string _root;
  TraceManifest _manifest;
};

}  // namespace lrvx::run
