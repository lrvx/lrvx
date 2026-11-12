/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// What a segment's header and index prove about where its events lie, shared
// by the readers in src/replay. Not installed.

#pragma once

#include "lrvx/replay/binary_format_v1.h"
#include "lrvx/replay/readers/binary_log_reader.h"

#include <cstdint>
#include <optional>
#include <span>

namespace lrvx::replay::detail
{

// Proven lower bound on every event of the segment: set at scan time for a
// Sorted segment from its first frame (uncompressed) or its block index
// (compressed).
inline std::optional<int64_t> provenMin(const SegmentInfo& segment)
{
  if (!segment.time_bounded)
  {
    return std::nullopt;
  }
  return segment.min_event_ns;
}

// Proven upper bound on every event of the segment. Only an uncompressed
// Sorted segment has one: its frames are in non-decreasing order, so the last
// one written is the maximum. A compressed segment's header records the last
// event in arrival order, which the writer then sorted into its block, and a
// segment without the Sorted flag records arrival order with no promise.
inline std::optional<int64_t> provenMax(const SegmentInfo& segment)
{
  if (!segment.sorted || segment.compressed)
  {
    return std::nullopt;
  }
  return segment.last_event_ns;
}

// True when the proven bounds place every event of the segment outside
// [from, to].
inline bool provenOutside(const SegmentInfo& segment, int64_t from, int64_t to)
{
  const auto max = provenMax(segment);
  const auto min = provenMin(segment);
  return (max && *max < from) || (min && *min > to);
}

// Byte window [start, end) of a Sorted segment that can hold events in
// [from, to], read off its index; nullopt when the index proves nothing.
// `start` is the last entry strictly below `from` (or the first frame), so
// events stamped exactly `from` that sit before an entry stamped `from` are
// kept. Defined in binary_log_reader.cpp.
struct IndexWindow
{
  uint64_t start;
  uint64_t end;
  bool empty() const { return end <= start; }
};

std::optional<IndexWindow> indexWindow(std::span<const IndexEntry> idx, const SegmentHeader& hdr,
                                       int64_t from, int64_t to);

}  // namespace lrvx::replay::detail
