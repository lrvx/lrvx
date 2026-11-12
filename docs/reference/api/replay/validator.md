# Validator

`SegmentValidator` and `DatasetValidator` verify integrity of binary log files, detecting corruption, CRC mismatches, timestamp anomalies, and structural issues.

```cpp
struct ValidatorConfig
{
  bool verify_crc{true};
  bool verify_timestamps{true};
  bool verify_index{true};
  bool scan_all_events{true};
  bool stop_on_first_error{false};
  int64_t max_timestamp_jump_ns{3600LL * 1000000000LL};  // 1 hour
};

class SegmentValidator
{
public:
  explicit SegmentValidator(ValidatorConfig config = {});

  SegmentValidationResult validate(const std::filesystem::path& segment_path);

  using ProgressCallback = std::function<void(uint64_t bytes_processed,
                                               uint64_t total_bytes)>;
  SegmentValidationResult validate(const std::filesystem::path& segment_path,
                                   ProgressCallback progress);
};

class DatasetValidator
{
public:
  explicit DatasetValidator(ValidatorConfig config = {});

  DatasetValidationResult validate(const std::filesystem::path& data_dir);

  using ProgressCallback = std::function<void(uint32_t segment_index,
                                               uint32_t total_segments,
                                               const std::filesystem::path& current_file)>;
  DatasetValidationResult validate(const std::filesystem::path& data_dir,
                                   ProgressCallback progress);
};
```

## Issue Types

| Type                        | Description                              |
|-----------------------------|------------------------------------------|
| `InvalidMagic`              | File magic number doesn't match          |
| `InvalidVersion`            | Unsupported format version               |
| `InvalidFlags`              | Segment header flags are not a valid combination |
| `HeaderCorrupted`           | Segment header unreadable                |
| `FrameCrcMismatch`          | Frame CRC32 check failed                 |
| `FrameSizeTooLarge`         | Frame size exceeds limits                |
| `FrameTypeUnknown`          | Unknown event type                       |
| `FrameTruncated`            | Incomplete frame data                    |
| `BlockMagicInvalid`         | Compressed block magic mismatch          |
| `BlockDecompressionFailed`  | LZ4 decompression error                  |
| `BlockSizeMismatch`         | Decompressed block size differs from the header's |
| `IndexCrcMismatch`          | Index CRC32 check failed                 |
| `IndexMagicInvalid`         | Index magic number doesn't match         |
| `IndexOutOfBounds`          | Index entry points outside the file      |
| `IndexNotSorted`            | Index entries not in order               |
| `TimestampOutOfOrder`       | Events not chronologically sorted        |
| `TimestampJumpTooLarge`     | Suspicious timestamp gap                 |
| `EventCountMismatch`        | Header count doesn't match actual        |
| `FileTruncated`             | File ends unexpectedly                   |
| `FileNotFound`              | The path does not exist                  |
| `FileReadError`             | The file could not be read               |
| `SegmentRangeOverlap`       | Two segments cover intersecting time ranges (dataset-level) |

## Severity Levels

The enum is named `IssueSeverity`.

```cpp
enum class IssueSeverity
{
  Info,
  Warning,
  Error,
  Critical,
};
```

| Level      | Meaning                                    |
|------------|---------------------------------------------|
| `Info`     | Informational, not a problem               |
| `Warning`  | Potential issue, data still readable       |
| `Error`    | Definite problem, some data may be lost    |
| `Critical` | File unusable                              |

## Validation Results

### SegmentValidationResult

```cpp
struct SegmentValidationResult
{
  std::filesystem::path path;
  bool valid;
  std::vector<ValidationIssue> issues;

  bool header_valid;
  uint32_t reported_event_count;
  int64_t reported_first_ts;
  int64_t reported_last_ts;
  bool is_compressed;
  CompressionType compression_type;

  uint32_t actual_event_count;
  int64_t actual_first_ts;
  int64_t actual_last_ts;
  uint64_t bytes_scanned;

  bool has_index;
  bool index_valid;
  uint32_t index_entry_count;

  uint32_t trades_found;
  uint32_t book_updates_found;
  uint32_t crc_errors;
  uint32_t timestamp_anomalies;

  bool hasErrors() const;
  bool hasCritical() const;
};
```

### DatasetValidationResult

```cpp
struct DatasetValidationResult
{
  std::filesystem::path data_dir;
  bool valid;
  std::vector<SegmentValidationResult> segments;

  // Issues about the dataset rather than about any one segment.
  std::vector<ValidationIssue> issues;

  uint32_t total_segments;
  uint32_t valid_segments;
  uint32_t corrupted_segments;
  uint64_t total_events;
  uint64_t total_bytes;

  int64_t first_timestamp;
  int64_t last_timestamp;

  uint32_t total_errors;
  uint32_t total_warnings;

  bool hasErrors() const;
};
```

`SegmentValidator` looks at one file at a time and cannot see a fault that lives between files. `DatasetValidator` adds the cross-segment pass: two segments whose `[actual_first_ts, actual_last_ts]` ranges intersect mean the same window was recorded twice, and such a dataset replays with that window's events duplicated. Readers merge overlapping `Sorted` segments event by event, so those arrive in time order; segments without the `Sorted` flag keep their file-name position, so overlapping ones replay reordered or -- through the streaming reorder buffer's late drop -- with events missing (see [BinaryLogReader, Ordering guarantee](binary_log_reader.md#ordering-guarantee)). Each segment on its own is well formed, so the finding lands on `DatasetValidationResult::issues` as a `SegmentRangeOverlap` error and `valid` is false. Segments that merely touch at the seam (`a.last == b.first`) are the normal output of rotation and are not reported.

## Usage

### Single Segment

```cpp
SegmentValidator validator;
auto result = validator.validate("/data/market.lrvx");

if (result.hasErrors()) {
    for (const auto& issue : result.issues) {
        std::cerr << "Issue at offset " << issue.file_offset
                  << ": " << issue.message << "\n";
    }
}
```

### Entire Dataset

```cpp
DatasetValidator validator;
auto result = validator.validate("/data/market");

std::cout << "Valid: " << result.valid_segments
          << "/" << result.total_segments << " segments\n";
std::cout << "Events: " << result.total_events << "\n";
std::cout << "Errors: " << result.total_errors << "\n";
```

### With Progress

```cpp
DatasetValidator validator;
auto result = validator.validate("/data/market",
    [](uint32_t current, uint32_t total, const auto& path) {
        std::cout << "Validating " << current << "/" << total
                  << ": " << path.filename() << "\n";
    });
```

## Repair

`SegmentRepairer` can fix common issues in corrupted segments.

```cpp
struct RepairConfig
{
  bool backup_before_repair{true};
  std::string backup_suffix{".backup"};
  bool fix_header_timestamps{true};
  bool fix_event_count{true};
  bool rebuild_index{true};
};

class SegmentRepairer
{
public:
  explicit SegmentRepairer(RepairConfig config = {});

  RepairResult repair(const std::filesystem::path& segment_path);
  RepairResult repair(const std::filesystem::path& segment_path,
                      const SegmentValidationResult& validation);
};
```

### Repair Usage

```cpp
SegmentValidator validator;
auto validation = validator.validate("/data/corrupted.lrvx");

if (validation.hasErrors()) {
    SegmentRepairer repairer;
    auto repair = repairer.repair("/data/corrupted.lrvx", validation);

    if (repair.success) {
        for (const auto& action : repair.actions_taken) {
            std::cout << "Fixed: " << action << "\n";
        }
    }
}
```

## Convenience Functions

```cpp
// Quick validation
bool ok = replay::isValidSegment("/data/market.lrvx");
bool ok = replay::isValidDataset("/data/market");
```

## Notes

* Validation scans entire file by default for complete integrity check.
* CRC verification can be disabled for faster validation.
* Timestamp jump detection catches clock sync issues in recorded data.
* Repair always creates backups unless explicitly disabled.
