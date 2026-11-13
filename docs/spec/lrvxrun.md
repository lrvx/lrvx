# `.lrvxrun` strategy-trace format specification

**Version 1.0, frozen 2026-05-08.** This is the on-disk format lrvx uses to record what a strategy did during a run: the signals it produced, the orders it submitted, and the fills it received. Tape (`.lrvx`) records what came in from the exchange; this format records what the strategy emitted in response. The two live side by side.

The spec is published so third-party tooling can read and write trace artifacts without depending on lrvx itself.

## At a glance

A `.lrvxrun` is a directory of segment files plus a manifest. Each segment holds one record type (signals, order events, or fills) as length-prefixed CRC-checked frames. Frames carry both a wall-clock timestamp (when the strategy emitted the event) and a feed timestamp (the source tape event that triggered the decision), so a reader can line trace events up against any of the consumed tapes without guessing.

Everything is little-endian. All structures are 8-byte aligned. CRC32 uses the standard reflected polynomial `0xEDB88320` (ISO 3309). The on-disk layout, segment-header shape, frame-header shape, and CRC are intentionally identical to `.lrvx` so existing reader infrastructure ports over with minimal change.

## Why a separate format

Tape is per-feed: one symbol from one exchange, one stream of trades and book updates as they arrived from the wire. A strategy run is per-run: it consumes N tapes, emits decisions that may span several symbols, and produces orders and fills that don't belong to any one feed.

Putting trace inside `.lrvx` was rejected for three reasons:

- A multi-symbol decision (for example, "BTC/ETH ratio crossed threshold, short BTC and long ETH") doesn't belong to either input tape. Writing it into one would lie about its scope; writing it into both would duplicate it.
- Tape is exchange-portable. A Bitget BTC tape should replay against any strategy. Mixing one strategy's signals into the tape couples them.
- Tape is a deterministic input format. Replay tooling should not have to filter a "trace" subtype out before feeding the strategy.

The trace lives alongside the tape, and the run's manifest references the tapes it was produced from.

## Layout on disk

```
my-run.lrvxrun/
├── manifest.json
├── signals-000000.bin
├── orders-000000.bin
└── fills-000000.bin
```

`manifest.json` is the index. It carries strategy identity, the list of consumed tapes with content hashes, the run's wall-clock window, and per-segment metadata. Segments are independently parseable; the manifest is an index, not a requirement.

Per-type segment files mean a tool that only cares about, say, fills can read just the fills segment without paying for signal decoding.

A directory named `*.floxrun`, the name traces recorded under FLOX carry, holds the same format. `TraceReader` opens a trace by directory path whatever its suffix, `lrvx tape view` accepts `.floxrun` alongside `.lrvxrun`, and bundle validation reads `expected.floxrun/` when a bundle has no `expected.lrvxrun/`.

## Magic numbers and constants

| Constant | Value | Meaning |
|---|---|---|
| `MAGIC_RUN_SEGMENT` | `0x4E555246` (`"FRUN"`) | Run-segment header sentinel. |
| `FORMAT_VERSION` | `1` | Bump triggers a new `.lrvxrun` major. |

The frame-header magic, CRC algorithm, and compressed-block header (when used) are reused verbatim from `.lrvx`.

## Run-segment file

A segment is a `RunSegmentHeader` followed by a stream of frames. Frames carry one record per the segment's record type. Compression is supported via the same `CompressedBlock` shape as `.lrvx`.

### `RunSegmentHeader` (64 bytes, 8-byte aligned)

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0  | 4 | `magic` | `MAGIC_RUN_SEGMENT`. |
| 4  | 2 | `version` | `1`. |
| 6  | 1 | `flags` | Bitfield. See below. |
| 7  | 1 | `record_kind` | `1` Signal, `2` OrderEvent, `3` Fill. |
| 8  | 8 | `created_ns` | Wall-clock nanoseconds when the segment was opened. |
| 16 | 8 | `first_event_ns` | Earliest `run_ts_ns` in segment. |
| 24 | 8 | `last_event_ns` | Latest `run_ts_ns` in segment. |
| 32 | 4 | `event_count` | Total events written. |
| 36 | 4 | `_pad0` | Zero. |
| 40 | 8 | `index_offset` | Byte offset of the index trailer; `0` if absent. |
| 48 | 1 | `compression` | `0` none, `1` LZ4. |
| 49 | 15 | `reserved[15]` | Zero-filled. |

A reader that sees an unknown `record_kind` must reject the segment.

### `RunSegmentFlags`

| Bit | Name | Meaning |
|---:|---|---|
| `0x01` | `HasIndex` | Sparse index trailer present. Same shape as `.lrvx`. |
| `0x02` | `Compressed` | Frame stream partitioned into `CompressedBlock`s. |
| `0x04` | `Sorted` | Writer guarantees `run_ts_ns` is monotonically non-decreasing. |

### Frame stream

A frame is the standard 12-byte `FrameHeader` (the same struct `.lrvx` uses) followed by `size` bytes of payload. Payload meaning depends on `type`:

- `type = 10` → `SignalRecord` followed by `payload_len` bytes
- `type = 11` → `OrderEventRecord` followed by `payload_len` bytes
- `type = 12` → `FillRecord` (fixed-size, no trailing payload)

A run-segment file holds frames of one `record_kind` only. Mixing kinds within a segment is not allowed.

The `FrameHeader` from `.lrvx` is reused exactly:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | `size` |
| 4 | 4 | `crc32` |
| 8 | 1 | `type` |
| 9 | 1 | `rec_version` |
| 10 | 2 | `flags` |

`type` for run frames lives in `[10, 12]` to leave space for tape frame types in `[1, 9]`.

### `SignalRecord` (48 bytes fixed + variable payload, 8-byte aligned)

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0  | 8 | `run_ts_ns` | Wall-clock nanoseconds when the strategy emitted the signal. |
| 8  | 8 | `feed_ts_ns` | Timestamp of the tape event that triggered the decision; `0` if not applicable. |
| 16 | 4 | `signal_id` | Low 32 bits of the signal's id. |
| 20 | 2 | `name_len` | Bytes of UTF-8 signal name in the payload. |
| 22 | 2 | `symbol_count` | Number of `symbol_id` entries in the payload. |
| 24 | 4 | `payload_len` | Bytes of caller-defined payload after the trailing fields. |
| 28 | 4 | `flags` | Bit `0x01` = decision is "enter", `0x02` = "exit", `0x04` = "rebalance". |
| 32 | 8 | `strength_raw` | Signal strength, fixed-point with scale `1e8`. |
| 40 | 4 | `signal_id_hi` | High 32 bits of the signal's id. |
| 44 | 4 | `_reserved` | Zero. |

The id is 64 bits wide: `(signal_id_hi << 32) | signal_id`. A signal a runner captures carries the id of the order it asks for, `(subscriber id << 32) + n`, so its high half is the strategy's subscriber id; a later cancel or modify of the same order carries the same id. Until `signal_id_hi` existed bytes 40 to 47 were all reserved and written as zero, and capture kept only the low half. The layout and the 48-byte size did not change: a record whose id is below 2^32 is byte for byte what it was, a reader that predates the field still reads the low half at offset 16, and a record written before it reads back with a zero high half.

Readers report both: the 32-bit `signal_id` (C++ `OwnedSignal::signal_id`, C `lrvx_run_reader_signal_header`, `signal_id` / `signalId` in the bindings) is the low half, unchanged, and the whole id is C++ `OwnedSignal::signalId64()`, C `lrvx_run_reader_signal_id64`, Python `signal_id64`, Node and QuickJS `signalId64`, Codon `TraceReader.signal_id64(index)`.

Writers take the whole id: C++ `SignalView::signal_id`, C `lrvx_run_recorder_write_signal64` (`lrvx_run_recorder_write_signal` takes 32 bits and writes a zero high half), and `write_signal` / `writeSignal` in Python, Node, QuickJS and Codon.

Followed in order by:

1. `name_len` bytes of UTF-8 signal name (no terminator).
2. `symbol_count` × `uint32_t` (4 bytes each) symbol ids.
3. `payload_len` bytes of caller-defined payload (typically a JSON blob; opaque to the format).
4. Padding to align the frame end to 8 bytes.

A single signal can carry several symbol ids because cross-symbol decisions are first-class. A pair-trade signal lists both symbols; a single-symbol signal lists one.

### `OrderEventRecord` (64 bytes fixed + optional payload, 8-byte aligned)

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0  | 8 | `run_ts_ns` | Wall-clock when the event was emitted. |
| 8  | 8 | `feed_ts_ns` | Triggering tape event timestamp; `0` if not applicable. |
| 16 | 8 | `order_id` | Numeric order id from the executor's registry. |
| 24 | 8 | `parent_signal_id` | The 64-bit id of the signal that produced this event; `0` if unrooted. The capture listener (`TraceExecutionListener`) writes the order's id here, which is the id of the captured signal that asked for the order, so the two join on it. |
| 32 | 8 | `price_raw` | Fixed-point price, scale `1e8`. `0` if not applicable. |
| 40 | 8 | `qty_raw` | Fixed-point quantity, scale `1e8`. |
| 48 | 4 | `symbol_id` | |
| 52 | 1 | `event_kind` | `1` submit, `2` cancel, `3` modify, `4` ack, `5` reject, `6` expire. |
| 53 | 1 | `side` | `0` buy, `1` sell. |
| 54 | 1 | `order_type` | `0` market, `1` limit, `2` stop, `3` stop-limit. |
| 55 | 1 | `_pad0` | Zero. |
| 56 | 4 | `reason_len` | Bytes of UTF-8 reason text in the payload (e.g. for reject). |
| 60 | 4 | `flags` | Bit `0x01` = post-only, `0x02` = reduce-only, `0x04` = ioc. |

Followed by `reason_len` bytes of UTF-8 reason text and padding to 8-byte alignment.

### `FillRecord` (64 bytes, 8-byte aligned, no payload)

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0  | 8 | `run_ts_ns` | Wall-clock when the fill was processed. |
| 8  | 8 | `feed_ts_ns` | Triggering trade tape event; `0` if not applicable. |
| 16 | 8 | `order_id` | Order this fill belongs to. |
| 24 | 8 | `fill_id` | Exchange fill id; `0` if unknown. The capture listener (`TraceExecutionListener`) and a bundle's trace number the run's fills 1, 2, 3, ... here; the listener used to write the order id, so the partial fills of one order shared it. |
| 32 | 8 | `price_raw` | Fixed-point price. |
| 40 | 8 | `qty_raw` | Fixed-point quantity filled. |
| 48 | 8 | `fee_raw` | Fixed-point fee in fee currency, scale `1e8`. |
| 56 | 4 | `symbol_id` | |
| 60 | 1 | `side` | `0` buy, `1` sell. |
| 61 | 1 | `liquidity` | `0` unknown, `1` maker, `2` taker. |
| 62 | 2 | `_pad0` | Zero. |

### Sparse index

When `flags.HasIndex` is set, the `RunSegmentHeader.index_offset` points at a trailer whose layout is identical to `.lrvx`'s `SegmentIndexHeader` followed by `IndexEntry` rows keyed by `run_ts_ns`.

## Manifest

`manifest.json` is one JSON object:

```json
{
  "schema_version": 1,
  "format_version": 1,
  "strategy_id": "moving-average-cross",
  "strategy_hash": "sha256:abc123...",
  "run_started_ns": 1714123456000000000,
  "run_ended_ns": 1714123556000000000,
  "tape_refs": [
    {
      "path": "BTCUSDT.lrvx",
      "content_hash": "sha256:def456...",
      "first_event_ns": 1714123456000000000,
      "last_event_ns": 1714123556000000000
    }
  ],
  "segments": [
    {
      "name": "signals-000000.bin",
      "record_kind": "signals",
      "size_bytes": 12345,
      "first_event_ns": 1714123456000000000,
      "last_event_ns": 1714123556000000000,
      "event_count": 42
    },
    { "name": "orders-000000.bin", "record_kind": "orders", "size_bytes": 6789, ... },
    { "name": "fills-000000.bin", "record_kind": "fills", "size_bytes": 4321, ... }
  ]
}
```

`tape_refs[].path` is relative to the `.lrvxrun` directory. A tooling layer that wants to verify that the run was produced against a specific tape compares `content_hash` against a hash of the referenced `.lrvx` directory.

`strategy_hash` is the hash of the strategy source plus its build inputs. A reader can use it to detect that two runs claiming the same `strategy_id` actually used different code.

## Reader contract

A reader walks each segment file in isolation. The order of records inside a segment is defined by `run_ts_ns`. Across segments, a tool that wants a unified timeline merges by `run_ts_ns`. A tool that wants to align trace events with tape events merges trace records with tape records by `feed_ts_ns`.

A frame whose `type` is outside `[10, 12]` must be skipped. A frame whose `rec_version` is unknown must be rejected. A segment whose `record_kind` doesn't match the frame `type` it carries must be rejected.

## Versioning

`format_version = 1` is frozen. Adding a new record kind, a new flag, or a new field requires a minor bump and an explicit description of the backward-compatibility rule. Removing a field or changing a layout requires a major bump.

The trace format and the tape format version independently. A `.lrvxrun 1.0` can reference a `.lrvx 1.0` or a future `.lrvx 1.1`; the reader checks the version of each artifact separately.

## Bundle integration

`lrvx bundle pack` produces a tarball containing the strategy source, the consumed tape, the run parameters, and a `.lrvxrun` directory. `lrvx bundle replay` extracts the tarball, replays the strategy against the included tape, and compares the new `.lrvxrun` against the bundled one as the actual-vs-expected diff.

The bundle's outer layout is:

```
my-bundle.tar[.zst]
├── manifest.json           versions + integrity hashes + config
├── strategy/
│   └── <name>.py           the strategy module
├── config/
│   └── params.json         runtime params (slippage, queue model,
│                           initial capital, ...)
├── tape/                   the .lrvx tape this run drove against
├── expected_output.json    PnL, fill sequence, final positions
└── expected.lrvxrun/
```

Phase 1 covers single-strategy, single-tape bundles — hence `tape/`, singular. Bundles produced before the `.lrvxrun` ride-along landed have no `expected.lrvxrun/` entry; `replay` falls back to a JSON-only diff for those.
