/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// A tape's segments are named by the wall-clock time they were opened, so a
// tape filled out of order (an archive import of day 3, then of day 1) lists
// its segments out of time order. The reader must still deliver events in
// exchange_ts_ns order wherever the segments' recorded bounds prove an order:
// segments are sorted by their first event, and segments whose ranges overlap
// are merged event by event. Equal timestamps go to the segment whose file
// name sorts first, then to the earlier event inside the segment.
//
// Segments that prove nothing (no Sorted flag, still being written) keep
// their file-name position and split the tape into runs that are ordered
// independently; the tests below pin that down as well.
//
// MergedTapeReader::streamEvents walks each tape's segments in the same read
// order but merges overlapping segments only inside its per-tape reorder
// window, so it is left out of the overlap cases.

#include "lrvx/common.h"
#include "lrvx/replay/abstract_event_reader.h"
#include "lrvx/replay/aggregator.h"
#include "lrvx/replay/binary_format_v1.h"
#include "lrvx/replay/merged_tape_reader.h"
#include "lrvx/replay/readers/binary_log_reader.h"
#include "lrvx/replay/recording_metadata.h"
#include "lrvx/replay/writers/binary_log_writer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

using namespace lrvx;
using namespace lrvx::replay;

namespace
{

constexpr int64_t kBaseNs = 1'700'000'000'000'000'000;
constexpr int64_t kSecond = 1'000'000'000;
constexpr int64_t kDay = 86'400 * kSecond;

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

bool nonDecreasing(const std::vector<Row>& rows)
{
  for (size_t i = 1; i < rows.size(); ++i)
  {
    if (rows[i].ts < rows[i - 1].ts)
    {
      return false;
    }
  }
  return true;
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
  ForEachFrom,
  Stream,
  StreamFrom,
  Run1,
  Adapter,
  Merged,
};

const char* pathName(Path p)
{
  switch (p)
  {
    case Path::ForEach:
      return "forEach";
    case Path::ForEachFrom:
      return "forEachFrom";
    case Path::Stream:
      return "streamForEach";
    case Path::StreamFrom:
      return "streamForEachFrom";
    case Path::Run1:
      return "run(1)";
    case Path::Adapter:
      return "createMultiSegmentReader";
    case Path::Merged:
      return "MergedTapeReader::streamEvents";
  }
  return "?";
}

// Every sequential entry point. run() with several threads partitions the
// tape and is checked separately.
constexpr Path kSequentialPaths[] = {Path::ForEach, Path::ForEachFrom, Path::Stream,
                                     Path::StreamFrom, Path::Run1, Path::Adapter,
                                     Path::Merged};

class ReplaySegmentOrderTest : public ::testing::TestWithParam<CompressionType>
{
 protected:
  void SetUp() override
  {
    std::string name = ::testing::UnitTest::GetInstance()->current_test_info()->name();
    std::replace(name.begin(), name.end(), '/', '_');
    _dir = std::filesystem::temp_directory_path() / ("lrvx_replay_segment_order_" + name);
    std::filesystem::remove_all(_dir);
    std::filesystem::create_directories(_dir);
    writeMetadata();
  }

  void TearDown() override { std::filesystem::remove_all(_dir); }

  CompressionType compression() const { return GetParam(); }

  void writeMetadata() const
  {
    RecordingMetadata meta;
    meta.recording_id = "segment-order";
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

  // One segment named `name` holding a trade at kBaseNs + each offset, written
  // in the order given. Returns the rows in the order they sit in the file.
  std::vector<Row> writeSegment(const std::string& name, const std::vector<int64_t>& offsets_ns,
                                std::optional<CompressionType> comp = std::nullopt,
                                uint16_t index_interval = 4)
  {
    WriterConfig cfg{};
    cfg.output_dir = _dir;
    cfg.output_filename = name;
    cfg.compression = comp.value_or(compression());
    cfg.index_interval = index_interval;
    BinaryLogWriter writer(cfg);
    std::vector<Row> rows;
    for (int64_t off : offsets_ns)
    {
      TradeRecord r{};
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

  // `count` trades `step` apart starting at `start`.
  static std::vector<int64_t> span(int64_t start, size_t count, int64_t step = kSecond)
  {
    std::vector<int64_t> out;
    for (size_t i = 0; i < count; ++i)
    {
      out.push_back(start + static_cast<int64_t>(i) * step);
    }
    return out;
  }

  // Every event of every segment, read one file at a time in file-name order,
  // then stable-sorted by timestamp: the order the reader must produce for
  // Sorted segments. stable_sort keeps (file order, position) for ties.
  std::vector<Row> reference(std::optional<int64_t> from = std::nullopt,
                             std::optional<int64_t> to = std::nullopt) const
  {
    std::vector<std::filesystem::path> paths;
    for (const auto& e : std::filesystem::directory_iterator(_dir))
    {
      if (isSegmentExtension(e.path().extension().string()))
      {
        paths.push_back(e.path());
      }
    }
    std::sort(paths.begin(), paths.end());
    std::vector<Row> rows;
    for (const auto& p : paths)
    {
      BinaryLogIterator it(p);
      ReplayEvent ev;
      while (it.next(ev))
      {
        if ((from && ev.timestamp_ns < *from) || (to && ev.timestamp_ns > *to))
        {
          continue;
        }
        rows.push_back(rowOf(ev));
      }
    }
    std::stable_sort(rows.begin(), rows.end(),
                     [](const Row& a, const Row& b)
                     { return a.ts < b.ts; });
    return rows;
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

  // `from_start` is the start passed to the *From entry points; it defaults
  // to the config's from_ns, or to the earliest possible timestamp.
  std::vector<Row> read(const ReaderConfig& cfg, Path path, ReaderStats* stats = nullptr,
                        std::optional<int64_t> from_start = std::nullopt,
                        size_t stop_after = std::numeric_limits<size_t>::max()) const
  {
    std::vector<Row> rows;
    auto collect = [&rows, stop_after](const ReplayEvent& ev)
    {
      rows.push_back(rowOf(ev));
      return rows.size() < stop_after;
    };
    const int64_t start =
        from_start.value_or(cfg.from_ns.value_or(std::numeric_limits<int64_t>::min()));
    const bool full = stop_after == std::numeric_limits<size_t>::max();
    switch (path)
    {
      case Path::ForEach:
      case Path::ForEachFrom:
      case Path::Stream:
      case Path::StreamFrom:
      {
        BinaryLogReader reader(cfg);
        bool ok = false;
        if (path == Path::ForEach)
        {
          ok = reader.forEach(collect);
        }
        else if (path == Path::ForEachFrom)
        {
          ok = reader.forEachFrom(start, collect);
        }
        else if (path == Path::Stream)
        {
          ok = reader.streamForEach(collect);
        }
        else
        {
          ok = reader.streamForEachFrom(start, collect);
        }
        EXPECT_EQ(ok, full) << pathName(path);
        if (stats != nullptr)
        {
          *stats = reader.stats();
        }
        break;
      }
      case Path::Run1:
      {
        BinaryLogReader reader(cfg);
        CollectAggregator agg;
        IAggregator* panel[] = {&agg};
        EXPECT_TRUE(reader.run(panel, 1));
        rows = std::move(agg.rows);
        if (stats != nullptr)
        {
          *stats = reader.stats();
        }
        break;
      }
      case Path::Adapter:
      {
        ReaderFilter filter;
        filter.from_ns = cfg.from_ns;
        filter.to_ns = cfg.to_ns;
        auto reader = createMultiSegmentReader(_dir, filter);
        reader->forEach(collect);
        break;
      }
      case Path::Merged:
      {
        MergedTapeReaderConfig mcfg{};
        mcfg.tape_dirs = {_dir};
        mcfg.from_ns = cfg.from_ns;
        mcfg.to_ns = cfg.to_ns;
        MergedTapeReader merged(mcfg);
        merged.streamEvents([&collect](uint32_t, const ReplayEvent& ev)
                            { return collect(ev); });
        break;
      }
    }
    return rows;
  }

  // Sorted segments only: every sequential entry point equals the reference.
  void expectReferenceEverywhere(std::optional<int64_t> from = std::nullopt,
                                 std::optional<int64_t> to = std::nullopt,
                                 bool include_merged = true)
  {
    const auto want = reference(from, to);
    ASSERT_TRUE(nonDecreasing(want));
    for (Path p : kSequentialPaths)
    {
      if (p == Path::Merged && !include_merged)
      {
        continue;
      }
      EXPECT_EQ(read(config(from, to), p), want) << pathName(p);
    }
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

  // A segment as a crashed or still-running writer leaves it: no Sorted flag,
  // counters and bounds zero.
  void makeLive(const std::string& name) const
  {
    auto h = headerOf(name);
    h.flags &= static_cast<uint8_t>(~SegmentFlags::Sorted);
    h.first_event_ns = 0;
    h.last_event_ns = 0;
    h.event_count = 0;
    patchHeader(name, h);
  }

  static int64_t at(int64_t offset) { return kBaseNs + offset; }

  std::filesystem::path _dir;
  uint64_t _next_id{1};
};

}  // namespace

// ---------------------------------------------------------------------------
// Disjoint segments listed out of time order.
// ---------------------------------------------------------------------------

TEST_P(ReplaySegmentOrderTest, DisjointSegmentsOutOfFileOrderAreDeliveredInTimeOrder)
{
  // Imported day 3, then day 1, then day 2: file names follow import order.
  const auto day3 = writeSegment("a.lrvx", span(2 * kDay, 20));
  const auto day1 = writeSegment("b.lrvx", span(0, 20));
  const auto day2 = writeSegment("c.lrvx", span(kDay, 20));

  std::vector<Row> want = day1;
  want.insert(want.end(), day2.begin(), day2.end());
  want.insert(want.end(), day3.begin(), day3.end());
  EXPECT_EQ(reference(), want);
  expectReferenceEverywhere();
}

TEST_P(ReplaySegmentOrderTest, StatsReportReorderingAndNoInterleaving)
{
  writeSegment("a.lrvx", span(2 * kDay, 10));
  writeSegment("b.lrvx", span(0, 10));
  writeSegment("c.lrvx", span(kDay, 10));

  ReaderStats stats{};
  read(config(), Path::ForEach, &stats);
  // Delivery order b, c, a: every segment moved.
  EXPECT_EQ(stats.segments_reordered, 3u);
  EXPECT_EQ(stats.segments_interleaved, 0u);
  EXPECT_EQ(stats.files_read, 3u);
}

TEST_P(ReplaySegmentOrderTest, FileNameOrderSwitchRestoresFileOrder)
{
  const auto day3 = writeSegment("a.lrvx", span(2 * kDay, 10));
  const auto day1 = writeSegment("b.lrvx", span(0, 10));
  std::vector<Row> file_order = day3;
  file_order.insert(file_order.end(), day1.begin(), day1.end());

  auto cfg = config();
  cfg.order_segments_by_time = false;
  for (Path p : {Path::ForEach, Path::ForEachFrom, Path::Stream, Path::StreamFrom, Path::Run1})
  {
    ReaderStats stats{};
    EXPECT_EQ(read(cfg, p, &stats), file_order) << pathName(p);
    EXPECT_EQ(stats.segments_reordered, 0u) << pathName(p);
  }
}

TEST_P(ReplaySegmentOrderTest, FromEntryPointsDoNotLoseSegmentsOnOutOfOrderTape)
{
  // File order day3, day1, day2. A binary search over file order for the
  // first segment ending at or after a day-2 start lands on day 2 and never
  // looks at day 3.
  writeSegment("a.lrvx", span(2 * kDay, 10));
  writeSegment("b.lrvx", span(0, 10));
  writeSegment("c.lrvx", span(kDay, 10));

  const int64_t start = at(kDay + 5 * kSecond);
  const auto want = reference(start);
  ASSERT_EQ(want.size(), 15u);
  for (Path p : {Path::ForEachFrom, Path::StreamFrom})
  {
    EXPECT_EQ(read(config(), p, nullptr, start), want) << pathName(p);
  }
}

// ---------------------------------------------------------------------------
// Overlapping Sorted segments are merged event by event.
// ---------------------------------------------------------------------------

TEST_P(ReplaySegmentOrderTest, OverlappingSegmentsAreInterleaved)
{
  // Three captures of overlapping windows, file order unrelated to time.
  writeSegment("a.lrvx", span(30 * kSecond, 25, 2 * kSecond));
  writeSegment("b.lrvx", span(0, 25, 3 * kSecond));
  writeSegment("c.lrvx", span(10 * kSecond + 500, 25, kSecond));
  expectReferenceEverywhere(std::nullopt, std::nullopt, /*include_merged=*/false);

  ReaderStats stats{};
  read(config(), Path::ForEach, &stats);
  EXPECT_EQ(stats.segments_interleaved, 3u);
}

TEST_P(ReplaySegmentOrderTest, EqualTimestampsAcrossSegmentsFollowFileOrderThenPosition)
{
  // a and b share timestamps; c starts on a's and b's last timestamp.
  const auto a = writeSegment("a.lrvx", {20, 20, 30, 40, 40});
  const auto b = writeSegment("b.lrvx", {10, 20, 20, 40});
  const auto c = writeSegment("c.lrvx", {40, 40, 50});
  const std::vector<Row> want = {b[0], a[0], a[1], b[1], b[2], a[2], a[3],
                                 a[4], b[3], c[0], c[1], c[2]};
  ASSERT_EQ(reference(), want);
  expectReferenceEverywhere(std::nullopt, std::nullopt, /*include_merged=*/false);
}

TEST_P(ReplaySegmentOrderTest, SegmentStartingOnPreviousEndIsNotInterleaved)
{
  // Back-to-back rotation: the next segment opens on the timestamp the
  // previous one closed on. That is in order, not an overlap.
  writeSegment("a.lrvx", {10, 20, 30});
  writeSegment("b.lrvx", {30, 40});
  expectReferenceEverywhere();
  ReaderStats stats{};
  read(config(), Path::ForEach, &stats);
  EXPECT_EQ(stats.segments_interleaved, 0u);
  EXPECT_EQ(stats.segments_reordered, 0u);
}

TEST_P(ReplaySegmentOrderTest, MixedCompressionOutOfOrderAndOverlapping)
{
  writeSegment("a.lrvx", span(50 * kSecond, 30), CompressionType::LZ4);
  writeSegment("b.lrvx", span(0, 30), CompressionType::None);
  writeSegment("c.lrvx", span(20 * kSecond + 7, 30), CompressionType::LZ4);
  writeSegment("d.lrvx", span(65 * kSecond + 3, 30), CompressionType::None);
  expectReferenceEverywhere(std::nullopt, std::nullopt, /*include_merged=*/false);
}

TEST_P(ReplaySegmentOrderTest, CompressedBlocksSortedOnWriteKeepTheirStoredOrder)
{
  // Inversions inside one block still leave a compressed segment Sorted: the
  // writer sorts the block. The reference reads the stored order.
  writeSegment("a.lrvx", {300, 100, 200, 400, 500, 450, 600, 700}, std::nullopt, 4);
  writeSegment("b.lrvx", {150, 250, 350, 650});
  if (compression() == CompressionType::None)
  {
    // Uncompressed: the inversion leaves segment a unsorted; covered by the
    // unsorted tests.
    GTEST_SKIP();
  }
  ASSERT_TRUE(headerOf("a.lrvx").isSorted());
  expectReferenceEverywhere();
}

// ---------------------------------------------------------------------------
// Time-ranged reads and early exit on an out-of-order tape.
// ---------------------------------------------------------------------------

TEST_P(ReplaySegmentOrderTest, TimeRangedReadsOnOutOfOrderTape)
{
  writeSegment("a.lrvx", span(300 * kSecond, 40));
  writeSegment("b.lrvx", span(0, 40));
  writeSegment("c.lrvx", span(100 * kSecond, 40));
  writeSegment("d.lrvx", span(120 * kSecond + 9, 40));  // overlaps c

  const std::pair<std::optional<int64_t>, std::optional<int64_t>> ranges[] = {
      {at(10 * kSecond), at(30 * kSecond)},
      {at(110 * kSecond), at(140 * kSecond)},
      {at(50 * kSecond), std::nullopt},
      {std::nullopt, at(130 * kSecond)},
      {at(200 * kSecond), at(250 * kSecond)},
      {at(0), at(400 * kSecond)},
  };
  for (const auto& [from, to] : ranges)
  {
    SCOPED_TRACE(testing::Message() << "from " << from.value_or(-1) - kBaseNs << " to "
                                    << to.value_or(-1) - kBaseNs);
    expectReferenceEverywhere(from, to, /*include_merged=*/false);
  }

  // A range that only covers day b leaves the others closed.
  ReaderStats stats{};
  read(config(at(10 * kSecond), at(30 * kSecond)), Path::ForEach, &stats);
  if (compression() == CompressionType::None)
  {
    EXPECT_EQ(stats.files_read, 1u);
    EXPECT_EQ(stats.segments_skipped, 3u);
  }
  EXPECT_EQ(stats.events_read, 21u);
}

TEST_P(ReplaySegmentOrderTest, EarlyExitStopsAtTheSameEventAsTheReference)
{
  writeSegment("a.lrvx", span(30 * kSecond, 20, 2 * kSecond));
  writeSegment("b.lrvx", span(0, 20, 3 * kSecond));
  writeSegment("c.lrvx", span(100 * kSecond, 20));
  const auto want = reference();
  for (size_t stop : {size_t{1}, size_t{7}, size_t{25}, size_t{41}})
  {
    const std::vector<Row> prefix(want.begin(), want.begin() + static_cast<long>(stop));
    for (Path p : {Path::ForEach, Path::ForEachFrom, Path::Stream, Path::StreamFrom})
    {
      EXPECT_EQ(read(config(), p, nullptr, std::nullopt, stop), prefix)
          << pathName(p) << " stop " << stop;
    }
  }
}

// ---------------------------------------------------------------------------
// Segments that prove nothing keep their file-name position.
// ---------------------------------------------------------------------------

TEST_P(ReplaySegmentOrderTest, UnsortedSegmentSplitsTheTapeIntoRuns)
{
  // Runs: {a, b} | u | {c, d}. Each run is time-ordered; the unsorted u is
  // delivered where its file name puts it.
  const auto a = writeSegment("a.lrvx", span(30 * kSecond, 5));
  const auto b = writeSegment("b.lrvx", span(0, 5));
  // The inversion crosses a block boundary, so a compressed copy is not
  // Sorted either.
  const auto u = writeSegment("c_unsorted.lrvx",
                              {20 * kSecond, 21 * kSecond, 22 * kSecond, 23 * kSecond,
                               20 * kSecond + 5, 24 * kSecond});
  const auto c = writeSegment("d.lrvx", span(60 * kSecond, 5));
  const auto d = writeSegment("e.lrvx", span(40 * kSecond, 5));
  ASSERT_FALSE(headerOf("c_unsorted.lrvx").isSorted());

  std::vector<Row> u_sorted = u;
  std::stable_sort(u_sorted.begin(), u_sorted.end(),
                   [](const Row& x, const Row& y)
                   { return x.ts < y.ts; });
  std::vector<Row> want = b;
  want.insert(want.end(), a.begin(), a.end());
  want.insert(want.end(), u_sorted.begin(), u_sorted.end());
  want.insert(want.end(), d.begin(), d.end());
  want.insert(want.end(), c.begin(), c.end());

  for (Path p : {Path::ForEach, Path::ForEachFrom, Path::Stream, Path::StreamFrom, Path::Run1,
                 Path::Adapter})
  {
    EXPECT_EQ(read(config(), p), want) << pathName(p);
  }
}

TEST_P(ReplaySegmentOrderTest, FromReadsDoNotSeekIntoALaterUnsortedSegment)
{
  // An unsorted segment's index is not monotonic: entries 130, 140, 600 (one
  // per two frames). Seeking it to 300 lands on the entry at 140 and passes
  // over the frame at 400. Only the first segment a *From read visits is
  // positioned by its index; later ones are filtered event by event.
  const auto a = writeSegment("a.lrvx", {100, 500}, std::nullopt, 2);
  const auto u = writeSegment("b_unsorted.lrvx", {130, 400, 140, 145, 600, 610}, std::nullopt, 2);
  ASSERT_FALSE(headerOf("b_unsorted.lrvx").isSorted());

  const std::vector<Row> want = {a[1], u[1], u[4], u[5]};
  for (Path p : {Path::ForEachFrom, Path::StreamFrom})
  {
    EXPECT_EQ(read(config(), p, nullptr, at(300)), want) << pathName(p);
  }
}

TEST_P(ReplaySegmentOrderTest, SegmentStillBeingWrittenStaysLast)
{
  const auto day3 = writeSegment("a.lrvx", span(2 * kDay, 10));
  const auto day1 = writeSegment("b.lrvx", span(0, 10));
  const auto live = writeSegment("c.lrvx", span(3 * kDay, 10));
  makeLive("c.lrvx");

  std::vector<Row> want = day1;
  want.insert(want.end(), day3.begin(), day3.end());
  want.insert(want.end(), live.begin(), live.end());
  for (Path p : {Path::ForEach, Path::ForEachFrom, Path::Stream, Path::StreamFrom, Path::Run1,
                 Path::Adapter})
  {
    EXPECT_EQ(read(config(), p), want) << pathName(p);
  }
}

TEST_P(ReplaySegmentOrderTest, ZeroLedSegmentIsOrderedByItsZeros)
{
  // The header records the first non-zero timestamp; the leading zero-stamped
  // events still come before everything else.
  const auto a = writeSegment("a.lrvx", span(10 * kSecond, 5));
  WriterConfig cfg{};
  cfg.output_dir = _dir;
  cfg.output_filename = "b.lrvx";
  cfg.compression = compression();
  cfg.index_interval = 4;
  std::vector<Row> b;
  {
    BinaryLogWriter writer(cfg);
    for (int64_t ts : {int64_t{0}, int64_t{0}, kBaseNs + 20 * kSecond})
    {
      TradeRecord r{};
      r.exchange_ts_ns = ts;
      r.symbol_id = 1;
      r.trade_id = _next_id++;
      writer.writeTrade(r);
      b.push_back(Row{ts, r.trade_id});
    }
    writer.close();
  }
  std::vector<Row> want = {b[0], b[1]};
  want.insert(want.end(), a.begin(), a.end());
  want.push_back(b[2]);
  ASSERT_EQ(reference(), want);
  for (Path p : {Path::ForEach, Path::Stream, Path::Run1})
  {
    EXPECT_TRUE(nonDecreasing(read(config(), p))) << pathName(p);
  }
  if (compression() == CompressionType::None)
  {
    // An uncompressed Sorted segment is bounded by its first frame.
    EXPECT_EQ(read(config(), Path::ForEach), want);
  }
}

// ---------------------------------------------------------------------------
// In-order tapes: time order changes nothing.
// ---------------------------------------------------------------------------

TEST_P(ReplaySegmentOrderTest, InOrderTapeIsDeliveredExactlyAsInFileNameOrder)
{
  writeSegment("a.lrvx", span(0, 30));
  writeSegment("b.lrvx", span(29 * kSecond, 30));  // starts on a's last timestamp
  writeSegment("c_unsorted.lrvx", {70 * kSecond, 69 * kSecond, 71 * kSecond});
  writeSegment("d.lrvx", span(100 * kSecond, 30));
  writeSegment("e.lrvx", span(200 * kSecond, 30));
  makeLive("e.lrvx");

  const std::pair<std::optional<int64_t>, std::optional<int64_t>> ranges[] = {
      {std::nullopt, std::nullopt},
      {at(10 * kSecond), at(40 * kSecond)},
      {at(69 * kSecond), std::nullopt},
      {std::nullopt, at(110 * kSecond)},
  };
  for (const auto& [from, to] : ranges)
  {
    for (Path p : {Path::ForEach, Path::ForEachFrom, Path::Stream, Path::StreamFrom, Path::Run1})
    {
      auto by_name = config(from, to);
      by_name.order_segments_by_time = false;
      ReaderStats s_time{};
      ReaderStats s_name{};
      EXPECT_EQ(read(config(from, to), p, &s_time), read(by_name, p, &s_name)) << pathName(p);
      EXPECT_EQ(s_time.files_read, s_name.files_read) << pathName(p);
      EXPECT_EQ(s_time.segments_skipped, s_name.segments_skipped) << pathName(p);
      EXPECT_EQ(s_time.events_read, s_name.events_read) << pathName(p);
      EXPECT_EQ(s_time.segments_reordered, 0u) << pathName(p);
      EXPECT_EQ(s_time.segments_interleaved, 0u) << pathName(p);
    }
  }
}

// ---------------------------------------------------------------------------
// run() with several threads.
// ---------------------------------------------------------------------------

TEST_P(ReplaySegmentOrderTest, ParallelRunVisitsDisjointSegmentsInTimeOrder)
{
  // Enough blocks per segment for the block-partitioned workers to engage on
  // compressed segments.
  writeSegment("a.lrvx", span(2 * kDay, 200));
  writeSegment("b.lrvx", span(0, 200));
  writeSegment("c.lrvx", span(kDay, 200));
  const auto want = reference();

  BinaryLogReader reader(config());
  CollectAggregator agg;
  IAggregator* panel[] = {&agg};
  ASSERT_TRUE(reader.run(panel, 4));
  EXPECT_EQ(agg.rows, want);
}

TEST_P(ReplaySegmentOrderTest, ParallelRunOnOverlapAndRangeLosesNothing)
{
  writeSegment("a.lrvx", span(30 * kSecond, 100, kSecond / 2));
  writeSegment("b.lrvx", span(0, 100));
  writeSegment("c.lrvx", span(200 * kSecond, 100));
  for (auto [from, to] : {std::pair<std::optional<int64_t>, std::optional<int64_t>>{},
                          {at(40 * kSecond), at(250 * kSecond)}})
  {
    auto want = reference(from, to);
    BinaryLogReader reader(config(from, to));
    CollectAggregator agg;
    IAggregator* panel[] = {&agg};
    ASSERT_TRUE(reader.run(panel, 4));
    auto got = agg.rows;
    auto by_ts_id = [](const Row& x, const Row& y)
    { return std::tie(x.ts, x.id) < std::tie(y.ts, y.id); };
    std::sort(got.begin(), got.end(), by_ts_id);
    std::sort(want.begin(), want.end(), by_ts_id);
    EXPECT_EQ(got, want);
  }
}

INSTANTIATE_TEST_SUITE_P(Compression, ReplaySegmentOrderTest,
                         ::testing::Values(CompressionType::None, CompressionType::LZ4),
                         [](const ::testing::TestParamInfo<CompressionType>& info)
                         { return info.param == CompressionType::None ? "None" : "LZ4"; });
