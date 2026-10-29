/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/replay/writers/binary_log_writer.h"
#include "lrvx/replay/ops/compression.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace lrvx::replay
{

BinaryLogWriter::BinaryLogWriter(WriterConfig config) : _config(std::move(config))
{
  _buffer.reserve(_config.buffer_size);
  // Reserve for typical max book payload: header + 2 * max_depth * sizeof(BookLevel)
  // Uses buffer_size as a reasonable upper bound for any single event payload
  _payload_buffer.reserve(_config.buffer_size);
  std::filesystem::create_directories(_config.output_dir);

  // Initialize metadata from config if provided
  if (_config.metadata)
  {
    _metadata = *_config.metadata;
  }
}

BinaryLogWriter::~BinaryLogWriter() { close(); }

BinaryLogWriter::BinaryLogWriter(BinaryLogWriter&& other) noexcept
    : _config(std::move(other._config)),
      _stats(other._stats),
      _file(other._file),
      _current_path(std::move(other._current_path)),
      _buffer(std::move(other._buffer)),
      _segment_header(other._segment_header),
      _segment_bytes(other._segment_bytes),
      _header_written(other._header_written),
      _index_entries(std::move(other._index_entries)),
      _events_since_last_index(other._events_since_last_index),
      _block_buffer(std::move(other._block_buffer)),
      _compress_buffer(std::move(other._compress_buffer)),
      _block_event_count(other._block_event_count),
      _block_first_timestamp(other._block_first_timestamp),
      _payload_buffer(std::move(other._payload_buffer))
{
  other._file = nullptr;
}

BinaryLogWriter& BinaryLogWriter::operator=(BinaryLogWriter&& other) noexcept
{
  if (this != &other)
  {
    close();
    _config = std::move(other._config);
    _stats = other._stats;
    _file = other._file;
    _current_path = std::move(other._current_path);
    _buffer = std::move(other._buffer);
    _segment_header = other._segment_header;
    _segment_bytes = other._segment_bytes;
    _header_written = other._header_written;
    _index_entries = std::move(other._index_entries);
    _events_since_last_index = other._events_since_last_index;
    _block_buffer = std::move(other._block_buffer);
    _compress_buffer = std::move(other._compress_buffer);
    _block_event_count = other._block_event_count;
    _block_first_timestamp = other._block_first_timestamp;
    _payload_buffer = std::move(other._payload_buffer);
    other._file = nullptr;
  }
  return *this;
}

std::filesystem::path BinaryLogWriter::generateSegmentPath()
{
  ++_segment_number;

  // First segment: use output_filename if provided
  if (_segment_number == 1 && !_config.output_filename.empty())
  {
    return _config.output_dir / _config.output_filename;
  }

  // Use rotation callback if provided
  if (_config.rotation_callback)
  {
    return _config.rotation_callback(_config.rotation_user_data, _config.output_dir,
                                     _segment_number);
  }

  // Rotating away from a requested name: keep the name and add a suffix.
  // Wall-clock names would sort before the requested one ('0' < most letters)
  // while holding later events, and readers that order a directory by
  // filename would then binary-search a vector that is not in time order.
  // '_' sorts after '.', so "<stem>_0002.lrvx" lands right behind
  // "<stem>.lrvx" and ahead of the next day's segment.
  if (!_config.output_filename.empty())
  {
    const std::filesystem::path first(_config.output_filename);
    std::string ext = first.extension().string();
    if (ext.empty())
    {
      ext = ".lrvx";
    }
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), "_%04u", _segment_number);
    return _config.output_dir / (first.stem().string() + suffix + ext);
  }

  // Default: timestamp-based naming
  using namespace std::chrono;
  auto now = system_clock::now();
  auto ns = duration_cast<nanoseconds>(now.time_since_epoch()).count();
  return _config.output_dir / (std::to_string(ns) + ".lrvx");
}

bool BinaryLogWriter::ensureOpen()
{
  if (_file)
  {
    return true;
  }

  _current_path = generateSegmentPath();
  _file = std::fopen(_current_path.string().c_str(), "wb");
  if (!_file)
  {
    return false;
  }
  if (_config.stdio_buffer_size > 0)
  {
    std::setvbuf(_file, nullptr, _IOFBF, _config.stdio_buffer_size);
  }

  // Initialize segment header
  _segment_header = SegmentHeader{};
  _segment_header.exchange_id = _config.exchange_id;

  auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
  _segment_header.created_ns = now_ns;

  // Set compression info in header
  if (isCompressed())
  {
    _segment_header.flags |= SegmentFlags::Compressed;
    _segment_header.compression = static_cast<uint8_t>(_config.compression);
  }

  // Write placeholder header (will be updated on close)
  if (std::fwrite(&_segment_header, sizeof(_segment_header), 1, _file) != 1)
  {
    std::fclose(_file);
    _file = nullptr;
    return false;
  }

  _segment_bytes = sizeof(SegmentHeader);
  _header_written = true;
  ++_stats.segments_created;

  // Reset index state for new segment
  _index_entries.clear();
  _events_since_last_index = 0;

  // Reset block state for new segment
  _block_buffer.clear();
  _block_event_count = 0;
  _block_first_timestamp = 0;
  _last_block_max_ts = 0;
  _last_event_ts = 0;
  _have_last_event_ts = false;
  _segment_has_inversion = false;

  return true;
}

bool BinaryLogWriter::maybeRotate(size_t needed_bytes)
{
  if (!_file)
  {
    return true;
  }

  if (_segment_bytes + needed_bytes > _config.max_segment_bytes)
  {
    closeInternal();
    return ensureOpen();
  }
  return true;
}

bool BinaryLogWriter::writeFrame(EventType type, const void* payload, size_t size)
{
  std::lock_guard lock(_mutex);

  const size_t frame_size = sizeof(FrameHeader) + size;
  if (!maybeRotate(frame_size))
  {
    return false;
  }
  if (!ensureOpen())
  {
    return false;
  }

  // Record position before writing for index
  uint64_t frame_offset = _segment_bytes;

  FrameHeader header{};
  header.size = static_cast<uint32_t>(size);
  header.crc32 = Crc32::compute(payload, size);
  header.type = static_cast<uint8_t>(type);
  header.rec_version = 1;

  if (std::fwrite(&header, sizeof(header), 1, _file) != 1)
  {
    return false;
  }
  if (std::fwrite(payload, 1, size, _file) != size)
  {
    return false;
  }

  _segment_bytes += frame_size;
  _stats.bytes_written += frame_size;
  ++_stats.events_written;

  // Update segment header timestamps
  int64_t event_ts = 0;
  if (type == EventType::Trade)
  {
    event_ts = reinterpret_cast<const TradeRecord*>(payload)->exchange_ts_ns;
  }
  else
  {
    event_ts = reinterpret_cast<const BookRecordHeader*>(payload)->exchange_ts_ns;
  }

  if (_segment_header.first_event_ns == 0)
  {
    _segment_header.first_event_ns = event_ts;
  }
  _segment_header.last_event_ns = event_ts;
  ++_segment_header.event_count;
  noteUncompressedEventOrder(event_ts);

  // Build index: add entry at interval or for first event
  if (_config.create_index)
  {
    if (_index_entries.empty() || _events_since_last_index >= _config.index_interval)
    {
      _index_entries.push_back(IndexEntry{.timestamp_ns = event_ts, .file_offset = frame_offset});
      _events_since_last_index = 0;
    }
    ++_events_since_last_index;
  }

  return true;
}

bool BinaryLogWriter::writeFrameToBlock(EventType type, const void* payload, size_t size,
                                        int64_t timestamp)
{
  // Build frame in block buffer (without writing to file yet)
  FrameHeader header{};
  header.size = static_cast<uint32_t>(size);
  header.crc32 = Crc32::compute(payload, size);
  header.type = static_cast<uint8_t>(type);
  header.rec_version = 1;

  // Reserve space and append frame header + payload
  size_t old_size = _block_buffer.size();
  _block_buffer.resize(old_size + sizeof(FrameHeader) + size);
  std::memcpy(_block_buffer.data() + old_size, &header, sizeof(header));
  std::memcpy(_block_buffer.data() + old_size + sizeof(header), payload, size);

  // Track the first timestamp in the block; the index entry and the
  // cross-block inversion check both use the block's post-sort minimum, which
  // is only known in flushBlock.
  if (_block_event_count == 0)
  {
    _block_first_timestamp = timestamp;
  }
  ++_block_event_count;

  // Update segment header timestamps
  if (_segment_header.first_event_ns == 0)
  {
    _segment_header.first_event_ns = timestamp;
  }
  _segment_header.last_event_ns = timestamp;
  ++_segment_header.event_count;
  ++_stats.events_written;

  // Flush block when it reaches index_interval events
  if (_block_event_count >= _config.index_interval)
  {
    if (!flushBlock())
    {
      return false;
    }

    // A compressed segment can only end on a block boundary: the block is the
    // unit the reader decompresses, so a file cut mid-block is unreadable. The
    // bound is measured on _segment_bytes, which counts what actually reaches
    // the file (compressed), because that is the number max_segment_bytes
    // exists to cap -- partial reads, rsync-as-you-record and per-segment
    // recovery all see the file, and none of them can know the uncompressed
    // size. The overshoot is therefore at most one block plus the index the
    // closing segment appends.
    if (_segment_bytes >= _config.max_segment_bytes)
    {
      closeInternal();
      return ensureOpen();
    }
  }

  return true;
}

bool BinaryLogWriter::flushBlock()
{
  if (_block_buffer.empty() || _block_event_count == 0)
  {
    return true;
  }
  if (!_file)
  {
    return false;
  }

  // Record position before writing block (for index)
  uint64_t block_offset = _segment_bytes;

  // Sorting rewrites the block, so the block's real first timestamp is its
  // post-sort minimum -- not the first event that happened to arrive. Comparing
  // the arrival order against the previous block's maximum missed every
  // inversion that was not the first event of the block, and the segment then
  // shipped with the Sorted flag on. Readers trust that flag enough to switch
  // off the sort, the reorder buffer and the window check all at once.
  BlockSpan span{_block_first_timestamp, _block_first_timestamp};
  if (_block_event_count > 1)
  {
    span = sortBlockBuffer();
  }
  if (_last_block_max_ts > 0 && span.min_ts < _last_block_max_ts)
  {
    _segment_has_inversion = true;
  }
  _last_block_max_ts = span.max_ts;
  const int64_t block_index_ts = span.min_ts;

  // Compress the block
  size_t max_compressed = Compressor::maxCompressedSize(_config.compression, _block_buffer.size());
  if (max_compressed == 0)
  {
    return false;  // Compression not available
  }

  _compress_buffer.resize(max_compressed);
  size_t compressed_size =
      Compressor::compress(_config.compression, _block_buffer.data(), _block_buffer.size(),
                           _compress_buffer.data(), max_compressed);

  if (compressed_size == 0)
  {
    return false;  // Compression failed
  }

  // Build and write block header
  CompressedBlockHeader block_header{};
  block_header.compressed_size = static_cast<uint32_t>(compressed_size);
  block_header.original_size = static_cast<uint32_t>(_block_buffer.size());
  block_header.event_count = _block_event_count;

  if (std::fwrite(&block_header, sizeof(block_header), 1, _file) != 1)
  {
    return false;
  }
  if (std::fwrite(_compress_buffer.data(), 1, compressed_size, _file) != compressed_size)
  {
    return false;
  }

  // Flush stdio buffer to kernel so the complete block is visible to external
  // readers (e.g. rsync) even if the process is killed before close().
  std::fflush(_file);

  size_t total_written = sizeof(CompressedBlockHeader) + compressed_size;
  _segment_bytes += total_written;
  _stats.bytes_written += total_written;
  _stats.uncompressed_bytes += _block_buffer.size();
  _stats.compressed_bytes += compressed_size;
  ++_stats.blocks_written;

  // Add index entry for this block (points to block header, not individual frames)
  if (_config.create_index)
  {
    _index_entries.push_back(
        IndexEntry{.timestamp_ns = block_index_ts, .file_offset = block_offset});
  }

  // Keep segment header current so readers see valid metadata even if the
  // process is killed before closeInternal() is reached.
  updateSegmentHeader();

  // Reset block state
  _block_buffer.clear();
  _block_event_count = 0;
  _block_first_timestamp = 0;

  return true;
}

bool BinaryLogWriter::writeTrade(const TradeRecord& trade)
{
  std::lock_guard lock(_mutex);

  if (!ensureOpen())
  {
    return false;
  }

  bool ok;
  if (isCompressed())
  {
    ok = writeFrameToBlock(EventType::Trade, &trade, sizeof(trade), trade.exchange_ts_ns);
  }
  else
  {
    // Uncompressed path - use existing writeFrame (but unlock first since writeFrame locks)
    // Need to refactor: extract unlocked version
    const size_t frame_size = sizeof(FrameHeader) + sizeof(trade);
    if (!maybeRotate(frame_size))
    {
      return false;
    }

    uint64_t frame_offset = _segment_bytes;

    FrameHeader header{};
    header.size = static_cast<uint32_t>(sizeof(trade));
    header.crc32 = Crc32::compute(&trade, sizeof(trade));
    header.type = static_cast<uint8_t>(EventType::Trade);
    header.rec_version = 1;

    if (std::fwrite(&header, sizeof(header), 1, _file) != 1)
    {
      return false;
    }
    if (std::fwrite(&trade, sizeof(trade), 1, _file) != 1)
    {
      return false;
    }

    _segment_bytes += frame_size;
    _stats.bytes_written += frame_size;
    ++_stats.events_written;

    if (_segment_header.first_event_ns == 0)
    {
      _segment_header.first_event_ns = trade.exchange_ts_ns;
    }
    _segment_header.last_event_ns = trade.exchange_ts_ns;
    ++_segment_header.event_count;
    noteUncompressedEventOrder(trade.exchange_ts_ns);

    if (_config.create_index)
    {
      if (_index_entries.empty() || _events_since_last_index >= _config.index_interval)
      {
        _index_entries.push_back(
            IndexEntry{.timestamp_ns = trade.exchange_ts_ns, .file_offset = frame_offset});
        _events_since_last_index = 0;
      }
      ++_events_since_last_index;
    }
    ok = true;
  }

  if (ok)
  {
    ++_stats.trades_written;
  }
  return ok;
}

bool BinaryLogWriter::writeOptionQuote(const OptionQuoteRecord& quote)
{
  std::lock_guard lock(_mutex);

  if (!ensureOpen())
  {
    return false;
  }

  bool ok;
  if (isCompressed())
  {
    ok = writeFrameToBlock(EventType::OptionQuote, &quote, sizeof(quote), quote.exchange_ts_ns);
  }
  else
  {
    const size_t frame_size = sizeof(FrameHeader) + sizeof(quote);
    if (!maybeRotate(frame_size))
    {
      return false;
    }

    uint64_t frame_offset = _segment_bytes;

    FrameHeader header{};
    header.size = static_cast<uint32_t>(sizeof(quote));
    header.crc32 = Crc32::compute(&quote, sizeof(quote));
    header.type = static_cast<uint8_t>(EventType::OptionQuote);
    header.rec_version = 1;

    if (std::fwrite(&header, sizeof(header), 1, _file) != 1)
    {
      return false;
    }
    if (std::fwrite(&quote, sizeof(quote), 1, _file) != 1)
    {
      return false;
    }

    _segment_bytes += frame_size;
    _stats.bytes_written += frame_size;
    ++_stats.events_written;

    if (_segment_header.first_event_ns == 0)
    {
      _segment_header.first_event_ns = quote.exchange_ts_ns;
    }
    _segment_header.last_event_ns = quote.exchange_ts_ns;
    ++_segment_header.event_count;
    noteUncompressedEventOrder(quote.exchange_ts_ns);

    if (_config.create_index)
    {
      if (_index_entries.empty() || _events_since_last_index >= _config.index_interval)
      {
        _index_entries.push_back(
            IndexEntry{.timestamp_ns = quote.exchange_ts_ns, .file_offset = frame_offset});
        _events_since_last_index = 0;
      }
      ++_events_since_last_index;
    }
    ok = true;
  }

  if (ok)
  {
    ++_stats.option_quotes_written;
  }
  return ok;
}

bool BinaryLogWriter::writePoolState(const PoolStateRecordHeader& hdr, const void* payload,
                                     size_t payload_size)
{
  std::lock_guard lock(_mutex);

  if (!ensureOpen())
  {
    return false;
  }

  // The frame is [PoolStateRecordHeader][payload]; the header's payload_len must
  // match so the reader can carve the payload back out.
  PoolStateRecordHeader record = hdr;
  record.payload_len = static_cast<uint32_t>(payload_size);

  const size_t frame_payload_size = sizeof(PoolStateRecordHeader) + payload_size;
  _payload_buffer.resize(frame_payload_size);
  std::memcpy(_payload_buffer.data(), &record, sizeof(record));
  if (payload_size > 0)
  {
    std::memcpy(_payload_buffer.data() + sizeof(record), payload, payload_size);
  }
  auto* frame_payload = _payload_buffer.data();

  bool ok;
  if (isCompressed())
  {
    ok = writeFrameToBlock(EventType::PoolState, frame_payload, frame_payload_size,
                           record.exchange_ts_ns);
  }
  else
  {
    const size_t frame_size = sizeof(FrameHeader) + frame_payload_size;
    if (!maybeRotate(frame_size))
    {
      return false;
    }

    uint64_t frame_offset = _segment_bytes;

    FrameHeader frame_hdr{};
    frame_hdr.size = static_cast<uint32_t>(frame_payload_size);
    frame_hdr.crc32 = Crc32::compute(frame_payload, frame_payload_size);
    frame_hdr.type = static_cast<uint8_t>(EventType::PoolState);
    frame_hdr.rec_version = 1;

    if (std::fwrite(&frame_hdr, sizeof(frame_hdr), 1, _file) != 1)
    {
      return false;
    }
    if (std::fwrite(frame_payload, 1, frame_payload_size, _file) != frame_payload_size)
    {
      return false;
    }

    _segment_bytes += frame_size;
    _stats.bytes_written += frame_size;
    ++_stats.events_written;

    if (_segment_header.first_event_ns == 0)
    {
      _segment_header.first_event_ns = record.exchange_ts_ns;
    }
    _segment_header.last_event_ns = record.exchange_ts_ns;
    ++_segment_header.event_count;
    noteUncompressedEventOrder(record.exchange_ts_ns);

    if (_config.create_index)
    {
      if (_index_entries.empty() || _events_since_last_index >= _config.index_interval)
      {
        _index_entries.push_back(
            IndexEntry{.timestamp_ns = record.exchange_ts_ns, .file_offset = frame_offset});
        _events_since_last_index = 0;
      }
      ++_events_since_last_index;
    }
    ok = true;
  }

  return ok;
}

bool BinaryLogWriter::writeBook(const BookRecordHeader& hdr, std::span<const BookLevel> bids,
                                std::span<const BookLevel> asks)
{
  std::lock_guard lock(_mutex);

  if (!ensureOpen())
  {
    return false;
  }

  // Build the complete payload (reuse pre-allocated buffer)
  const size_t payload_size = sizeof(BookRecordHeader) + bids.size_bytes() + asks.size_bytes();

  _payload_buffer.resize(payload_size);
  std::memcpy(_payload_buffer.data(), &hdr, sizeof(hdr));
  if (!bids.empty())
  {
    std::memcpy(_payload_buffer.data() + sizeof(hdr), bids.data(), bids.size_bytes());
  }
  if (!asks.empty())
  {
    std::memcpy(_payload_buffer.data() + sizeof(hdr) + bids.size_bytes(), asks.data(), asks.size_bytes());
  }
  auto* payload = _payload_buffer.data();

  EventType type = (hdr.type == 0)   ? EventType::BookSnapshot
                   : (hdr.type == 2) ? EventType::BookTop
                                     : EventType::BookDelta;
  if (hdr.type == 2)
  {
    // Inline, not setHasBookTops(): writeBook already holds _mutex and the
    // setter takes it again.
    if (!_metadata)
    {
      _metadata = RecordingMetadata{};
    }
    _metadata->has_book_tops = true;
  }

  bool ok;
  if (isCompressed())
  {
    ok = writeFrameToBlock(type, payload, payload_size, hdr.exchange_ts_ns);
  }
  else
  {
    // Uncompressed path
    const size_t frame_size = sizeof(FrameHeader) + payload_size;
    if (!maybeRotate(frame_size))
    {
      return false;
    }

    uint64_t frame_offset = _segment_bytes;

    FrameHeader frame_hdr{};
    frame_hdr.size = static_cast<uint32_t>(payload_size);
    frame_hdr.crc32 = Crc32::compute(payload, payload_size);
    frame_hdr.type = static_cast<uint8_t>(type);
    frame_hdr.rec_version = 1;

    if (std::fwrite(&frame_hdr, sizeof(frame_hdr), 1, _file) != 1)
    {
      return false;
    }
    if (std::fwrite(payload, 1, payload_size, _file) != payload_size)
    {
      return false;
    }

    _segment_bytes += frame_size;
    _stats.bytes_written += frame_size;
    ++_stats.events_written;

    if (_segment_header.first_event_ns == 0)
    {
      _segment_header.first_event_ns = hdr.exchange_ts_ns;
    }
    _segment_header.last_event_ns = hdr.exchange_ts_ns;
    ++_segment_header.event_count;
    noteUncompressedEventOrder(hdr.exchange_ts_ns);

    if (_config.create_index)
    {
      if (_index_entries.empty() || _events_since_last_index >= _config.index_interval)
      {
        _index_entries.push_back(
            IndexEntry{.timestamp_ns = hdr.exchange_ts_ns, .file_offset = frame_offset});
        _events_since_last_index = 0;
      }
      ++_events_since_last_index;
    }
    ok = true;
  }

  if (ok)
  {
    ++_stats.book_updates_written;
  }
  return ok;
}

void BinaryLogWriter::flush()
{
  std::lock_guard lock(_mutex);
  if (_file)
  {
    std::fflush(_file);
  }
}

void BinaryLogWriter::updateSegmentHeader()
{
  if (!_file || !_header_written)
  {
    return;
  }

  // Seek to beginning and rewrite header with updated stats
  long current_pos = std::ftell(_file);
  std::fseek(_file, 0, SEEK_SET);
  std::fwrite(&_segment_header, sizeof(_segment_header), 1, _file);
  std::fseek(_file, current_pos, SEEK_SET);
}

void BinaryLogWriter::writeIndex()
{
  if (!_file || !_config.create_index || _index_entries.empty())
  {
    return;
  }

  // Record where index starts
  uint64_t index_offset = _segment_bytes;

  // Build index header
  SegmentIndexHeader idx_header{};
  idx_header.interval = _config.index_interval;
  idx_header.entry_count = static_cast<uint32_t>(_index_entries.size());
  idx_header.first_ts_ns = _index_entries.front().timestamp_ns;
  idx_header.last_ts_ns = _index_entries.back().timestamp_ns;

  // Compute CRC of index entries
  idx_header.crc32 =
      Crc32::compute(_index_entries.data(), _index_entries.size() * sizeof(IndexEntry));

  // Write index header
  if (std::fwrite(&idx_header, sizeof(idx_header), 1, _file) != 1)
  {
    return;
  }

  // Write index entries
  if (std::fwrite(_index_entries.data(), sizeof(IndexEntry), _index_entries.size(), _file) !=
      _index_entries.size())
  {
    return;
  }

  _segment_bytes += sizeof(SegmentIndexHeader) + _index_entries.size() * sizeof(IndexEntry);

  // Update segment header with index info
  _segment_header.index_offset = index_offset;
  _segment_header.flags |= SegmentFlags::HasIndex;
}

void BinaryLogWriter::closeInternal()
{
  if (_file)
  {
    // Flush any remaining events in the block buffer (for compressed mode)
    if (isCompressed() && _block_event_count > 0)
    {
      flushBlock();
    }

    // The flag is the writer's promise that exchange_ts_ns never goes
    // backwards inside the segment, and an uncompressed segment written in
    // order keeps that promise exactly as a compressed one does. Withholding
    // it there made readers buffer and re-sort every uncompressed segment, and
    // made streamForEach judge each one against a watermark carried from the
    // segment before -- dropping the head of any segment whose range started
    // behind the previous segment's.
    if (!_segment_has_inversion)
    {
      _segment_header.flags |= SegmentFlags::Sorted;
    }

    // Write index before closing (must be done before updateSegmentHeader)
    writeIndex();
    updateSegmentHeader();
    if (_config.sync_on_rotate)
    {
      std::fflush(_file);
    }
    std::fclose(_file);
    _file = nullptr;
    _header_written = false;
    _segment_bytes = 0;
  }
}

void BinaryLogWriter::close()
{
  std::lock_guard lock(_mutex);
  closeInternal();

  // Write metadata.json if metadata is set
  if (_metadata)
  {
    // Update recording_end timestamp
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &time_t_now);
#else
    gmtime_r(&time_t_now, &tm_utc);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_utc);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch())
                  .count() %
              1000;
    char iso_buf[80];
    std::snprintf(iso_buf, sizeof(iso_buf), "%s.%03dZ", buf, static_cast<int>(ms));
    _metadata->recording_end = iso_buf;
    _metadata->total_trades = _stats.trades_written;
    _metadata->total_book_updates = _stats.book_updates_written;

    _metadata->save(RecordingMetadata::metadataPath(_config.output_dir));
  }
}

WriterStats BinaryLogWriter::stats() const
{
  std::lock_guard lock(_mutex);
  return _stats;
}

std::filesystem::path BinaryLogWriter::currentSegmentPath() const
{
  std::lock_guard lock(_mutex);
  return _current_path;
}

void BinaryLogWriter::setMetadata(const RecordingMetadata& meta)
{
  std::lock_guard lock(_mutex);
  _metadata = meta;
}

void BinaryLogWriter::addSymbol(const SymbolInfo& symbol)
{
  std::lock_guard lock(_mutex);
  if (!_metadata)
  {
    _metadata = RecordingMetadata{};
  }
  _metadata->symbols.push_back(symbol);
}

void BinaryLogWriter::setHasTrades(bool v)
{
  std::lock_guard lock(_mutex);
  if (!_metadata)
  {
    _metadata = RecordingMetadata{};
  }
  _metadata->has_trades = v;
}

void BinaryLogWriter::setHasBookSnapshots(bool v)
{
  std::lock_guard lock(_mutex);
  if (!_metadata)
  {
    _metadata = RecordingMetadata{};
  }
  _metadata->has_book_snapshots = v;
}

void BinaryLogWriter::setHasBookTops(bool v)
{
  std::lock_guard lock(_mutex);
  if (!_metadata)
  {
    _metadata = RecordingMetadata{};
  }
  _metadata->has_book_tops = v;
}

void BinaryLogWriter::setHasBookDeltas(bool v)
{
  std::lock_guard lock(_mutex);
  if (!_metadata)
  {
    _metadata = RecordingMetadata{};
  }
  _metadata->has_book_deltas = v;
}

void BinaryLogWriter::noteUncompressedEventOrder(int64_t event_ts_ns)
{
  if (_have_last_event_ts && event_ts_ns < _last_event_ts)
  {
    _segment_has_inversion = true;
  }
  _last_event_ts = event_ts_ns;
  _have_last_event_ts = true;
}

BinaryLogWriter::BlockSpan BinaryLogWriter::sortBlockBuffer()
{
  struct FrameRef
  {
    size_t offset;
    size_t total_size;
    int64_t timestamp;
  };

  std::vector<FrameRef> refs;
  refs.reserve(_block_event_count);

  size_t pos = 0;
  while (pos < _block_buffer.size())
  {
    if (pos + sizeof(FrameHeader) > _block_buffer.size())
    {
      break;
    }

    FrameHeader hdr;
    std::memcpy(&hdr, _block_buffer.data() + pos, sizeof(FrameHeader));

    size_t total = sizeof(FrameHeader) + hdr.size;
    if (pos + total > _block_buffer.size())
    {
      break;
    }

    // Every record type (Trade, Book, OptionQuote, PoolState) has exchange_ts_ns
    // as its first field, so the block timestamp is the first int64 of the payload.
    int64_t ts = 0;
    if (hdr.size >= sizeof(int64_t))
    {
      std::memcpy(&ts, _block_buffer.data() + pos + sizeof(FrameHeader), sizeof(int64_t));
    }

    refs.push_back({pos, total, ts});
    pos += total;
  }

  // Fast path: already sorted (common case when exchange is well-behaved).
  bool sorted = true;
  for (size_t i = 1; i < refs.size(); ++i)
  {
    if (refs[i].timestamp < refs[i - 1].timestamp)
    {
      sorted = false;
      break;
    }
  }
  if (sorted)
  {
    if (refs.empty())
    {
      return BlockSpan{0, 0};
    }
    return BlockSpan{refs.front().timestamp, refs.back().timestamp};
  }

  std::stable_sort(refs.begin(), refs.end(),
                   [](const FrameRef& a, const FrameRef& b)
                   { return a.timestamp < b.timestamp; });

  _sort_buffer.resize(_block_buffer.size());
  size_t out_pos = 0;
  for (const auto& ref : refs)
  {
    std::memcpy(_sort_buffer.data() + out_pos, _block_buffer.data() + ref.offset, ref.total_size);
    out_pos += ref.total_size;
  }

  std::swap(_block_buffer, _sort_buffer);

  return BlockSpan{refs.front().timestamp, refs.back().timestamp};
}

}  // namespace lrvx::replay
