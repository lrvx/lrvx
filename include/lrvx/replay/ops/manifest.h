/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/replay/readers/binary_log_reader.h"

#include <filesystem>
#include <set>
#include <vector>

namespace lrvx::replay
{

constexpr uint32_t kManifestMagic = 0x464D414E;  // "FMAN"
constexpr uint8_t kManifestVersion = 1;

struct ManifestHeader
{
  uint32_t magic{kManifestMagic};
  uint8_t version{kManifestVersion};
  uint8_t reserved[3]{0};
  uint64_t segment_count{0};
  uint64_t total_events{0};
  int64_t first_timestamp_ns{0};
  int64_t last_timestamp_ns{0};
  uint64_t total_bytes{0};
  uint32_t symbol_count{0};
  uint32_t checksum{0};

  bool isValid() const { return magic == kManifestMagic && version == kManifestVersion; }
};

struct ManifestSegmentEntry
{
  char filename[256];
  // The segment's earliest and latest non-zero event. build() takes them from
  // a walk of every event, so they are exact whatever the segment's flags; a
  // manifest saved by an older build holds the header's first and last event
  // written instead.
  int64_t first_event_ns{0};
  int64_t last_event_ns{0};
  uint64_t event_count{0};
  uint64_t file_size{0};
  uint32_t flags{0};
  uint32_t reserved{0};

  static constexpr uint32_t kFlagHasIndex = 1 << 0;
  static constexpr uint32_t kFlagCompressed = 1 << 1;

  bool hasIndex() const { return flags & kFlagHasIndex; }
  bool isCompressed() const { return flags & kFlagCompressed; }
};

class SegmentManifest
{
 public:
  SegmentManifest() = default;

  static std::optional<SegmentManifest> load(const std::filesystem::path& manifest_path);
  static SegmentManifest build(const std::filesystem::path& data_dir);
  static SegmentManifest buildAndSave(const std::filesystem::path& data_dir);

  bool save(const std::filesystem::path& manifest_path) const;
  bool save() const;

  bool empty() const { return _segments.empty(); }

  uint64_t segmentCount() const { return _segments.size(); }
  uint64_t totalEvents() const { return _total_events; }
  uint64_t totalBytes() const { return _total_bytes; }

  int64_t firstTimestamp() const { return _first_ts; }
  int64_t lastTimestamp() const { return _last_ts; }

  const std::set<uint32_t>& symbols() const { return _symbols; }
  const std::vector<SegmentInfo>& segments() const { return _segments; }

  const std::filesystem::path& dataDir() const { return _data_dir; }

  std::vector<SegmentInfo> segmentsInRange(int64_t from_ns, int64_t to_ns) const;
  std::vector<SegmentInfo> segmentsWithSymbols(const std::set<uint32_t>& symbols) const;
  bool isUpToDate() const;

  TimeRange timeRange() const { return {_first_ts, _last_ts}; }

  double durationSeconds() const
  {
    return static_cast<double>(_last_ts - _first_ts) / 1e9;
  }

  double durationHours() const { return durationSeconds() / 3600.0; }
  double durationDays() const { return durationHours() / 24.0; }

 private:
  std::filesystem::path _data_dir;
  std::vector<SegmentInfo> _segments;
  std::set<uint32_t> _symbols;
  uint64_t _total_events{0};
  uint64_t _total_bytes{0};
  int64_t _first_ts{0};
  int64_t _last_ts{0};
  std::filesystem::file_time_type _build_time;
};

SegmentManifest getOrBuildManifest(const std::filesystem::path& data_dir);

inline std::filesystem::path manifestPath(const std::filesystem::path& data_dir)
{
  return data_dir / ".manifest";
}

}  // namespace lrvx::replay
