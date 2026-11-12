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

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <vector>

namespace lrvx::replay
{

class IAggregator;
class BinaryLogIterator;

struct TimeRange
{
  int64_t start_ns{0};
  int64_t end_ns{0};

  bool empty() const { return start_ns == 0 && end_ns == 0; }

  std::chrono::nanoseconds duration() const
  {
    return std::chrono::nanoseconds(end_ns - start_ns);
  }

  double durationSeconds() const
  {
    return static_cast<double>(end_ns - start_ns) / 1e9;
  }

  bool contains(int64_t timestamp_ns) const
  {
    return timestamp_ns >= start_ns && timestamp_ns <= end_ns;
  }
};

namespace time_utils
{

inline int64_t toNanos(std::chrono::system_clock::time_point tp)
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
}

inline std::chrono::system_clock::time_point fromNanos(int64_t ns)
{
  using namespace std::chrono;
  return time_point_cast<system_clock::duration>(
      time_point<system_clock, nanoseconds>(nanoseconds(ns)));
}

inline int64_t nowNanos()
{
  return toNanos(std::chrono::system_clock::now());
}

inline int64_t secondsToNanos(int64_t seconds)
{
  return seconds * 1'000'000'000LL;
}

inline int64_t millisToNanos(int64_t millis)
{
  return millis * 1'000'000LL;
}

inline int64_t microsToNanos(int64_t micros)
{
  return micros * 1'000LL;
}

inline double nanosToSeconds(int64_t ns)
{
  return static_cast<double>(ns) / 1e9;
}

}  // namespace time_utils

struct ReaderConfig
{
  std::filesystem::path data_dir;
  std::optional<int64_t> from_ns;
  std::optional<int64_t> to_ns;
  std::set<uint32_t> symbols;
  bool verify_crc{true};

  // Cross-block reorder window used by streamForEach / run() on
  // segments without the Sorted flag. Events with exchange_ts_ns
  // < (watermark - reorder_window_ns) cannot be emitted in sorted
  // order anymore (we already emitted past their position); the
  // reader throws LrvxError with the observed delta when it sees
  // one. The default (10s) covers exchange-WS jitter and the
  // 99th-percentile of reconnect-induced cross-block inversions
  // measured on real md_collector tapes. Bump it for tapes with
  // longer real-world reconnect gaps.
  //
  // Memory bound: roughly reorder_window_ns × peak_event_rate ×
  // sizeof(ReplayEvent). At 10s × 10k ev/s burst that's ~36 MB —
  // 100× smaller than the legacy buffer-the-whole-segment path.
  int64_t reorder_window_ns{10'000'000'000};  // 10s default

  // How the reader reacts to an event that arrives past the reorder
  // window. Default (false): drop the event, count it in
  // ReaderStats::late_dropped, and log one warning per segment. A run
  // that drops a handful of frames out of millions and says so beats a
  // run that dies halfway with half-fed aggregators and no rollback.
  // Set to true for byte-exact reproducibility work: the reader then
  // throws LrvxError E_DATA_002 on the first late event, with the
  // symbol, event type, file and offset in the message.
  bool strict_ordering{false};

  // With from_ns or to_ns set, segments whose recorded time bounds prove
  // they hold no event in [from_ns, to_ns] are not decoded, and Sorted
  // segments are entered at the last index entry strictly below from_ns and
  // left at the first event past to_ns. The result is identical either way; false
  // restores the full walk (benchmarks, equivalence tests).
  bool skip_segments_by_time{true};

  // Deliver segments in exchange_ts_ns order instead of file-name order.
  // Segment files are named by the wall-clock time they were opened, so a
  // tape filled out of order (an archive import of day 3, then of day 1)
  // lists them out of time order. With this set, segments whose recorded
  // bounds prove where they start are delivered by their first event, and
  // segments whose ranges overlap are merged event by event. Segments that
  // prove nothing (no Sorted flag, still being written) keep their file-name
  // position. false restores plain file-name order.
  bool order_segments_by_time{true};
};

struct DatasetSummary
{
  std::filesystem::path data_dir;

  int64_t first_event_ns{0};
  int64_t last_event_ns{0};

  uint64_t total_events{0};
  uint32_t segment_count{0};
  uint64_t total_bytes{0};

  std::set<uint32_t> symbols;

  uint32_t segments_with_index{0};
  uint32_t segments_without_index{0};

  bool empty() const { return total_events == 0; }

  std::chrono::nanoseconds duration() const
  {
    return std::chrono::nanoseconds(last_event_ns - first_event_ns);
  }

  double durationSeconds() const
  {
    return static_cast<double>(last_event_ns - first_event_ns) / 1e9;
  }

  double durationMinutes() const { return durationSeconds() / 60.0; }
  double durationHours() const { return durationMinutes() / 60.0; }

  bool fullyIndexed() const { return segments_without_index == 0 && segment_count > 0; }
};

struct ReaderStats
{
  // Segments whose frames were decoded, wholly or in part.
  uint64_t files_read{0};
  // Segments not decoded at all because their time bounds lie outside the
  // reader's [from_ns, to_ns] (see ReaderConfig::skip_segments_by_time).
  uint64_t segments_skipped{0};
  uint64_t events_read{0};
  uint64_t trades_read{0};
  uint64_t book_updates_read{0};
  // Segment-file bytes consumed as event data: each frame (header and payload)
  // of an uncompressed segment, each block (header and compressed payload) of
  // a compressed one. Segment headers, indexes and the segment scan are not
  // counted, and a segment the range skips adds nothing. A full read adds,
  // per segment, the bytes between its header and its index, whichever read
  // path or thread count delivered it.
  uint64_t bytes_read{0};
  uint64_t crc_errors{0};
  // Events discarded because they arrived past reorder_window_ns.
  // Always 0 when ReaderConfig::strict_ordering is set (the reader
  // throws instead).
  uint64_t late_dropped{0};
  // Frames whose type this build does not know. The format is
  // additive, so they are skipped by FrameHeader.size rather than
  // ending the read.
  uint64_t unknown_frames_skipped{0};
  // Segments visited at a different position than their file name gives
  // them (ReaderConfig::order_segments_by_time).
  uint64_t segments_reordered{0};
  // Segments whose events were merged with another segment's because their
  // time ranges overlap.
  uint64_t segments_interleaved{0};
};

struct ReplayEvent
{
  EventType type{};
  int64_t timestamp_ns{0};

  TradeRecord trade{};

  BookRecordHeader book_header{};
  std::vector<BookLevel> bids;
  std::vector<BookLevel> asks;

  OptionQuoteRecord option_quote{};

  // Pool-state record: the fixed header plus the raw u256 payload bytes, carried
  // opaquely for the pool-state-tape layer to interpret.
  PoolStateRecordHeader pool_state_header{};
  std::vector<std::byte> pool_state_payload;

  // Symbol id for the active record, whichever event type this is. Keeps
  // symbol filtering correct as new record types are added.
  uint32_t symbolId() const
  {
    switch (type)
    {
      case EventType::Trade:
        return trade.symbol_id;
      case EventType::OptionQuote:
        return option_quote.symbol_id;
      case EventType::PoolState:
        return pool_state_header.symbol_id;
      default:
        return book_header.symbol_id;
    }
  }
};

struct SegmentInfo
{
  std::filesystem::path path;
  int64_t first_event_ns{0};
  int64_t last_event_ns{0};
  uint32_t event_count{0};
  bool has_index{false};
  uint64_t index_offset{0};
  // Header flags. Whether first/last_event_ns are bounds depends on both:
  // see BinaryLogReader::segmentOutsideRange.
  bool sorted{false};
  bool compressed{false};
  // True when min_event_ns is a proven lower bound on every event in the
  // segment: a Sorted segment, bounded by its first frame (uncompressed) or
  // by its block index (compressed). Segments without the Sorted flag, still
  // being written, or with no usable index are not bounded.
  bool time_bounded{false};
  int64_t min_event_ns{0};
};

class BinaryLogReader
{
 public:
  explicit BinaryLogReader(ReaderConfig config);
  ~BinaryLogReader();

  BinaryLogReader(const BinaryLogReader&) = delete;
  BinaryLogReader& operator=(const BinaryLogReader&) = delete;
  BinaryLogReader(BinaryLogReader&&) noexcept;
  BinaryLogReader& operator=(BinaryLogReader&&) noexcept;

  static DatasetSummary inspect(const std::filesystem::path& data_dir);
  static DatasetSummary inspectWithSymbols(const std::filesystem::path& data_dir);

  DatasetSummary summary();
  uint64_t count();
  std::set<uint32_t> availableSymbols();

  using EventCallback = std::function<bool(const ReplayEvent&)>;

  // Progress reporter invoked from inside `run()` at most once per
  // `interval`. `pct` is in [0.0, 1.0] based on bytes-of-segments
  // processed / total bytes (events-per-tape is not known up front).
  // `cursor_ts_ns` is the exchange timestamp of the most recent
  // dispatched event. Returning `false` requests cancellation —
  // `run()` stops the walk, calls `finalize()` once on every
  // aggregator with whatever partial state has been accumulated, and
  // returns `false` so the caller can distinguish a cancelled run
  // from a successful one. Returning `true` (or omitting the bool by
  // wrapping a `void`-returning lambda inside the std::function) lets
  // the run continue.
  using ProgressCallback = std::function<bool(double pct, int64_t cursor_ts_ns)>;
  void setProgressCallback(
      ProgressCallback cb,
      std::chrono::milliseconds interval = std::chrono::milliseconds(1000));
  void clearProgressCallback();

  // Sorted-order delivery. Segments are visited in read order (see
  // ReaderConfig::order_segments_by_time): Sorted segments are delivered
  // in timestamp order across segments, overlapping ones merged event by
  // event. For segments without the Sorted flag the entire segment is
  // buffered into memory and stable_sort'ed before dispatch —
  // O(N_events × sizeof(ReplayEvent)) per segment; such a segment keeps its
  // file-name position. Use this when the caller requires strict timestamp
  // ordering (replay engine, tape_diff).
  bool forEach(EventCallback callback);
  bool forEachFrom(int64_t start_ts_ns, EventCallback callback);

  // Streaming delivery — never buffers a whole segment. Sorted segments
  // arrive exactly as from forEach. Segments without the Sorted flag go
  // through the bounded reorder buffer (reorder_window_ns) instead of a
  // whole-segment sort, so an event later than the window is dropped.
  //
  // Use this when memory matters more than strict timestamp order.
  // `run(panel)` below uses this path.
  bool streamForEach(EventCallback callback);
  bool streamForEachFrom(int64_t start_ts_ns, EventCallback callback);

  // Single-pass streaming aggregator dispatch. Walks the tape once
  // via `streamForEach`, forwarding each event to every aggregator's
  // onEvent, then calling finalize() on each. An empty span is a
  // no-op and performs no decompression.
  //
  // `n_threads` policy:
  //   0 (the default): auto — `min(segment_count,
  //                              std::thread::hardware_concurrency())`.
  //                    Caller doesn't have to know the tape layout
  //                    or core count; small captures stay
  //                    single-threaded, large captures saturate
  //                    available cores without oversubscription.
  //   1:               explicit single-thread (no clone/merge
  //                    overhead, useful for benchmarking).
  //   >1:              explicit worker count, capped at the segment
  //                    count internally — surplus workers are
  //                    not allocated.
  //
  // Multi-threaded path partitions the segment list across that
  // many worker threads. Each worker clones the panel via
  // `IAggregator::cloneEmpty()`, runs its segment slice, and the
  // reader then merges every worker's panel into the caller's
  // original instances via `IAggregator::merge()` before calling
  // finalize() once on each.
  //
  // Sliding-window aggregators (Peak / Quantile) cannot observe
  // windows that straddle worker boundaries. See
  // `IAggregator::merge` for the boundary semantics discussion.
  bool run(std::span<IAggregator* const> aggregators,
           std::size_t n_threads = 0);

  std::optional<std::pair<int64_t, int64_t>> timeRange() const;
  ReaderStats stats() const;
  // Segments in file-name order.
  std::vector<std::filesystem::path> segmentFiles() const;
  const std::vector<SegmentInfo>& segments() const;
  // Segments in the order the reader visits them (see
  // ReaderConfig::order_segments_by_time). Segments whose ranges overlap are
  // listed by their first event; the reader interleaves their events.
  std::vector<SegmentInfo> segmentsInReadOrder() const;

 private:
  // A slice of _read_order: either one segment read on its own, or a run of
  // time-bounded Sorted segments merged by mergeSortedRun.
  struct ReadGroup
  {
    size_t begin{0};
    size_t end{0};
    bool merge{false};
  };

  bool scanSegments();

  // Build the segment index exactly once, safely, from a const context.
  // The index is immutable after the first scan, so const readers may then run
  // concurrently; without this the first scan could race a reader on another
  // thread (a replay loop scanning lazily while the owner asks for timeRange).
  bool ensureScanned() const;
  bool readSegment(const std::filesystem::path& path, EventCallback& callback);
  bool readSegmentFrom(const SegmentInfo& segment, int64_t start_ts_ns, EventCallback& callback);
  bool readSegmentStreaming(const std::filesystem::path& path, EventCallback& callback);
  bool readSegmentStreamingFrom(const SegmentInfo& segment, int64_t start_ts_ns,
                                EventCallback& callback);

  // Fills _read_order / _read_groups from _segments.
  void planReadOrder();
  // Walks the tape in read order. `buffered` selects how segments without
  // time bounds are read: whole-segment sort (forEach) or the bounded reorder
  // buffer (streamForEach). `start` is the *From entry points' start.
  bool walk(bool buffered, std::optional<int64_t> start, EventCallback& callback);
  // Delivers a run of time-bounded Sorted segments, sorted by first event,
  // as one stream ordered by (timestamp, file order, position).
  bool mergeSortedRun(const ReadGroup& run, std::optional<int64_t> start,
                      EventCallback& callback);
  // Counts ReaderStats::segments_reordered for the segment at `read_pos`.
  void noteVisited(size_t read_pos);
  // Streams a single segment using the same intra-block sort + bounded
  // reorder buffer logic as readSegmentStreaming, but writes counter
  // updates into the caller-supplied ReaderStats. Used by parallel
  // run(panel, n_threads) so workers accumulate into thread-local
  // stats without locking shared _stats. The single-threaded
  // readSegmentStreaming delegates to this with `_stats`.
  bool streamSegmentWithStats(const std::filesystem::path& path,
                              EventCallback& callback, ReaderStats& stats);
  bool passesFilter(const ReplayEvent& event) const;

  bool rangeSkipActive() const;
  // True when the segment's proven bounds, recorded at scan time, place it
  // outside the configured range: an uncompressed Sorted segment by its first
  // frame and last event, a compressed Sorted one by its block index minimum.
  bool segmentOutsideRange(const SegmentInfo& segment) const;
  // For a Sorted segment just opened: positions `iter` at the index entry
  // nearest from_ns and sets `stop_past_to` when the segment is monotonic, so
  // the walk may end at the first event past to_ns. Returns false when the
  // segment's index proves it holds nothing in range.
  bool narrowToRange(BinaryLogIterator& iter, bool& stop_past_to) const;

  ReaderConfig _config;
  ReaderStats _stats;
  std::vector<SegmentInfo> _segments;
  std::vector<size_t> _read_order;
  std::vector<ReadGroup> _read_groups;
  bool _scanned{false};
  mutable std::once_flag _scan_once;
  ProgressCallback _progress_cb;
  std::chrono::milliseconds _progress_interval{1000};
  // Ordering watermark for one streaming walk, carried from segment to
  // segment. Restarting it per segment hid every inversion that straddled a
  // segment boundary. Each public streaming entry point resets it.
  int64_t _stream_watermark{std::numeric_limits<int64_t>::min()};
};

class BinaryLogIterator
{
 public:
  explicit BinaryLogIterator(const std::filesystem::path& segment_path);
  ~BinaryLogIterator();

  BinaryLogIterator(const BinaryLogIterator&) = delete;
  BinaryLogIterator& operator=(const BinaryLogIterator&) = delete;

  bool next(ReplayEvent& out);

  const SegmentHeader& header() const { return _header; }
  bool isValid() const { return _file != nullptr; }
  bool isCompressed() const { return _header.isCompressed(); }

  // Moves to the last index entry strictly below the target, so reading on and
  // skipping earlier events yields every event at or after it. Only a Sorted
  // segment with a usable index is positioned; otherwise returns false and
  // leaves the iterator at the first event.
  bool seekToTimestamp(int64_t target_ts_ns);
  bool loadIndex();
  // Answers from the segment header, the way MmapSegmentReader::hasIndex
  // does. Reporting "no index" until loadIndex() had run left every
  // `if (hasIndex()) seek(...)` branch in the readers unreachable.
  bool hasIndex() const { return _header.hasIndex(); }
  bool indexLoaded() const { return !_index_entries.empty(); }
  std::span<const IndexEntry> indexEntries() const { return _index_entries; }

  // Moves to a frame (uncompressed) or block (compressed) boundary taken
  // from the index and drops any decompressed block held.
  bool seekToFileOffset(uint64_t file_offset);

  // Block-level positioning for intra-segment parallel workers. The
  // caller is responsible for picking valid block boundaries (use
  // `scanBlockOffsets` on the reader side to discover them); the
  // iterator does not verify the offset points at a real
  // CompressedBlockHeader. `setStopAtBlockEnd(true)` causes next()
  // to return false at the end of the current block instead of
  // auto-advancing to the next block — workers use this to confine
  // themselves to their assigned block range.
  bool seekToBlockOffset(uint64_t file_offset);
  void setStopAtBlockEnd(bool stop) { _stop_at_block_end = stop; }

  // Frames rejected because the payload CRC did not match the frame
  // header, and frames skipped because their type is unknown to this
  // build. Both are counted across the iterator's lifetime.
  uint64_t crcErrors() const { return _crc_errors; }
  uint64_t unknownFramesSkipped() const { return _unknown_frames_skipped; }

  // Event-data bytes read from the file so far: each whole frame (header and
  // payload) of an uncompressed segment, each whole block (header and
  // compressed payload) of a compressed one. The segment header and the index
  // are not counted. This is what ReaderStats::bytes_read sums.
  uint64_t bytesConsumed() const { return _bytes_consumed; }

  // File offset the event returned by the last successful next() came from:
  // the frame offset on an uncompressed segment, the enclosing block's offset
  // on a compressed one. Diagnostics only -- it gives a late or malformed
  // event a place in the file the reader can name.
  uint64_t lastEventOffset() const { return _last_event_offset; }

 private:
  bool nextUncompressed(ReplayEvent& out);
  bool nextCompressed(ReplayEvent& out);
  bool loadNextBlock();
  bool parseFrame(EventType type, const std::byte* data, size_t size, ReplayEvent& out);
  void discardBlockState();

  std::FILE* _file{nullptr};
  SegmentHeader _header{};
  std::vector<std::byte> _payload_buffer;
  std::vector<IndexEntry> _index_entries;

  std::vector<std::byte> _block_data;
  // Scratch buffer reused across sortBlockFramesInPlace calls. Empty
  // when the block was already monotonic (fast path) — only allocated
  // on the first unsorted block of the iterator's lifetime.
  std::vector<std::byte> _sort_scratch;
  size_t _block_offset{0};
  size_t _block_events_remaining{0};
  // Worker-mode flag: when true, `next()` returns false at the end of
  // the current block instead of calling loadNextBlock to advance.
  // Lets intra-segment parallel workers confine themselves to an
  // assigned block range.
  bool _stop_at_block_end{false};

  uint64_t _crc_errors{0};
  uint64_t _unknown_frames_skipped{0};
  uint64_t _bytes_consumed{0};
  uint64_t _last_event_offset{0};
  uint64_t _block_file_offset{0};
};

}  // namespace lrvx::replay
