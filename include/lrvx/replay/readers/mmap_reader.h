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

#include <cstddef>
#include <filesystem>
#include <functional>

namespace lrvx::replay
{

class MmapSegmentReader
{
 public:
  explicit MmapSegmentReader(const std::filesystem::path& segment_path);
  ~MmapSegmentReader();

  MmapSegmentReader(const MmapSegmentReader&) = delete;
  MmapSegmentReader& operator=(const MmapSegmentReader&) = delete;
  MmapSegmentReader(MmapSegmentReader&&) noexcept;
  MmapSegmentReader& operator=(MmapSegmentReader&&) noexcept;

  bool isValid() const;
  bool isCompressed() const;
  bool hasIndex() const;

  const SegmentHeader& header() const;

  const std::byte* data() const;
  size_t dataSize() const;
  size_t totalSize() const;

  bool next(ReplayEvent& out);
  void reset();
  // As BinaryLogIterator::seekToTimestamp: the last index entry strictly below
  // the target, in a Sorted segment only; otherwise false and back to the
  // first event.
  bool seekToTimestamp(int64_t target_ts_ns);
  size_t position() const;

  const FrameHeader* currentFrame() const;
  bool advanceFrame();

  bool loadIndex();
  const std::vector<IndexEntry>& indexEntries() const;

 private:
  void cleanup();

  void* _mapped_data{nullptr};
  size_t _mapped_size{0};

#ifdef _WIN32
  void* _file_handle{nullptr};
  void* _mapping_handle{nullptr};
#else
  int _fd{-1};
#endif

  const SegmentHeader* _header{nullptr};
  const std::byte* _data_start{nullptr};
  const std::byte* _data_end{nullptr};
  const std::byte* _current{nullptr};

  std::vector<IndexEntry> _index_entries;
  bool _index_loaded{false};
};

struct MmapReaderStats
{
  uint64_t segments_mapped{0};
  uint64_t bytes_mapped{0};
  uint64_t events_read{0};
  uint64_t page_faults{0};
};

class MmapReader
{
 public:
  struct Config
  {
    std::filesystem::path data_dir;
    bool preload_index{true};
    bool prefault_pages{false};
    std::optional<int64_t> from_ns;
    std::optional<int64_t> to_ns;
    std::set<uint32_t> symbols;
  };

  explicit MmapReader(const Config& config);
  ~MmapReader();

  MmapReader(const MmapReader&) = delete;
  MmapReader& operator=(const MmapReader&) = delete;

  using EventCallback = std::function<bool(const ReplayEvent&)>;

  uint64_t forEach(EventCallback callback);
  uint64_t forEachFrom(int64_t start_ts_ns, EventCallback callback);

  using RawTradeCallback = std::function<bool(const TradeRecord*)>;
  using RawBookCallback = std::function<bool(const BookRecordHeader*, const BookLevel*, const BookLevel*)>;

  uint64_t forEachRawTrade(RawTradeCallback callback);

  MmapReaderStats stats() const;
  const std::vector<SegmentInfo>& segments() const { return _segments; }
  uint64_t totalEvents() const;

 private:
  bool passesFilter(int64_t timestamp, uint32_t symbol_id) const;

  Config _config;
  std::vector<SegmentInfo> _segments;
  std::vector<std::unique_ptr<MmapSegmentReader>> _readers;
  MmapReaderStats _stats;
};

inline uint64_t mmapCount(const std::filesystem::path& data_dir)
{
  MmapReader::Config config{.data_dir = data_dir};
  MmapReader reader(config);
  return reader.totalEvents();
}

}  // namespace lrvx::replay
