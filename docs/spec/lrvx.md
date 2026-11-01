# `.lrvx` binary tape format specification

**Version 1, last revised 2026-07-22.** This is the on-disk format lrvx uses for deterministic event capture and replay. The format is published so third-party tooling can read and write tapes without depending on lrvx itself.

## At a glance

A `.lrvx` is a directory of segment files plus two sidecars. Segments hold trades, order-book updates and side-channel records as length-prefixed CRC-checked frames in one interleaved, timestamp-ordered stream, optionally LZ4-compressed in fixed-size blocks. Each segment can carry a sparse index for seek. Timestamps are integer nanoseconds since Unix epoch; prices and quantities are int64 fixed-point with a scale of `1e8`.

Everything is little-endian. All structures are 8-byte aligned. CRC32 uses the standard reflected polynomial `0xEDB88320` (ISO 3309).

## Layout on disk

```
my-tape.lrvx/
├── .manifest            # binary segment index (optional, rebuildable)
├── metadata.json        # human-readable recording metadata (optional)
├── 1714123456000000000.lrvx
├── 1714123459000000000.lrvx
└── ...
```

Segment files are named `<created_ns>.lrvx`, where `created_ns` is the wall-clock nanosecond timestamp at which the writer opened the segment. A custom rotation callback can override the naming. There is **no** per-type split: trades, book snapshots, book deltas and the side-channel record types all live interleaved in the same frame stream, ordered by timestamp.

Segments named `<created_ns>.floxlog`, the extension tapes recorded under the FLOX name carry, are segments too: every directory scan (reader, inspector, validator, manifest, index builder, merge, `lrvx tape view`) accepts both extensions, so a directory mixing `.floxlog` and `.lrvx` segments reads as one tape in filename order. Writers always produce `.lrvx`.

A writer rotates when the segment reaches its size bound, measured on the bytes written to the file. An uncompressed segment rotates between frames; a compressed one rotates between blocks, since a block is the unit the reader decompresses. A segment may therefore overshoot the bound by at most one block plus the index appended when it closes.

Segments are independently parseable; both sidecars are conveniences, not requirements.

`.manifest` is a **binary** file — see [Manifest](#manifest) — not JSON. It can be rebuilt from the segments at any time.

`metadata.json` is the only JSON sidecar. It carries recording-level metadata the segment headers do not: `recording_id`, `description`, `exchange`, `exchange_type`, `instrument_type`, `connector_version`, the symbol table, `has_trades` / `has_book_snapshots` / `has_book_deltas` / `has_book_tops`, `total_trades`, `total_book_updates`, `book_depth`, `recording_start` / `recording_end` (ISO 8601), `price_scale` / `qty_scale`, `hostname`, `timezone`, `lrvx_version`, and a free-form `custom` string map. It is written by `BinaryLogWriter::close()` and only when metadata was set on the writer.

## Magic numbers and constants

| Constant | Value | Meaning |
|---|---|---|
| `MAGIC_SEGMENT` | `0x584F4C46` (`"FLOX"`) | Segment header sentinel. |
| `MAGIC_BLOCK` | `0x4B4C4246` (`"FBLK"`) | Compressed block header sentinel. |
| `MAGIC_INDEX` | `0x58444E49` (`"INDX"`) | Sparse index header sentinel. |
| `MAGIC_MANIFEST` | `0x464D414E` (`"FMAN"`) | Binary manifest header sentinel. |
| `FORMAT_VERSION` | `1` | Bump triggers a new `.lrvx` major. |
| `INDEX_VERSION` | `1` | Bump triggers a new index format minor. |
| `MANIFEST_VERSION` | `1` | Manifest header version. |

## Segment file

A segment is a `SegmentHeader` followed by a stream of frames. If `Compressed` is set, the stream is partitioned into `CompressedBlock`s, each holding LZ4-compressed frame bytes plus an inline header. If `HasIndex` is set, the index trailer lives at `index_offset` from the file start.

### `SegmentHeader` (64 bytes, 8-byte aligned)

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0  | 4 | `magic` | `MAGIC_SEGMENT`. |
| 4  | 2 | `version` | `1`. |
| 6  | 1 | `flags` | Bitfield. See below. |
| 7  | 1 | `exchange_id` | Numeric exchange tag; `0` if not used. |
| 8  | 8 | `created_ns` | Wall-clock nanoseconds when the segment was opened. |
| 16 | 8 | `first_event_ns` | Earliest event timestamp in segment. |
| 24 | 8 | `last_event_ns` | Latest event timestamp. |
| 32 | 4 | `event_count` | Total events written. |
| 36 | 4 | `symbol_count` | Distinct symbols seen. |
| 40 | 8 | `index_offset` | Byte offset of the index trailer; `0` if absent. |
| 48 | 1 | `compression` | `0` = none, `1` = LZ4. |
| 49 | 15 | `reserved[15]` | Zero-filled. Bumping any reserved byte requires a version bump. |

### `SegmentFlags`

| Bit | Name | Meaning |
|---:|---|---|
| `0x01` | `HasIndex` | Sparse index trailer present. |
| `0x02` | `Compressed` | Frame stream is partitioned into `CompressedBlock`s. |
| `0x04` | `Encrypted` | Reserved. Not used by lrvx 1.0. |
| `0x08` | `Sorted` | Writer guarantees `exchange_ts_ns` is monotonically non-decreasing across all events in the segment. |

A reader that sees an unknown flag set must reject the segment with a clear error. New flags need a new minor version of the format.

`Sorted` is set at close on any segment -- compressed or not -- whose events all reached the writer in non-decreasing `exchange_ts_ns` order. It is a per-segment promise and says nothing about the segments around it: a reader may stream a `Sorted` segment as it stands, and must not drop its leading events because an earlier segment of the same dataset ended later in time. Segments whose time ranges overlap are a recording fault the validator reports; they are not licence to discard recorded frames.

### Frame stream

A frame is a `FrameHeader` followed by `size` bytes of payload. Payload meaning depends on `type`:

- `type = 1` → `TradeRecord`
- `type = 2` → `BookRecordHeader` followed by `bid_count + ask_count` `BookLevel` entries (book snapshot)
- `type = 3` → same shape as `type = 2`, but the levels are deltas (positive qty = upsert, qty == 0 = remove)
- `type = 4` → `OptionQuoteRecord` (112 bytes) — option side-channel: mark price, index price, underlying forward, implied vol, best bid/ask with sizes and IVs, open interest
- `type = 5` → `PoolStateRecordHeader` (32 bytes) followed by `payload_len` bytes of u256-native DEX pool payload
- `type = 6` → same shape as `type = 2`, but the record is a one-level top-of-book refresh (`BookUpdateType::TOP_OF_BOOK`); depth consumers must not apply it as book depth

Types 4 through 6 are **additive**. A reader that does not know a type skips the frame using `FrameHeader.size` and continues; it must not reject the segment. Unknown types are therefore not an error, unlike unknown `flags` bits or an unknown `rec_version`.

If `Compressed` is set, the entire frame stream from the byte after the segment header lives inside one or more `CompressedBlock`s.

### `FrameHeader` (12 bytes)

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 4 | `size` | Payload bytes, excluding this header. |
| 4 | 4 | `crc32` | CRC32 of the payload bytes (header excluded). |
| 8 | 1 | `type` | `1` Trade, `2` BookSnapshot, `3` BookDelta, `4` OptionQuote, `5` PoolState, `6` BookTop. |
| 9 | 1 | `rec_version` | Per-record version. `1` for the layouts below. |
| 10 | 2 | `flags` | Reserved. Must be zero. |

A reader that sees an unknown `rec_version` must reject the frame.

### `TradeRecord` (48 bytes, 8-byte aligned)

| Offset | Size | Field | Units |
|---:|---:|---|---|
| 0  | 8 | `exchange_ts_ns` | Exchange-side timestamp, integer nanoseconds. |
| 8  | 8 | `recv_ts_ns` | Receiver-side wall-clock timestamp at frame write. |
| 16 | 8 | `price_raw` | Fixed-point price, scale `1e8`. |
| 24 | 8 | `qty_raw` | Fixed-point quantity, scale `1e8`. |
| 32 | 8 | `trade_id` | Exchange trade id; `0` if unknown. |
| 40 | 4 | `symbol_id` | Numeric symbol tag from the writer's registry. |
| 44 | 1 | `side` | `0` buy, `1` sell. |
| 45 | 1 | `instrument` | `0` spot, `1` perp, etc. (see Instrument codes). |
| 46 | 2 | `exchange_id` | Numeric exchange tag. |

#### Side byte

`side` is the aggressor of the trade, never the resting maker, and it is encoded as `lrvx::Side`: **`0` = buy, `1` = sell**. Every writer of the format has always used that encoding -- the C++ recorder hook, `lrvx_data_writer_write_trade`, the Node and Python `DataWriter`s and all the exchange-archive importers -- so the bytes on disk are what this table says and no tape needs migrating.

What was wrong was the read side. Until this was fixed, `ReplayConnector`, `BacktestRunner::runTape`, `StrategyPump` and `preagg_bars` decoded `side == 1` as the buy and so **inverted** the aggressor of every tape they replayed, while the aggregators (`BinCountAggregator`, `VolumeBinAggregator`) and the Node and QuickJS readers decoded the same byte correctly. Anything derived from the aggressor through one of those four readers -- queue position, maker/taker classification, buy/sell-driven strategy logic, signed volume -- is inverted in results produced before the fix and has to be recomputed. The tapes themselves are unaffected, which is why the format carries no marker for this: a marker would say something about the writer, and the writer was never the side that was wrong.

### `BookRecordHeader` (40 bytes, 8-byte aligned)

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0  | 8 | `exchange_ts_ns` | |
| 8  | 8 | `recv_ts_ns` | |
| 16 | 8 | `seq` | Monotonic sequence number from the source. `0` if not provided. |
| 24 | 4 | `symbol_id` | |
| 28 | 2 | `bid_count` | Number of `BookLevel` entries that follow on the bid side. |
| 30 | 2 | `ask_count` | Number of `BookLevel` entries on the ask side. |
| 32 | 1 | `type` | `BookUpdateType` numeric space: `0` snapshot, `1` delta, `2` top-of-book. (Every writer has always emitted `0`/`1` here; this table used to claim the frame-type space.) |
| 33 | 1 | `instrument` | |
| 34 | 2 | `exchange_id` | |
| 36 | 4 | `_pad` | Zero. |

Followed by `bid_count + ask_count` `BookLevel` entries, bids first.

### `BookLevel` (16 bytes)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | `price_raw` |
| 8 | 8 | `qty_raw` |

Snapshot semantics: the listed levels replace the entire side. Delta semantics: positive `qty_raw` upserts a price level; `qty_raw == 0` removes the level.

### Compressed block (when `flags.Compressed` is set)

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0  | 4 | `magic` | `MAGIC_BLOCK`. |
| 4  | 4 | `compressed_size` | Bytes of LZ4 payload that follow this header. |
| 8  | 4 | `original_size` | Decompressed size in bytes. |
| 12 | 2 | `event_count` | Number of frames packed into this block. |
| 14 | 2 | `flags` | Reserved. |

The block header is 16 bytes; the payload that follows is exactly `compressed_size` LZ4-compressed bytes which decompress to `original_size` bytes of frame stream. Blocks are concatenated until end of segment (or until `index_offset` if present).

LZ4 frame format: raw block, no LZ4 frame wrapper. Use the LZ4 block API directly.

### Sparse index (when `flags.HasIndex` is set)

The trailer at `index_offset`:

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0  | 4 | `magic` | `MAGIC_INDEX`. |
| 4  | 2 | `version` | `1`. |
| 6  | 2 | `interval` | Events between consecutive index entries. The writer stores its configured `index_interval`; the default is `1000`. |
| 8  | 4 | `entry_count` | Number of `IndexEntry` rows that follow. |
| 12 | 4 | `crc32` | CRC32 over the entries section. |
| 16 | 8 | `first_ts_ns` | First indexed timestamp. |
| 24 | 8 | `last_ts_ns` | Last indexed timestamp. |

Followed by `entry_count` `IndexEntry` rows.

### `IndexEntry` (16 bytes)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | `timestamp_ns` |
| 8 | 8 | `file_offset` |

`file_offset` points at the start of a `FrameHeader` (or, for compressed segments, the start of a `CompressedBlock`). Indexes are sparse: a reader that wants timestamp `T` finds the largest indexed entry with `timestamp_ns <= T`, seeks there, and scans forward.

## Manifest

`.manifest` is a binary file: one `ManifestHeader`, then `segment_count` `ManifestSegmentEntry` rows, read and written with plain struct I/O. It is a cache — `SegmentManifest::build(dir)` regenerates it from the segments — so a reader may ignore it entirely.

### `ManifestHeader`

| Size | Field | Notes |
|---:|---|---|
| 4 | `magic` | `MAGIC_MANIFEST`. |
| 1 | `version` | `MANIFEST_VERSION` (`1`). |
| 3 | `reserved[3]` | Zero-filled. |
| 8 | `segment_count` | Number of `ManifestSegmentEntry` rows that follow. |
| 8 | `total_events` | Across all segments. |
| 8 | `first_timestamp_ns` | Earliest event in the tape. |
| 8 | `last_timestamp_ns` | Latest event in the tape. |
| 8 | `total_bytes` | Sum of segment file sizes. |
| 4 | `symbol_count` | Distinct symbols across the tape. |
| 4 | `checksum` | |

A header whose `magic` or `version` does not match is treated as absent; the reader falls back to rebuilding.

### `ManifestSegmentEntry`

| Size | Field | Notes |
|---:|---|---|
| 256 | `filename[256]` | NUL-padded, relative to the tape directory. |
| 8 | `first_event_ns` | |
| 8 | `last_event_ns` | |
| 8 | `event_count` | |
| 8 | `file_size` | |
| 4 | `flags` | `0x01` = segment has an index, `0x02` = segment is compressed. |
| 4 | `reserved` | Zero. |

## Instrument codes

| Code | Meaning |
|---:|---|
| 0 | spot |
| 1 | perp |
| 2 | future |
| 3 | option |
| 4-255 | reserved |

## Numeric conventions

- All multi-byte integers are **little-endian**.
- `price_raw` and `qty_raw` are signed int64 fixed-point with **scale 1e8**. To convert to a double price, divide by `1e8`. To go from a double to fixed-point, round to nearest then clamp to int64 range. Negative values are valid (used for short-side qty in some derived analytics, never in raw recorded fields).
- Timestamps are **integer nanoseconds since Unix epoch (UTC)**. No leap-second adjustment. Writers should pull from `clock_gettime(CLOCK_REALTIME)` or its OS equivalent.

## Versioning policy

- `format_version` follows semantic versioning at the major level. A new major is incompatible. Old readers of a new tape must reject with a clear error.
- New `EventType` codes are **additive within the same `format_version`**. Because every frame is length-prefixed, a reader that meets an unknown `type` skips `FrameHeader.size` bytes and carries on. `OptionQuote` (4) and `PoolState` (5) were both added this way under `format_version = 1`.
- New `flags` bits, new `Instrument` codes, and new fields in reserved space do require a new major: unlike frame types, an unknown flag changes how the rest of the segment must be parsed, so it cannot be skipped.
- Adding new `rec_version` values for a single record type within the same `format_version` is allowed when the record's size grows monotonically and the new version stays parseable as a prefix of the new layout. This path is rarely worth the complexity; prefer a new major.

The layouts above are stable: existing offsets and sizes do not move within `format_version = 1`. Any change that would move them ships as version 2 with migration guidance in the corresponding spec revision.

## Reference implementations

- **C++.** `include/lrvx/replay/binary_format_v1.h` is the canonical layout. `src/replay/binary_log_writer.cpp` and `src/replay/binary_log_reader.cpp` implement read and write.
- **Python.** `lrvx.DataWriter` and `lrvx.DataReader` are pybind11 bindings of the C++ writer and reader. They are the simplest entry point for tooling outside lrvx.
- **Round-trip exercise.** `docs/examples/python_tape_roundtrip.py` writes synthetic trades through `lrvx.DataWriter`, reads them back through `lrvx.DataReader`, and asserts byte-equality field by field. CI runs it on every push, so the reference impl never silently drifts from this spec.

## Worked example: a 7-trade tape

The replay-equivalence CI gate (`scripts/replay_equivalence_gate.py`) writes a fixed 7-trade tape, runs four frozen order scenarios against it, and compares each scenario's captured output with its frozen `expected_output.json` field by field over the parsed JSON, not byte for byte. The tape generator (`tests/replay-equivalence/build_tape.py`) builds a tape from scratch: it registers one symbol, plugs `lrvx.tape.make_recorder_hook` (the in-tree `BinaryLogWriter` behind `lrvx.BinaryLogRecorderHook`) into an `lrvx.Runner` as its market-data recorder, and feeds the seven trades, each with an explicit exchange timestamp, through `runner.on_trade`. Read it before writing your own implementation; everything important about the writer's calling convention is there.

## Compatibility commitments

- Files written by `lrvx` 0.5.x and later remain readable by every lrvx 1.x release.
- A reader for an older lrvx must refuse to load a newer tape rather than skip frames it does not understand.
- New `EventType` codes must be additive and skippable via `FrameHeader.size`. New `flags` bits are not skippable: a reader must reject a segment whose `SegmentFlags` carry a bit it does not know.

## Reporting issues

Bugs in this spec or in the reference implementation: open an issue on the public repository with the byte ranges that failed to parse and the sha256 of the offending segment. Include the writer version and any LZ4 library version if compression was used.
