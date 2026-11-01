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
#include "lrvx/replay/recording_metadata.h"

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace lrvx::replay
{

using RotationCallback = std::filesystem::path (*)(void* user_data,
                                                   const std::filesystem::path& output_dir,
                                                   uint32_t segment_number);

struct WriterConfig
{
  std::filesystem::path output_dir;
  std::string output_filename;  // If set, use this filename for first segment
  uint64_t max_segment_bytes{256ull << 20};
  uint64_t buffer_size{64ull << 10};
  uint8_t exchange_id{0};
  bool sync_on_rotate{true};
  bool create_index{true};
  uint16_t index_interval{kDefaultIndexInterval};
  CompressionType compression{CompressionType::None};
  uint64_t stdio_buffer_size{64ull << 10};  // setvbuf size (default 64KB, 0 = system default 8KB)

  /// Optional callback for custom segment naming on rotation.
  /// If not set, rotated segments use timestamp-based names.
  RotationCallback rotation_callback{nullptr};
  void* rotation_user_data{nullptr};

  /// Optional metadata to write alongside the log files.
  /// If set, a metadata.json file will be created in output_dir.
  std::optional<RecordingMetadata> metadata;
};

struct WriterStats
{
  uint64_t bytes_written{0};
  uint64_t events_written{0};
  uint64_t segments_created{0};
  uint64_t trades_written{0};
  uint64_t book_updates_written{0};
  uint64_t option_quotes_written{0};
  uint64_t blocks_written{0};
  uint64_t uncompressed_bytes{0};
  uint64_t compressed_bytes{0};
};

class BinaryLogWriter
{
 public:
  explicit BinaryLogWriter(WriterConfig config);
  ~BinaryLogWriter();

  BinaryLogWriter(const BinaryLogWriter&) = delete;
  BinaryLogWriter& operator=(const BinaryLogWriter&) = delete;
  BinaryLogWriter(BinaryLogWriter&&) noexcept;
  BinaryLogWriter& operator=(BinaryLogWriter&&) noexcept;

  bool writeTrade(const TradeRecord& trade);
  bool writeOptionQuote(const OptionQuoteRecord& quote);
  // A DEX pool-state record: the fixed header (its payload_len must equal
  // payload_size) followed by payload_size bytes of the u256-native pool payload.
  bool writePoolState(const PoolStateRecordHeader& header, const void* payload, size_t payload_size);
  bool writeBook(const BookRecordHeader& header, std::span<const BookLevel> bids,
                 std::span<const BookLevel> asks);

  void flush();
  void close();

  WriterStats stats() const;
  std::filesystem::path currentSegmentPath() const;

  /// Update metadata (can be called during recording to add symbols, etc.)
  void setMetadata(const RecordingMetadata& meta);
  RecordingMetadata* metadata() { return _metadata ? &*_metadata : nullptr; }

  /// Add a symbol to metadata
  void addSymbol(const SymbolInfo& symbol);

  /// Set content flags in metadata
  void setHasTrades(bool v);
  void setHasBookSnapshots(bool v);
  void setHasBookDeltas(bool v);
  void setHasBookTops(bool v);

 private:
  bool ensureOpen();
  bool maybeRotate(size_t needed_bytes);
  bool writeFrame(EventType type, const void* payload, size_t size);
  bool writeFrameToBlock(EventType type, const void* payload, size_t size, int64_t timestamp);
  bool flushBlock();
  struct BlockSpan
  {
    int64_t min_ts{0};
    int64_t max_ts{0};
  };
  BlockSpan sortBlockBuffer();  // first and last timestamp in the block after sorting
  // Records an uncompressed frame's timestamp against the segment's ordering
  // promise. Uncompressed frames reach the file in arrival order, so the
  // promise is decided one frame at a time; the compressed path decides it in
  // flushBlock from each block's post-sort span instead.
  void noteUncompressedEventOrder(int64_t event_ts_ns);
  void updateSegmentHeader();
  void writeIndex();
  void closeInternal();
  std::filesystem::path generateSegmentPath();

  bool isCompressed() const { return _config.compression != CompressionType::None; }

  WriterConfig _config;
  WriterStats _stats;

  std::FILE* _file{nullptr};
  std::filesystem::path _current_path;
  std::vector<std::byte> _buffer;

  SegmentHeader _segment_header{};
  uint64_t _segment_bytes{0};
  uint32_t _segment_number{0};  // current segment number (1-based after first open)
  bool _header_written{false};

  std::vector<IndexEntry> _index_entries;
  uint32_t _events_since_last_index{0};

  std::vector<std::byte> _block_buffer;
  std::vector<std::byte> _compress_buffer;
  std::vector<std::byte> _sort_buffer;
  uint16_t _block_event_count{0};
  int64_t _block_first_timestamp{0};
  int64_t _last_block_max_ts{0};
  int64_t _last_event_ts{0};
  bool _have_last_event_ts{false};
  // True once this segment has seen a timestamp go backwards, whichever path
  // wrote it. Cleared per segment in ensureOpen; read in closeInternal, where
  // it decides SegmentFlags::Sorted.
  bool _segment_has_inversion{false};

  mutable std::mutex _mutex;

  std::vector<std::byte> _payload_buffer;  // reusable scratch for writeBook/writeTrade

  std::optional<RecordingMetadata> _metadata;
};

}  // namespace lrvx::replay
