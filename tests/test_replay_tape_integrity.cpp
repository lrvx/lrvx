/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/error/lrvx_error.h"
#include "lrvx/replay/binary_format_v1.h"
#include "lrvx/replay/ops/segment_ops.h"
#include "lrvx/replay/ops/validator.h"
#include "lrvx/replay/readers/binary_log_reader.h"
#include "lrvx/replay/readers/parallel_reader.h"
#include "lrvx/replay/writers/binary_log_writer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace lrvx::replay;

namespace
{

constexpr int64_t kSec = 1'000'000'000LL;

TradeRecord makeTrade(int64_t ts, uint32_t symbol, int64_t price = 100)
{
  TradeRecord t{};
  t.exchange_ts_ns = ts;
  t.recv_ts_ns = ts;
  t.price_raw = price;
  t.qty_raw = 1;
  t.trade_id = static_cast<uint64_t>(ts);
  t.symbol_id = symbol;
  t.side = 0;
  return t;
}

BookRecordHeader makeBookHeader(int64_t ts, uint32_t symbol, uint16_t bids, uint16_t asks)
{
  BookRecordHeader h{};
  h.exchange_ts_ns = ts;
  h.recv_ts_ns = ts;
  h.seq = ts;
  h.symbol_id = symbol;
  h.bid_count = bids;
  h.ask_count = asks;
  h.type = static_cast<uint8_t>(EventType::BookDelta);
  return h;
}

OptionQuoteRecord makeQuote(int64_t ts, uint32_t symbol)
{
  OptionQuoteRecord q{};
  q.exchange_ts_ns = ts;
  q.recv_ts_ns = ts;
  q.mark_price_raw = 4242;
  q.symbol_id = symbol;
  return q;
}

PoolStateRecordHeader makePoolHeader(int64_t ts, uint32_t symbol, uint32_t payload_len)
{
  PoolStateRecordHeader h{};
  h.exchange_ts_ns = ts;
  h.recv_ts_ns = ts;
  h.symbol_id = symbol;
  h.payload_len = payload_len;
  h.sub_type = static_cast<uint8_t>(PoolStateKind::SwapDelta);
  return h;
}

struct ReadBack
{
  std::vector<EventType> types;
  std::vector<int64_t> timestamps;
  std::vector<uint32_t> symbols;
};

ReadBack readSegment(const std::filesystem::path& path)
{
  ReadBack out;
  BinaryLogIterator iter(path);
  if (!iter.isValid())
  {
    return out;
  }
  ReplayEvent ev;
  while (iter.next(ev))
  {
    out.types.push_back(ev.type);
    out.timestamps.push_back(ev.timestamp_ns);
    out.symbols.push_back(ev.symbolId());
  }
  return out;
}

// Five events of five different shapes on one tape: a book delta with real
// levels, an option quote, a trade, a pool-state record with payload, and a
// second trade. Any op that rewrites the tape has to give all five back.
void writeMixedTape(const std::filesystem::path& dir, const std::string& name)
{
  WriterConfig cfg{.output_dir = dir, .output_filename = name, .create_index = false};
  BinaryLogWriter w(cfg);

  BookLevel bids[2] = {{100, 5}, {99, 7}};
  BookLevel asks[1] = {{101, 3}};
  ASSERT_TRUE(w.writeBook(makeBookHeader(100, 1, 2, 1), bids, asks));
  ASSERT_TRUE(w.writeOptionQuote(makeQuote(200, 7)));
  ASSERT_TRUE(w.writeTrade(makeTrade(300, 1)));

  const std::byte payload[8] = {};
  ASSERT_TRUE(w.writePoolState(makePoolHeader(400, 9, 8), payload, sizeof(payload)));
  ASSERT_TRUE(w.writeTrade(makeTrade(500, 1)));
  w.close();
}

void expectMixedTapeIntact(const ReadBack& rb, const std::string& where)
{
  ASSERT_EQ(rb.types.size(), 5u) << where;
  EXPECT_EQ(rb.types[0], EventType::BookDelta) << where;
  EXPECT_EQ(rb.types[1], EventType::OptionQuote) << where;
  EXPECT_EQ(rb.types[2], EventType::Trade) << where;
  EXPECT_EQ(rb.types[3], EventType::PoolState) << where;
  EXPECT_EQ(rb.types[4], EventType::Trade) << where;

  EXPECT_EQ(rb.timestamps, (std::vector<int64_t>{100, 200, 300, 400, 500})) << where;
  EXPECT_EQ(rb.symbols, (std::vector<uint32_t>{1, 7, 1, 9, 1})) << where;
}

// Rewrites a closed segment's header so it looks like the tail a killed
// collector leaves behind: bytes on disk, zeroes in the header.
void zeroSegmentHeaderCounters(const std::filesystem::path& path)
{
  std::FILE* f = std::fopen(path.string().c_str(), "r+b");
  ASSERT_NE(f, nullptr);
  SegmentHeader header{};
  ASSERT_EQ(std::fread(&header, sizeof(header), 1, f), 1u);
  header.event_count = 0;
  header.first_event_ns = 0;
  header.last_event_ns = 0;
  ASSERT_EQ(std::fseek(f, 0, SEEK_SET), 0);
  ASSERT_EQ(std::fwrite(&header, sizeof(header), 1, f), 1u);
  std::fclose(f);
}

void appendRawFrame(const std::filesystem::path& path, uint8_t type, const void* payload,
                    uint32_t size, bool corrupt_crc)
{
  std::FILE* f = std::fopen(path.string().c_str(), "ab");
  ASSERT_NE(f, nullptr);
  FrameHeader fh{};
  fh.size = size;
  fh.crc32 = Crc32::compute(payload, size);
  if (corrupt_crc)
  {
    fh.crc32 ^= 0xFFFFFFFFu;
  }
  fh.type = type;
  fh.rec_version = 1;
  ASSERT_EQ(std::fwrite(&fh, sizeof(fh), 1, f), 1u);
  ASSERT_EQ(std::fwrite(payload, 1, size, f), size);
  std::fclose(f);
}

class ReplayTapeIntegrityTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    _dir = std::filesystem::temp_directory_path() /
           ("lrvx_replay_integrity_" +
            std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(_dir);
    std::filesystem::create_directories(_dir);
  }

  void TearDown() override { std::filesystem::remove_all(_dir); }

  std::filesystem::path sub(const std::string& name)
  {
    auto p = _dir / name;
    std::filesystem::create_directories(p);
    return p;
  }

  std::filesystem::path _dir;
};

// --------------------------------------------------------------------------
// Every segment op has to carry option quotes and pool state through intact.
// --------------------------------------------------------------------------

TEST_F(ReplayTapeIntegrityTest, FilterKeepsEveryRecordType)
{
  auto in = sub("in");
  writeMixedTape(in, "src.lrvx");

  auto out = sub("out") / "out.lrvx";
  WriterConfig wc{.output_dir = out.parent_path(),
                  .output_filename = out.filename().string(),
                  .create_index = false};
  uint64_t written = SegmentOps::extractTimeRange(in / "src.lrvx", out, 0, kSec, wc);

  EXPECT_EQ(written, 5u);
  expectMixedTapeIntact(readSegment(out), "extractTimeRange");
}

TEST_F(ReplayTapeIntegrityTest, ExtractSymbolsKeepsEveryRecordType)
{
  auto in = sub("in");
  writeMixedTape(in, "src.lrvx");

  auto out = sub("out") / "out.lrvx";
  WriterConfig wc{.output_dir = out.parent_path(),
                  .output_filename = out.filename().string(),
                  .create_index = false};
  uint64_t written =
      SegmentOps::extractSymbols(in / "src.lrvx", out, {1u, 7u, 9u}, wc);

  EXPECT_EQ(written, 5u);
  expectMixedTapeIntact(readSegment(out), "extractSymbols");
}

TEST_F(ReplayTapeIntegrityTest, RecompressKeepsEveryRecordType)
{
  auto in = sub("in");
  writeMixedTape(in, "src.lrvx");

  auto out = sub("out") / "out.lrvx";
  ASSERT_TRUE(SegmentOps::recompress(in / "src.lrvx", out, CompressionType::None));
  expectMixedTapeIntact(readSegment(out), "recompress");
}

TEST_F(ReplayTapeIntegrityTest, MergeKeepsEveryRecordType)
{
  auto in = sub("in");
  writeMixedTape(in, "src.lrvx");

  auto out_dir = sub("out");
  MergeConfig mc{.output_dir = out_dir, .output_name = "merged.lrvx", .create_index = false};
  auto res = SegmentOps::merge({in / "src.lrvx"}, mc);

  ASSERT_TRUE(res.success);
  EXPECT_EQ(res.events_written, 5u);
  expectMixedTapeIntact(readSegment(res.output_path), "merge");
}

TEST_F(ReplayTapeIntegrityTest, SortingMergeKeepsEveryRecordType)
{
  auto in = sub("in");
  writeMixedTape(in, "a.lrvx");
  writeMixedTape(in, "b.lrvx");

  auto out_dir = sub("out");
  MergeConfig mc{.output_dir = out_dir,
                 .output_name = "merged.lrvx",
                 .create_index = false,
                 .sort_by_timestamp = true};
  auto res = SegmentOps::merge({in / "a.lrvx", in / "b.lrvx"}, mc);

  ASSERT_TRUE(res.success);
  EXPECT_EQ(res.events_written, 10u);

  auto rb = readSegment(res.output_path);
  ASSERT_EQ(rb.types.size(), 10u);
  // Two copies of each record, in timestamp order.
  EXPECT_EQ(rb.types[2], EventType::OptionQuote);
  EXPECT_EQ(rb.types[3], EventType::OptionQuote);
  EXPECT_EQ(rb.types[6], EventType::PoolState);
  EXPECT_EQ(rb.types[7], EventType::PoolState);
  EXPECT_EQ(rb.symbols[2], 7u);
  EXPECT_EQ(rb.symbols[6], 9u);
}

TEST_F(ReplayTapeIntegrityTest, SplitKeepsEveryRecordType)
{
  auto in = sub("in");
  writeMixedTape(in, "src.lrvx");

  auto out_dir = sub("out");
  SplitConfig sc{.output_dir = out_dir,
                 .mode = SplitMode::ByEventCount,
                 .events_per_file = 100,
                 .create_index = false};
  auto res = SegmentOps::split(in / "src.lrvx", sc);

  ASSERT_TRUE(res.success);
  ASSERT_EQ(res.output_paths.size(), 1u);
  EXPECT_EQ(res.events_written, 5u);
  expectMixedTapeIntact(readSegment(res.output_paths[0]), "split");
}

TEST_F(ReplayTapeIntegrityTest, SplitBySymbolKeepsEveryRecordType)
{
  auto in = sub("in");
  writeMixedTape(in, "src.lrvx");

  auto out_dir = sub("out");
  SplitConfig sc{.output_dir = out_dir, .mode = SplitMode::BySymbol, .create_index = false};
  auto res = SegmentOps::split(in / "src.lrvx", sc);

  ASSERT_TRUE(res.success);
  EXPECT_EQ(res.events_written, 5u);

  size_t total = 0;
  bool saw_quote = false;
  bool saw_pool = false;
  for (const auto& p : res.output_paths)
  {
    auto rb = readSegment(p);
    total += rb.types.size();
    for (size_t i = 0; i < rb.types.size(); ++i)
    {
      if (rb.types[i] == EventType::OptionQuote)
      {
        saw_quote = true;
        EXPECT_EQ(rb.symbols[i], 7u);
        EXPECT_EQ(rb.timestamps[i], 200);
      }
      if (rb.types[i] == EventType::PoolState)
      {
        saw_pool = true;
        EXPECT_EQ(rb.symbols[i], 9u);
        EXPECT_EQ(rb.timestamps[i], 400);
      }
    }
  }
  EXPECT_EQ(total, 5u);
  EXPECT_TRUE(saw_quote);
  EXPECT_TRUE(saw_pool);
}

// --------------------------------------------------------------------------
// The Sorted flag must describe the file, not the arrival order.
// --------------------------------------------------------------------------

#if LRVX_LZ4_ENABLED
TEST_F(ReplayTapeIntegrityTest, SortedFlagNotSetWhenLaterBlockStartsEarlier)
{
  auto dir = sub("sorted");
  {
    WriterConfig cfg{.output_dir = dir,
                     .output_filename = "seg.lrvx",
                     .create_index = true,
                     .index_interval = 4,
                     .compression = CompressionType::LZ4};
    BinaryLogWriter w(cfg);
    for (int64_t ts : {10, 20, 30, 40})
    {
      ASSERT_TRUE(w.writeTrade(makeTrade(ts * kSec, 1)));
    }
    // The first arrival of block two (50) is later than block one's max, so a
    // detector that only looks at the first arrival sees nothing. After the
    // block sort the block actually starts at 25.
    for (int64_t ts : {50, 25, 60, 70})
    {
      ASSERT_TRUE(w.writeTrade(makeTrade(ts * kSec, 1)));
    }
    w.close();
  }

  BinaryLogIterator iter(dir / "seg.lrvx");
  ASSERT_TRUE(iter.isValid());
  EXPECT_FALSE(iter.header().isSorted());

  // The reader trusts the flag; with it clear, the reorder buffer restores
  // monotonic delivery. The window is wide enough to hold the inversion, so
  // nothing is dropped.
  ReaderConfig rc{.data_dir = dir, .reorder_window_ns = 30 * kSec};
  BinaryLogReader reader(std::move(rc));
  std::vector<int64_t> delivered;
  ASSERT_TRUE(reader.streamForEach([&](const ReplayEvent& ev)
                                   {
                                     delivered.push_back(ev.timestamp_ns);
                                     return true; }));
  ASSERT_EQ(delivered.size(), 8u);
  for (size_t i = 1; i < delivered.size(); ++i)
  {
    EXPECT_GE(delivered[i], delivered[i - 1]) << "backward jump at index " << i;
  }
}

TEST_F(ReplayTapeIntegrityTest, SortedFlagStillSetWhenBlocksAreOrdered)
{
  auto dir = sub("sorted_ok");
  {
    WriterConfig cfg{.output_dir = dir,
                     .output_filename = "seg.lrvx",
                     .create_index = true,
                     .index_interval = 4,
                     .compression = CompressionType::LZ4};
    BinaryLogWriter w(cfg);
    // Inversion inside a block is lifted by the block sort, so the closed
    // segment really is monotonic and keeps the flag.
    for (int64_t ts : {10, 30, 20, 40})
    {
      ASSERT_TRUE(w.writeTrade(makeTrade(ts * kSec, 1)));
    }
    for (int64_t ts : {50, 60, 70, 80})
    {
      ASSERT_TRUE(w.writeTrade(makeTrade(ts * kSec, 1)));
    }
    w.close();
  }

  BinaryLogIterator iter(dir / "seg.lrvx");
  ASSERT_TRUE(iter.isValid());
  EXPECT_TRUE(iter.header().isSorted());
}

// --------------------------------------------------------------------------
// A seek on a compressed segment must drop the block it was holding.
// --------------------------------------------------------------------------

TEST_F(ReplayTapeIntegrityTest, SeekOnCompressedSegmentDiscardsStaleBlock)
{
  auto dir = sub("seek");
  {
    WriterConfig cfg{.output_dir = dir,
                     .output_filename = "seg.lrvx",
                     .create_index = true,
                     .index_interval = 4,
                     .compression = CompressionType::LZ4};
    BinaryLogWriter w(cfg);
    for (int64_t ts = 10; ts <= 21; ++ts)
    {
      ASSERT_TRUE(w.writeTrade(makeTrade(ts * kSec, 1)));
    }
    w.close();
  }

  BinaryLogIterator iter(dir / "seg.lrvx");
  ASSERT_TRUE(iter.isValid());
  ASSERT_TRUE(iter.loadIndex());

  ReplayEvent ev;
  ASSERT_TRUE(iter.next(ev));
  EXPECT_EQ(ev.timestamp_ns, 10 * kSec);

  ASSERT_TRUE(iter.seekToTimestamp(18 * kSec));
  std::vector<int64_t> after;
  while (iter.next(ev))
  {
    after.push_back(ev.timestamp_ns);
  }
  ASSERT_FALSE(after.empty());
  // Blocks 10-13 | 14-17 | 18-21: the seek lands on the last block whose
  // minimum lies strictly below 18. The block held before it was 10-13.
  EXPECT_EQ(after.front(), 14 * kSec)
      << "seek returned stale events from the block held before the seek";
}

// hasIndex() has to answer from the header, the way MmapSegmentReader does;
// otherwise every "seek if the segment has an index" branch is dead.
TEST_F(ReplayTapeIntegrityTest, IteratorReportsIndexBeforeLoadIndex)
{
  auto dir = sub("hasindex");
  {
    WriterConfig cfg{.output_dir = dir,
                     .output_filename = "seg.lrvx",
                     .create_index = true,
                     .index_interval = 4,
                     .compression = CompressionType::LZ4};
    BinaryLogWriter w(cfg);
    for (int64_t ts = 10; ts <= 21; ++ts)
    {
      ASSERT_TRUE(w.writeTrade(makeTrade(ts * kSec, 1)));
    }
    w.close();
  }

  BinaryLogIterator iter(dir / "seg.lrvx");
  ASSERT_TRUE(iter.isValid());
  EXPECT_TRUE(iter.hasIndex());
  EXPECT_TRUE(iter.seekToTimestamp(18 * kSec));
}
#endif  // LRVX_LZ4_ENABLED

// --------------------------------------------------------------------------
// An uncompressed segment with a zeroed header still has to be readable.
// --------------------------------------------------------------------------

TEST_F(ReplayTapeIntegrityTest, UncompressedLiveTailRecoversMetadata)
{
  auto dir = sub("livetail");
  {
    WriterConfig cfg{.output_dir = dir, .output_filename = "seg.lrvx", .create_index = false};
    BinaryLogWriter w(cfg);
    for (int64_t ts = 1; ts <= 20; ++ts)
    {
      ASSERT_TRUE(w.writeTrade(makeTrade(ts * kSec, 1)));
    }
    w.close();
  }
  zeroSegmentHeaderCounters(dir / "seg.lrvx");

  auto summary = BinaryLogReader::inspect(dir);
  EXPECT_EQ(summary.total_events, 20u);
  EXPECT_EQ(summary.first_event_ns, 1 * kSec);
  EXPECT_EQ(summary.last_event_ns, 20 * kSec);

  ReaderConfig rc{.data_dir = dir};
  BinaryLogReader reader(std::move(rc));
  auto s = reader.summary();
  EXPECT_EQ(s.total_events, 20u);
  EXPECT_EQ(s.first_event_ns, 1 * kSec);

  uint64_t seen = 0;
  ASSERT_TRUE(reader.forEachFrom(10 * kSec,
                                 [&](const ReplayEvent&)
                                 {
                                   ++seen;
                                   return true;
                                 }));
  EXPECT_EQ(seen, 11u) << "range read skipped a segment whose header reads zero";
}

// A zeroed first_event_ns on one segment must not drag the dataset's start
// back to the epoch.
TEST_F(ReplayTapeIntegrityTest, SummaryIgnoresUnknownSegmentStart)
{
  auto dir = sub("summary");
  {
    WriterConfig cfg{.output_dir = dir, .output_filename = "a.lrvx", .create_index = false};
    BinaryLogWriter w(cfg);
    for (int64_t ts = 100; ts <= 105; ++ts)
    {
      ASSERT_TRUE(w.writeTrade(makeTrade(ts * kSec, 1)));
    }
    w.close();
  }

  ReaderConfig rc{.data_dir = dir};
  BinaryLogReader reader(std::move(rc));
  auto s = reader.summary();
  EXPECT_EQ(s.first_event_ns, 100 * kSec);
  EXPECT_EQ(s.total_events, 6u);
}

// --------------------------------------------------------------------------
// The validator has to be able to say "not valid".
// --------------------------------------------------------------------------

TEST_F(ReplayTapeIntegrityTest, ValidatorRejectsEventCountMismatch)
{
  auto dir = sub("validate");
  {
    WriterConfig cfg{.output_dir = dir, .output_filename = "seg.lrvx", .create_index = false};
    BinaryLogWriter w(cfg);
    for (int64_t ts = 1; ts <= 10; ++ts)
    {
      ASSERT_TRUE(w.writeTrade(makeTrade(ts * kSec, 1)));
    }
    w.close();
  }
  zeroSegmentHeaderCounters(dir / "seg.lrvx");

  SegmentValidator validator;
  auto result = validator.validate(dir / "seg.lrvx");

  EXPECT_EQ(result.reported_event_count, 0u);
  EXPECT_EQ(result.actual_event_count, 10u);
  EXPECT_TRUE(result.hasErrors());
  EXPECT_FALSE(result.valid);
  EXPECT_FALSE(isValidSegment(dir / "seg.lrvx"));
}

TEST_F(ReplayTapeIntegrityTest, ValidatorRejectsIndexFlagWithoutIndex)
{
  auto dir = sub("badindex");
  {
    WriterConfig cfg{.output_dir = dir, .output_filename = "seg.lrvx", .create_index = false};
    BinaryLogWriter w(cfg);
    for (int64_t ts = 1; ts <= 10; ++ts)
    {
      ASSERT_TRUE(w.writeTrade(makeTrade(ts * kSec, 1)));
    }
    w.close();
  }

  // Claim an index that sits exactly at EOF: the flag is on, the bytes are not
  // there. This is what the closed production segments carry.
  const auto path = dir / "seg.lrvx";
  const auto size = std::filesystem::file_size(path);
  {
    std::FILE* f = std::fopen(path.string().c_str(), "r+b");
    ASSERT_NE(f, nullptr);
    SegmentHeader header{};
    ASSERT_EQ(std::fread(&header, sizeof(header), 1, f), 1u);
    header.flags |= SegmentFlags::HasIndex;
    header.index_offset = size;
    ASSERT_EQ(std::fseek(f, 0, SEEK_SET), 0);
    ASSERT_EQ(std::fwrite(&header, sizeof(header), 1, f), 1u);
    std::fclose(f);
  }

  SegmentValidator validator;
  auto result = validator.validate(path);
  EXPECT_TRUE(result.has_index);
  EXPECT_FALSE(result.index_valid);
  EXPECT_FALSE(result.valid);
}

TEST_F(ReplayTapeIntegrityTest, ValidatorAcceptsIntactSegment)
{
  auto dir = sub("good");
  {
    WriterConfig cfg{.output_dir = dir, .output_filename = "seg.lrvx", .create_index = true};
    BinaryLogWriter w(cfg);
    for (int64_t ts = 1; ts <= 10; ++ts)
    {
      ASSERT_TRUE(w.writeTrade(makeTrade(ts * kSec, 1)));
    }
    w.close();
  }

  SegmentValidator validator;
  auto result = validator.validate(dir / "seg.lrvx");
  EXPECT_TRUE(result.valid) << (result.issues.empty() ? "" : result.issues.front().message);
  EXPECT_TRUE(isValidSegment(dir / "seg.lrvx"));
}

// --------------------------------------------------------------------------
// Late events: filtered symbols must not move the watermark, and a late event
// is dropped and counted rather than thrown.
// --------------------------------------------------------------------------

// sym 1 marches forward; sym 2 joins late with a clock 30s behind.
void writeLaggingTape(const std::filesystem::path& dir, const std::string& name)
{
  WriterConfig cfg{.output_dir = dir, .output_filename = name, .create_index = false};
  BinaryLogWriter w(cfg);
  for (int64_t i = 0; i < 50; ++i)
  {
    ASSERT_TRUE(w.writeTrade(makeTrade((100 + i) * kSec, 1)));
  }
  ASSERT_TRUE(w.writeTrade(makeTrade(110 * kSec, 2)));
  for (int64_t i = 50; i < 100; ++i)
  {
    ASSERT_TRUE(w.writeTrade(makeTrade((100 + i) * kSec, 1)));
  }
  w.close();
}

TEST_F(ReplayTapeIntegrityTest, FilteredSymbolDoesNotTripTheReorderWindow)
{
  auto dir = sub("lag");
  writeLaggingTape(dir, "seg.lrvx");

  ReaderConfig rc{.data_dir = dir, .symbols = {1u}, .reorder_window_ns = 10 * kSec};
  BinaryLogReader reader(std::move(rc));

  uint64_t delivered = 0;
  ASSERT_NO_THROW({
    reader.streamForEach([&](const ReplayEvent& ev)
                         {
                           EXPECT_EQ(ev.symbolId(), 1u);
                           ++delivered;
                           return true; });
  });
  EXPECT_EQ(delivered, 100u);
  EXPECT_EQ(reader.stats().late_dropped, 0u)
      << "an event of a symbol that is not replayed was counted as late";
}

TEST_F(ReplayTapeIntegrityTest, LateEventIsDroppedAndCountedByDefault)
{
  auto dir = sub("late");
  writeLaggingTape(dir, "seg.lrvx");

  ReaderConfig rc{.data_dir = dir, .reorder_window_ns = 10 * kSec};
  BinaryLogReader reader(std::move(rc));

  uint64_t delivered = 0;
  int64_t previous = 0;
  ASSERT_NO_THROW({
    reader.streamForEach([&](const ReplayEvent& ev)
                         {
                           EXPECT_GE(ev.timestamp_ns, previous);
                           previous = ev.timestamp_ns;
                           ++delivered;
                           return true; });
  });
  EXPECT_EQ(delivered, 100u);
  EXPECT_EQ(reader.stats().late_dropped, 1u);
}

TEST_F(ReplayTapeIntegrityTest, StrictOrderingStillThrows)
{
  auto dir = sub("strict");
  writeLaggingTape(dir, "seg.lrvx");

  ReaderConfig rc{
      .data_dir = dir, .reorder_window_ns = 10 * kSec, .strict_ordering = true};
  BinaryLogReader reader(std::move(rc));

  EXPECT_THROW(reader.streamForEach([](const ReplayEvent&)
                                    { return true; }),
               lrvx::LrvxError);
}

// The watermark spans the segments of one streaming walk, so an inversion
// that straddles a segment boundary is seen instead of being reset away.
//
// Both segments are written with a one-second step backwards inside them, so
// neither earns SegmentFlags::Sorted and both go through the reorder buffer --
// which is the machinery this test is about. That detail used to be free:
// every uncompressed segment was unflagged by construction, and the original
// version of this test wrote both segments in order. It is no longer free. A
// segment written in order now advertises it and streams on its own guarantee,
// so it is never judged against a watermark left by the segment before, and
// nothing in it is dropped -- test_replay_sorted_flag.cpp
// (InOrderSegmentsKeepEveryEventWhenTheirRangesOverlap) pins that. The
// expectation below survives unchanged because the inversions keep both
// segments off the sorted path; the step is one second, well inside the ten
// second window, so it costs nothing on its own.
TEST_F(ReplayTapeIntegrityTest, WatermarkCarriesAcrossSegments)
{
  auto dir = sub("xseg");
  // Ten consecutive seconds with the third and fourth swapped: one step back,
  // enough to keep the segment off the sorted path and small enough to cost
  // nothing in the reorder buffer.
  auto unsortedRun = [](int64_t first_s)
  {
    std::vector<int64_t> offsets;
    for (int64_t i = 0; i < 10; ++i)
    {
      offsets.push_back(first_s + i);
    }
    std::swap(offsets[2], offsets[3]);
    return offsets;
  };

  {
    WriterConfig cfg{.output_dir = dir, .output_filename = "a.lrvx", .create_index = false};
    BinaryLogWriter w(cfg);
    for (int64_t s : unsortedRun(1000))
    {
      ASSERT_TRUE(w.writeTrade(makeTrade(s * kSec, 1)));
    }
    w.close();
  }
  {
    WriterConfig cfg{.output_dir = dir, .output_filename = "b.lrvx", .create_index = false};
    BinaryLogWriter w(cfg);
    // Starts 900s before the first segment ended: invisible while the
    // watermark restarts per segment.
    ASSERT_TRUE(w.writeTrade(makeTrade(109 * kSec, 1)));
    for (int64_t s : unsortedRun(1010))
    {
      ASSERT_TRUE(w.writeTrade(makeTrade(s * kSec, 1)));
    }
    w.close();
  }

  ReaderConfig rc{.data_dir = dir, .reorder_window_ns = 10 * kSec};
  BinaryLogReader reader(std::move(rc));

  int64_t previous = 0;
  uint64_t delivered = 0;
  ASSERT_NO_THROW({
    reader.streamForEach([&](const ReplayEvent& ev)
                         {
                           EXPECT_GE(ev.timestamp_ns, previous);
                           previous = ev.timestamp_ns;
                           ++delivered;
                           return true; });
  });
  EXPECT_EQ(delivered, 20u);
  EXPECT_EQ(reader.stats().late_dropped, 1u)
      << "a cross-segment inversion went unnoticed";
}

// --------------------------------------------------------------------------
// An unknown frame type is not the end of the file.
// --------------------------------------------------------------------------

TEST_F(ReplayTapeIntegrityTest, UnknownFrameTypeIsSkippedNotTreatedAsEof)
{
  auto dir = sub("unknown");
  const auto path = dir / "seg.lrvx";
  {
    WriterConfig cfg{.output_dir = dir, .output_filename = "seg.lrvx", .create_index = false};
    BinaryLogWriter w(cfg);
    ASSERT_TRUE(w.writeTrade(makeTrade(100, 1)));
    w.close();
  }

  const uint8_t future_payload[24] = {};
  appendRawFrame(path, 9, future_payload, sizeof(future_payload), false);
  auto tail = makeTrade(300, 1);
  appendRawFrame(path, static_cast<uint8_t>(EventType::Trade), &tail, sizeof(tail), false);

  auto rb = readSegment(path);
  ASSERT_EQ(rb.timestamps.size(), 2u);
  EXPECT_EQ(rb.timestamps[0], 100);
  EXPECT_EQ(rb.timestamps[1], 300);

  BinaryLogIterator iter(path);
  ASSERT_TRUE(iter.isValid());
  ReplayEvent ev;
  while (iter.next(ev))
  {
  }
  EXPECT_EQ(iter.unknownFramesSkipped(), 1u);
  EXPECT_EQ(iter.crcErrors(), 0u);
}

TEST_F(ReplayTapeIntegrityTest, CrcMismatchIsCounted)
{
  auto dir = sub("crc");
  const auto path = dir / "seg.lrvx";
  {
    WriterConfig cfg{.output_dir = dir, .output_filename = "seg.lrvx", .create_index = false};
    BinaryLogWriter w(cfg);
    ASSERT_TRUE(w.writeTrade(makeTrade(100, 1)));
    w.close();
  }
  auto bad = makeTrade(200, 1);
  appendRawFrame(path, static_cast<uint8_t>(EventType::Trade), &bad, sizeof(bad), true);

  BinaryLogIterator iter(path);
  ASSERT_TRUE(iter.isValid());
  ReplayEvent ev;
  uint64_t n = 0;
  while (iter.next(ev))
  {
    ++n;
  }
  EXPECT_EQ(n, 1u);
  EXPECT_EQ(iter.crcErrors(), 1u);
}

// --------------------------------------------------------------------------
// segments() before the first read.
// --------------------------------------------------------------------------

TEST_F(ReplayTapeIntegrityTest, SegmentsListedBeforeFirstRead)
{
  auto dir = sub("segs");
  for (const char* name : {"a.lrvx", "b.lrvx"})
  {
    WriterConfig cfg{.output_dir = dir, .output_filename = name, .create_index = false};
    BinaryLogWriter w(cfg);
    ASSERT_TRUE(w.writeTrade(makeTrade(100, 1)));
    w.close();
  }

  ReaderConfig rc{.data_dir = dir};
  BinaryLogReader reader(std::move(rc));
  EXPECT_EQ(reader.segments().size(), 2u);
}

// --------------------------------------------------------------------------
// Rotation must not scatter an op's output under wall-clock names.
// --------------------------------------------------------------------------

TEST_F(ReplayTapeIntegrityTest, OpsOutputStaysInTheNamedFile)
{
  auto in = sub("rot_in");
  {
    WriterConfig cfg{.output_dir = in, .output_filename = "src.lrvx", .create_index = false};
    BinaryLogWriter w(cfg);
    for (int64_t i = 0; i < 2000; ++i)
    {
      ASSERT_TRUE(w.writeTrade(makeTrade((1000 + i) * kSec, 1)));
    }
    w.close();
  }

  auto out_dir = sub("rot_out");
  auto out = out_dir / "out.lrvx";
  // A rotation limit small enough that the whole result cannot fit: the op
  // still owes the caller one file at the path it was given.
  WriterConfig wc{.output_dir = out_dir,
                  .output_filename = "out.lrvx",
                  .max_segment_bytes = 4096,
                  .create_index = false};
  uint64_t written =
      SegmentOps::extractTimeRange(in / "src.lrvx", out, 0, 100000 * kSec, wc);
  EXPECT_EQ(written, 2000u);

  size_t files = 0;
  for (const auto& e : std::filesystem::directory_iterator(out_dir))
  {
    if (e.path().extension() == ".lrvx")
    {
      ++files;
    }
  }
  EXPECT_EQ(files, 1u);
  EXPECT_EQ(readSegment(out).timestamps.size(), 2000u);
}

TEST_F(ReplayTapeIntegrityTest, RotatedSegmentNamesFollowTheRequestedName)
{
  auto dir = sub("rotname");
  {
    WriterConfig cfg{.output_dir = dir,
                     .output_filename = "2025-12-22_001.lrvx",
                     .max_segment_bytes = 4096,
                     .create_index = false};
    BinaryLogWriter w(cfg);
    for (int64_t i = 0; i < 500; ++i)
    {
      ASSERT_TRUE(w.writeTrade(makeTrade((1000 + i) * kSec, 1)));
    }
    w.close();
  }

  std::vector<std::string> names;
  for (const auto& e : std::filesystem::directory_iterator(dir))
  {
    if (e.path().extension() == ".lrvx")
    {
      names.push_back(e.path().filename().string());
    }
  }
  ASSERT_GT(names.size(), 1u);
  std::sort(names.begin(), names.end());
  EXPECT_EQ(names.front(), "2025-12-22_001.lrvx");
  for (size_t i = 1; i < names.size(); ++i)
  {
    EXPECT_EQ(names[i].rfind("2025-12-22_001_", 0), 0u)
        << "rotated segment " << names[i] << " does not sort with its siblings";
  }

  // Name order is timestamp order, which is what the range search assumes.
  ReaderConfig rc{.data_dir = dir};
  BinaryLogReader reader(std::move(rc));
  const auto& segs = reader.segments();
  ASSERT_GT(segs.size(), 1u);
  for (size_t i = 1; i < segs.size(); ++i)
  {
    EXPECT_GE(segs[i].first_event_ns, segs[i - 1].first_event_ns);
  }
}

// --------------------------------------------------------------------------
// Range extraction: index-driven, and honest about a range it cannot cover.
// --------------------------------------------------------------------------

TEST_F(ReplayTapeIntegrityTest, ExtractTimeRangeReportsRangeMismatch)
{
  auto in = sub("range_in");
  {
    WriterConfig cfg{.output_dir = in, .output_filename = "src.lrvx", .create_index = true};
    BinaryLogWriter w(cfg);
    for (int64_t ts = 100; ts <= 200; ++ts)
    {
      ASSERT_TRUE(w.writeTrade(makeTrade(ts * kSec, 1)));
    }
    w.close();
  }

  auto out_dir = sub("range_out");
  auto out = out_dir / "out.lrvx";
  WriterConfig wc{.output_dir = out_dir,
                  .output_filename = "out.lrvx",
                  .create_index = false};

  RangeExtractStats stats;
  uint64_t written =
      SegmentOps::extractTimeRange(in / "src.lrvx", out, 10 * kSec, 500 * kSec, wc, &stats);

  EXPECT_EQ(written, 101u);
  EXPECT_EQ(stats.input_first_ns, 100 * kSec);
  EXPECT_EQ(stats.input_last_ns, 200 * kSec);
  EXPECT_TRUE(stats.requested_from_before_data);
  EXPECT_TRUE(stats.requested_to_after_data);

  RangeExtractStats inside;
  auto out2 = out_dir / "out2.lrvx";
  WriterConfig wc2{.output_dir = out_dir,
                   .output_filename = "out2.lrvx",
                   .create_index = false};
  uint64_t written2 =
      SegmentOps::extractTimeRange(in / "src.lrvx", out2, 150 * kSec, 160 * kSec, wc2, &inside);
  EXPECT_EQ(written2, 11u);
  EXPECT_FALSE(inside.requested_from_before_data);
  EXPECT_FALSE(inside.requested_to_after_data);
  EXPECT_TRUE(inside.used_index) << "the index was present and not used";
}

// --------------------------------------------------------------------------
// The parallel reader must agree with the single-threaded one.
// --------------------------------------------------------------------------

TEST_F(ReplayTapeIntegrityTest, ParallelReaderMatchesSingleThreadOnUnsortedSegment)
{
  auto dir = sub("par");
  {
    WriterConfig cfg{.output_dir = dir, .output_filename = "seg.lrvx", .create_index = false};
    BinaryLogWriter w(cfg);
    for (int64_t ts : {10, 20, 30, 100, 40, 50})
    {
      ASSERT_TRUE(w.writeTrade(makeTrade(ts * kSec, 1)));
    }
    w.close();
  }

  ReaderConfig rc{.data_dir = dir, .to_ns = 50 * kSec};
  BinaryLogReader reader(rc);
  std::vector<int64_t> single;
  reader.forEach([&](const ReplayEvent& ev)
                 {
                   single.push_back(ev.timestamp_ns);
                   return true; });

  ParallelReaderConfig pc{.data_dir = dir, .num_threads = 1, .to_ns = 50 * kSec};
  ParallelReader par(std::move(pc));
  std::vector<int64_t> parallel;
  par.forEach([&](const ReplayEvent& ev)
              {
                parallel.push_back(ev.timestamp_ns);
                return true; });

  std::sort(single.begin(), single.end());
  std::sort(parallel.begin(), parallel.end());
  EXPECT_EQ(parallel, single);
}

}  // namespace
