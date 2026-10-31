/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/replay/ops/validator.h"

#include "lrvx/replay/ops/index_builder.h"

#include <algorithm>
#include <cstring>

namespace lrvx::replay
{

namespace
{
// Largest frame payload a sane segment carries; the readers use the same
// ceiling. Anything above it is a corrupt length field.
constexpr uint32_t kMaxValidatorFrameBytes = 10u * 1024u * 1024u;
}  // namespace

SegmentValidator::SegmentValidator(ValidatorConfig config) : _config(std::move(config)) {}

SegmentValidationResult SegmentValidator::validate(const std::filesystem::path& segment_path)
{
  ProgressCallback no_progress = nullptr;
  return validate(segment_path, no_progress);
}

SegmentValidationResult SegmentValidator::validate(const std::filesystem::path& segment_path,
                                                   ProgressCallback progress)
{
  SegmentValidationResult result;
  result.path = segment_path;

  // Check file exists
  if (!std::filesystem::exists(segment_path))
  {
    addIssue(result, IssueType::FileNotFound, IssueSeverity::Critical,
             "File not found: " + segment_path.string());
    result.valid = false;
    return result;
  }

  std::FILE* file = std::fopen(segment_path.string().c_str(), "rb");
  if (!file)
  {
    addIssue(result, IssueType::FileReadError, IssueSeverity::Critical,
             "Cannot open file for reading");
    result.valid = false;
    return result;
  }

  // Get file size
  std::fseek(file, 0, SEEK_END);
  uint64_t file_size = static_cast<uint64_t>(std::ftell(file));
  std::fseek(file, 0, SEEK_SET);

  // Validate header
  if (!validateHeader(file, result))
  {
    std::fclose(file);
    result.valid = false;
    return result;
  }

  // Validate events
  if (_config.scan_all_events)
  {
    bool events_ok;
    if (result.is_compressed)
    {
      events_ok = validateEventsCompressed(file, result, progress);
    }
    else
    {
      events_ok = validateEventsUncompressed(file, result, progress);
    }

    if (!events_ok && _config.stop_on_first_error)
    {
      std::fclose(file);
      result.valid = false;
      return result;
    }
  }

  // Validate index
  if (_config.verify_index && result.has_index)
  {
    validateIndex(file, result);
  }

  std::fclose(file);

  // Check header vs actual data
  if (result.reported_event_count != result.actual_event_count)
  {
    if (result.reported_event_count == 0 && result.actual_event_count > 0)
    {
      // The shape a killed collector leaves: payload on disk, counters never
      // written back. Range reads skip the segment entirely, so reporting this
      // as a warning and then passing the segment is the worst of both.
      addIssue(result, IssueType::FileTruncated, IssueSeverity::Error,
               "Segment header was never finalised: it reports 0 events while " +
                   std::to_string(result.actual_event_count) +
                   " are present. Repair the header before reading by time range.");
    }
    else
    {
      addIssue(result, IssueType::EventCountMismatch, IssueSeverity::Error,
               "Header reports " + std::to_string(result.reported_event_count) +
                   " events, found " + std::to_string(result.actual_event_count));
    }
  }

  // A segment that claims an index it does not carry sends every seek to the
  // wrong place; it is not a valid segment either.
  if (result.has_index && !result.index_valid)
  {
    addIssue(result, IssueType::IndexOutOfBounds, IssueSeverity::Error,
             "Header carries the index flag but no readable index was found");
  }

  // Determine overall validity. hasErrors() has to count here: with only
  // hasCritical() the verdict was true for every corruption short of an
  // unreadable header, which made the function useless as a gate.
  result.valid = !result.hasCritical() && !result.hasErrors();

  return result;
}

bool SegmentValidator::validateHeader(std::FILE* file, SegmentValidationResult& result)
{
  SegmentHeader header;
  if (std::fread(&header, sizeof(header), 1, file) != 1)
  {
    addIssue(result, IssueType::HeaderCorrupted, IssueSeverity::Critical,
             "Cannot read segment header");
    return false;
  }

  // Validate magic
  if (header.magic != kMagic)
  {
    addIssue(result, IssueType::InvalidMagic, IssueSeverity::Critical,
             "Invalid magic number: expected 0x" + std::to_string(kMagic) + ", got 0x" +
                 std::to_string(header.magic));
    return false;
  }

  // Validate version
  if (header.version != kFormatVersion)
  {
    addIssue(result, IssueType::InvalidVersion, IssueSeverity::Critical,
             "Unsupported format version: " + std::to_string(header.version));
    return false;
  }

  result.header_valid = true;
  result.reported_event_count = header.event_count;
  result.reported_first_ts = header.first_event_ns;
  result.reported_last_ts = header.last_event_ns;
  result.is_compressed = header.isCompressed();
  result.compression_type = header.compressionType();
  result.has_index = header.hasIndex();

  return true;
}

bool SegmentValidator::validateEventsUncompressed(std::FILE* file, SegmentValidationResult& result,
                                                  ProgressCallback& progress)
{
  int64_t last_timestamp = 0;
  uint64_t event_index = 0;

  // Get file size for progress reporting
  long start_pos = std::ftell(file);
  std::fseek(file, 0, SEEK_END);
  uint64_t file_size = static_cast<uint64_t>(std::ftell(file));
  std::fseek(file, start_pos, SEEK_SET);

  // Calculate end position (before index if present)
  uint64_t end_pos = file_size;
  if (result.has_index)
  {
    std::fseek(file, 0, SEEK_SET);
    SegmentHeader header;
    std::fread(&header, sizeof(header), 1, file);
    end_pos = header.index_offset;
    std::fseek(file, sizeof(SegmentHeader), SEEK_SET);
  }

  while (true)
  {
    uint64_t current_pos = static_cast<uint64_t>(std::ftell(file));

    // Check if we've reached the index
    if (current_pos >= end_pos)
    {
      break;
    }

    // Report progress
    if (progress)
    {
      progress(current_pos, file_size);
    }

    // Read frame header
    FrameHeader frame;
    if (std::fread(&frame, sizeof(frame), 1, file) != 1)
    {
      if (std::feof(file))
      {
        break;  // Normal EOF
      }
      addIssue(result, IssueType::FrameTruncated, IssueSeverity::Error,
               "Truncated frame header at offset " + std::to_string(current_pos), current_pos);
      return false;
    }

    // Sanity check frame size
    if (frame.size > kMaxValidatorFrameBytes)
    {
      addIssue(result, IssueType::FrameSizeTooLarge, IssueSeverity::Error,
               "Frame size too large: " + std::to_string(frame.size) + " bytes", current_pos);
      return false;
    }

    // Read payload
    std::vector<std::byte> payload(frame.size);
    if (std::fread(payload.data(), 1, frame.size, file) != frame.size)
    {
      addIssue(result, IssueType::FrameTruncated, IssueSeverity::Error,
               "Truncated frame payload at offset " + std::to_string(current_pos), current_pos);
      return false;
    }

    // Verify CRC
    if (_config.verify_crc)
    {
      uint32_t computed_crc = Crc32::compute(payload.data(), payload.size());
      if (computed_crc != frame.crc32)
      {
        addIssue(result, IssueType::FrameCrcMismatch, IssueSeverity::Error,
                 "CRC mismatch at event " + std::to_string(event_index), current_pos);
        ++result.crc_errors;

        if (_config.stop_on_first_error)
        {
          return false;
        }
      }
    }

    // Parse event to check timestamp
    int64_t event_ts = 0;
    EventType type = static_cast<EventType>(frame.type);

    if (type == EventType::Trade)
    {
      if (frame.size >= sizeof(TradeRecord))
      {
        TradeRecord trade;
        std::memcpy(&trade, payload.data(), sizeof(trade));
        event_ts = trade.exchange_ts_ns;
        ++result.trades_found;
      }
    }
    else if (type == EventType::BookSnapshot || type == EventType::BookDelta ||
             type == EventType::BookTop)
    {
      if (frame.size >= sizeof(BookRecordHeader))
      {
        BookRecordHeader book;
        std::memcpy(&book, payload.data(), sizeof(book));
        event_ts = book.exchange_ts_ns;
        ++result.book_updates_found;
      }
    }
    else if (type == EventType::OptionQuote)
    {
      if (frame.size >= sizeof(OptionQuoteRecord))
      {
        OptionQuoteRecord quote;
        std::memcpy(&quote, payload.data(), sizeof(quote));
        event_ts = quote.exchange_ts_ns;
      }
    }
    else if (type == EventType::PoolState)
    {
      if (frame.size >= sizeof(PoolStateRecordHeader))
      {
        PoolStateRecordHeader pool;
        std::memcpy(&pool, payload.data(), sizeof(pool));
        event_ts = pool.exchange_ts_ns;
      }
    }
    else
    {
      addIssue(result, IssueType::FrameTypeUnknown, IssueSeverity::Warning,
               "Unknown frame type: " + std::to_string(frame.type), current_pos);
    }

    // Check timestamp ordering
    if (_config.verify_timestamps && event_ts > 0)
    {
      if (result.actual_event_count == 0)
      {
        result.actual_first_ts = event_ts;
      }
      result.actual_last_ts = event_ts;

      if (last_timestamp > 0)
      {
        if (event_ts < last_timestamp)
        {
          addIssue(result, IssueType::TimestampOutOfOrder, IssueSeverity::Warning,
                   "Timestamp out of order at event " + std::to_string(event_index), current_pos);
          ++result.timestamp_anomalies;
        }
        else if (event_ts - last_timestamp > _config.max_timestamp_jump_ns)
        {
          addIssue(result, IssueType::TimestampJumpTooLarge, IssueSeverity::Warning,
                   "Large timestamp jump at event " + std::to_string(event_index), current_pos);
          ++result.timestamp_anomalies;
        }
      }
      last_timestamp = event_ts;
    }

    ++result.actual_event_count;
    ++event_index;
    result.bytes_scanned = current_pos + sizeof(FrameHeader) + frame.size - sizeof(SegmentHeader);
  }

  return true;
}

bool SegmentValidator::validateEventsCompressed(std::FILE* file, SegmentValidationResult& result,
                                                ProgressCallback& progress)
{
  int64_t last_timestamp = 0;

  // Get file size
  long start_pos = std::ftell(file);
  std::fseek(file, 0, SEEK_END);
  uint64_t file_size = static_cast<uint64_t>(std::ftell(file));
  std::fseek(file, start_pos, SEEK_SET);

  // Calculate end position
  uint64_t end_pos = file_size;
  if (result.has_index)
  {
    std::fseek(file, 0, SEEK_SET);
    SegmentHeader header;
    std::fread(&header, sizeof(header), 1, file);
    end_pos = header.index_offset;
    std::fseek(file, sizeof(SegmentHeader), SEEK_SET);
  }

  std::vector<std::byte> compressed_buffer;
  std::vector<std::byte> decompressed_buffer;

  while (true)
  {
    uint64_t current_pos = static_cast<uint64_t>(std::ftell(file));

    if (current_pos >= end_pos)
    {
      break;
    }

    if (progress)
    {
      progress(current_pos, file_size);
    }

    // Read block header
    CompressedBlockHeader block_header;
    if (std::fread(&block_header, sizeof(block_header), 1, file) != 1)
    {
      if (std::feof(file))
      {
        break;
      }
      addIssue(result, IssueType::FrameTruncated, IssueSeverity::Error,
               "Truncated block header at offset " + std::to_string(current_pos), current_pos);
      return false;
    }

    // Validate block magic
    if (!block_header.isValid())
    {
      addIssue(result, IssueType::BlockMagicInvalid, IssueSeverity::Error,
               "Invalid block magic at offset " + std::to_string(current_pos), current_pos);
      return false;
    }

    // Bounds-check `compressed_size` against the bytes remaining in the
    // segment region. Without this, validating an actively-written tape
    // whose tail block has a header on disk but not its payload would
    // resize the buffer to whatever stale value the unflushed header
    // bytes carried — potentially gigabytes — before fread short-reads.
    const uint64_t payload_pos = current_pos + sizeof(block_header);
    if (payload_pos > end_pos ||
        block_header.compressed_size > end_pos - payload_pos)
    {
      addIssue(result, IssueType::FrameTruncated, IssueSeverity::Error,
               "Truncated compressed block at offset " + std::to_string(current_pos), current_pos);
      return false;
    }

    // Read compressed data
    compressed_buffer.resize(block_header.compressed_size);
    if (std::fread(compressed_buffer.data(), 1, block_header.compressed_size, file) !=
        block_header.compressed_size)
    {
      addIssue(result, IssueType::FrameTruncated, IssueSeverity::Error,
               "Truncated compressed block at offset " + std::to_string(current_pos), current_pos);
      return false;
    }

    // Decompress
    decompressed_buffer.resize(block_header.original_size);
    size_t decompressed_size =
        Compressor::decompress(result.compression_type, compressed_buffer.data(),
                               block_header.compressed_size, decompressed_buffer.data(),
                               block_header.original_size);

    if (decompressed_size != block_header.original_size)
    {
      addIssue(result, IssueType::BlockDecompressionFailed, IssueSeverity::Error,
               "Decompression failed at offset " + std::to_string(current_pos), current_pos);
      if (_config.stop_on_first_error)
      {
        return false;
      }
      continue;
    }

    // Parse events from decompressed block
    size_t offset = 0;
    uint16_t events_in_block = 0;

    while (offset + sizeof(FrameHeader) <= decompressed_buffer.size() &&
           events_in_block < block_header.event_count)
    {
      FrameHeader frame;
      std::memcpy(&frame, decompressed_buffer.data() + offset, sizeof(frame));
      offset += sizeof(FrameHeader);

      if (offset + frame.size > decompressed_buffer.size())
      {
        addIssue(result, IssueType::FrameTruncated, IssueSeverity::Error,
                 "Truncated frame in block at offset " + std::to_string(current_pos), current_pos);
        break;
      }

      // Verify CRC
      if (_config.verify_crc)
      {
        uint32_t computed_crc = Crc32::compute(decompressed_buffer.data() + offset, frame.size);
        if (computed_crc != frame.crc32)
        {
          addIssue(result, IssueType::FrameCrcMismatch, IssueSeverity::Error,
                   "CRC mismatch in block at offset " + std::to_string(current_pos), current_pos);
          ++result.crc_errors;
        }
      }

      // Parse event for timestamp
      int64_t event_ts = 0;
      EventType type = static_cast<EventType>(frame.type);

      if (type == EventType::Trade && frame.size >= sizeof(TradeRecord))
      {
        TradeRecord trade;
        std::memcpy(&trade, decompressed_buffer.data() + offset, sizeof(trade));
        event_ts = trade.exchange_ts_ns;
        ++result.trades_found;
      }
      else if ((type == EventType::BookSnapshot || type == EventType::BookDelta ||
                type == EventType::BookTop) &&
               frame.size >= sizeof(BookRecordHeader))
      {
        BookRecordHeader book;
        std::memcpy(&book, decompressed_buffer.data() + offset, sizeof(book));
        event_ts = book.exchange_ts_ns;
        ++result.book_updates_found;
      }
      else if (type == EventType::OptionQuote && frame.size >= sizeof(OptionQuoteRecord))
      {
        OptionQuoteRecord quote;
        std::memcpy(&quote, decompressed_buffer.data() + offset, sizeof(quote));
        event_ts = quote.exchange_ts_ns;
      }
      else if (type == EventType::PoolState && frame.size >= sizeof(PoolStateRecordHeader))
      {
        PoolStateRecordHeader pool;
        std::memcpy(&pool, decompressed_buffer.data() + offset, sizeof(pool));
        event_ts = pool.exchange_ts_ns;
      }

      // Check timestamps
      if (_config.verify_timestamps && event_ts > 0)
      {
        if (result.actual_event_count == 0)
        {
          result.actual_first_ts = event_ts;
        }
        result.actual_last_ts = event_ts;

        if (last_timestamp > 0 && event_ts < last_timestamp)
        {
          ++result.timestamp_anomalies;
        }
        last_timestamp = event_ts;
      }

      offset += frame.size;
      ++result.actual_event_count;
      ++events_in_block;
    }

    result.bytes_scanned = current_pos + sizeof(CompressedBlockHeader) +
                           block_header.compressed_size - sizeof(SegmentHeader);
  }

  return true;
}

bool SegmentValidator::validateIndex(std::FILE* file, SegmentValidationResult& result)
{
  // Seek to index position
  std::fseek(file, 0, SEEK_SET);
  SegmentHeader header;
  if (std::fread(&header, sizeof(header), 1, file) != 1)
  {
    return false;
  }

  if (!header.hasIndex())
  {
    return true;
  }

  std::fseek(file, static_cast<long>(header.index_offset), SEEK_SET);

  // Read index header
  SegmentIndexHeader idx_header;
  if (std::fread(&idx_header, sizeof(idx_header), 1, file) != 1)
  {
    addIssue(result, IssueType::IndexOutOfBounds, IssueSeverity::Warning,
             "Cannot read index header");
    return false;
  }

  if (!idx_header.isValid())
  {
    addIssue(result, IssueType::IndexMagicInvalid, IssueSeverity::Warning, "Invalid index magic");
    return false;
  }

  result.index_entry_count = idx_header.entry_count;

  // Read and verify index entries
  std::vector<IndexEntry> entries(idx_header.entry_count);
  if (std::fread(entries.data(), sizeof(IndexEntry), idx_header.entry_count, file) !=
      idx_header.entry_count)
  {
    addIssue(result, IssueType::IndexOutOfBounds, IssueSeverity::Warning,
             "Truncated index entries");
    return false;
  }

  // Verify CRC
  uint32_t computed_crc = Crc32::compute(entries.data(), entries.size() * sizeof(IndexEntry));
  if (computed_crc != idx_header.crc32)
  {
    addIssue(result, IssueType::IndexCrcMismatch, IssueSeverity::Warning, "Index CRC mismatch");
    return false;
  }

  // Check entries are sorted
  for (size_t i = 1; i < entries.size(); ++i)
  {
    if (entries[i].timestamp_ns < entries[i - 1].timestamp_ns)
    {
      addIssue(result, IssueType::IndexNotSorted, IssueSeverity::Warning,
               "Index entries not sorted at position " + std::to_string(i));
      break;
    }
  }

  result.index_valid = true;
  return true;
}

void SegmentValidator::addIssue(SegmentValidationResult& result, IssueType type,
                                IssueSeverity severity, const std::string& message,
                                uint64_t offset)
{
  result.issues.push_back(ValidationIssue{
      .type = type, .severity = severity, .message = message, .file_offset = offset});

  if (severity == IssueSeverity::Critical || severity == IssueSeverity::Error)
  {
    result.valid = false;
  }
}

namespace
{

// Dataset-level check. Two segments whose [first_event_ns, last_event_ns]
// ranges intersect mean the same wall-clock window was recorded twice, and no
// reader can make that whole again: segments are walked in filename order, so
// the overlap replays as duplicated events, as events out of order, or -- on
// the streaming path, through the reorder buffer's late drop -- as events that
// silently disappear. Reporting it is the only honest answer; each segment on
// its own is perfectly well formed, which is why the per-segment pass sees
// nothing.
//
// The comparison is strict. Rotation cuts a segment between frames, so two
// events that share a timestamp can land either side of the seam and leave
// a.last == b.first; that is an ordinary rotation, not a double recording, and
// it replays in order.
void checkSegmentRangeOverlap(DatasetValidationResult& result)
{
  struct Range
  {
    size_t index{0};
    int64_t first{0};
    int64_t last{0};
  };

  std::vector<Range> ranges;
  ranges.reserve(result.segments.size());
  for (size_t i = 0; i < result.segments.size(); ++i)
  {
    const auto& seg = result.segments[i];
    if (seg.actual_event_count == 0)
    {
      continue;
    }
    ranges.push_back(Range{i, seg.actual_first_ts, seg.actual_last_ts});
  }

  std::sort(ranges.begin(), ranges.end(),
            [](const Range& a, const Range& b)
            {
              if (a.first != b.first)
              {
                return a.first < b.first;
              }
              return a.last < b.last;
            });

  if (ranges.size() < 2)
  {
    return;
  }

  // Compare each segment against the one reaching furthest in time so far, not
  // merely against its predecessor: a segment fully contained in an earlier,
  // wider one overlaps it even though the segment just before it ended sooner.
  Range widest = ranges.front();
  for (size_t k = 1; k < ranges.size(); ++k)
  {
    const Range& r = ranges[k];
    if (r.first < widest.last)
    {
      const auto& a = result.segments[widest.index];
      const auto& b = result.segments[r.index];
      result.issues.push_back(ValidationIssue{
          .type = IssueType::SegmentRangeOverlap,
          .severity = IssueSeverity::Error,
          .message = "Segments cover overlapping time ranges: " +
                     a.path.filename().string() + " [" + std::to_string(widest.first) + ", " +
                     std::to_string(widest.last) + "] overlaps " + b.path.filename().string() +
                     " [" + std::to_string(r.first) + ", " + std::to_string(r.last) +
                     "]. The same window was recorded twice; replaying the dataset "
                     "duplicates, reorders or drops events from it.",
          .file_offset = 0,
          .event_index = 0,
          .timestamp_ns = r.first});
    }
    if (r.last > widest.last)
    {
      widest = r;
    }
  }
}

}  // namespace

DatasetValidator::DatasetValidator(ValidatorConfig config) : _config(std::move(config)) {}

DatasetValidationResult DatasetValidator::validate(const std::filesystem::path& data_dir)
{
  ProgressCallback no_progress = nullptr;
  return validate(data_dir, no_progress);
}

DatasetValidationResult DatasetValidator::validate(const std::filesystem::path& data_dir,
                                                   ProgressCallback progress)
{
  DatasetValidationResult result;
  result.data_dir = data_dir;

  if (!std::filesystem::exists(data_dir))
  {
    result.valid = false;
    return result;
  }

  // Collect segment files
  std::vector<std::filesystem::path> segment_paths;
  for (const auto& entry : std::filesystem::directory_iterator(data_dir))
  {
    if (entry.is_regular_file() && isSegmentExtension(entry.path().extension().string()))
    {
      segment_paths.push_back(entry.path());
    }
  }

  std::sort(segment_paths.begin(), segment_paths.end());
  result.total_segments = static_cast<uint32_t>(segment_paths.size());

  // Validate each segment
  SegmentValidator segment_validator(_config);

  for (uint32_t i = 0; i < segment_paths.size(); ++i)
  {
    if (progress)
    {
      progress(i, result.total_segments, segment_paths[i]);
    }

    auto segment_result = segment_validator.validate(segment_paths[i]);
    result.segments.push_back(std::move(segment_result));

    auto& seg = result.segments.back();

    // Update aggregates
    if (seg.valid && !seg.hasErrors())
    {
      ++result.valid_segments;
    }
    else
    {
      ++result.corrupted_segments;
    }

    result.total_events += seg.actual_event_count;

    std::error_code ec;
    auto file_size = std::filesystem::file_size(segment_paths[i], ec);
    if (!ec)
    {
      result.total_bytes += file_size;
    }

    // Update time range
    if (seg.actual_first_ts > 0)
    {
      if (result.first_timestamp == 0 || seg.actual_first_ts < result.first_timestamp)
      {
        result.first_timestamp = seg.actual_first_ts;
      }
    }
    if (seg.actual_last_ts > 0)
    {
      if (seg.actual_last_ts > result.last_timestamp)
      {
        result.last_timestamp = seg.actual_last_ts;
      }
    }

    // Count issues
    for (const auto& issue : seg.issues)
    {
      if (issue.severity == IssueSeverity::Error || issue.severity == IssueSeverity::Critical)
      {
        ++result.total_errors;
      }
      else if (issue.severity == IssueSeverity::Warning)
      {
        ++result.total_warnings;
      }
    }
  }

  checkSegmentRangeOverlap(result);

  for (const auto& issue : result.issues)
  {
    if (issue.severity == IssueSeverity::Error || issue.severity == IssueSeverity::Critical)
    {
      ++result.total_errors;
    }
    else if (issue.severity == IssueSeverity::Warning)
    {
      ++result.total_warnings;
    }
  }

  result.valid = (result.corrupted_segments == 0) && !result.hasErrors();

  return result;
}

SegmentRepairer::SegmentRepairer(RepairConfig config) : _config(std::move(config)) {}

RepairResult SegmentRepairer::repair(const std::filesystem::path& segment_path)
{
  SegmentValidator validator;
  auto validation = validator.validate(segment_path);
  return repair(segment_path, validation);
}

RepairResult SegmentRepairer::repair(const std::filesystem::path& segment_path,
                                     const SegmentValidationResult& validation)
{
  RepairResult result;
  result.path = segment_path;

  if (!std::filesystem::exists(segment_path))
  {
    result.errors.push_back("File not found");
    return result;
  }

  // Create backup if requested
  if (_config.backup_before_repair)
  {
    if (!createBackup(segment_path, result))
    {
      return result;
    }
  }

  // Fix header timestamps if needed
  if (_config.fix_header_timestamps && validation.actual_first_ts > 0)
  {
    if (validation.reported_first_ts != validation.actual_first_ts ||
        validation.reported_last_ts != validation.actual_last_ts)
    {
      fixHeaderTimestamps(segment_path, validation, result);
    }
  }

  // Fix event count if needed
  if (_config.fix_event_count && validation.reported_event_count != validation.actual_event_count)
  {
    fixEventCount(segment_path, validation, result);
  }

  // Rebuild index if needed
  if (_config.rebuild_index && validation.has_index && !validation.index_valid)
  {
    rebuildIndex(segment_path, result);
  }

  result.success = result.errors.empty();
  return result;
}

bool SegmentRepairer::createBackup(const std::filesystem::path& path, RepairResult& result)
{
  result.backup_path = path;
  result.backup_path += _config.backup_suffix;

  std::error_code ec;
  std::filesystem::copy_file(path, result.backup_path,
                             std::filesystem::copy_options::overwrite_existing, ec);

  if (ec)
  {
    result.errors.push_back("Failed to create backup: " + ec.message());
    return false;
  }

  result.backup_created = true;
  result.actions_taken.push_back("Created backup: " + result.backup_path.string());
  return true;
}

bool SegmentRepairer::fixHeaderTimestamps(const std::filesystem::path& path,
                                          const SegmentValidationResult& val, RepairResult& result)
{
  std::FILE* file = std::fopen(path.string().c_str(), "r+b");
  if (!file)
  {
    result.errors.push_back("Cannot open file for writing");
    return false;
  }

  // Read header
  SegmentHeader header;
  if (std::fread(&header, sizeof(header), 1, file) != 1)
  {
    std::fclose(file);
    result.errors.push_back("Cannot read header");
    return false;
  }

  // Update timestamps
  header.first_event_ns = val.actual_first_ts;
  header.last_event_ns = val.actual_last_ts;

  // Write back
  std::fseek(file, 0, SEEK_SET);
  if (std::fwrite(&header, sizeof(header), 1, file) != 1)
  {
    std::fclose(file);
    result.errors.push_back("Cannot write header");
    return false;
  }

  std::fclose(file);
  result.actions_taken.push_back("Fixed header timestamps");
  return true;
}

bool SegmentRepairer::fixEventCount(const std::filesystem::path& path,
                                    const SegmentValidationResult& val, RepairResult& result)
{
  std::FILE* file = std::fopen(path.string().c_str(), "r+b");
  if (!file)
  {
    result.errors.push_back("Cannot open file for writing");
    return false;
  }

  // Read header
  SegmentHeader header;
  if (std::fread(&header, sizeof(header), 1, file) != 1)
  {
    std::fclose(file);
    result.errors.push_back("Cannot read header");
    return false;
  }

  // Update event count
  header.event_count = val.actual_event_count;

  // Write back
  std::fseek(file, 0, SEEK_SET);
  if (std::fwrite(&header, sizeof(header), 1, file) != 1)
  {
    std::fclose(file);
    result.errors.push_back("Cannot write header");
    return false;
  }

  std::fclose(file);
  result.actions_taken.push_back("Fixed event count: " + std::to_string(val.actual_event_count));
  return true;
}

bool SegmentRepairer::rebuildIndex(const std::filesystem::path& path, RepairResult& result)
{
  // Actually rebuild the index (scan the record stream, write a fresh index)
  // and verify it, instead of claiming success without doing anything.
  IndexBuilder builder;  // default: verify_crc = true
  const IndexBuildResult r = builder.buildForSegment(path);
  if (!r.success)
  {
    result.errors.push_back("Index rebuild failed: " + r.error);
    return false;
  }
  result.actions_taken.push_back("Rebuilt index: " + std::to_string(r.index_entries_created) +
                                 " entries over " + std::to_string(r.events_scanned) + " events");
  return true;
}

}  // namespace lrvx::replay
