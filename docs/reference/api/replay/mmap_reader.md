# MmapReader

`MmapReader` provides memory-mapped access to binary log segments for zero-copy reads. Cross-platform implementation supports both POSIX (Linux/macOS) and Windows.

```cpp
class MmapSegmentReader
{
public:
  explicit MmapSegmentReader(const std::filesystem::path& segment_path);
  ~MmapSegmentReader();

  MmapSegmentReader(MmapSegmentReader&&) noexcept;
  MmapSegmentReader& operator=(MmapSegmentReader&&) noexcept;

  bool isValid() const;
  bool isCompressed() const;
  bool hasIndex() const;

  const SegmentHeader& header() const;

  const std::byte* data() const;
  size_t dataSize() const;
  size_t totalSize() const;

  bool next(ReplayEvent& out);
  void reset();
  bool seekToTimestamp(int64_t target_ts_ns);
  size_t position() const;

  const FrameHeader* currentFrame() const;
  bool advanceFrame();

  bool loadIndex();
  const std::vector<IndexEntry>& indexEntries() const;

  void setVerifyCrc(bool verify);
  uint64_t crcErrors() const;
  bool truncated() const;
};
```

## Purpose

* Provide zero-copy access to segment data via memory mapping.
* Enable direct pointer access to event records without buffer copying.
* Support fast timestamp-based seeking using segment indexes.

`MmapSegmentReader::next()` reads frames the way `BinaryLogIterator` does on an uncompressed segment. It checks each payload against the frame's CRC32 unless `setVerifyCrc(false)` was called, and skips a frame that fails or whose payload is too short for its record type, counting it in `crcErrors()`. It walks past a frame of a type the build does not know. It stops and sets `truncated()` on bytes it cannot walk past: a frame longer than the 10 MiB ceiling, or, in a segment with an index, data that ends before the index. A segment without an index runs to the end of the file, so a partial last frame there is the tail of a file still being written and ends the read without setting `truncated()`. `reset()` clears both counters. `currentFrame()` and `advanceFrame()` walk the raw frames with no check.

`MmapSegmentReader::seekToTimestamp(t)` follows `BinaryLogIterator::seekToTimestamp`: it moves to the last index entry strictly below `t`, and only in a segment flagged `Sorted` whose index is usable. Otherwise it returns `false` and goes back to the first event.

## MmapReader (Multi-Segment)

```cpp
class MmapReader
{
public:
  struct Config
  {
    std::filesystem::path data_dir;
    bool preload_index{true};
    bool prefault_pages{false};
    std::optional<int64_t> from_ns;
    std::optional<int64_t> to_ns;
    std::set<uint32_t> symbols;
    bool verify_crc{true};
  };

  explicit MmapReader(const Config& config);

  using EventCallback = std::function<bool(const ReplayEvent&)>;
  uint64_t forEach(EventCallback callback);
  uint64_t forEachFrom(int64_t start_ts_ns, EventCallback callback);

  using RawTradeCallback = std::function<bool(const TradeRecord*)>;
  uint64_t forEachRawTrade(RawTradeCallback callback);

  MmapReaderStats stats() const;
  const std::vector<SegmentInfo>& segments() const;
  uint64_t totalEvents() const;
};
```

## Configuration

| Field          | Type                 | Description                              |
|----------------|----------------------|------------------------------------------|
| data_dir       | `filesystem::path`   | Directory containing segments            |
| preload_index  | `bool`               | Load indexes on construction             |
| prefault_pages | `bool`               | Touch pages to prefault into memory      |
| from_ns        | `optional<int64_t>`  | Start timestamp filter                   |
| to_ns          | `optional<int64_t>`  | End timestamp filter                     |
| symbols        | `set<uint32_t>`      | Symbol IDs to include                    |
| verify_crc     | `bool`               | Check each frame against its CRC32 and skip one that fails (default `true`, as `ReaderConfig::verify_crc`) |

`verify_crc` applies to `forEach`, `forEachFrom` and `forEachRawTrade`. `createMultiSegmentReader(dir, filter, false, /*mmap=*/true)` and `createSegmentReader(path)` (which prefers the mapped reader for an uncompressed segment) build readers with the default, so they check too. With the hardware CRC32 path on AArch64, checking costs about 9% of a full mapped read (13.8 ms against 12.7 ms for 1.2 M events in 60 segments on an Apple M-series core); on a target that computes the CRC from the table it is several times the read itself, and `verify_crc = false` is the unchecked fast path.

## Statistics

```cpp
struct MmapReaderStats
{
  uint64_t segments_mapped;
  uint64_t bytes_mapped;
  uint64_t events_read;
  uint64_t page_faults;
  uint64_t crc_errors;
  uint64_t segments_truncated;
};
```

`crc_errors` counts frames skipped for a CRC mismatch or a payload too short for its record type, and `segments_truncated` counts segment reads that stopped on data they could not walk past, with the meaning `ReaderStats` gives them. Both add up over every read of the reader. `events_read` is the count of the last read.

## Usage

```cpp
replay::MmapReader::Config config{
    .data_dir = "/data/market",
    .preload_index = true
};

replay::MmapReader reader(config);

reader.forEach([](const replay::ReplayEvent& event) {
    // Process event
    return true;
});

// Trade records only, without decoding the other frames
reader.forEachRawTrade([](const replay::TradeRecord* trade) {
    // `trade` points at an aligned copy of the mapped record, valid for the call
    return true;
});
```

## Quick Count

```cpp
uint64_t count = replay::mmapCount("/data/market");
```

## Platform Support

| Platform | Implementation                                    |
|----------|---------------------------------------------------|
| Linux    | `mmap()` / `munmap()` / `madvise()`               |
| macOS    | `mmap()` / `munmap()` / `madvise()`               |
| Windows  | `CreateFileMapping()` / `MapViewOfFile()`         |

## Notes

* Memory-mapped I/O avoids kernel-to-userspace copies.
* Segments remain mapped until the reader is destroyed.
* `prefault_pages` can improve first-access latency at the cost of startup time.
* Does not support compressed segments (use `BinaryLogReader` for those).

## Ordering

`MmapReader` visits segments in `BinaryLogReader::segmentsInReadOrder()` order: by proven first event where a segment proves one, by file name otherwise (see [BinaryLogReader, Ordering guarantee](binary_log_reader.md#ordering-guarantee)). `segments()` lists them in that order. Each segment's events are delivered as stored, one segment after another:

* A tape of uncompressed `Sorted` segments whose time ranges do not overlap arrives in timestamp order.
* Segments whose ranges overlap are not merged; time steps back where the next one starts.
* A segment without the `Sorted` flag is not sorted.

Use `BinaryLogReader` when ordered output is required. `forEachFrom(start)` passes over a segment only when it is `Sorted` and its last event lies before `start`, and enters a `Sorted` segment at its last index entry strictly below `start`.
