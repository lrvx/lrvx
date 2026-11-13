# Segment Operations

Utilities for merging, splitting, exporting, validating, and partitioning binary log data.

## Merge

### `merge(input_paths, output_dir, output_name="merged", compression="none", sort=True) -> dict`

Merge multiple segment files into one.

```python
result = lrvx.merge(
    input_paths=["seg1.lrvx", "seg2.lrvx"],
    output_dir="./merged",
    output_name="combined",
    compression="lz4",
    sort=True,
)
print(f"Merged {result['segments_merged']} segments, {result['events_written']} events")
```

**Returns dict:**

| Key | Type | Description |
|-----|------|-------------|
| `success` | `bool` | Whether merge succeeded |
| `output_path` | `str` | Output file path |
| `segments_merged` | `int` | Input segments processed |
| `events_written` | `int` | Events in output |
| `bytes_written` | `int` | Output size |
| `errors` | `list` | Error messages |

### `merge_dir(input_dir, output_dir) -> dict`

Merge all segments in a directory (shorthand for `merge`).

```python
result = lrvx.merge_dir("./raw_data", "./merged_data")
```

A segment that cannot be opened is skipped and listed in `errors` as `"Cannot open: <path>"`; the
others are still merged, `segments_merged` counts them, and `success` is `False` because `errors`
is not empty.

---

## Split

### `split(input_path, output_dir, mode="time", time_interval_ns=3600000000000, events_per_file=1000000) -> dict`

Split a segment file into multiple files.

```python
result = lrvx.split(
    input_path="large_file.lrvx",
    output_dir="./split",
    mode="time",
    time_interval_ns=3600_000_000_000,  # 1 hour
)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `mode` | `str` | `"time"` | Split mode: `"time"`, `"event_count"`, `"size"`, `"symbol"` |
| `time_interval_ns` | `int` | 1 hour | Time interval for `"time"` mode |
| `events_per_file` | `int` | 1000000 | Events per file for `"event_count"` mode |

**Returns dict:**

| Key | Type | Description |
|-----|------|-------------|
| `success` | `bool` | Whether split succeeded |
| `output_paths` | `list[str]` | Output file paths |
| `segments_created` | `int` | Files created |
| `events_written` | `int` | Total events |
| `errors` | `list` | Error messages |

---

## Export

### `export_data(input_path, output_path, format="csv", from_ns=None, to_ns=None, symbols=None) -> dict`

Export binary data to CSV, JSON, or JSONLines.

```python
result = lrvx.export_data(
    input_path="data.lrvx",
    output_path="trades.csv",
    format="csv",
    symbols=[1, 2],
)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `format` | `str` | `"csv"` | Output format: `"csv"`, `"json"`, `"jsonlines"`, `"binary"` |
| `from_ns` | `int` | `None` | Start timestamp filter |
| `to_ns` | `int` | `None` | End timestamp filter |
| `symbols` | `list[int]` | `None` | Symbol ID filter |

**Returns dict:**

| Key | Type | Description |
|-----|------|-------------|
| `success` | `bool` | Whether export succeeded |
| `output_path` | `str` | Output file path |
| `events_exported` | `int` | Events exported |
| `bytes_written` | `int` | Output size |
| `errors` | `list` | Error messages |

---

## Other Operations

### `recompress(input_path, output_path, compression="lz4") -> bool`

Re-compress a segment file. Returns `True` on success, `False` when the input cannot be read or
the output cannot be written.

```python
ok = lrvx.recompress("uncompressed.lrvx", "compressed.lrvx", "lz4")
```

### `extract_symbols(input_path, output_path, symbols) -> int`

Extract specific symbols to a new file. Returns event count.

```python
count = lrvx.extract_symbols("all_data.lrvx", "btc_only.lrvx", [1])
```

### `extract_time_range(input_path, output_path, from_ns, to_ns) -> int`

Extract a time range to a new file. Returns event count.

```python
count = lrvx.extract_time_range("data.lrvx", "morning.lrvx",
                                 from_ns=start, to_ns=end)
```

### `last_error() -> str`

Why the last `recompress` returned `False` or the last `extract_symbols` / `extract_time_range`
returned `0`: a string that opens with the C function's name and names the file that could not
be read or written. Each of the three clears it on entry, so `''` means the call did not fail; an
extract that matched nothing returns `0` with `''`. `FundingSchedule.load_tape(path)` reports a
file it could not open the same way. The record is per thread.

```python
if not lrvx.recompress("in.lrvx", "out.lrvx", "lz4"):
    print(lrvx.last_error())
```

---

## Validation

### `validate(segment_path, verify_crc=True, verify_timestamps=True) -> dict`

Validate a single segment file.

```python
result = lrvx.validate("data.lrvx")
if not result['valid']:
    for issue in result['issues']:
        print(f"[{issue['severity']}] {issue['message']}")
```

**Returns dict:**

| Key | Type | Description |
|-----|------|-------------|
| `path` | `str` | File path |
| `valid` | `bool` | Overall validity |
| `header_valid` | `bool` | Header integrity |
| `reported_event_count` | `int` | Events per header |
| `actual_event_count` | `int` | Events found |
| `has_index` | `bool` | Has index section |
| `index_valid` | `bool` | Index integrity |
| `trades_found` | `int` | Trade count |
| `book_updates_found` | `int` | Book update count |
| `crc_errors` | `int` | CRC failures |
| `timestamp_anomalies` | `int` | Out-of-order timestamps |
| `has_errors` | `bool` | Has error-level issues |
| `has_critical` | `bool` | Has critical issues |
| `issues` | `list[dict]` | Issue details |

### `validate_dataset(data_dir) -> dict`

Validate all segments in a directory.

```python
result = lrvx.validate_dataset("./data")
print(f"{result['valid_segments']}/{result['total_segments']} valid")
```

---

## Partitioner

Partition datasets for parallel processing or walk-forward backtesting.

```python
part = lrvx.Partitioner(data_dir="./data")
```

### Methods

#### `partition_by_time(num_partitions, warmup_ns=0) -> list[dict]`

Split into N equal time-based partitions.

```python
partitions = part.partition_by_time(num_partitions=4, warmup_ns=3600_000_000_000)
```

#### `partition_by_duration(duration_ns, warmup_ns=0) -> list[dict]`

Partition by fixed time duration.

#### `partition_by_calendar(unit, warmup_ns=0) -> list[dict]`

Partition by calendar unit.

| Unit | Description |
|------|-------------|
| `"hour"` | Hourly partitions |
| `"day"` | Daily partitions |
| `"week"` | Weekly partitions |
| `"month"` | Monthly partitions |

#### `partition_by_symbol(num_partitions) -> list[dict]`

Group symbols into N partitions.

#### `partition_per_symbol() -> list[dict]`

One partition per symbol.

#### `partition_by_event_count(num_partitions) -> list[dict]`

Split by event count.

### Partition Dict

Each partition is a dict:

| Key | Type | Description |
|-----|------|-------------|
| `partition_id` | `int` | Partition index |
| `from_ns` | `int` | Start timestamp |
| `to_ns` | `int` | End timestamp |
| `warmup_from_ns` | `int` | Warmup start timestamp |
| `estimated_events` | `int` | Estimated event count |
| `estimated_bytes` | `int` | Estimated data size |
| `symbols` | `list[int]` | Symbols in partition |
