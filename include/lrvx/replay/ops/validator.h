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
#include "lrvx/replay/ops/compression.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace lrvx::replay
{

enum class IssueType
{
  InvalidMagic,
  InvalidVersion,
  InvalidFlags,
  HeaderCorrupted,

  FrameCrcMismatch,
  FrameSizeTooLarge,
  FrameTypeUnknown,
  FrameTruncated,

  BlockMagicInvalid,
  BlockDecompressionFailed,
  BlockSizeMismatch,

  IndexCrcMismatch,
  IndexMagicInvalid,
  IndexOutOfBounds,
  IndexNotSorted,

  TimestampOutOfOrder,
  TimestampJumpTooLarge,
  EventCountMismatch,
  FileTruncated,

  FileNotFound,
  FileReadError,

  // Dataset-level, not segment-level: two segments cover intersecting
  // exchange-timestamp ranges, so the same window was recorded twice.
  SegmentRangeOverlap,
};

enum class IssueSeverity
{
  Info,
  Warning,
  Error,
  Critical,
};

struct ValidationIssue
{
  IssueType type;
  IssueSeverity severity;
  std::string message;
  uint64_t file_offset{0};
  // For an issue found at one event (a CRC mismatch, a timestamp out of
  // order or too far ahead): that event's index in the segment, counted from
  // 0, and its timestamp where it was read. 0 for every other issue.
  uint64_t event_index{0};
  int64_t timestamp_ns{0};
};

struct SegmentValidationResult
{
  std::filesystem::path path;
  bool valid{true};
  std::vector<ValidationIssue> issues;

  bool header_valid{false};
  uint32_t reported_event_count{0};
  int64_t reported_first_ts{0};
  int64_t reported_last_ts{0};
  bool is_compressed{false};
  CompressionType compression_type{CompressionType::None};

  uint32_t actual_event_count{0};
  int64_t actual_first_ts{0};
  int64_t actual_last_ts{0};
  uint64_t bytes_scanned{0};

  bool has_index{false};
  bool index_valid{false};
  uint32_t index_entry_count{0};

  uint32_t trades_found{0};
  uint32_t book_updates_found{0};
  uint32_t crc_errors{0};
  uint32_t timestamp_anomalies{0};

  bool hasErrors() const
  {
    for (const auto& issue : issues)
    {
      if (issue.severity == IssueSeverity::Error || issue.severity == IssueSeverity::Critical)
      {
        return true;
      }
    }
    return false;
  }

  bool hasCritical() const
  {
    for (const auto& issue : issues)
    {
      if (issue.severity == IssueSeverity::Critical)
      {
        return true;
      }
    }
    return false;
  }
};

struct DatasetValidationResult
{
  std::filesystem::path data_dir;
  bool valid{true};
  std::vector<SegmentValidationResult> segments;

  // Issues about the dataset rather than about any one segment: a caller with
  // only `valid` and `total_errors` cannot tell a CRC failure from a pair of
  // segments that overlap in time, and the two call for different repairs.
  // Per-segment issues stay on their SegmentValidationResult.
  std::vector<ValidationIssue> issues;

  uint32_t total_segments{0};
  uint32_t valid_segments{0};
  uint32_t corrupted_segments{0};
  uint64_t total_events{0};
  uint64_t total_bytes{0};

  int64_t first_timestamp{0};
  int64_t last_timestamp{0};

  uint32_t total_errors{0};
  uint32_t total_warnings{0};

  bool hasErrors() const
  {
    for (const auto& issue : issues)
    {
      if (issue.severity == IssueSeverity::Error || issue.severity == IssueSeverity::Critical)
      {
        return true;
      }
    }
    return false;
  }
};

struct ValidatorConfig
{
  bool verify_crc{true};
  bool verify_timestamps{true};
  bool verify_index{true};
  bool scan_all_events{true};
  bool stop_on_first_error{false};
  int64_t max_timestamp_jump_ns{3600LL * 1000000000LL};
};

class SegmentValidator
{
 public:
  explicit SegmentValidator(ValidatorConfig config = {});

  SegmentValidationResult validate(const std::filesystem::path& segment_path);

  using ProgressCallback = std::function<void(uint64_t bytes_processed, uint64_t total_bytes)>;
  SegmentValidationResult validate(const std::filesystem::path& segment_path,
                                   ProgressCallback progress);

 private:
  bool validateHeader(std::FILE* file, SegmentValidationResult& result);
  bool validateEventsUncompressed(std::FILE* file, SegmentValidationResult& result,
                                  ProgressCallback& progress);
  bool validateEventsCompressed(std::FILE* file, SegmentValidationResult& result,
                                ProgressCallback& progress);
  bool validateIndex(std::FILE* file, SegmentValidationResult& result);

  void addIssue(SegmentValidationResult& result, IssueType type, IssueSeverity severity,
                const std::string& message, uint64_t offset = 0, uint64_t eventIndex = 0,
                int64_t timestampNs = 0);

  ValidatorConfig _config;
};

class DatasetValidator
{
 public:
  explicit DatasetValidator(ValidatorConfig config = {});

  DatasetValidationResult validate(const std::filesystem::path& data_dir);

  using ProgressCallback = std::function<void(uint32_t segment_index, uint32_t total_segments,
                                              const std::filesystem::path& current_file)>;
  DatasetValidationResult validate(const std::filesystem::path& data_dir, ProgressCallback progress);

 private:
  ValidatorConfig _config;
};

struct RepairConfig
{
  bool backup_before_repair{true};
  std::string backup_suffix{".backup"};
  bool fix_header_timestamps{true};
  bool fix_event_count{true};
  bool rebuild_index{true};
  // remove_corrupted_frames / truncate_at_corruption were config knobs the
  // repair pass never read. Removed rather than left as silent no-ops; frame
  // removal / truncation is future work.
};

struct RepairResult
{
  std::filesystem::path path;
  bool success{false};
  bool backup_created{false};
  std::filesystem::path backup_path;
  std::vector<std::string> actions_taken;
  std::vector<std::string> errors;
};

class SegmentRepairer
{
 public:
  explicit SegmentRepairer(RepairConfig config = {});

  RepairResult repair(const std::filesystem::path& segment_path);
  RepairResult repair(const std::filesystem::path& segment_path,
                      const SegmentValidationResult& validation);

 private:
  bool createBackup(const std::filesystem::path& path, RepairResult& result);
  bool fixHeaderTimestamps(const std::filesystem::path& path, const SegmentValidationResult& val,
                           RepairResult& result);
  bool fixEventCount(const std::filesystem::path& path, const SegmentValidationResult& val,
                     RepairResult& result);
  bool rebuildIndex(const std::filesystem::path& path, RepairResult& result);

  RepairConfig _config;
};

inline bool isValidSegment(const std::filesystem::path& path)
{
  SegmentValidator validator;
  auto result = validator.validate(path);
  return result.valid && !result.hasErrors();
}

inline bool isValidDataset(const std::filesystem::path& data_dir)
{
  DatasetValidator validator;
  auto result = validator.validate(data_dir);
  return result.valid;
}

}  // namespace lrvx::replay
