# BinaryLogReader

`BinaryLogReader` provides sequential access to market data stored in the binary log format. It handles segment discovery, time filtering, symbol filtering, and CRC verification.

```cpp
struct ReaderConfig
{
  std::filesystem::path data_dir;
  std::optional<int64_t> from_ns;
  std::optional<int64_t> to_ns;
  std::set<uint32_t> symbols;
  bool verify_crc{true};
  int64_t reorder_window_ns{10'000'000'000};  // 10s
  bool strict_ordering{false};
  bool skip_segments_by_time{true};
};

class BinaryLogReader
{
public:
  explicit BinaryLogReader(ReaderConfig config);

  // Static inspection (no event reading)
  static DatasetSummary inspect(const std::filesystem::path& data_dir);
  static DatasetSummary inspectWithSymbols(const std::filesystem::path& data_dir);

  // Instance methods
  DatasetSummary summary();
  uint64_t count();
  std::set<uint32_t> availableSymbols();

  // Iteration
  using EventCallback = std::function<bool(const ReplayEvent&)>;
  bool forEach(EventCallback callback);
  bool forEachFrom(int64_t start_ts_ns, EventCallback callback);

  // Progress reporting. The callback returns false to cancel the run; forEach
  // then returns false so the caller can tell a cancelled run from a completed
  // one. Returning true continues.
  using ProgressCallback = std::function<bool(double pct, int64_t cursor_ts_ns)>;
  void setProgressCallback(
      ProgressCallback cb,
      std::chrono::milliseconds interval = std::chrono::milliseconds(1000));
  void clearProgressCallback();

  // Metadata
  std::optional<std::pair<int64_t, int64_t>> timeRange() const;
  ReaderStats stats() const;
  std::vector<std::filesystem::path> segmentFiles() const;
  const std::vector<SegmentInfo>& segments() const;
  std::vector<SegmentInfo> segmentsInReadOrder() const;
};
```

## Purpose

* Read market data from binary log segments in chronological order.
* Filter events by time range and symbol set.
* Support both full scans and timestamp-based seeks.

## Configuration

| Field      | Type                    | Description                              |
|------------|-------------------------|------------------------------------------|
| data_dir   | `filesystem::path`      | Directory containing `.lrvx` files    |
| from_ns    | `optional<int64_t>`     | Start timestamp filter (inclusive)       |
| to_ns      | `optional<int64_t>`     | End timestamp filter (inclusive)         |
| symbols    | `set<uint32_t>`         | Symbol IDs to include (empty = all)      |
| verify_crc | `bool`                  | Check each frame's payload against its CRC32 and skip a frame that fails (default: true). `false` delivers such a frame as it parses. See [ReaderStats](#readerstats) for the cost |
| reorder_window_ns | `int64_t`        | Bounded reorder buffer, default 10 s. Events arriving more than this far behind the emit cursor cannot be placed in order any more, so the reader throws `LrvxError` with the observed delta. The default covers exchange-WS jitter and the 99th percentile of reconnect-induced cross-block inversions on real tapes. Memory bound is roughly `reorder_window_ns x peak_event_rate x sizeof(ReplayEvent)` — about 36 MB at 10 s and a 10k ev/s burst |
| skip_segments_by_time | `bool`           | With `from_ns` or `to_ns` set, do not decode segments that provably hold nothing in range (default: true). See [Time-ranged reads](#time-ranged-reads). The events returned are the same either way |
| order_segments_by_time | `bool`          | Read segments in time order of their first event and merge overlapping ones (default: true). `false` reads them in file-name order. See [Ordering guarantee](#ordering-guarantee) |

## Core Methods

| Method             | Description                                              |
|--------------------|----------------------------------------------------------|
| `inspect()`        | Static scan of directory, returns metadata without reading events |
| `inspectWithSymbols()` | Like `inspect()` but also collects symbol IDs        |
| `summary()`        | Returns dataset metadata after scanning                  |
| `count()`          | Returns total event count across all segments            |
| `forEach()`        | Iterate all events matching filters                      |
| `forEachFrom()`    | Iterate events starting from a timestamp                 |
| `setProgressCallback()` | Install a progress callback for `run(aggregators, 1)`, invoked at most once per `interval` with the fraction of events delivered and the timestamp of the last event delivered. A run that completes ends with one more call at `1.0` and that same timestamp. Return `false` from it to cancel; `run` then returns `false` |
| `clearProgressCallback()` | Remove the progress callback                        |
| `timeRange()`      | Returns the earliest and latest event timestamp; see [Reported time range](#reported-time-range) |
| `stats()`          | Returns read statistics (events, bytes, errors)          |
| `segmentFiles()`   | Returns segment file paths in file-name order            |
| `segments()`       | Returns detailed segment information                     |

## Data Structures

### DatasetSummary

```cpp
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

  // Helper methods
  bool empty() const;
  std::chrono::nanoseconds duration() const;
  double durationSeconds() const;
  double durationMinutes() const;
  double durationHours() const;
  bool fullyIndexed() const;
};
```

#### Reported time range

`first_event_ns` / `last_event_ns` of `summary()` and `inspect()`, and the
pair `timeRange()` returns, are the earliest and latest non-zero
`exchange_ts_ns` over the segments, taken per segment as cheaply as the
segment allows:

* **Uncompressed segment flagged `Sorted`:** its header, which is exact (its
  events are in non-decreasing order). No extra I/O.
* **Compressed segment flagged `Sorted`, with a usable index:** the first
  index entry (the smallest block minimum) and the maximum of its last block,
  which is exact. Costs reading the index and decompressing that one block,
  once per segment per reader.
* **Any other segment** (no `Sorted` flag, no usable index): the first and
  last event it *wrote*, from its header (or recovered from its blocks while
  the header is still zero). Arrival order, not its extremes: those would take
  a walk of the whole segment.

So on a tape whose segments are all `Sorted` the range is exact. A segment
header's own `first_event_ns` / `last_event_ns` (what `SegmentInfo` carries)
are always the first and last event written, which on a compressed segment can
sit inside the segment's range, because the writer sorts each block before
writing it.

`inspectWithSymbols()` walks every event to collect the symbols, and reports
the exact range from that walk whatever the segments' flags.

### ReplayEvent

```cpp
struct ReplayEvent
{
  EventType type;           // Trade, BookSnapshot, BookDelta, OptionQuote, PoolState
  int64_t timestamp_ns;     // Event timestamp

  TradeRecord trade;        // Populated for Trade events

  BookRecordHeader book_header;  // Populated for Book events
  std::vector<BookLevel> bids;
  std::vector<BookLevel> asks;

  OptionQuoteRecord option_quote;  // Populated for OptionQuote events

  // Pool-state record: the fixed header plus the raw u256 payload bytes,
  // carried opaquely for the pool-state-tape layer to interpret.
  PoolStateRecordHeader pool_state_header;
  std::vector<std::byte> pool_state_payload;

  // Symbol id for the active record, whichever event type this is. Keeps symbol
  // filtering correct as new record types are added.
  uint32_t symbolId() const;
};
```

Read the symbol through `symbolId()` rather than reaching into a per-type record; it switches on
`type` and stays correct as record types are added.

### ReaderStats

```cpp
struct ReaderStats
{
  uint64_t files_read{0};             // segments decoded, wholly or in part
  uint64_t segments_skipped{0};       // segments not decoded: outside [from_ns, to_ns]
  uint64_t events_read{0};
  uint64_t trades_read{0};
  uint64_t book_updates_read{0};
  uint64_t bytes_read{0};             // event-data bytes read from segment files
  uint64_t crc_errors{0};             // frames skipped: CRC mismatch or unparseable payload
  uint64_t late_dropped{0};           // events past reorder_window_ns, dropped
  uint64_t unknown_frames_skipped{0}; // frames of a record type this build does not know
  uint64_t segments_reordered{0};     // see Ordering guarantee
  uint64_t segments_interleaved{0};   // see Ordering guarantee
  uint64_t segments_truncated{0};     // segments cut short by undecodable data
};
```

A frame whose payload does not match its CRC is skipped and counted in
`crc_errors`, and the read goes on with the next frame; nothing is thrown. A
frame of a known type whose payload is too short to parse is counted the same
way. A frame of a type this build does not know is stepped over by its length
and counted in `unknown_frames_skipped`.

Data that cannot be decoded ends the segment instead, since no next frame can
be found past it: a compressed block that fails to decompress, has an invalid
header, sizes above 100 MiB or fewer frames than its header counts; an
uncompressed frame whose length is above 10 MiB; and a segment with an index
whose file ends before it. The reader delivers the events before that point,
counts the segment in `segments_truncated`, and goes on with the next segment;
the read still returns `true`. `run(aggregators, n)` with several workers on
one segment delivers the same events: the ranges after the worker that met
the damage are dropped. A segment without an index is still being written, so
a frame or block cut short at its end is its live tail and is not counted. The
validator reports every segment counted here as damaged.

`ReaderConfig::verify_crc` (default `true`) switches the CRC check on every
read path, `ParallelReader` included. With it off a damaged payload is
delivered as whatever it parses to, provided it is long enough for its record
type; the segment index's own CRC is checked either way. On a 60-segment tape
of 6 million trades (Apple M-series, one thread, `streamForEach`), the check
costs 0.75 s against 0.42 s without it uncompressed, and 0.37 s against 0.13 s
with LZ4. Every read path counts these, at any thread count.

`bytes_read` counts the segment-file bytes a read consumed as event data:
every frame (header and payload) of an uncompressed segment, every block
(header and compressed payload) of a compressed one. Segment headers, the
index and the directory scan are not counted, and a segment the range skips
adds nothing. A full read therefore adds, per segment, the bytes between its
header and its index, whichever of `forEach`, `forEachFrom`,
`streamForEach`, `streamForEachFrom` or `run` (any thread count) delivered
it. Counters accumulate across reads on the same reader.

The metadata accessors (`segmentFiles()`, `segments()`,
`segmentsInReadOrder()`, `timeRange()`, `count()`, `summary()`) scan the
directory on first use, so a freshly constructed reader answers them
without a prior read.

### SegmentInfo

```cpp
struct SegmentInfo
{
  std::filesystem::path path;
  int64_t first_event_ns{0};
  int64_t last_event_ns{0};
  uint32_t event_count{0};
  bool has_index{false};
  uint64_t index_offset{0};
  bool sorted{false};      // SegmentFlags::Sorted
  bool compressed{false};  // SegmentFlags::Compressed
};
```

## Time-ranged reads

With `from_ns` or `to_ns` set, `forEach`, `streamForEach` and `run` leave out
segments whose recorded time bounds lie outside the range. `forEachFrom` and
`streamForEachFrom` pass over a segment only when its proven maximum (first
case below) lies before the start. Only bounds the writer guarantees are used,
so the result is exactly the full read filtered to the range:

* **Uncompressed segment flagged `Sorted`.** Written in non-decreasing
  `exchange_ts_ns` order, so its first frame and the header's `last_event_ns`
  are its minimum and maximum. A segment outside the range is not opened. (The
  header's `first_event_ns` is not used: the writer records it from the first
  non-zero timestamp, past any leading events stamped 0.)
* **Compressed segment flagged `Sorted`.** The header records the first and
  last events in arrival order, which the writer re-sorts inside each block, so
  it is not a bound. The block index is: each entry holds its block's minimum
  and the blocks do not overlap. A segment whose first block starts after
  `to_ns` is not decoded; one that ends before `from_ns` costs its last block,
  since a block's maximum is not on disk: the segment is opened, its index
  read and that block decompressed (about 70 us per segment for 1000-event
  blocks; a range at the end of a 60-segment LZ4 tape reads in 6.6 ms against
  2.4 ms at its start). No exact upper bound is available without a format
  change: the next segment's minimum bounds nothing unless the segments are
  already known to be disjoint, and caching the last block's maximum would
  only help a second read on the same reader. Not used when the first
  block's minimum is `<= 0`, where writers before the current one did not
  check block order.
* **Any other segment** (no `Sorted` flag, still being written, no usable
  index) is read in full, as before.

Inside a `Sorted` segment the reader also starts at the last index entry
strictly below `from_ns` (or the `*From` start) and stops at the first event
past `to_ns`. Strictly below: when several events share the start timestamp,
an entry stamped with it can have some of them before it. A segment without
the `Sorted` flag is never entered by its index, whose entries are in arrival
order. Bounds and the range filter are both on `exchange_ts_ns`.
`stats().segments_skipped` counts the segments left out;
`skip_segments_by_time = false` restores the full walk.

## Usage

```cpp
replay::ReaderConfig config{
    .data_dir = "/data/market",
    .from_ns = start_timestamp,
    .to_ns = end_timestamp,
    .symbols = {1, 2, 3}
};

replay::BinaryLogReader reader(config);

reader.forEach([](const replay::ReplayEvent& event) {
    if (event.type == replay::EventType::Trade) {
        // Process trade
    } else {
        // Process book update
    }
    return true;  // Continue iteration
});
```

## Time Utilities

The `time_utils` namespace provides helper functions:

```cpp
namespace replay::time_utils
{
  int64_t toNanos(std::chrono::system_clock::time_point tp);
  std::chrono::system_clock::time_point fromNanos(int64_t ns);
  int64_t nowNanos();
  int64_t secondsToNanos(int64_t seconds);
  int64_t millisToNanos(int64_t millis);
  int64_t microsToNanos(int64_t micros);
  double nanosToSeconds(int64_t ns);
}
```

## BinaryLogIterator

Low-level iterator for reading a single segment file:

```cpp
class BinaryLogIterator
{
public:
  explicit BinaryLogIterator(const std::filesystem::path& segment_path);

  bool next(ReplayEvent& out);
  bool seekToTimestamp(int64_t target_ts_ns);
  bool loadIndex();

  const SegmentHeader& header() const;
  bool isValid() const;
  bool isCompressed() const;
  bool hasIndex() const;

  void setVerifyCrc(bool verify);  // default true
  uint64_t crcErrors() const;
  uint64_t unknownFramesSkipped() const;
  bool truncated() const;          // stopped on undecodable data
};
```

`seekToTimestamp(t)` moves to the last index entry strictly below `t`, so that
reading on from there and skipping events below `t` yields every event at or
after `t`. It positions only a `Sorted` segment with a usable index (entries
ascending, starting at the first frame; on a compressed segment, a first block
minimum above 0). Otherwise it returns `false` and leaves the iterator at the
first event, so the caller reads the whole segment.

## Ordering guarantee

Segment files are named by the wall-clock time they were opened. A tape filled out of order, such as an archive import of day 3 followed by one of day 1, therefore lists its segments out of time order. The reader does not read them in file-name order. It orders them by time wherever a segment's recorded bounds prove where it starts.

**Time-bounded segments.** A segment flagged `Sorted` has a proven first event:

* uncompressed: its first frame, since its frames are in non-decreasing order;
* compressed: the smallest entry of its block index, since each entry is a block's minimum. A compressed segment without a usable index (none written, failed CRC) is not bounded.

Between two segments that are not bounded, the bounded segments form a run. Within a run, `forEach`, `forEachFrom`, `streamForEach`, `streamForEachFrom` and `run(aggregators, 1)` deliver every event in non-decreasing `timestamp_ns` order:

* Segments are sorted by first event.
* Segments whose ranges overlap are merged event by event. Each overlapping segment has one cursor open (one decompressed block for a compressed segment); nothing is buffered beyond that. Segments overlapping one point in time are all open at once, so a tape of very many fully overlapping segments needs as many open files: past the process's open-file limit the read returns `false` and logs the segment it could not open.
* Equal timestamps go first to the segment whose file name sorts first, then to the earlier event inside the segment.
* A tape recorded in order is read exactly as in file-name order: the same events, in the same order, from the same segments. A segment is opened only when the previous one is exhausted, and a `false` return from the callback stops at once.

**Segments that are not bounded** keep the position their file name gives them, and each one ends a run. These are segments without the `Sorted` flag: tapes from external writers, and a segment still being written or left by a writer that was killed before closing it. Events inside such a segment are put in order:

* `forEach` / `forEachFrom` buffer the whole segment and sort it: O(segment events) memory, and early exit stops delivery after the segment has been read in full.
* the streaming paths use the bounded reorder buffer (`reorder_window_ns`).

Either way, events sharing a timestamp keep the order they have in the segment.

Nothing orders such a segment against the runs around it. If an unsorted segment or a later run starts before the previous run ends, time steps back at that boundary. A live recording by one writer is not affected: the segment still being written is the newest file and holds the newest events, so it stays last.

`run(aggregators, n)` with more than one thread (and `n = 0`, the default, which picks the thread count) visits segments in the same order and delivers in the same order guarantee as `run(aggregators, 1)`:

* A run of `Sorted` segments whose ranges overlap is merged event by event, on the calling thread, straight into the caller's aggregators, as `run(aggregators, 1)` merges it. Overlap is judged on proven bounds: a segment overlaps when its first event lies strictly before the latest event of the segments before it in the run (exact for every `Sorted` segment; costs one block read per compressed one). Segments that only touch, the last event of one stamped like the first of the next, are not merged.
* Every other segment is partitioned: its blocks are split into contiguous ranges, one per worker, each worker feeds a clone of the panel (`IAggregator::cloneEmpty`) its range in time order, and the clones are merged into the caller's aggregators (`IAggregator::merge`) before the next segment. A segment with fewer than two blocks per worker, and any uncompressed segment, is read on the calling thread.

So every aggregator instance sees its events in non-decreasing time order wherever `run(aggregators, 1)` delivers them that way, and the caller's instances receive the clones' partial results in time order of their slices. A tape recorded in order has no overlapping run and is partitioned exactly as before. What partitioning still changes is documented at `IAggregator::merge`: a sliding-window aggregator does not see windows that straddle a slice boundary.

A segment holding no events (left by a writer killed right after opening it, or by an older compressed writer that rotated on its last block) takes no place in the order: it sits in the run around it, at its file-name position, and neither ends nor splits it.

`stats().segments_reordered` counts the segments visited at a different position than their file name gives them. `stats().segments_interleaved` counts the segments whose events were merged with another segment's. `order_segments_by_time = false` restores plain file-name order. `segmentsInReadOrder()` lists the segments in the order they are visited; `segments()` keeps file-name order.

`MergedTapeReader::streamEvents` walks each tape's segments in this read order. It merges overlapping segments of one tape only inside its per-tape reorder window. With `from_ns` / `to_ns` it leaves out a segment only on the proven bounds above, and the first event past `to_ns` ends the current `Sorted` segment, not the tape.

`ParallelReader` and `MmapReader` visit segments in the same read order (`segmentsInReadOrder()`); see their pages for what each delivers.

`BinaryLogIterator` is a low-level block-by-block streaming API with **no ordering guarantee**. Use `BinaryLogReader` when ordered output is required.

## Notes

* Segments are discovered automatically and read in time order (see [Ordering guarantee](#ordering-guarantee)).
* Compressed segments (LZ4) are transparently decompressed.
* Seeking uses segment indexes when available for O(log n) lookup.
* The callback returning `false` stops iteration early.
* File extension is `.lrvx`.

## Live-tail safety

Active segments — those whose writer is still appending — are safe to read. The writer fflushes after each compressed block and refreshes the segment header, so a snapshot of the file taken mid-run (e.g. via `rsync`) contains complete blocks and a non-zero `SegmentHeader`.

If the header is still zero-initialized at the time of read (`event_count == 0` on a compressed segment), `scanSegments()` and `inspect()` recover the metadata: every `CompressedBlockHeader` is walked to sum `event_count`, and the first / last viable blocks are decompressed for `first_event_ns` / `last_event_ns`. The very last block is often truncated because the writer flushes its block header before the compressed payload is fully written, so the scan iterates backwards until one block decompresses successfully.
