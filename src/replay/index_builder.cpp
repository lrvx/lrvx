/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/replay/ops/index_builder.h"
#include "segment_bounds.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace lrvx::replay
{

IndexBuilder::IndexBuilder(IndexBuilderConfig config) : _config(std::move(config)) {}

IndexBuildResult IndexBuilder::buildForSegment(const std::filesystem::path& segment_path)
{
  IndexBuildResult result;

  // Open file for reading
  std::FILE* file = std::fopen(segment_path.string().c_str(), "rb");
  if (!file)
  {
    result.error = "Failed to open file: " + segment_path.string();
    return result;
  }

  // Read segment header
  SegmentHeader header;
  if (std::fread(&header, sizeof(header), 1, file) != 1 || !header.isValid())
  {
    result.error = "Invalid segment header";
    std::fclose(file);
    return result;
  }

  // Determine where to stop reading (either at existing index or end of file)
  uint64_t data_end = header.hasIndex() ? header.index_offset : 0;
  if (data_end == 0)
  {
    std::fseek(file, 0, SEEK_END);
    data_end = static_cast<uint64_t>(std::ftell(file));
    std::fseek(file, sizeof(SegmentHeader), SEEK_SET);
  }
  else
  {
    std::fseek(file, sizeof(SegmentHeader), SEEK_SET);
  }

  // Scan events and build index
  std::vector<IndexEntry> index_entries;
  std::vector<std::byte> payload_buffer;
  payload_buffer.reserve(64 * 1024);

  uint32_t event_count = 0;
  uint32_t events_since_last_entry = 0;
  int64_t first_ts = 0;
  int64_t last_ts = 0;

  while (static_cast<uint64_t>(std::ftell(file)) < data_end)
  {
    uint64_t frame_offset = static_cast<uint64_t>(std::ftell(file));

    FrameHeader frame;
    if (std::fread(&frame, sizeof(frame), 1, file) != 1)
    {
      break;  // EOF
    }

    if (frame.size > 10 * 1024 * 1024)
    {
      result.error = "Frame size too large at offset " + std::to_string(frame_offset);
      std::fclose(file);
      return result;
    }

    payload_buffer.resize(frame.size);
    if (std::fread(payload_buffer.data(), 1, frame.size, file) != frame.size)
    {
      result.error = "Failed to read payload at offset " + std::to_string(frame_offset);
      std::fclose(file);
      return result;
    }

    // Verify CRC if enabled
    if (_config.verify_crc)
    {
      uint32_t computed_crc = Crc32::compute(payload_buffer);
      if (computed_crc != frame.crc32)
      {
        result.error = "CRC mismatch at offset " + std::to_string(frame_offset);
        std::fclose(file);
        return result;
      }
    }

    // Get timestamp from event
    int64_t event_ts = 0;
    if (frame.type == static_cast<uint8_t>(EventType::Trade))
    {
      if (frame.size >= sizeof(TradeRecord))
      {
        TradeRecord trade;
        std::memcpy(&trade, payload_buffer.data(), sizeof(trade));
        event_ts = trade.exchange_ts_ns;
      }
    }
    else
    {
      if (frame.size >= sizeof(BookRecordHeader))
      {
        BookRecordHeader book_header;
        std::memcpy(&book_header, payload_buffer.data(), sizeof(book_header));
        event_ts = book_header.exchange_ts_ns;
      }
    }

    if (first_ts == 0)
    {
      first_ts = event_ts;
    }
    last_ts = event_ts;

    // Add index entry at interval
    if (index_entries.empty() || events_since_last_entry >= _config.index_interval)
    {
      index_entries.push_back(IndexEntry{.timestamp_ns = event_ts, .file_offset = frame_offset});
      events_since_last_entry = 0;
    }

    ++event_count;
    ++events_since_last_entry;
  }

  std::fclose(file);

  if (event_count == 0)
  {
    result.success = true;
    result.error = "No events to index";
    return result;
  }

  // Create backup if requested
  if (_config.backup_original)
  {
    auto backup_path = segment_path;
    backup_path += ".bak";
    std::filesystem::copy_file(segment_path, backup_path,
                               std::filesystem::copy_options::overwrite_existing);
  }

  // Reopen file for writing (append mode won't work, need to truncate at data_end)
  file = std::fopen(segment_path.string().c_str(), "r+b");
  if (!file)
  {
    result.error = "Failed to reopen file for writing";
    return result;
  }

  // Seek to data end (where index will start)
  std::fseek(file, static_cast<long>(data_end), SEEK_SET);

  // Write index header
  SegmentIndexHeader idx_header{};
  idx_header.interval = _config.index_interval;
  idx_header.entry_count = static_cast<uint32_t>(index_entries.size());
  idx_header.first_ts_ns = first_ts;
  idx_header.last_ts_ns = last_ts;
  idx_header.crc32 = Crc32::compute(index_entries.data(), index_entries.size() * sizeof(IndexEntry));

  if (std::fwrite(&idx_header, sizeof(idx_header), 1, file) != 1)
  {
    result.error = "Failed to write index header";
    std::fclose(file);
    return result;
  }

  // Write index entries
  if (std::fwrite(index_entries.data(), sizeof(IndexEntry), index_entries.size(), file) !=
      index_entries.size())
  {
    result.error = "Failed to write index entries";
    std::fclose(file);
    return result;
  }

  // Update segment header
  header.index_offset = data_end;
  header.flags |= SegmentFlags::HasIndex;
  header.first_event_ns = first_ts;
  header.last_event_ns = last_ts;
  header.event_count = event_count;

  std::fseek(file, 0, SEEK_SET);
  if (std::fwrite(&header, sizeof(header), 1, file) != 1)
  {
    result.error = "Failed to update segment header";
    std::fclose(file);
    return result;
  }

  std::fclose(file);

  result.success = true;
  result.events_scanned = event_count;
  result.index_entries_created = static_cast<uint32_t>(index_entries.size());
  return result;
}

std::vector<IndexBuildResult> IndexBuilder::buildForDirectory(const std::filesystem::path& dir)
{
  std::vector<IndexBuildResult> results;

  if (!std::filesystem::exists(dir))
  {
    return results;
  }

  for (const auto& entry : std::filesystem::directory_iterator(dir))
  {
    if (entry.is_regular_file() && isSegmentExtension(entry.path().extension().string()))
    {
      results.push_back(buildForSegment(entry.path()));
    }
  }

  return results;
}

bool IndexBuilder::hasIndex(const std::filesystem::path& segment_path)
{
  std::FILE* file = std::fopen(segment_path.string().c_str(), "rb");
  if (!file)
  {
    return false;
  }

  SegmentHeader header;
  bool has_idx = false;
  if (std::fread(&header, sizeof(header), 1, file) == 1 && header.isValid())
  {
    has_idx = header.hasIndex();
  }

  std::fclose(file);
  return has_idx;
}

bool IndexBuilder::removeIndex(const std::filesystem::path& segment_path)
{
  std::FILE* file = std::fopen(segment_path.string().c_str(), "r+b");
  if (!file)
  {
    return false;
  }

  SegmentHeader header;
  if (std::fread(&header, sizeof(header), 1, file) != 1 || !header.isValid())
  {
    std::fclose(file);
    return false;
  }

  if (!header.hasIndex())
  {
    std::fclose(file);
    return true;  // No index to remove
  }

  // Truncate file at index offset
  uint64_t new_size = header.index_offset;

  // Update header
  header.index_offset = 0;
  header.flags &= ~SegmentFlags::HasIndex;

  std::fseek(file, 0, SEEK_SET);
  std::fwrite(&header, sizeof(header), 1, file);

  std::fclose(file);

  // Truncate the file
  std::filesystem::resize_file(segment_path, new_size);

  return true;
}

GlobalIndexBuildResult GlobalIndexBuilder::build(const std::filesystem::path& data_dir,
                                                 const std::filesystem::path& output_path)
{
  GlobalIndexBuildResult result;

  if (!std::filesystem::exists(data_dir))
  {
    result.error = "Directory does not exist: " + data_dir.string();
    return result;
  }

  // Collect segment info
  std::vector<std::pair<std::filesystem::path, SegmentHeader>> segments;

  for (const auto& entry : std::filesystem::directory_iterator(data_dir))
  {
    if (!entry.is_regular_file() || !isSegmentExtension(entry.path().extension().string()))
    {
      continue;
    }

    std::FILE* file = std::fopen(entry.path().string().c_str(), "rb");
    if (!file)
    {
      continue;
    }

    SegmentHeader header;
    if (std::fread(&header, sizeof(header), 1, file) == 1 && header.isValid())
    {
      // The global index reports each segment's earliest and latest event
      // where the segment proves them (detail::segmentSpan); the header's
      // fields are the first and last events written.
      SegmentInfo info;
      info.path = entry.path();
      info.first_event_ns = header.first_event_ns;
      info.last_event_ns = header.last_event_ns;
      info.event_count = header.event_count;
      info.sorted = header.isSorted();
      info.compressed = header.isCompressed();
      const auto span = detail::segmentSpan(info);
      header.first_event_ns = span.first_ns;
      header.last_event_ns = span.last_ns;
      segments.emplace_back(entry.path(), header);
    }
    std::fclose(file);
  }

  if (segments.empty())
  {
    result.error = "No valid segments found";
    return result;
  }

  // Sort by first event timestamp
  std::sort(segments.begin(), segments.end(),
            [](const auto& a, const auto& b)
            { return a.second.first_event_ns < b.second.first_event_ns; });

  // Build string table and segment entries
  std::vector<GlobalIndexSegment> entries;
  std::string string_table;
  int64_t min_ts = INT64_MAX;
  int64_t max_ts = INT64_MIN;
  uint64_t total_events = 0;

  for (const auto& [path, header] : segments)
  {
    GlobalIndexSegment seg{};
    seg.first_event_ns = header.first_event_ns;
    seg.last_event_ns = header.last_event_ns;
    seg.event_count = header.event_count;
    seg.flags = header.flags;
    seg.file_size = std::filesystem::file_size(path);
    seg.filename_offset = string_table.size();

    // Add filename to string table (just the filename, not full path)
    std::string filename = path.filename().string();
    string_table += filename;
    string_table += '\0';

    entries.push_back(seg);

    if (header.first_event_ns > 0)
    {
      min_ts = std::min(min_ts, header.first_event_ns);
    }
    if (header.last_event_ns > 0)
    {
      max_ts = std::max(max_ts, header.last_event_ns);
    }
    total_events += header.event_count;
  }

  // Determine output path
  std::filesystem::path idx_path = output_path.empty() ? (data_dir / "index.lrvxidx") : output_path;

  // Write global index
  std::FILE* file = std::fopen(idx_path.string().c_str(), "wb");
  if (!file)
  {
    result.error = "Failed to create index file: " + idx_path.string();
    return result;
  }

  GlobalIndexHeader global_header{};
  global_header.created_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
  global_header.first_event_ns = (min_ts == INT64_MAX) ? 0 : min_ts;
  global_header.last_event_ns = (max_ts == INT64_MIN) ? 0 : max_ts;
  global_header.segment_count = static_cast<uint32_t>(entries.size());
  global_header.total_events = total_events;
  global_header.string_table_offset =
      sizeof(GlobalIndexHeader) + entries.size() * sizeof(GlobalIndexSegment);
  global_header.crc32 = Crc32::compute(entries.data(), entries.size() * sizeof(GlobalIndexSegment));

  std::fwrite(&global_header, sizeof(global_header), 1, file);
  std::fwrite(entries.data(), sizeof(GlobalIndexSegment), entries.size(), file);
  std::fwrite(string_table.data(), 1, string_table.size(), file);

  std::fclose(file);

  result.success = true;
  result.segments_indexed = static_cast<uint32_t>(entries.size());
  result.total_events = total_events;
  return result;
}

std::optional<std::vector<GlobalIndexSegment>> GlobalIndexBuilder::load(
    const std::filesystem::path& index_path)
{
  std::FILE* file = std::fopen(index_path.string().c_str(), "rb");
  if (!file)
  {
    return std::nullopt;
  }

  GlobalIndexHeader header;
  if (std::fread(&header, sizeof(header), 1, file) != 1 || !header.isValid())
  {
    std::fclose(file);
    return std::nullopt;
  }

  std::vector<GlobalIndexSegment> segments(header.segment_count);
  if (std::fread(segments.data(), sizeof(GlobalIndexSegment), header.segment_count, file) !=
      header.segment_count)
  {
    std::fclose(file);
    return std::nullopt;
  }

  // Verify CRC
  uint32_t computed_crc = Crc32::compute(segments.data(), segments.size() * sizeof(GlobalIndexSegment));
  if (computed_crc != header.crc32)
  {
    std::fclose(file);
    return std::nullopt;
  }

  std::fclose(file);
  return segments;
}

}  // namespace lrvx::replay
