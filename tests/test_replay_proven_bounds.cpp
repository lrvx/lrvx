/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// A segment header's first_event_ns / last_event_ns are the segment's minimum
// and maximum only on an uncompressed segment flagged Sorted. On a compressed
// Sorted segment they are the first and last events in arrival order, and the
// writer then sorted each block; the block index (one entry per block, each
// the block's minimum) is what bounds it. On a segment without Sorted they
// bound nothing, and neither does its index. Every reader below must prune,
// seek and order segments by what is proven, never by the arrival-order
// fields, and a seek must land strictly below its target so events sharing
// the target timestamp are not passed over.

#include "lrvx/common.h"
#include "lrvx/replay/aggregator.h"
#include "lrvx/replay/binary_format_v1.h"
#include "lrvx/replay/merged_tape_reader.h"
#include "lrvx/replay/readers/binary_log_reader.h"
#include "lrvx/replay/readers/mmap_reader.h"
#include "lrvx/replay/readers/parallel_reader.h"
#include "lrvx/replay/recording_metadata.h"
#include "lrvx/replay/writers/binary_log_writer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace lrvx;
using namespace lrvx::replay;

namespace
{

constexpr int64_t kBaseNs = 1'700'000'000'000'000'000;
constexpr int64_t kMs = 1'000'000;

struct Row
{
  int64_t ts;
  uint64_t id;
  bool operator==(const Row&) const = default;
};

std::ostream& operator<<(std::ostream& os, const Row& r)
{
  return os << "{" << (r.ts - kBaseNs) << "," << r.id << "}";
}

Row rowOf(const ReplayEvent& ev) { return Row{ev.timestamp_ns, ev.trade.trade_id}; }

int64_t at(int64_t offset) { return kBaseNs + offset; }

std::vector<Row> pick(const std::vector<Row>& rows, std::initializer_list<size_t> idx)
{
  std::vector<Row> out;
  for (size_t i : idx)
  {
    out.push_back(rows[i]);
  }
  return out;
}

std::vector<Row> stableByTime(std::vector<Row> rows)
{
  std::stable_sort(rows.begin(), rows.end(),
                   [](const Row& a, const Row& b)
                   { return a.ts < b.ts; });
  return rows;
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

class ReplayProvenBoundsTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    std::string name = ::testing::UnitTest::GetInstance()->current_test_info()->name();
    std::replace(name.begin(), name.end(), '/', '_');
    _dir = std::filesystem::temp_directory_path() / ("lrvx_replay_proven_bounds_" + name);
    std::filesystem::remove_all(_dir);
    std::filesystem::create_directories(_dir);
    RecordingMetadata meta;
    meta.recording_id = "proven-bounds";
    meta.exchange = "test";
    meta.exchange_type = "cex";
    meta.instrument_type = "perpetual";
    meta.has_trades = true;
    SymbolInfo sym;
    sym.symbol_id = 1;
    sym.name = "BTCUSDT";
    sym.base_asset = "BTC";
    sym.quote_asset = "USDT";
    meta.symbols.push_back(sym);
    meta.save(RecordingMetadata::metadataPath(_dir));
  }

  void TearDown() override { std::filesystem::remove_all(_dir); }

  // One segment named `name` holding a trade at kBaseNs + each offset, in the
  // order given. Returns the rows in write order.
  std::vector<Row> writeSegment(const std::string& name, const std::vector<int64_t>& offsets,
                                CompressionType comp, uint16_t index_interval = 4)
  {
    WriterConfig cfg{};
    cfg.output_dir = _dir;
    cfg.output_filename = name;
    cfg.compression = comp;
    cfg.index_interval = index_interval;
    BinaryLogWriter writer(cfg);
    std::vector<Row> rows;
    for (int64_t off : offsets)
    {
      replay::TradeRecord r{};
      r.exchange_ts_ns = kBaseNs + off;
      r.recv_ts_ns = r.exchange_ts_ns + 1;
      r.price_raw = Price::fromDouble(100.0).raw();
      r.qty_raw = Quantity::fromDouble(1.0).raw();
      r.symbol_id = 1;
      r.trade_id = _next_id++;
      writer.writeTrade(r);
      rows.push_back(Row{r.exchange_ts_ns, r.trade_id});
    }
    writer.close();
    return rows;
  }

  SegmentHeader headerOf(const std::string& name) const
  {
    BinaryLogIterator it(_dir / name);
    return it.header();
  }

  ReaderConfig config(std::optional<int64_t> from = std::nullopt,
                      std::optional<int64_t> to = std::nullopt) const
  {
    ReaderConfig cfg;
    cfg.data_dir = _dir;
    cfg.from_ns = from;
    cfg.to_ns = to;
    return cfg;
  }

  std::vector<Row> forEachFrom(const ReaderConfig& cfg, int64_t start) const
  {
    std::vector<Row> rows;
    BinaryLogReader reader(cfg);
    EXPECT_TRUE(reader.forEachFrom(start, [&rows](const ReplayEvent& ev)
                                   { rows.push_back(rowOf(ev)); return true; }));
    return rows;
  }

  std::vector<Row> streamFrom(const ReaderConfig& cfg, int64_t start) const
  {
    std::vector<Row> rows;
    BinaryLogReader reader(cfg);
    EXPECT_TRUE(reader.streamForEachFrom(start, [&rows](const ReplayEvent& ev)
                                         { rows.push_back(rowOf(ev)); return true; }));
    return rows;
  }

  std::vector<Row> stream(const ReaderConfig& cfg) const
  {
    std::vector<Row> rows;
    BinaryLogReader reader(cfg);
    EXPECT_TRUE(reader.streamForEach([&rows](const ReplayEvent& ev)
                                     { rows.push_back(rowOf(ev)); return true; }));
    return rows;
  }

  std::vector<Row> forEach(const ReaderConfig& cfg) const
  {
    std::vector<Row> rows;
    BinaryLogReader reader(cfg);
    EXPECT_TRUE(reader.forEach([&rows](const ReplayEvent& ev)
                               { rows.push_back(rowOf(ev)); return true; }));
    return rows;
  }

  std::vector<Row> run(const ReaderConfig& cfg, size_t threads) const
  {
    BinaryLogReader reader(cfg);
    CollectAggregator agg;
    IAggregator* panel[] = {&agg};
    EXPECT_TRUE(reader.run(panel, threads));
    return agg.rows;
  }

  std::vector<Row> merged(std::optional<int64_t> from, std::optional<int64_t> to) const
  {
    MergedTapeReaderConfig cfg{};
    cfg.tape_dirs = {_dir};
    cfg.from_ns = from;
    cfg.to_ns = to;
    MergedTapeReader reader(cfg);
    std::vector<Row> rows;
    EXPECT_TRUE(reader.streamEvents([&rows](uint32_t, const ReplayEvent& ev)
                                    { rows.push_back(rowOf(ev)); return true; }));
    return rows;
  }

  std::vector<Row> parallel(uint32_t threads, std::optional<int64_t> from = std::nullopt,
                            std::optional<int64_t> to = std::nullopt) const
  {
    ParallelReaderConfig cfg{.data_dir = _dir, .num_threads = threads, .from_ns = from, .to_ns = to};
    ParallelReader reader(cfg);
    std::vector<Row> rows;
    reader.forEach([&rows](const ReplayEvent& ev)
                   { rows.push_back(rowOf(ev)); return true; });
    return rows;
  }

  std::vector<Row> mmapFrom(int64_t start) const
  {
    MmapReader::Config cfg{.data_dir = _dir};
    MmapReader reader(cfg);
    std::vector<Row> rows;
    reader.forEachFrom(start, [&rows](const ReplayEvent& ev)
                       { rows.push_back(rowOf(ev)); return true; });
    return rows;
  }

  std::vector<Row> mmapAll() const
  {
    MmapReader::Config cfg{.data_dir = _dir};
    MmapReader reader(cfg);
    std::vector<Row> rows;
    reader.forEach([&rows](const ReplayEvent& ev)
                   { rows.push_back(rowOf(ev)); return true; });
    return rows;
  }

  std::filesystem::path _dir;
  uint64_t _next_id{1};
};

class ReplayProvenBoundsCompressionTest
    : public ReplayProvenBoundsTest,
      public ::testing::WithParamInterface<CompressionType>
{
};

}  // namespace

// ---------------------------------------------------------------------------
// forEachFrom / streamForEachFrom: a segment is passed over only when its
// proven maximum lies below the start.
// ---------------------------------------------------------------------------

TEST_F(ReplayProvenBoundsTest, FromReadsKeepCompressedSegmentWhoseLastArrivalIsBeforeStart)
{
  // One block written 100, 300, 200: the writer sorts it, the segment stays
  // Sorted, and the header records 200 as its last event.
  const auto a = writeSegment("a.lrvx", {100, 300, 200}, CompressionType::LZ4);
  const SegmentHeader h = headerOf("a.lrvx");
  ASSERT_TRUE(h.isSorted());
  ASSERT_EQ(h.last_event_ns, at(200));

  for (bool by_time : {true, false})
  {
    auto cfg = config();
    cfg.order_segments_by_time = by_time;
    EXPECT_EQ(forEachFrom(cfg, at(250)), pick(a, {1})) << "forEachFrom by_time=" << by_time;
    EXPECT_EQ(streamFrom(cfg, at(250)), pick(a, {1})) << "streamForEachFrom by_time=" << by_time;
  }
}

TEST_P(ReplayProvenBoundsCompressionTest, FromReadsKeepUnsortedSegmentWhoseLastArrivalIsBeforeStart)
{
  // Blocks of two events: 100, 400 | 200, 210. The second block starts below
  // the first one's maximum, so neither copy is Sorted; the header's last
  // event is 210 while the segment holds 400.
  const auto u = writeSegment("a.lrvx", {100, 400, 200, 210}, GetParam(), 2);
  ASSERT_FALSE(headerOf("a.lrvx").isSorted());
  ASSERT_EQ(headerOf("a.lrvx").last_event_ns, at(210));

  EXPECT_EQ(forEachFrom(config(), at(300)), pick(u, {1}));
  EXPECT_EQ(streamFrom(config(), at(300)), pick(u, {1}));
}

TEST_F(ReplayProvenBoundsTest, FromReadsDoNotSeekAnUnsortedSegmentVisitedFirst)
{
  // a ends before the start and is passed over, so the unsorted b is the
  // first segment visited. Its index (130, 140, 600; one entry per two
  // frames) is not monotonic: seeking it to 300 lands on 140 and passes over
  // the frame at 400.
  writeSegment("a.lrvx", {100, 150}, CompressionType::None, 2);
  const auto b = writeSegment("b.lrvx", {130, 400, 140, 145, 600, 610}, CompressionType::None, 2);
  ASSERT_FALSE(headerOf("b.lrvx").isSorted());

  const auto want = pick(b, {1, 4, 5});
  EXPECT_EQ(forEachFrom(config(), at(300)), want);
  EXPECT_EQ(streamFrom(config(), at(300)), want);
}

// ---------------------------------------------------------------------------
// Seeking lands on the last index entry strictly below the target.
// ---------------------------------------------------------------------------

TEST_P(ReplayProvenBoundsCompressionTest, SeekKeepsEventsEqualToTheTargetBeforeTheIndexEntry)
{
  // Index entries (or blocks) every four events: 10 | 20 20 20 20 | ...
  // The entry at the fifth event is stamped 20, and three events stamped 20
  // sit before it.
  const auto a =
      writeSegment("a.lrvx", {10, 20, 20, 20, 20, 20, 30, 40, 50}, GetParam(), 4);
  ASSERT_TRUE(headerOf("a.lrvx").isSorted());
  const std::vector<Row> want(a.begin() + 1, a.end());

  {
    BinaryLogIterator it(_dir / "a.lrvx");
    ASSERT_TRUE(it.seekToTimestamp(at(20)));
    std::vector<Row> got;
    ReplayEvent ev;
    while (it.next(ev))
    {
      if (ev.timestamp_ns >= at(20))
      {
        got.push_back(rowOf(ev));
      }
    }
    EXPECT_EQ(got, want) << "BinaryLogIterator::seekToTimestamp";
  }
  EXPECT_EQ(forEachFrom(config(), at(20)), want) << "forEachFrom";
  EXPECT_EQ(streamFrom(config(), at(20)), want) << "streamForEachFrom";
  EXPECT_EQ(merged(at(20), std::nullopt), want) << "MergedTapeReader";
  EXPECT_EQ(parallel(1, at(20)), want) << "ParallelReader";
  if (GetParam() == CompressionType::None)
  {
    EXPECT_EQ(mmapFrom(at(20)), want) << "MmapReader::forEachFrom";
    MmapSegmentReader seg(_dir / "a.lrvx");
    ASSERT_TRUE(seg.seekToTimestamp(at(20)));
    std::vector<Row> got;
    ReplayEvent ev;
    while (seg.next(ev))
    {
      if (ev.timestamp_ns >= at(20))
      {
        got.push_back(rowOf(ev));
      }
    }
    EXPECT_EQ(got, want) << "MmapSegmentReader::seekToTimestamp";
  }
}

TEST_F(ReplayProvenBoundsTest, SeekRefusesASegmentWithoutTheSortedFlag)
{
  // An unsorted segment's index proves no order; a seek must not move into it.
  const auto u = writeSegment("a.lrvx", {130, 400, 140, 145, 600, 610}, CompressionType::None, 2);
  ASSERT_FALSE(headerOf("a.lrvx").isSorted());
  BinaryLogIterator it(_dir / "a.lrvx");
  EXPECT_FALSE(it.seekToTimestamp(at(300)));
  ReplayEvent ev;
  ASSERT_TRUE(it.next(ev));
  EXPECT_EQ(rowOf(ev), u.front()) << "the iterator is left at the first event";

  MmapSegmentReader seg(_dir / "a.lrvx");
  EXPECT_FALSE(seg.seekToTimestamp(at(300)));
  ASSERT_TRUE(seg.next(ev));
  EXPECT_EQ(rowOf(ev), u.front());
}

// ---------------------------------------------------------------------------
// MergedTapeReader prunes segments only on proven bounds.
// ---------------------------------------------------------------------------

TEST_F(ReplayProvenBoundsTest, MergedKeepsCompressedSegmentWhoseLastArrivalIsBeforeFrom)
{
  const auto a = writeSegment("a.lrvx", {100, 300, 200}, CompressionType::LZ4);
  ASSERT_TRUE(headerOf("a.lrvx").isSorted());
  EXPECT_EQ(merged(at(250), std::nullopt), pick(a, {1}));
}

TEST_F(ReplayProvenBoundsTest, MergedKeepsCompressedSegmentWhoseFirstArrivalIsAfterTo)
{
  // Written 300, 100, 200 into one block: Sorted, header first event 300.
  const auto a = writeSegment("a.lrvx", {300, 100, 200}, CompressionType::LZ4);
  ASSERT_TRUE(headerOf("a.lrvx").isSorted());
  ASSERT_EQ(headerOf("a.lrvx").first_event_ns, at(300));
  EXPECT_EQ(merged(std::nullopt, at(250)), pick(a, {1, 2}));
}

TEST_F(ReplayProvenBoundsTest, MergedKeepsUnsortedSegmentWhoseFirstArrivalIsAfterTo)
{
  const auto u = writeSegment("a.lrvx", {300, 100, 200}, CompressionType::None);
  ASSERT_FALSE(headerOf("a.lrvx").isSorted());
  EXPECT_EQ(merged(std::nullopt, at(250)), pick(u, {1, 2}));
}

TEST_F(ReplayProvenBoundsTest, MergedDoesNotEndTheTapeAtTheFirstEventPastTo)
{
  // Unsorted: an event past `to` does not mean the rest of the segment is.
  {
    const auto u = writeSegment("a.lrvx", {100, 400, 200}, CompressionType::None);
    ASSERT_FALSE(headerOf("a.lrvx").isSorted());
    EXPECT_EQ(merged(std::nullopt, at(300)), pick(u, {0, 2}));
  }
  std::filesystem::remove(_dir / "a.lrvx");
  // Sorted and overlapping: the first event past `to` ends segment b, not the
  // tape; c still holds events in range.
  std::vector<int64_t> wide;
  for (int64_t t = 0; t <= 100; t += 10)
  {
    wide.push_back(t * kMs);
  }
  const auto b = writeSegment("b.lrvx", wide, CompressionType::None);
  const auto c = writeSegment("c.lrvx", {50 * kMs + 5, 60 * kMs + 5}, CompressionType::None);
  std::vector<Row> all = b;
  all.insert(all.end(), c.begin(), c.end());
  std::vector<Row> want;
  for (const Row& r : stableByTime(all))
  {
    if (r.ts <= at(70 * kMs))
    {
      want.push_back(r);
    }
  }
  EXPECT_EQ(merged(std::nullopt, at(70 * kMs)), want);
}

// ---------------------------------------------------------------------------
// ParallelReader and MmapReader.
// ---------------------------------------------------------------------------

TEST_P(ReplayProvenBoundsCompressionTest, ParallelReaderInterleavesOverlappingSegments)
{
  // Two overlapping Sorted segments take the single-thread path.
  const auto a = writeSegment("a.lrvx", {0, 20, 40, 60}, GetParam());
  const auto b = writeSegment("b.lrvx", {10, 30, 50}, GetParam());
  std::vector<Row> all = a;
  all.insert(all.end(), b.begin(), b.end());
  const auto want = stableByTime(all);
  ASSERT_EQ(forEach(config()), want);
  EXPECT_EQ(parallel(1), want) << "one thread";
  EXPECT_EQ(parallel(4), want) << "two segments";
}

TEST_P(ReplayProvenBoundsCompressionTest, ParallelReaderSortsAnUnsortedSegment)
{
  // Three segments and four threads: the merging path. The unsorted b
  // overlaps nothing; its inversion crosses a block boundary.
  const auto a = writeSegment("a.lrvx", {0, 10, 20}, GetParam());
  const auto b = writeSegment("b.lrvx", {100, 140, 120, 130}, GetParam(), 2);
  const auto c = writeSegment("c.lrvx", {200, 210}, GetParam());
  ASSERT_FALSE(headerOf("b.lrvx").isSorted());
  const auto want = forEach(config());
  ASSERT_EQ(want, pick([&]
                       {
              std::vector<Row> v = a;
              v.insert(v.end(), b.begin(), b.end());
              v.insert(v.end(), c.begin(), c.end());
              return v; }(),
                       {0, 1, 2, 3, 5, 6, 4, 7, 8}));
  EXPECT_EQ(parallel(4), want);
}

TEST_P(ReplayProvenBoundsCompressionTest, ParallelReaderBreaksTiesByFileOrderThenPosition)
{
  std::vector<Row> all;
  for (const char* name : {"a.lrvx", "b.lrvx", "c.lrvx", "d.lrvx"})
  {
    const auto rows = writeSegment(name, std::vector<int64_t>(6, 500), GetParam());
    all.insert(all.end(), rows.begin(), rows.end());
  }
  ASSERT_EQ(forEach(config()), all);
  EXPECT_EQ(parallel(4), all);
}

TEST_F(ReplayProvenBoundsTest, MmapReaderFromKeepsUnsortedSegmentWhoseLastArrivalIsBeforeStart)
{
  const auto u = writeSegment("a.lrvx", {100, 400, 200}, CompressionType::None);
  ASSERT_FALSE(headerOf("a.lrvx").isSorted());
  EXPECT_EQ(mmapFrom(at(300)), pick(u, {1}));
}

TEST_F(ReplayProvenBoundsTest, MmapReaderVisitsSegmentsInReadOrder)
{
  // Disjoint segments listed out of time order, and an unsorted one that
  // keeps its file-name position: MmapReader concatenates segments in the
  // order BinaryLogReader::segmentsInReadOrder gives.
  const auto a = writeSegment("a.lrvx", {300, 310}, CompressionType::None);
  const auto b = writeSegment("b.lrvx", {100, 110}, CompressionType::None);
  const auto u = writeSegment("c.lrvx", {500, 400}, CompressionType::None);
  const auto d = writeSegment("d.lrvx", {200, 210}, CompressionType::None);
  ASSERT_FALSE(headerOf("c.lrvx").isSorted());
  std::vector<Row> want = b;
  want.insert(want.end(), a.begin(), a.end());
  want.insert(want.end(), u.begin(), u.end());
  want.insert(want.end(), d.begin(), d.end());
  EXPECT_EQ(mmapAll(), want);

  BinaryLogReader reader(config());
  MmapReader mmap(MmapReader::Config{.data_dir = _dir});
  const auto read_order = reader.segmentsInReadOrder();
  ASSERT_EQ(mmap.segments().size(), read_order.size());
  for (size_t i = 0; i < read_order.size(); ++i)
  {
    EXPECT_EQ(mmap.segments()[i].path, read_order[i].path) << i;
  }
}

// ---------------------------------------------------------------------------
// The reorder buffer keeps arrival order among equal timestamps.
// ---------------------------------------------------------------------------

TEST_P(ReplayProvenBoundsCompressionTest, ReorderBufferKeepsArrivalOrderOnEqualTimestamps)
{
  // 1000 first, then forty events stamped 500, then 2000: not Sorted (blocks
  // of eight, the second starting below the first one's maximum), and every
  // event stamped 500 waits in the reorder buffer together.
  std::vector<int64_t> offsets = {1000};
  offsets.insert(offsets.end(), 40, 500);
  offsets.push_back(2000);
  const auto u = writeSegment("a.lrvx", offsets, GetParam(), 8);
  ASSERT_FALSE(headerOf("a.lrvx").isSorted());
  const auto want = stableByTime(u);

  EXPECT_EQ(forEach(config()), want) << "forEach";
  EXPECT_EQ(stream(config()), want) << "streamForEach";
  EXPECT_EQ(streamFrom(config(), at(0)), want) << "streamForEachFrom";
  EXPECT_EQ(run(config(), 1), want) << "run(1)";
}

TEST_F(ReplayProvenBoundsTest, ParallelRunKeepsArrivalOrderOnEqualTimestampsInsideAWorker)
{
  // Compressed and unsorted, enough blocks for block-partitioned workers;
  // each worker's range repeats the pattern above.
  std::vector<int64_t> offsets;
  for (int64_t k = 0; k < 16; ++k)
  {
    offsets.push_back(k * 10'000 + 1000);
    offsets.insert(offsets.end(), 15, k * 10'000 + 500);
  }
  const auto u = writeSegment("a.lrvx", offsets, CompressionType::LZ4, 8);
  ASSERT_FALSE(headerOf("a.lrvx").isSorted());
  auto got = run(config(), 4);
  // Workers deliver their ranges independently; within the events stamped
  // with one timestamp the arrival order must hold.
  std::vector<Row> want = stableByTime(u);
  std::stable_sort(got.begin(), got.end(),
                   [](const Row& a, const Row& b)
                   { return a.ts < b.ts; });
  EXPECT_EQ(got, want);
}

INSTANTIATE_TEST_SUITE_P(Compression, ReplayProvenBoundsCompressionTest,
                         ::testing::Values(CompressionType::None, CompressionType::LZ4),
                         [](const ::testing::TestParamInfo<CompressionType>& info)
                         { return info.param == CompressionType::None ? "None" : "LZ4"; });
