/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/util/crc32.h"

#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace lrvx::replay
{

inline constexpr uint32_t kMagic = 0x584F4C46;  // "FLOX": the on-disk sentinel keeps the
                                                // historical spelling so existing tapes stay readable
inline constexpr uint16_t kFormatVersion = 1;

// Writers name segments with kSegmentExtension. Directory scans also accept
// kLegacySegmentExtension, the FLOX-era name, so existing archives and
// directories that mix both read completely.
inline constexpr std::string_view kSegmentExtension = ".lrvx";
inline constexpr std::string_view kLegacySegmentExtension = ".floxlog";

inline bool isSegmentExtension(std::string_view ext) noexcept
{
  return ext == kSegmentExtension || ext == kLegacySegmentExtension;
}

enum class EventType : uint8_t
{
  Trade = 1,
  BookSnapshot = 2,
  BookDelta = 3,
  // Option side-channel: mark price, implied vol, index price, open interest.
  // Additive — readers that predate this type skip the frame via FrameHeader.size
  // (see MmapSegmentReader::next unknown-type branch), so old tapes/readers are
  // unaffected and no format-version bump is needed.
  OptionQuote = 4,
  // DEX pool-state record (a Descriptor, Checkpoint, SwapDelta, or LiquidityDelta).
  // The frame is a fixed PoolStateRecordHeader followed by payload_len bytes of the
  // u256-native pool payload (32-byte big-endian, chain-native). It carries the pool
  // history -- swaps, checkpoints, liquidity events -- on the same timestamp-ordered
  // timeline as trades and books. Additive, like OptionQuote: an old reader skips it
  // via FrameHeader.size. The payload bytes are interpreted by the pool-state-tape
  // layer (PoolStateReplay), which the format header does not depend on.
  PoolState = 5,
  // One-level top-of-book refresh (BookUpdateType::TOP_OF_BOOK): same payload
  // shape as BookSnapshot/BookDelta — a BookRecordHeader (type = 2) and up to
  // one level per side. Additive, like OptionQuote: an old reader skips the
  // frame via FrameHeader.size instead of misreading a touch as book depth.
  BookTop = 6
};

// Fixed-point scale for implied volatility stored in OptionQuoteRecord.iv_raw.
// 0.65 (65 vol points) -> 65000000. Matches the 1e8 convention used elsewhere.
inline constexpr int64_t kIvScale = 100000000;

namespace SegmentFlags
{
inline constexpr uint8_t HasIndex = 0x01;
inline constexpr uint8_t Compressed = 0x02;
inline constexpr uint8_t Encrypted = 0x04;
// Set when the writer guarantees exchange_ts_ns is monotonically non-decreasing
// across all events in the segment (within and across block boundaries).
inline constexpr uint8_t Sorted = 0x08;
}  // namespace SegmentFlags

enum class CompressionType : uint8_t
{
  None = 0,
  LZ4 = 1,
};

struct alignas(8) SegmentHeader
{
  uint32_t magic{kMagic};
  uint16_t version{kFormatVersion};
  uint8_t flags{0};
  uint8_t exchange_id{0};
  int64_t created_ns{0};
  int64_t first_event_ns{0};
  int64_t last_event_ns{0};
  uint32_t event_count{0};
  uint32_t symbol_count{0};
  uint64_t index_offset{0};
  uint8_t compression{0};
  uint8_t reserved[15]{};

  bool isValid() const noexcept { return magic == kMagic && version == kFormatVersion; }
  bool hasIndex() const noexcept { return (flags & SegmentFlags::HasIndex) && index_offset > 0; }
  // Some older writers set `compression` field but forgot the Compressed flag.
  // Treat either signal as compressed.
  bool isCompressed() const noexcept
  {
    return ((flags & SegmentFlags::Compressed) != 0) || (compression != static_cast<uint8_t>(CompressionType::None));
  }
  bool isSorted() const noexcept { return (flags & SegmentFlags::Sorted) != 0; }
  CompressionType compressionType() const noexcept { return static_cast<CompressionType>(compression); }
};
static_assert(sizeof(SegmentHeader) == 64, "SegmentHeader must be 64 bytes");

struct FrameHeader
{
  uint32_t size{0};
  uint32_t crc32{0};
  uint8_t type{0};
  uint8_t rec_version{1};
  uint16_t flags{0};
};
static_assert(sizeof(FrameHeader) == 12, "FrameHeader must be 12 bytes");

struct alignas(8) TradeRecord
{
  int64_t exchange_ts_ns{0};
  int64_t recv_ts_ns{0};
  int64_t price_raw{0};
  int64_t qty_raw{0};
  uint64_t trade_id{0};
  uint32_t symbol_id{0};
  // Aggressor side, encoded as lrvx::Side: 0 = buy, 1 = sell. The same byte in
  // docs/spec/lrvx.md, in every archive importer and in the C, Node and
  // Python writers. Readers that treated 1 as the buy inverted the aggressor of
  // every tape they touched; see docs/spec/lrvx.md, "Side byte".
  uint8_t side{0};
  uint8_t instrument{0};
  uint16_t exchange_id{0};
};
static_assert(sizeof(TradeRecord) == 48, "TradeRecord must be 48 bytes");

// Option side-channel quote. All *_price_raw use the same PRICE_SCALE as
// TradeRecord; *_iv_raw use kIvScale; *_size_raw / open_interest_raw use the
// venue's contract/quantity scale (QUANTITY_SCALE). Fields are independent of
// any trade — emitted whenever the venue publishes a mark/IV/quote snapshot.
// Every field is 0 when the venue does not publish it. The set is what an
// options backtest actually consumes:
//   - index   = spot index of the base coin (delta-hedge leg).
//   - underlying = per-expiry FORWARD (pricing / greeks / log-moneyness).
//   - bid/ask price + size = the touch the realistic fill model crosses.
//   - mark_iv / bid_iv / ask_iv = vol at the mark and at each touch (the
//     tradeable vol; the IV-vs-RV edge lives here).
struct alignas(8) OptionQuoteRecord
{
  int64_t exchange_ts_ns{0};
  int64_t recv_ts_ns{0};
  int64_t mark_price_raw{0};
  int64_t index_price_raw{0};       // spot index of the base coin
  int64_t underlying_price_raw{0};  // per-expiry forward (pricing/moneyness)
  int64_t iv_raw{0};                // implied vol at the mark
  int64_t bid_price_raw{0};         // best bid; 0 when not published
  int64_t ask_price_raw{0};         // best ask; 0 when not published
  int64_t bid_size_raw{0};          // size at the bid
  int64_t ask_size_raw{0};          // size at the ask
  int64_t bid_iv_raw{0};            // implied vol at the bid
  int64_t ask_iv_raw{0};            // implied vol at the ask
  int64_t open_interest_raw{0};
  uint32_t symbol_id{0};
  uint8_t instrument{0};
  uint8_t _pad{0};
  uint16_t exchange_id{0};
};
static_assert(sizeof(OptionQuoteRecord) == 112, "OptionQuoteRecord must be 112 bytes");

// Pool-state record subtype, mirroring the pool-state-tape PoolRecord enum
// (Descriptor / Checkpoint / SwapDelta / LiquidityDelta). Kept as a raw value so
// the format header stays free of the curve / tape headers.
enum class PoolStateKind : uint8_t
{
  Descriptor = 1,
  Checkpoint = 2,
  SwapDelta = 3,
  LiquidityDelta = 4,
};

// Fixed header for an EventType::PoolState frame. exchange_ts_ns is first so the
// block-level timestamp scan reads it like any other record. payload_len bytes of
// the u256-native pool payload follow this header within the same frame; sub_type
// (a PoolStateKind) and venue (a PoolVenue) say how to interpret them.
struct alignas(8) PoolStateRecordHeader
{
  int64_t exchange_ts_ns{0};
  int64_t recv_ts_ns{0};
  uint32_t symbol_id{0};
  uint32_t payload_len{0};
  uint8_t sub_type{0};
  uint8_t venue{0};
  uint16_t exchange_id{0};
  uint32_t _pad{0};
};
static_assert(sizeof(PoolStateRecordHeader) == 32, "PoolStateRecordHeader must be 32 bytes");

struct alignas(8) BookLevel
{
  int64_t price_raw{0};
  int64_t qty_raw{0};
};
static_assert(sizeof(BookLevel) == 16, "BookLevel must be 16 bytes");

struct alignas(8) BookRecordHeader
{
  int64_t exchange_ts_ns{0};
  int64_t recv_ts_ns{0};
  int64_t seq{0};
  uint32_t symbol_id{0};
  uint16_t bid_count{0};
  uint16_t ask_count{0};
  uint8_t type{0};
  uint8_t instrument{0};
  uint16_t exchange_id{0};
  uint32_t _pad{0};
};
static_assert(sizeof(BookRecordHeader) == 40, "BookRecordHeader must be 40 bytes");

// Crc32 lives in lrvx/util/crc32.h; kept reachable as lrvx::replay::Crc32 for
// the replay writers/readers that reference it unqualified.
using lrvx::util::Crc32;

inline constexpr size_t bookRecordSize(uint16_t bid_count, uint16_t ask_count) noexcept
{
  return sizeof(BookRecordHeader) + (bid_count + ask_count) * sizeof(BookLevel);
}

inline constexpr uint32_t kBlockMagic = 0x4B4C4246;

struct alignas(8) CompressedBlockHeader
{
  uint32_t magic{kBlockMagic};
  uint32_t compressed_size{0};
  uint32_t original_size{0};
  uint16_t event_count{0};
  uint16_t flags{0};

  bool isValid() const noexcept { return magic == kBlockMagic; }
};
static_assert(sizeof(CompressedBlockHeader) == 16, "CompressedBlockHeader must be 16 bytes");

inline constexpr uint32_t kIndexMagic = 0x58444E49;
inline constexpr uint16_t kIndexVersion = 1;

struct alignas(8) IndexEntry
{
  int64_t timestamp_ns{0};
  uint64_t file_offset{0};
};
static_assert(sizeof(IndexEntry) == 16, "IndexEntry must be 16 bytes");

struct alignas(8) SegmentIndexHeader
{
  uint32_t magic{kIndexMagic};
  uint16_t version{kIndexVersion};
  uint16_t interval{0};
  uint32_t entry_count{0};
  uint32_t crc32{0};
  int64_t first_ts_ns{0};
  int64_t last_ts_ns{0};

  bool isValid() const noexcept { return magic == kIndexMagic && version == kIndexVersion; }
};
static_assert(sizeof(SegmentIndexHeader) == 32, "SegmentIndexHeader must be 32 bytes");

inline constexpr uint32_t kGlobalIndexMagic = 0x58444947;
inline constexpr uint16_t kGlobalIndexVersion = 1;

struct alignas(8) GlobalIndexSegment
{
  int64_t first_event_ns{0};
  int64_t last_event_ns{0};
  uint32_t event_count{0};
  uint32_t flags{0};
  uint64_t file_size{0};
  uint64_t filename_offset{0};
  uint64_t _reserved{0};
};
static_assert(sizeof(GlobalIndexSegment) == 48, "GlobalIndexSegment must be 48 bytes");

struct alignas(8) GlobalIndexHeader
{
  uint32_t magic{kGlobalIndexMagic};
  uint16_t version{kGlobalIndexVersion};
  uint16_t flags{0};
  int64_t created_ns{0};
  int64_t first_event_ns{0};
  int64_t last_event_ns{0};
  uint32_t segment_count{0};
  uint32_t crc32{0};
  uint64_t total_events{0};
  uint64_t string_table_offset{0};
  uint8_t reserved[8]{};

  bool isValid() const noexcept
  {
    return magic == kGlobalIndexMagic && version == kGlobalIndexVersion;
  }
};
static_assert(sizeof(GlobalIndexHeader) == 64, "GlobalIndexHeader must be 64 bytes");

inline constexpr uint16_t kDefaultIndexInterval = 1000;

}  // namespace lrvx::replay
