# Binary Log Format (v1)

The replay system uses a compact binary format optimized for sequential reads and fast seeks. All multi-byte values are little-endian. Structures are aligned for direct memory mapping.

## File Structure

```
┌──────────────────────────────────────┐
│          SegmentHeader (64B)         │
├──────────────────────────────────────┤
│            Frame 0                   │
│  ┌────────────────────────────────┐  │
│  │     FrameHeader (12B)          │  │
│  │     Record (variable)          │  │
│  └────────────────────────────────┘  │
├──────────────────────────────────────┤
│            Frame 1                   │
│              ...                     │
├──────────────────────────────────────┤
│            Frame N                   │
├──────────────────────────────────────┤
│      SegmentIndex (optional)         │
└──────────────────────────────────────┘
```

## SegmentHeader (64 bytes)

| Offset | Size | Field           | Description                          |
|--------|------|-----------------|--------------------------------------|
| 0      | 4    | magic           | `0x584F4C46` ("FLOX")                |
| 4      | 2    | version         | Format version (currently 1)         |
| 6      | 1    | flags           | Bit flags (see below)                |
| 7      | 1    | exchange_id     | Exchange identifier                  |
| 8      | 8    | created_ns      | Segment creation timestamp (ns)      |
| 16     | 8    | first_event_ns  | First event timestamp (ns)           |
| 24     | 8    | last_event_ns   | Last event timestamp (ns)            |
| 32     | 4    | event_count     | Total events in segment              |
| 36     | 4    | symbol_count    | Unique symbols in segment            |
| 40     | 8    | index_offset    | Byte offset to index (0 if none)     |
| 48     | 1    | compression     | Compression type (0=none, 1=LZ4)     |
| 49     | 15   | reserved        | Reserved for future use              |

### Segment Flags

| Bit | Name       | Description                    |
|-----|------------|--------------------------------|
| 0   | HasIndex   | Segment contains seek index    |
| 1   | Compressed | Data is LZ4 compressed         |
| 2   | Encrypted  | Data is encrypted (reserved)   |
| 3   | Sorted     | `exchange_ts_ns` is non-decreasing over every event in the order the segment stores it, within and across block boundaries. On a compressed segment that is the order after the writer sorted each block, not the arrival order. Zero and negative timestamps order like any other value |

## FrameHeader (12 bytes)

Each event is wrapped in a frame for integrity checking:

| Offset | Size | Field       | Description                        |
|--------|------|-------------|------------------------------------|
| 0      | 4    | size        | Payload size (excluding header)    |
| 4      | 4    | crc32       | CRC32 of payload                   |
| 8      | 1    | type        | `EventType` (see below)            |
| 9      | 1    | rec_version | Record format version              |
| 10     | 2    | flags       | Reserved                           |

### EventType

| Value | Name | Payload |
|-------|------|---------|
| 1 | `Trade` | `TradeRecord` (48 bytes) |
| 2 | `BookSnapshot` | `BookRecordHeader` + `BookLevel[]` |
| 3 | `BookDelta` | `BookRecordHeader` + `BookLevel[]` |
| 4 | `OptionQuote` | `OptionQuoteRecord` (112 bytes) |
| 5 | `PoolState` | `PoolStateRecordHeader` (32 bytes) + `payload_len` bytes |

Types 4 and 5 are additive: a reader that predates them skips the frame via
`FrameHeader.size`, so old tapes and old readers are unaffected and no format-version bump was
needed.

## TradeRecord (48 bytes)

| Offset | Size | Field          | Description                      |
|--------|------|----------------|----------------------------------|
| 0      | 8    | exchange_ts_ns | Exchange timestamp (ns)          |
| 8      | 8    | recv_ts_ns     | Local receive timestamp (ns)     |
| 16     | 8    | price_raw      | Price (fixed-point, 8 decimals)  |
| 24     | 8    | qty_raw        | Quantity (fixed-point)           |
| 32     | 8    | trade_id       | Exchange trade ID                |
| 40     | 4    | symbol_id      | Symbol registry ID               |
| 44     | 1    | side           | 0=sell, 1=buy                    |
| 45     | 1    | instrument     | Instrument type                  |
| 46     | 2    | exchange_id    | Exchange identifier              |

## BookRecordHeader (40 bytes)

| Offset | Size | Field          | Description                      |
|--------|------|----------------|----------------------------------|
| 0      | 8    | exchange_ts_ns | Exchange timestamp (ns)          |
| 8      | 8    | recv_ts_ns     | Local receive timestamp (ns)     |
| 16     | 8    | seq            | Sequence number                  |
| 24     | 4    | symbol_id      | Symbol registry ID               |
| 28     | 2    | bid_count      | Number of bid levels             |
| 30     | 2    | ask_count      | Number of ask levels             |
| 32     | 1    | type           | 0=snapshot, 1=delta              |
| 33     | 1    | instrument     | Instrument type                  |
| 34     | 2    | exchange_id    | Exchange identifier              |
| 36     | 4    | _pad           | Alignment padding                |

Immediately following the header are `bid_count + ask_count` BookLevel entries.

## BookLevel (16 bytes)

| Offset | Size | Field     | Description                      |
|--------|------|-----------|----------------------------------|
| 0      | 8    | price_raw | Price (fixed-point, 8 decimals)  |
| 8      | 8    | qty_raw   | Quantity (fixed-point)           |

## OptionQuoteRecord (112 bytes)

Option side-channel snapshot, emitted whenever the venue publishes mark / IV / quote data.
Independent of any trade. Every field is 0 when the venue does not publish it. `*_price_raw` use the
same 8-decimal price scale as `TradeRecord`; `*_iv_raw` use `kIvScale` (also 1e8, so 0.65 = 65 vol
points = `65000000`); `*_size_raw` and `open_interest_raw` use the quantity scale.

| Offset | Size | Field                 | Description                      |
|--------|------|-----------------------|----------------------------------|
| 0      | 8    | exchange_ts_ns        | Exchange timestamp (ns)          |
| 8      | 8    | recv_ts_ns            | Local receive timestamp (ns)     |
| 16     | 8    | mark_price_raw        | Mark price                       |
| 24     | 8    | index_price_raw       | Spot index of the base coin (delta-hedge leg) |
| 32     | 8    | underlying_price_raw  | Per-expiry forward (pricing, greeks, log-moneyness) |
| 40     | 8    | iv_raw                | Implied vol at the mark          |
| 48     | 8    | bid_price_raw         | Best bid; 0 when not published   |
| 56     | 8    | ask_price_raw         | Best ask; 0 when not published   |
| 64     | 8    | bid_size_raw          | Size at the bid                  |
| 72     | 8    | ask_size_raw          | Size at the ask                  |
| 80     | 8    | bid_iv_raw            | Implied vol at the bid           |
| 88     | 8    | ask_iv_raw            | Implied vol at the ask           |
| 96     | 8    | open_interest_raw     | Open interest                    |
| 104    | 4    | symbol_id             | Symbol registry ID               |
| 108    | 1    | instrument            | Instrument type                  |
| 109    | 1    | _pad                  | Alignment padding                |
| 110    | 2    | exchange_id           | Exchange identifier              |

## PoolStateRecordHeader (32 bytes)

Fixed header for an `EventType::PoolState` frame. `exchange_ts_ns` comes first so the block-level
timestamp scan reads it like any other record. `payload_len` bytes of u256-native pool payload
(32-byte big-endian, chain-native) follow within the same frame. `sub_type` is a `PoolStateKind` and
`venue` a `PoolVenue`; together they say how to interpret the payload, which is decoded by the
pool-state-tape layer, not by the format layer.

| Offset | Size | Field          | Description                      |
|--------|------|----------------|----------------------------------|
| 0      | 8    | exchange_ts_ns | Exchange timestamp (ns)          |
| 8      | 8    | recv_ts_ns     | Local receive timestamp (ns)     |
| 16     | 4    | symbol_id      | Symbol registry ID               |
| 20     | 4    | payload_len    | Payload byte length              |
| 24     | 1    | sub_type       | `PoolStateKind`                  |
| 25     | 1    | venue          | `PoolVenue`                      |
| 26     | 2    | exchange_id    | Exchange identifier              |
| 28     | 4    | _pad           | Alignment padding                |

### PoolStateKind

| Value | Name |
|-------|------|
| 1 | `Descriptor` |
| 2 | `Checkpoint` |
| 3 | `SwapDelta` |
| 4 | `LiquidityDelta` |

## Compressed Block Format

When compression is enabled, events are grouped into blocks:

### CompressedBlockHeader (16 bytes)

| Offset | Size | Field           | Description                    |
|--------|------|-----------------|--------------------------------|
| 0      | 4    | magic           | `0x4B4C4246` ("FBLK")          |
| 4      | 4    | compressed_size | Size after compression         |
| 8      | 4    | original_size   | Size before compression        |
| 12     | 2    | event_count     | Events in this block           |
| 14     | 2    | flags           | Reserved                       |

## Segment Index

An optional index enables fast timestamp-based seeking:

### SegmentIndexHeader (32 bytes)

| Offset | Size | Field       | Description                      |
|--------|------|-------------|----------------------------------|
| 0      | 4    | magic       | `0x58444E49` ("INDX")            |
| 4      | 2    | version     | Index version (currently 1)      |
| 6      | 2    | interval    | Events between index entries     |
| 8      | 4    | entry_count | Number of index entries          |
| 12     | 4    | crc32       | CRC32 of index entries           |
| 16     | 8    | first_ts_ns | First indexed timestamp          |
| 24     | 8    | last_ts_ns  | Last indexed timestamp           |

### IndexEntry (16 bytes)

| Offset | Size | Field        | Description                     |
|--------|------|--------------|---------------------------------|
| 0      | 8    | timestamp_ns | Event timestamp                 |
| 8      | 8    | file_offset  | Byte offset in segment file     |

## Global Index

A manifest file tracks all segments in a data directory:

### GlobalIndexHeader (64 bytes)

| Offset | Size | Field              | Description                    |
|--------|------|--------------------|--------------------------------|
| 0      | 4    | magic              | `0x58444947` ("GIDX")          |
| 4      | 2    | version            | Version (currently 1)          |
| 6      | 2    | flags              | Reserved                       |
| 8      | 8    | created_ns         | Index creation timestamp       |
| 16     | 8    | first_event_ns     | Earliest event across segments |
| 24     | 8    | last_event_ns      | Latest event across segments   |
| 32     | 4    | segment_count      | Number of segments             |
| 36     | 4    | crc32              | CRC32 of segment entries       |
| 40     | 8    | total_events       | Total events across all segments |
| 48     | 8    | string_table_offset| Offset to filename strings     |
| 56     | 8    | reserved           | Reserved                       |

### GlobalIndexSegment (48 bytes)

| Offset | Size | Field           | Description                    |
|--------|------|-----------------|--------------------------------|
| 0      | 8    | first_event_ns  | First event in segment         |
| 8      | 8    | last_event_ns   | Last event in segment          |
| 16     | 4    | event_count     | Events in segment              |
| 20     | 4    | flags           | Segment flags                  |
| 24     | 8    | file_size       | Segment file size              |
| 32     | 8    | filename_offset | Offset into string table       |
| 40     | 8    | _reserved       | Reserved                       |

## CRC32

All CRC32 values use the standard polynomial `0xEDB88320` (IEEE 802.3).
