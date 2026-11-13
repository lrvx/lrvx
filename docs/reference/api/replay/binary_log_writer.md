# BinaryLogWriter

`BinaryLogWriter` writes market data to binary log segments. It handles segment rotation, compression, indexing, and CRC checksums.

```cpp
// Callback for custom segment naming on rotation
using RotationCallback = std::filesystem::path (*)(
    void* user_data,
    const std::filesystem::path& output_dir,
    uint32_t segment_number);

struct WriterConfig {
  std::filesystem::path output_dir;
  std::string output_filename;              // Optional: first segment name
  uint64_t max_segment_bytes{256ull << 20}; // 256 MB default
  uint64_t buffer_size{64ull << 10};        // 64 KB buffer
  uint8_t exchange_id{0};
  bool sync_on_rotate{true};
  bool create_index{true};
  uint16_t index_interval{1000};            // Events per index entry
  CompressionType compression{CompressionType::None};
  uint64_t stdio_buffer_size{64ull << 10};   // setvbuf size; 0 = system default 8 KB
  RotationCallback rotation_callback{nullptr};  // Custom naming on rotation
  void* rotation_user_data{nullptr};            // User data for callback
  std::optional<RecordingMetadata> metadata;    // Writes metadata.json when set
};

class BinaryLogWriter {
public:
  explicit BinaryLogWriter(WriterConfig config);
  ~BinaryLogWriter();

  // Move-only: copy construction and copy assignment are deleted.
  BinaryLogWriter(BinaryLogWriter&&) noexcept;
  BinaryLogWriter& operator=(BinaryLogWriter&&) noexcept;

  bool writeTrade(const TradeRecord& trade);
  bool writeOptionQuote(const OptionQuoteRecord& quote);
  bool writePoolState(const PoolStateRecordHeader& header, const void* payload,
                      size_t payload_size);
  bool writeBook(const BookRecordHeader& header,
                 std::span<const BookLevel> bids,
                 std::span<const BookLevel> asks);

  void flush();
  void close();

  WriterStats stats() const;
  std::filesystem::path currentSegmentPath() const;

  // Metadata
  void setMetadata(const RecordingMetadata& meta);
  RecordingMetadata* metadata();          // nullptr when unset
  void addSymbol(const SymbolInfo& symbol);
  void setHasTrades(bool v);
  void setHasBookSnapshots(bool v);
  void setHasBookDeltas(bool v);
};
```

## Purpose

* Write market data (trades and book updates) to compact binary segments.
* Support automatic segment rotation, compression, and indexing.
* Provide high-throughput, low-latency recording for live market data.

## Configuration

| Field | Default | Description |
|-------|---------|-------------|
| `output_dir` | - | Directory for segment files. |
| `output_filename` | - | Optional filename for first segment. |
| `max_segment_bytes` | 256 MB (`kDefaultMaxSegmentBytes`) | Maximum segment size before rotation. `0` is read as the default by the writer's constructor, so it means the same through every binding and the C API. |
| `buffer_size` | 64 KB | Internal write buffer size. |
| `exchange_id` | 0 | Exchange identifier in segment header. |
| `sync_on_rotate` | true | Sync to disk on segment rotation. |
| `create_index` | true | Build seek index in segments. |
| `index_interval` | 1000 | Events between index entries. |
| `compression` | None | Compression type (`None` or `LZ4`). |
| `rotation_callback` | nullptr | Custom callback for segment naming on rotation. |
| `rotation_user_data` | nullptr | User data passed to rotation callback. |
| `stdio_buffer_size` | 64 KB | `setvbuf` size. `0` falls back to the system default of 8 KB. |
| `metadata` | unset | Optional `RecordingMetadata`. When set, a `metadata.json` is created in `output_dir`. |

## Methods

| Method | Description |
|--------|-------------|
| `writeTrade(trade)` | Write a trade record. Returns `true` on success. |
| `writeOptionQuote(quote)` | Write an `OptionQuoteRecord`. Returns `true` on success. |
| `writePoolState(header, payload, payload_size)` | Write a DEX pool-state record: the fixed header, whose `payload_len` must equal `payload_size`, followed by `payload_size` bytes of u256-native pool payload. |
| `writeBook(header, bids, asks)` | Write a book update. Returns `true` on success. |
| `flush()` | Flush internal buffers to disk. |
| `close()` | Close current segment, write index and header. |
| `stats()` | Returns write statistics. |
| `currentSegmentPath()` | Returns path to current segment file. |
| `setMetadata(meta)` | Replace the recording metadata. Callable during recording. |
| `metadata()` | Mutable pointer to the metadata, or `nullptr` when unset. |
| `addSymbol(symbol)` | Append a `SymbolInfo` to the metadata. |
| `setHasTrades(v)` / `setHasBookSnapshots(v)` / `setHasBookDeltas(v)` | Set the content flags in the metadata. |

## Statistics

```cpp
struct WriterStats {
  uint64_t bytes_written{0};
  uint64_t events_written{0};
  uint64_t segments_created{0};
  uint64_t trades_written{0};
  uint64_t book_updates_written{0};
  uint64_t option_quotes_written{0};
  uint64_t blocks_written{0};        // For compressed mode
  uint64_t uncompressed_bytes{0};
  uint64_t compressed_bytes{0};
};
```

## Usage

```cpp
replay::WriterConfig config{
  .output_dir = "/data/market",
  .max_segment_bytes = 512ull << 20,  // 512 MB segments
  .create_index = true,
  .compression = replay::CompressionType::LZ4
};

replay::BinaryLogWriter writer(config);

// Write trades
replay::TradeRecord trade{...};
writer.writeTrade(trade);

// Write book updates
replay::BookRecordHeader header{...};
std::vector<replay::BookLevel> bids{...}, asks{...};
writer.writeBook(header, bids, asks);

// Periodic flush
writer.flush();

// Close cleanly
writer.close();
```

## Custom Rotation Callback

By default, rotated segments use timestamp-based names. To customize naming:

```cpp
// Context for the callback
struct MyContext {
  std::string prefix;
  int sequence{1};
};

// Callback function (must be a plain function, not a lambda with captures)
std::filesystem::path my_rotation_cb(
    void* user_data,
    const std::filesystem::path& output_dir,
    uint32_t segment_number)
{
  auto* ctx = static_cast<MyContext*>(user_data);
  std::ostringstream fname;
  fname << ctx->prefix << "_" << std::setfill('0') << std::setw(3)
        << (ctx->sequence + segment_number - 1) << ".lrvx";
  return output_dir / fname.str();
}

// Usage
MyContext ctx{"2025-01-15", 1};

replay::WriterConfig config{
  .output_dir = "/data/market",
  .output_filename = "2025-01-15_001.lrvx",  // First segment
  .max_segment_bytes = 256ull << 20,
  .rotation_callback = my_rotation_cb,
  .rotation_user_data = &ctx,  // Must outlive writer!
};

replay::BinaryLogWriter writer(config);
// When segment rotates, callback generates "2025-01-15_002.lrvx", etc.
```

**Important:** The `rotation_user_data` pointer must remain valid for the lifetime of the writer.

## Event ordering

When `compression` is set to `LZ4`, the writer automatically sorts events by `exchange_ts_ns` within each compressed block before flushing. This eliminates intra-block timestamp inversions that occur when exchange WebSocket feeds deliver trades out of order within a batch.

If no cross-block inversions are detected during recording, the segment is marked with `SegmentFlags::Sorted`. `BinaryLogReader` uses this flag to stream the segment directly without buffering, giving O(1) memory and correct early-exit behaviour. Segments without the flag (recorded before this feature, or with cross-block inversions) are handled by the reader via a buffer-and-sort fallback.

## Notes

* Thread-safe via internal mutex.
* Segments are automatically rotated when `max_segment_bytes` is reached.
* Index enables fast timestamp-based seeking during replay.
* Compression reduces storage but adds CPU overhead.
* Call `close()` before destruction to ensure index is written.

## See Also

* [Binary Format](binary_format.md) — File format specification
* [BinaryLogReader](binary_log_reader.md) — Reading segments
* [BinaryLogRecorderHook](market_data_recorder.md) — High-level recording interface
