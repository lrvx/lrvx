/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/replay/binary_format_v1.h"
#include "lrvx/replay/readers/binary_log_reader.h"

#include <functional>
#include <memory>
#include <optional>
#include <set>

namespace lrvx::replay
{

class ISegmentReader
{
 public:
  virtual ~ISegmentReader() = default;

  virtual bool isValid() const = 0;
  virtual bool isCompressed() const = 0;
  virtual bool hasIndex() const = 0;

  virtual bool next(ReplayEvent& out) = 0;
  virtual void reset() = 0;
  virtual bool seekToTimestamp(int64_t target_ts_ns) = 0;

  virtual const SegmentHeader& header() const = 0;
};

struct ReadProgress
{
  uint64_t events_processed{0};
  uint64_t total_events{0};
  uint32_t segments_processed{0};
  uint32_t total_segments{0};
  int64_t current_timestamp_ns{0};

  double percentComplete() const
  {
    return total_events > 0 ? (100.0 * events_processed / total_events) : 0.0;
  }

  double segmentProgress() const
  {
    return total_segments > 0 ? (100.0 * segments_processed / total_segments) : 0.0;
  }
};

using ProgressCallback = std::function<void(const ReadProgress&)>;

class IMultiSegmentReader
{
 public:
  virtual ~IMultiSegmentReader() = default;

  using EventCallback = std::function<bool(const ReplayEvent&)>;

  virtual uint64_t forEach(EventCallback callback) = 0;
  virtual uint64_t forEachFrom(int64_t start_ts_ns, EventCallback callback) = 0;

  virtual uint64_t forEachWithProgress(
      EventCallback callback,
      ProgressCallback progress,
      uint64_t progress_interval_events = 10000)
  {
    (void)progress;
    (void)progress_interval_events;
    return forEach(callback);
  }

  virtual const std::vector<SegmentInfo>& segments() const = 0;
  virtual uint64_t totalEvents() const = 0;
};

struct ReaderFilter
{
  std::optional<int64_t> from_ns;
  std::optional<int64_t> to_ns;
  std::set<uint32_t> symbols;

  bool passes(int64_t timestamp_ns, uint32_t symbol_id) const
  {
    if (from_ns.has_value() && timestamp_ns < *from_ns)
    {
      return false;
    }
    if (to_ns.has_value() && timestamp_ns > *to_ns)
    {
      return false;
    }
    if (!symbols.empty() && symbols.find(symbol_id) == symbols.end())
    {
      return false;
    }
    return true;
  }

  bool passes(const ReplayEvent& event) const
  {
    return passes(event.timestamp_ns, event.symbolId());
  }
};

// Both factories build readers that check every frame's CRC32 and skip one
// that fails, whichever reader they pick (ReaderConfig::verify_crc and
// MmapReader::Config::verify_crc both default to true). The mapped readers
// read uncompressed segments only: createSegmentReader falls back to the
// stream iterator for a compressed one.
std::unique_ptr<ISegmentReader> createSegmentReader(
    const std::filesystem::path& segment_path,
    bool prefer_mmap = true);

std::unique_ptr<IMultiSegmentReader> createMultiSegmentReader(
    const std::filesystem::path& data_dir,
    const ReaderFilter& filter = {},
    bool parallel = false,
    bool mmap = false);

}  // namespace lrvx::replay
