// python/segment_ops_bindings.h

#pragma once

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "lrvx/capi/lrvx_capi.h"
#include "lrvx/replay/ops/partitioner.h"
#include "lrvx/replay/ops/segment_ops.h"
#include "lrvx/replay/ops/validator.h"

#include <set>
#include <string>
#include <vector>

namespace py = pybind11;

namespace
{

using namespace lrvx::replay;

inline CompressionType parseCompression(const std::string& s)
{
  if (s == "lz4")
  {
    return CompressionType::LZ4;
  }
  return CompressionType::None;
}

inline SplitMode parseSplitMode(const std::string& s)
{
  if (s == "event_count")
  {
    return SplitMode::ByEventCount;
  }
  if (s == "size")
  {
    return SplitMode::BySize;
  }
  if (s == "symbol")
  {
    return SplitMode::BySymbol;
  }
  return SplitMode::ByTime;
}

inline ExportFormat parseExportFormat(const std::string& s)
{
  if (s == "json")
  {
    return ExportFormat::JSON;
  }
  if (s == "jsonlines")
  {
    return ExportFormat::JSONLines;
  }
  if (s == "binary")
  {
    return ExportFormat::Binary;
  }
  return ExportFormat::CSV;
}

inline Partitioner::CalendarUnit parseCalendarUnit(const std::string& s)
{
  if (s == "day")
  {
    return Partitioner::CalendarUnit::Day;
  }
  if (s == "week")
  {
    return Partitioner::CalendarUnit::Week;
  }
  if (s == "month")
  {
    return Partitioner::CalendarUnit::Month;
  }
  return Partitioner::CalendarUnit::Hour;
}

inline py::dict mergeResultToDict(const MergeResult& r)
{
  py::dict d;
  d["success"] = r.success;
  d["output_path"] = r.output_path.string();
  d["segments_merged"] = r.segments_merged;
  d["events_written"] = r.events_written;
  d["bytes_written"] = r.bytes_written;
  d["errors"] = r.errors;
  return d;
}

inline py::dict splitResultToDict(const SplitResult& r)
{
  py::dict d;
  d["success"] = r.success;
  py::list paths;
  for (const auto& p : r.output_paths)
  {
    paths.append(p.string());
  }
  d["output_paths"] = paths;
  d["segments_created"] = r.segments_created;
  d["events_written"] = r.events_written;
  d["errors"] = r.errors;
  return d;
}

inline py::dict exportResultToDict(const ExportResult& r)
{
  py::dict d;
  d["success"] = r.success;
  d["output_path"] = r.output_path.string();
  d["events_exported"] = r.events_exported;
  d["bytes_written"] = r.bytes_written;
  d["errors"] = r.errors;
  return d;
}

inline py::dict partitionToDict(const Partition& p)
{
  py::dict d;
  d["partition_id"] = p.partition_id;
  d["from_ns"] = p.from_ns;
  d["to_ns"] = p.to_ns;
  d["warmup_from_ns"] = p.warmup_from_ns;
  d["estimated_events"] = p.estimated_events;
  d["estimated_bytes"] = p.estimated_bytes;
  py::list syms;
  for (auto s : p.symbols)
  {
    syms.append(s);
  }
  d["symbols"] = syms;
  return d;
}

// "type" is the IssueType value; the module's ISSUE_* constants name them.
inline py::list issuesToList(const std::vector<ValidationIssue>& found)
{
  py::list issues;
  for (const auto& issue : found)
  {
    py::dict id;
    id["type"] = static_cast<int>(issue.type);
    id["message"] = issue.message;
    id["severity"] = static_cast<int>(issue.severity);
    id["file_offset"] = issue.file_offset;
    id["timestamp_ns"] = issue.timestamp_ns;
    issues.append(id);
  }
  return issues;
}

inline py::dict segmentValidationToDict(const SegmentValidationResult& r)
{
  py::dict d;
  d["path"] = r.path.string();
  d["valid"] = r.valid;
  d["header_valid"] = r.header_valid;
  d["reported_event_count"] = r.reported_event_count;
  d["actual_event_count"] = r.actual_event_count;
  d["has_index"] = r.has_index;
  d["index_valid"] = r.index_valid;
  d["trades_found"] = r.trades_found;
  d["book_updates_found"] = r.book_updates_found;
  d["crc_errors"] = r.crc_errors;
  d["timestamp_anomalies"] = r.timestamp_anomalies;
  d["has_errors"] = r.hasErrors();
  d["has_critical"] = r.hasCritical();

  d["issues"] = issuesToList(r.issues);
  return d;
}

inline py::dict datasetValidationToDict(const DatasetValidationResult& r)
{
  py::dict d;
  d["data_dir"] = r.data_dir.string();
  d["valid"] = r.valid;
  d["total_segments"] = r.total_segments;
  d["valid_segments"] = r.valid_segments;
  d["corrupted_segments"] = r.corrupted_segments;
  d["total_events"] = r.total_events;
  d["total_bytes"] = r.total_bytes;
  d["first_timestamp"] = r.first_timestamp;
  d["last_timestamp"] = r.last_timestamp;
  d["total_errors"] = r.total_errors;
  d["total_warnings"] = r.total_warnings;

  py::list segs;
  for (const auto& seg : r.segments)
  {
    segs.append(segmentValidationToDict(seg));
  }
  d["segments"] = segs;

  // Issues about the dataset rather than about one segment: a directory that
  // is not there or cannot be listed, segments that overlap in time.
  d["issues"] = issuesToList(r.issues);
  return d;
}

class PyPartitioner
{
 public:
  explicit PyPartitioner(const std::string& dataDir) : _partitioner(dataDir) {}

  py::list partitionByTime(uint32_t numPartitions, int64_t warmupNs) const
  {
    auto parts = _partitioner.partitionByTime(numPartitions, warmupNs);
    return toList(parts);
  }

  py::list partitionByDuration(int64_t durationNs, int64_t warmupNs) const
  {
    auto parts = _partitioner.partitionByDuration(durationNs, warmupNs);
    return toList(parts);
  }

  py::list partitionByCalendar(const std::string& unit, int64_t warmupNs) const
  {
    auto parts = _partitioner.partitionByCalendar(parseCalendarUnit(unit), warmupNs);
    return toList(parts);
  }

  py::list partitionBySymbol(uint32_t numPartitions) const
  {
    auto parts = _partitioner.partitionBySymbol(numPartitions);
    return toList(parts);
  }

  py::list partitionPerSymbol() const
  {
    auto parts = _partitioner.partitionPerSymbol();
    return toList(parts);
  }

  py::list partitionByEventCount(uint32_t numPartitions) const
  {
    auto parts = _partitioner.partitionByEventCount(numPartitions);
    return toList(parts);
  }

 private:
  static py::list toList(const std::vector<Partition>& parts)
  {
    py::list result;
    for (const auto& p : parts)
    {
      result.append(partitionToDict(p));
    }
    return result;
  }

  Partitioner _partitioner;
};

}  // namespace

inline void bindSegmentOps(py::module_& m)
{
  // ── Merge ─────────────────────────────────────────────────────────────

  m.def(
      "merge",
      [](std::vector<std::string> inputPaths, const std::string& outputDir,
         const std::string& outputName, const std::string& compression, bool sort)
      {
        MergeConfig cfg;
        cfg.output_dir = outputDir;
        cfg.output_name = outputName;
        cfg.compression = parseCompression(compression);
        cfg.sort_by_timestamp = sort;

        std::vector<std::filesystem::path> paths;
        paths.reserve(inputPaths.size());
        for (auto& p : inputPaths)
        {
          paths.emplace_back(std::move(p));
        }

        MergeResult result;
        {
          py::gil_scoped_release release;
          result = SegmentOps::merge(paths, cfg);
        }
        return mergeResultToDict(result);
      },
      py::arg("input_paths"), py::arg("output_dir"),
      py::arg("output_name") = "merged",
      py::arg("compression") = "none", py::arg("sort") = true);

  m.def(
      "merge_dir",
      [](const std::string& inputDir, const std::string& outputDir)
      {
        MergeResult result;
        {
          py::gil_scoped_release release;
          result = quickMerge(inputDir, outputDir);
        }
        return mergeResultToDict(result);
      },
      py::arg("input_dir"), py::arg("output_dir"));

  // ── Split ─────────────────────────────────────────────────────────────

  m.def(
      "split",
      [](const std::string& inputPath, const std::string& outputDir,
         const std::string& mode, int64_t timeIntervalNs, uint64_t eventsPerFile)
      {
        SplitConfig cfg;
        cfg.output_dir = outputDir;
        cfg.mode = parseSplitMode(mode);
        cfg.time_interval_ns = timeIntervalNs;
        cfg.events_per_file = eventsPerFile;

        SplitResult result;
        {
          py::gil_scoped_release release;
          result = SegmentOps::split(inputPath, cfg);
        }
        return splitResultToDict(result);
      },
      py::arg("input_path"), py::arg("output_dir"),
      py::arg("mode") = "time",
      py::arg("time_interval_ns") = 3600LL * 1000000000LL,
      py::arg("events_per_file") = 1000000);

  // ── Export ────────────────────────────────────────────────────────────

  m.def(
      "export_data",
      [](const std::string& inputPath, const std::string& outputPath,
         const std::string& format, py::object fromNs, py::object toNs,
         py::object symbols)
      {
        ExportConfig cfg;
        cfg.output_path = outputPath;
        cfg.format = parseExportFormat(format);
        if (!fromNs.is_none())
        {
          cfg.from_ts = fromNs.cast<int64_t>();
        }
        if (!toNs.is_none())
        {
          cfg.to_ts = toNs.cast<int64_t>();
        }
        if (!symbols.is_none())
        {
          auto syms = symbols.cast<std::vector<uint32_t>>();
          cfg.symbols.insert(syms.begin(), syms.end());
        }

        ExportResult result;
        {
          py::gil_scoped_release release;
          result = SegmentOps::exportData(inputPath, cfg);
        }
        return exportResultToDict(result);
      },
      py::arg("input_path"), py::arg("output_path"),
      py::arg("format") = "csv",
      py::arg("from_ns") = py::none(), py::arg("to_ns") = py::none(),
      py::arg("symbols") = py::none());

  // ── Other ops ─────────────────────────────────────────────────────────

  m.def(
      "recompress",
      [](const std::string& inputPath, const std::string& outputPath,
         const std::string& compression)
      {
        // The three below go through the C API, which leaves the reason for
        // a False or a 0 where lrvx.last_error() reads it. The record is per
        // thread and releasing the GIL does not change threads.
        const uint8_t codec = parseCompression(compression) == CompressionType::LZ4 ? 1 : 0;
        bool ok;
        lrvx_clear_last_error();
        {
          py::gil_scoped_release release;
          ok = lrvx_segment_recompress(inputPath.c_str(), outputPath.c_str(), codec) != 0;
        }
        return ok;
      },
      py::arg("input_path"), py::arg("output_path"), py::arg("compression") = "lz4");

  m.def(
      "extract_symbols",
      [](const std::string& inputPath, const std::string& outputPath,
         std::vector<uint32_t> symbols)
      {
        uint64_t count;
        lrvx_clear_last_error();
        {
          py::gil_scoped_release release;
          count = lrvx_segment_extract_symbols(inputPath.c_str(), outputPath.c_str(),
                                               symbols.data(),
                                               static_cast<uint32_t>(symbols.size()));
        }
        return count;
      },
      py::arg("input_path"), py::arg("output_path"), py::arg("symbols"));

  m.def(
      "extract_time_range",
      [](const std::string& inputPath, const std::string& outputPath,
         int64_t fromNs, int64_t toNs)
      {
        uint64_t count;
        lrvx_clear_last_error();
        {
          py::gil_scoped_release release;
          count = lrvx_segment_extract_time_range(inputPath.c_str(), outputPath.c_str(), fromNs,
                                                  toNs);
        }
        return count;
      },
      py::arg("input_path"), py::arg("output_path"),
      py::arg("from_ns"), py::arg("to_ns"));

  // ── Validation ────────────────────────────────────────────────────────

  m.def(
      "validate",
      [](const std::string& segmentPath, bool verifyCrc, bool verifyTimestamps)
      {
        ValidatorConfig cfg;
        cfg.verify_crc = verifyCrc;
        cfg.verify_timestamps = verifyTimestamps;
        SegmentValidator validator(cfg);

        SegmentValidationResult result;
        {
          py::gil_scoped_release release;
          result = validator.validate(segmentPath);
        }
        return segmentValidationToDict(result);
      },
      py::arg("segment_path"), py::arg("verify_crc") = true,
      py::arg("verify_timestamps") = true);

  m.def(
      "validate_dataset",
      [](const std::string& dataDir)
      {
        DatasetValidator validator;
        DatasetValidationResult result;
        {
          py::gil_scoped_release release;
          result = validator.validate(dataDir);
        }
        return datasetValidationToDict(result);
      },
      py::arg("data_dir"));

  // ── Partitioner ───────────────────────────────────────────────────────

  py::class_<PyPartitioner>(m, "Partitioner")
      .def(py::init<std::string>(), py::arg("data_dir"))
      .def("partition_by_time", &PyPartitioner::partitionByTime,
           py::arg("num_partitions"), py::arg("warmup_ns") = 0)
      .def("partition_by_duration", &PyPartitioner::partitionByDuration,
           py::arg("duration_ns"), py::arg("warmup_ns") = 0)
      .def("partition_by_calendar", &PyPartitioner::partitionByCalendar,
           py::arg("unit"), py::arg("warmup_ns") = 0)
      .def("partition_by_symbol", &PyPartitioner::partitionBySymbol,
           py::arg("num_partitions"))
      .def("partition_per_symbol", &PyPartitioner::partitionPerSymbol)
      .def("partition_by_event_count", &PyPartitioner::partitionByEventCount,
           py::arg("num_partitions"));

  // The values an issue's "type" takes, in IssueType order.
  const auto issueType = [&m](const char* name, IssueType type)
  {
    m.attr(name) = py::int_(static_cast<int>(type));
  };
  issueType("ISSUE_INVALID_MAGIC", IssueType::InvalidMagic);
  issueType("ISSUE_INVALID_VERSION", IssueType::InvalidVersion);
  issueType("ISSUE_INVALID_FLAGS", IssueType::InvalidFlags);
  issueType("ISSUE_HEADER_CORRUPTED", IssueType::HeaderCorrupted);
  issueType("ISSUE_FRAME_CRC_MISMATCH", IssueType::FrameCrcMismatch);
  issueType("ISSUE_FRAME_SIZE_TOO_LARGE", IssueType::FrameSizeTooLarge);
  issueType("ISSUE_FRAME_TYPE_UNKNOWN", IssueType::FrameTypeUnknown);
  issueType("ISSUE_FRAME_TRUNCATED", IssueType::FrameTruncated);
  issueType("ISSUE_BLOCK_MAGIC_INVALID", IssueType::BlockMagicInvalid);
  issueType("ISSUE_BLOCK_DECOMPRESSION_FAILED", IssueType::BlockDecompressionFailed);
  issueType("ISSUE_BLOCK_SIZE_MISMATCH", IssueType::BlockSizeMismatch);
  issueType("ISSUE_INDEX_CRC_MISMATCH", IssueType::IndexCrcMismatch);
  issueType("ISSUE_INDEX_MAGIC_INVALID", IssueType::IndexMagicInvalid);
  issueType("ISSUE_INDEX_OUT_OF_BOUNDS", IssueType::IndexOutOfBounds);
  issueType("ISSUE_INDEX_NOT_SORTED", IssueType::IndexNotSorted);
  issueType("ISSUE_TIMESTAMP_OUT_OF_ORDER", IssueType::TimestampOutOfOrder);
  issueType("ISSUE_TIMESTAMP_JUMP_TOO_LARGE", IssueType::TimestampJumpTooLarge);
  issueType("ISSUE_EVENT_COUNT_MISMATCH", IssueType::EventCountMismatch);
  issueType("ISSUE_FILE_TRUNCATED", IssueType::FileTruncated);
  issueType("ISSUE_FILE_NOT_FOUND", IssueType::FileNotFound);
  issueType("ISSUE_FILE_READ_ERROR", IssueType::FileReadError);
  issueType("ISSUE_SEGMENT_RANGE_OVERLAP", IssueType::SegmentRangeOverlap);
}
