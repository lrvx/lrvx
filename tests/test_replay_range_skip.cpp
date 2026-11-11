/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// A reader opened with a time range must not decode segments that provably
// hold nothing in that range, and must return exactly what a full read
// filtered to the range returns.
//
// What the segment header proves depends on how the segment was written:
//   - an uncompressed segment flagged Sorted was written in non-decreasing
//     exchange_ts_ns order, so its header first/last are its min/max;
//   - a compressed segment flagged Sorted has sorted, non-overlapping blocks,
//     but its header first/last are the first and last events in ARRIVAL
//     order, which the writer re-sorted inside each block -- they are not
//     bounds. The block index (one entry per block, holding the block's
//     minimum) is what bounds such a segment;
//   - a segment without the flag proves nothing and is read as before.
// Each of those cases has a test below where trusting the wrong field would
// lose events.

#include "lrvx/common.h"
#include "lrvx/replay/aggregator.h"
#include "lrvx/replay/binary_format_v1.h"
#include "lrvx/replay/readers/binary_log_reader.h"
#include "lrvx/replay/writers/binary_log_writer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace lrvx;
using namespace lrvx::replay;

namespace
{

constexpr int64_t kBaseNs = 1'700'000'000'000'000'000;
constexpr int64_t kSecond = 1'000'000'000;

struct Row
{
  int64_t ts;
  uint32_t sym;
  uint64_t id;
  bool operator==(const Row&) const = default;
  bool operator<(const Row& o) const
  {
    return std::tie(ts, sym, id) < std::tie(o.ts, o.sym, o.id);
  }
};

Row rowOf(const ReplayEvent& ev)
{
  return Row{ev.timestamp_ns, ev.symbolId(), ev.trade.trade_id};
}

struct Range
{
  std::optional<int64_t> from;
  std::optional<int64_t> to;
};

std::vector<Row> filtered(const std::vector<Row>& all, const Range& r,
                          const std::set<uint32_t>& symbols = {})
{
  std::vector<Row> out;
  for (const auto& row : all)
  {
    if (r.from && row.ts < *r.from)
    {
      continue;
    }
    if (r.to && row.ts > *r.to)
    {
      continue;
    }
    if (!symbols.empty() && !symbols.contains(row.sym))
    {
      continue;
    }
    out.push_back(row);
  }
  return out;
}

class CollectAggregator : public IAggregator
{
 public:
  void onEvent(const ReplayEvent& ev) override { rows.push_back(rowOf(ev)); }
  std::unique_ptr<IAggregator> cloneEmpty() const override
  {
    return std::make_unique<CollectAggregator>();
  }
  void merge(const IAggregator& other) override
  {
    const auto& o = dynamic_cast<const CollectAggregator&>(other);
    rows.insert(rows.end(), o.rows.begin(), o.rows.end());
  }
  std::vector<Row> rows;
};

enum class Path
{
  ForEach,
  Stream,
  Run1,
  RunParallel,
};

const char* pathName(Path p)
{
  switch (p)
  {
    case Path::ForEach:
      return "forEach";
    case Path::Stream:
      return "streamForEach";
    case Path::Run1:
      return "run(1)";
    case Path::RunParallel:
      return "run(4)";
  }
  return "?";
}

constexpr Path kAllPaths[] = {Path::ForEach, Path::Stream, Path::Run1, Path::RunParallel};

class ReplayRangeSkipTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    _dir = std::filesystem::temp_directory_path() /
           ("lrvx_replay_range_skip_" +
            std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(_dir);
    std::filesystem::create_directories(_dir);
  }

  void TearDown() override { std::filesystem::remove_all(_dir); }

  // One segment named `name`, a trade at kBaseNs + each offset, written in
  // the order given. Symbols alternate 1, 2.
  void writeSegment(const std::string& name, CompressionType compression,
                    const std::vector<int64_t>& offsets_ns, uint16_t index_interval = 8,
                    int64_t base = kBaseNs)
  {
    WriterConfig cfg{};
    cfg.output_dir = _dir;
    cfg.output_filename = name;
    cfg.compression = compression;
    cfg.index_interval = index_interval;

    BinaryLogWriter writer(cfg);
    for (size_t i = 0; i < offsets_ns.size(); ++i)
    {
      writer.writeTrade(trade(base + offsets_ns[i], static_cast<uint32_t>(1 + i % 2)));
    }
    writer.close();
  }

  TradeRecord trade(int64_t ts, uint32_t symbol)
  {
    TradeRecord r{};
    r.exchange_ts_ns = ts;
    r.recv_ts_ns = ts + 1;
    r.price_raw = Price::fromDouble(100.0).raw();
    r.qty_raw = Quantity::fromDouble(1.0).raw();
    r.symbol_id = symbol;
    r.trade_id = _next_id++;
    return r;
  }

  // Eight segments, segment k holding `per_segment` trades one second apart
  // from k*100s. Gaps of 100s - per_segment seconds sit between them.
  void writeGrid(CompressionType compression, size_t per_segment = 50,
                 uint16_t index_interval = 8)
  {
    for (int k = 0; k < kGridSegments; ++k)
    {
      std::vector<int64_t> offs;
      for (size_t i = 0; i < per_segment; ++i)
      {
        offs.push_back(k * 100 * kSecond + static_cast<int64_t>(i) * kSecond);
      }
      writeSegment("seg_" + std::to_string(k) + ".lrvx", compression, offs, index_interval);
    }
  }

  ReaderConfig config(const Range& r, std::set<uint32_t> symbols = {}) const
  {
    ReaderConfig cfg;
    cfg.data_dir = _dir;
    cfg.from_ns = r.from;
    cfg.to_ns = r.to;
    cfg.symbols = std::move(symbols);
    return cfg;
  }

  std::vector<Row> read(const ReaderConfig& cfg, Path path, ReaderStats* stats = nullptr) const
  {
    BinaryLogReader reader(cfg);
    std::vector<Row> rows;
    auto collect = [&rows](const ReplayEvent& ev)
    {
      rows.push_back(rowOf(ev));
      return true;
    };
    switch (path)
    {
      case Path::ForEach:
        EXPECT_TRUE(reader.forEach(collect));
        break;
      case Path::Stream:
        EXPECT_TRUE(reader.streamForEach(collect));
        break;
      case Path::Run1:
      case Path::RunParallel:
      {
        CollectAggregator agg;
        IAggregator* panel[] = {&agg};
        EXPECT_TRUE(reader.run(panel, path == Path::Run1 ? 1 : 4));
        rows = std::move(agg.rows);
        if (path == Path::RunParallel)
        {
          std::sort(rows.begin(), rows.end());
        }
        break;
      }
    }
    if (stats != nullptr)
    {
      *stats = reader.stats();
    }
    return rows;
  }

  // The whole tape, unranged, in forEach order.
  std::vector<Row> readAll() const { return read(config({}), Path::ForEach); }

  // A ranged read must equal the full read filtered to the range, on every
  // entry point. Returns the forEach stats for counter checks.
  ReaderStats expectEquivalent(const Range& r, std::set<uint32_t> symbols = {})
  {
    const auto all = readAll();
    auto expected = filtered(all, r, symbols);
    ReaderStats for_each_stats{};
    for (Path p : kAllPaths)
    {
      ReaderStats stats{};
      auto got = read(config(r, symbols), p, &stats);
      auto want = expected;
      if (p == Path::RunParallel)
      {
        std::sort(want.begin(), want.end());
      }
      EXPECT_EQ(got, want) << pathName(p) << " range [" << r.from.value_or(-1) << ", "
                           << r.to.value_or(-1) << "]";
      if (p == Path::ForEach)
      {
        for_each_stats = stats;
      }
    }
    return for_each_stats;
  }

  SegmentHeader headerOf(const std::string& name) const
  {
    std::ifstream in(_dir / name, std::ios::binary);
    SegmentHeader header{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    return header;
  }

  void patchHeader(const std::string& name, const SegmentHeader& header) const
  {
    std::fstream io(_dir / name, std::ios::binary | std::ios::in | std::ios::out);
    io.seekp(0);
    io.write(reinterpret_cast<const char*>(&header), sizeof(header));
  }

  static int64_t at(int64_t seconds) { return kBaseNs + seconds * kSecond; }

  static constexpr int kGridSegments = 8;
  std::filesystem::path _dir;
  uint64_t _next_id{1};
};

}  // namespace

// ---------------------------------------------------------------------------
// Uncompressed Sorted segments: header bounds are exact, the segment is never
// opened when they exclude the range.
// ---------------------------------------------------------------------------

TEST_F(ReplayRangeSkipTest, RangeInAGapReadsNoSegment)
{
  writeGrid(CompressionType::None);
  const auto stats = expectEquivalent({at(150), at(180)});
  EXPECT_EQ(stats.files_read, 0u);
  EXPECT_EQ(stats.events_read, 0u);
  EXPECT_EQ(stats.segments_skipped, static_cast<uint64_t>(kGridSegments));
}

TEST_F(ReplayRangeSkipTest, RangeInsideOneSegmentReadsOnlyThatSegment)
{
  writeGrid(CompressionType::None);
  const auto stats = expectEquivalent({at(310), at(320)});
  EXPECT_EQ(stats.files_read, 1u);
  EXPECT_EQ(stats.events_read, 11u);
  EXPECT_EQ(stats.segments_skipped, static_cast<uint64_t>(kGridSegments - 1));
}

TEST_F(ReplayRangeSkipTest, RangeAcrossSeveralSegmentsReadsOnlyThose)
{
  writeGrid(CompressionType::None);
  const auto stats = expectEquivalent({at(140), at(420)});
  EXPECT_EQ(stats.files_read, 4u);  // segments 1..4
  EXPECT_EQ(stats.segments_skipped, static_cast<uint64_t>(kGridSegments - 4));
}

// to_ns and from_ns are inclusive. A range that touches a segment only at its
// first or last event must still read that segment.
TEST_F(ReplayRangeSkipTest, BoundariesAreInclusive)
{
  writeGrid(CompressionType::None);

  // last event of segment 2 .. first event of segment 4
  auto stats = expectEquivalent({at(249), at(400)});
  EXPECT_EQ(stats.files_read, 3u);
  EXPECT_EQ(stats.events_read, 1u + 50u + 1u);

  // a single instant: the first event of segment 5
  stats = expectEquivalent({at(500), at(500)});
  EXPECT_EQ(stats.files_read, 1u);
  EXPECT_EQ(stats.events_read, 1u);

  // a single instant: the last event of segment 5
  stats = expectEquivalent({at(549), at(549)});
  EXPECT_EQ(stats.files_read, 1u);
  EXPECT_EQ(stats.events_read, 1u);

  // one nanosecond either side of a segment touches nothing
  stats = expectEquivalent({at(549) + 1, at(600) - 1});
  EXPECT_EQ(stats.files_read, 0u);
}

TEST_F(ReplayRangeSkipTest, OpenEndedRanges)
{
  writeGrid(CompressionType::None);

  auto stats = expectEquivalent({at(610), std::nullopt});
  EXPECT_EQ(stats.files_read, 2u);  // segments 6, 7

  stats = expectEquivalent({std::nullopt, at(120)});
  EXPECT_EQ(stats.files_read, 2u);  // segments 0, 1

  stats = expectEquivalent({at(10'000), std::nullopt});
  EXPECT_EQ(stats.files_read, 0u);

  stats = expectEquivalent({std::nullopt, at(-1)});
  EXPECT_EQ(stats.files_read, 0u);
}

TEST_F(ReplayRangeSkipTest, SymbolFilterCombinesWithRange)
{
  writeGrid(CompressionType::None);
  const auto stats = expectEquivalent({at(140), at(420)}, {2});
  EXPECT_EQ(stats.files_read, 4u);
}

TEST_F(ReplayRangeSkipTest, ManyRangesMatchTheFilteredFullRead)
{
  writeGrid(CompressionType::None);
  for (int64_t a = -10; a <= 800; a += 37)
  {
    for (int64_t len : {0, 1, 49, 50, 151})
    {
      expectEquivalent({at(a), at(a + len)});
    }
  }
}

// The *From entry points keep their own start cursor; the range skip must not
// change what they return.
TEST_F(ReplayRangeSkipTest, FromEntryPointsMatchTheUnrangedFromRead)
{
  writeGrid(CompressionType::None);
  for (int64_t start : {0, 120, 249, 333, 700})
  {
    const Range r{at(start - 50), at(start + 130)};
    for (bool streaming : {false, true})
    {
      auto collect_from = [&](const ReaderConfig& cfg, ReaderStats* stats)
      {
        BinaryLogReader reader(cfg);
        std::vector<Row> rows;
        auto cb = [&rows](const ReplayEvent& ev)
        {
          rows.push_back(rowOf(ev));
          return true;
        };
        EXPECT_TRUE(streaming ? reader.streamForEachFrom(at(start), cb)
                              : reader.forEachFrom(at(start), cb));
        if (stats != nullptr)
        {
          *stats = reader.stats();
        }
        return rows;
      };
      const auto expected = filtered(collect_from(config({}), nullptr), r);
      ReaderStats stats{};
      EXPECT_EQ(collect_from(config(r), &stats), expected)
          << (streaming ? "streamForEachFrom " : "forEachFrom ") << start;
    }
  }
}

// ---------------------------------------------------------------------------
// Compressed Sorted segments: bounds come from the block index.
// ---------------------------------------------------------------------------

TEST_F(ReplayRangeSkipTest, CompressedRangeMatchesAndSkipsLaterSegments)
{
  writeGrid(CompressionType::LZ4);
  for (int64_t a = -10; a <= 800; a += 37)
  {
    for (int64_t len : {0, 1, 49, 50, 151})
    {
      expectEquivalent({at(a), at(a + len)});
    }
  }

  // Segments after the range are proven empty by their first index entry and
  // never decoded. Segments before it are bounded only by their last block's
  // maximum, which is not on disk, so the reader decodes that one block.
  const auto stats = expectEquivalent({at(310), at(320)});
  EXPECT_EQ(stats.segments_skipped, 4u);  // 4..7
  EXPECT_EQ(stats.files_read, 4u);        // 0..2 (last block only) and 3
  EXPECT_EQ(stats.events_read, 11u);
}

TEST_F(ReplayRangeSkipTest, CompressedParallelRunMatches)
{
  // Large enough segments that run(4) partitions each one into blocks.
  writeGrid(CompressionType::LZ4, 90, 4);
  for (const Range& r : {Range{at(150), at(180)}, Range{at(110), at(130)},
                         Range{at(205), at(489)}, Range{at(0), at(899)}})
  {
    expectEquivalent(r);
  }
}

// The writer sorts each compressed block before writing it, so a segment can
// be flagged Sorted while the events ARRIVED out of order. The header then
// records the arrival-order first and last events, which are not the
// segment's minimum and maximum. Bounds taken from that header would lose the
// events at both ends.
TEST_F(ReplayRangeSkipTest, CompressedSortedHeaderIsNotABound)
{
  // Two blocks of 8; each arrives backwards, the blocks do not overlap.
  writeSegment("a.lrvx", CompressionType::LZ4,
               {7 * kSecond, 6 * kSecond, 5 * kSecond, 4 * kSecond, 3 * kSecond, 2 * kSecond,
                1 * kSecond, 0, 15 * kSecond, 14 * kSecond, 13 * kSecond, 12 * kSecond,
                11 * kSecond, 10 * kSecond, 9 * kSecond, 8 * kSecond});
  const auto h = headerOf("a.lrvx");
  ASSERT_TRUE(h.isSorted());
  ASSERT_EQ(h.first_event_ns, at(7));
  ASSERT_EQ(h.last_event_ns, at(8));

  auto stats = expectEquivalent({at(0), at(0)});
  EXPECT_EQ(stats.events_read, 1u);
  stats = expectEquivalent({at(15), at(15)});
  EXPECT_EQ(stats.events_read, 1u);
  stats = expectEquivalent({at(9), at(9)});
  EXPECT_EQ(stats.events_read, 1u);
}

// Many events share one timestamp across several index entries. Entering the
// segment at an entry whose timestamp EQUALS from_ns would drop the equal
// events stored before it; the window opens at the last entry strictly below.
TEST_F(ReplayRangeSkipTest, EqualTimestampsAcrossIndexEntriesAreAllReturned)
{
  for (auto compression : {CompressionType::None, CompressionType::LZ4})
  {
    std::filesystem::remove_all(_dir);
    std::filesystem::create_directories(_dir);
    std::vector<int64_t> offs{0, kSecond, 2 * kSecond};
    for (int i = 0; i < 21; ++i)
    {
      offs.push_back(5 * kSecond);
    }
    offs.push_back(9 * kSecond);
    writeSegment("a.lrvx", compression, offs, 4);
    ASSERT_TRUE(headerOf("a.lrvx").isSorted());

    auto stats = expectEquivalent({at(5), at(5)});
    EXPECT_EQ(stats.events_read, 21u);
    stats = expectEquivalent({at(3), at(5)});
    EXPECT_EQ(stats.events_read, 21u);
    stats = expectEquivalent({at(5), at(9)});
    EXPECT_EQ(stats.events_read, 22u);
  }
}

// The writer compares a compressed block's minimum with the previous block's
// maximum only once that maximum is positive, so with timestamps <= 0 a
// segment can be flagged Sorted while its blocks go backwards. Its index is
// not used as a bound.
TEST_F(ReplayRangeSkipTest, CompressedNonPositiveTimestampsAreNotTrusted)
{
  // Second block entirely below the first.
  writeSegment("a.lrvx", CompressionType::LZ4, {-10, -9, -8, -7, -40, -30, -20, -15}, 4, 0);
  ASSERT_TRUE(headerOf("a.lrvx").isSorted());

  auto stats = expectEquivalent({-40, -40});
  EXPECT_EQ(stats.events_read, 1u);
  stats = expectEquivalent({-30, -11});
  EXPECT_EQ(stats.events_read, 3u);

  // Block minima ascend (-10, -5), so the index looks ordered, but the second
  // block starts below the first block's maximum: -4 sits in the first block.
  std::filesystem::remove_all(_dir);
  std::filesystem::create_directories(_dir);
  writeSegment("b.lrvx", CompressionType::LZ4, {-10, -9, -4, -3, -5, 100, 200, 300}, 4, 0);
  ASSERT_TRUE(headerOf("b.lrvx").isSorted());

  stats = expectEquivalent({-4, -4});
  EXPECT_EQ(stats.events_read, 1u);
  stats = expectEquivalent({-4, 150});
  EXPECT_EQ(stats.events_read, 3u);  // -4, -3, 100
}

// ---------------------------------------------------------------------------
// Segments whose header proves nothing are read as before.
// ---------------------------------------------------------------------------

// No Sorted flag: header first/last are arrival order and are not bounds.
TEST_F(ReplayRangeSkipTest, UnsortedSegmentIsAlwaysRead)
{
  for (auto compression : {CompressionType::None, CompressionType::LZ4})
  {
    std::filesystem::remove_all(_dir);
    std::filesystem::create_directories(_dir);
    std::vector<int64_t> offs;
    for (int i = 0; i < 20; ++i)
    {
      offs.push_back(i * kSecond);
    }
    offs.push_back(50 * kSecond);  // the maximum
    offs.push_back(2 * kSecond);   // a late event: header last = 2s
    writeSegment("a.lrvx", compression, offs, 4);
    ASSERT_FALSE(headerOf("a.lrvx").isSorted());
    ASSERT_EQ(headerOf("a.lrvx").last_event_ns, at(2));

    const auto stats = expectEquivalent({at(40), at(60)});
    EXPECT_EQ(stats.files_read, 1u);
    EXPECT_EQ(stats.events_read, 1u);
  }
}

// A Sorted segment whose header counters were zeroed (a header rewritten by an
// older tool, or a collector killed between frame and header writes) is
// recovered from its frames and still read.
TEST_F(ReplayRangeSkipTest, ZeroedHeaderFieldsAreRecoveredAndRead)
{
  writeGrid(CompressionType::None);
  auto h = headerOf("seg_3.lrvx");
  h.first_event_ns = 0;
  h.last_event_ns = 0;
  h.event_count = 0;
  patchHeader("seg_3.lrvx", h);

  auto stats = expectEquivalent({at(310), at(320)});
  EXPECT_EQ(stats.events_read, 11u);
  stats = expectEquivalent({at(150), at(180)});
  EXPECT_EQ(stats.events_read, 0u);
}

// A segment still being written has no Sorted flag (it is set at close) and
// for an uncompressed segment a header of zeros. Both are read.
TEST_F(ReplayRangeSkipTest, SegmentStillBeingWrittenIsRead)
{
  for (auto compression : {CompressionType::None, CompressionType::LZ4})
  {
    std::filesystem::remove_all(_dir);
    std::filesystem::create_directories(_dir);
    writeSegment("a.lrvx", compression, {0, kSecond, 2 * kSecond}, 2);

    WriterConfig cfg{};
    cfg.output_dir = _dir;
    cfg.output_filename = "b.lrvx";
    cfg.compression = compression;
    cfg.index_interval = 2;
    BinaryLogWriter live(cfg);
    for (int i = 0; i < 6; ++i)
    {
      live.writeTrade(trade(at(100 + i), 1));
    }
    live.flush();
    ASSERT_FALSE(headerOf("b.lrvx").isSorted());

    const auto stats = expectEquivalent({at(100), at(103)});
    EXPECT_EQ(stats.events_read, 4u);
    live.close();
  }
}

// A Sorted segment whose events lead with exchange_ts_ns == 0: the writer only
// records first_event_ns once it sees a non-zero timestamp, so the header's
// first is not the minimum. A range containing 0 must still read it.
TEST_F(ReplayRangeSkipTest, LeadingZeroTimestampsAreNotSkipped)
{
  writeSegment("a.lrvx", CompressionType::None, {0, 0, 1000, 2000}, 8, 0);
  ASSERT_TRUE(headerOf("a.lrvx").isSorted());
  ASSERT_EQ(headerOf("a.lrvx").first_event_ns, 1000);

  auto stats = expectEquivalent({-10, 10});
  EXPECT_EQ(stats.events_read, 2u);
  stats = expectEquivalent({0, 0});
  EXPECT_EQ(stats.events_read, 2u);
  stats = expectEquivalent({1, 999});
  EXPECT_EQ(stats.events_read, 0u);
}

// ---------------------------------------------------------------------------
// Directory shape.
// ---------------------------------------------------------------------------

TEST_F(ReplayRangeSkipTest, LegacyExtensionAndMixedDirectory)
{
  writeSegment("1000.floxlog", CompressionType::None, {0, kSecond, 2 * kSecond});
  writeSegment("2000.floxlog", CompressionType::LZ4, {10 * kSecond, 11 * kSecond, 12 * kSecond});
  writeSegment("3000.lrvx", CompressionType::None, {20 * kSecond, 21 * kSecond});
  writeSegment("4000.lrvx", CompressionType::LZ4, {30 * kSecond, 31 * kSecond});

  auto stats = expectEquivalent({at(1), at(1)});
  EXPECT_EQ(stats.events_read, 1u);
  EXPECT_EQ(stats.files_read, 1u);

  stats = expectEquivalent({at(20), at(21)});
  EXPECT_EQ(stats.events_read, 2u);
  stats = expectEquivalent({at(11), at(30)});
  EXPECT_EQ(stats.events_read, 5u);
}

// A segment appended later whose events lie earlier in time sorts last by name
// but must be read for a range covering its own span.
TEST_F(ReplayRangeSkipTest, AppendedOutOfOrderSegmentIsNotSkipped)
{
  for (auto compression : {CompressionType::None, CompressionType::LZ4})
  {
    std::filesystem::remove_all(_dir);
    std::filesystem::create_directories(_dir);
    writeSegment("a.lrvx", compression, {0, 10 * kSecond, 20 * kSecond, 30 * kSecond});
    writeSegment("b.lrvx", compression, {100 * kSecond, 110 * kSecond});
    writeSegment("c.lrvx", compression, {200 * kSecond, 210 * kSecond});
    writeSegment("z_append.lrvx", compression, {5 * kSecond, 15 * kSecond, 25 * kSecond});

    auto stats = expectEquivalent({at(12), at(16)});
    EXPECT_EQ(stats.events_read, 1u);  // 15s from the appended segment
    stats = expectEquivalent({at(0), at(30)});
    EXPECT_EQ(stats.events_read, 7u);
  }
}

// ---------------------------------------------------------------------------
// The switch.
// ---------------------------------------------------------------------------

TEST_F(ReplayRangeSkipTest, SkippingCanBeDisabled)
{
  writeGrid(CompressionType::None);
  auto cfg = config({at(310), at(320)});
  cfg.skip_segments_by_time = false;
  ReaderStats off{};
  const auto rows_off = read(cfg, Path::ForEach, &off);
  EXPECT_EQ(off.files_read, static_cast<uint64_t>(kGridSegments));
  EXPECT_EQ(off.segments_skipped, 0u);

  ReaderStats on{};
  const auto rows_on = read(config({at(310), at(320)}), Path::ForEach, &on);
  EXPECT_EQ(rows_on, rows_off);
  EXPECT_EQ(on.files_read, 1u);
}

// Without a range nothing is skipped.
TEST_F(ReplayRangeSkipTest, NoRangeSkipsNothing)
{
  writeGrid(CompressionType::LZ4);
  ReaderStats stats{};
  const auto rows = read(config({}), Path::Stream, &stats);
  EXPECT_EQ(rows.size(), static_cast<size_t>(kGridSegments * 50));
  EXPECT_EQ(stats.files_read, static_cast<uint64_t>(kGridSegments));
  EXPECT_EQ(stats.segments_skipped, 0u);
}
