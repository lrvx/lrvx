/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// The time ranges the replay tools report: BinaryLogReader::timeRange(),
// summary() and inspect(), inspectWithSymbols(), the final progress tick of
// run(1), SegmentOps::extractTimeRange's "outside the data" check, the segment
// manifest and the global index.
//
// A segment header's first_event_ns / last_event_ns are the first and last
// events the writer was handed. On a compressed segment the writer sorts each
// block before writing it, so those two can sit inside the segment's range:
// a block written t+5, t+1, ..., t+3 starts at t+1 and ends at t+5 however it
// arrived. Reporting the header there made the range too narrow. Each of the
// tools above now reports the earliest and latest event, exactly wherever the
// segment proves them (a Sorted segment: header or index plus its last block)
// or the tool walks every event anyway; a segment without the Sorted flag is
// reported by the events it wrote first and last where only a full walk could
// say more.

#include "lrvx/replay/aggregators/event_type_stats.h"
#include "lrvx/replay/binary_format_v1.h"
#include "lrvx/replay/ops/index_builder.h"
#include "lrvx/replay/ops/manifest.h"
#include "lrvx/replay/ops/segment_ops.h"
#include "lrvx/replay/readers/binary_log_reader.h"
#include "lrvx/replay/writers/binary_log_writer.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace lrvx::replay;

namespace
{

constexpr int64_t kBase = 1'700'000'000'000'000'000LL;
constexpr int64_t kStep = 1'000'000;
constexpr int kBlock = 10;

TradeRecord trade(int64_t ts, int i)
{
  TradeRecord t{};
  t.exchange_ts_ns = ts;
  t.recv_ts_ns = ts;
  t.price_raw = 100'000 + i;
  t.qty_raw = 1;
  t.trade_id = static_cast<uint64_t>(i);
  t.symbol_id = 1 + i % 2;
  return t;
}

SegmentHeader readHeader(const std::filesystem::path& path)
{
  SegmentHeader hdr{};
  std::ifstream in(path, std::ios::binary);
  in.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
  return hdr;
}

// Three blocks of kBlock trades covering [kBase, kBase + 29 * kStep]. Inside
// every block the arrival order is rotated by one, so each block's first
// arrival is its second-smallest event and its last arrival is its smallest:
// the header then records kBase + kStep and kBase + 20 * kStep, while the
// segment spans kBase to kBase + 29 * kStep. The blocks do not overlap, so a
// Sorted segment stays Sorted. With `unsorted`, the third block is moved
// below the second's maximum, which withholds the flag.
std::vector<int64_t> arrivalOrder(bool unsorted)
{
  std::vector<int64_t> ts;
  for (int b = 0; b < 3; ++b)
  {
    int64_t block_base = kBase + b * kBlock * kStep;
    if (unsorted && b == 2)
    {
      block_base -= 3 * kStep;
    }
    for (int k = 1; k <= kBlock; ++k)
    {
      ts.push_back(block_base + (k % kBlock) * kStep);
    }
  }
  return ts;
}

void writeSegment(const std::filesystem::path& dir, const std::string& name,
                  CompressionType compression, const std::vector<int64_t>& ts)
{
  WriterConfig cfg{.output_dir = dir,
                   .output_filename = name,
                   .max_segment_bytes = 1ull << 30,
                   .index_interval = kBlock,
                   .compression = compression};
  BinaryLogWriter w(cfg);
  for (size_t i = 0; i < ts.size(); ++i)
  {
    ASSERT_TRUE(w.writeTrade(trade(ts[i], static_cast<int>(i))));
  }
  w.close();
}

class ReportedTimeRangeTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    _dir = std::filesystem::temp_directory_path() /
           (std::string("lrvx_reported_range_") + info->name());
    std::filesystem::remove_all(_dir);
    std::filesystem::create_directories(_dir);
  }
  void TearDown() override { std::filesystem::remove_all(_dir); }

  std::filesystem::path _dir;
};

constexpr int64_t kFirst = kBase;
constexpr int64_t kLast = kBase + 29 * kStep;

}  // namespace

TEST_F(ReportedTimeRangeTest, CompressedSortedSegmentHeaderIsNarrowerThanItsEvents)
{
  writeSegment(_dir, "a.lrvx", CompressionType::LZ4, arrivalOrder(false));
  const SegmentHeader hdr = readHeader(_dir / "a.lrvx");
  ASSERT_TRUE(hdr.isSorted());
  ASSERT_TRUE(hdr.isCompressed());
  // The premise every test below rests on.
  EXPECT_EQ(hdr.first_event_ns, kBase + kStep);
  EXPECT_EQ(hdr.last_event_ns, kBase + 20 * kStep);
}

TEST_F(ReportedTimeRangeTest, ReaderReportsTheEarliestAndLatestEventOfASortedSegment)
{
  writeSegment(_dir, "a.lrvx", CompressionType::LZ4, arrivalOrder(false));

  BinaryLogReader reader(ReaderConfig{.data_dir = _dir});
  const auto range = reader.timeRange();
  ASSERT_TRUE(range.has_value());
  EXPECT_EQ(range->first, kFirst);
  EXPECT_EQ(range->second, kLast);

  const DatasetSummary s = reader.summary();
  EXPECT_EQ(s.first_event_ns, kFirst);
  EXPECT_EQ(s.last_event_ns, kLast);
  EXPECT_DOUBLE_EQ(s.durationSeconds(), 29 * kStep / 1e9);

  const DatasetSummary i = BinaryLogReader::inspect(_dir);
  EXPECT_EQ(i.first_event_ns, kFirst);
  EXPECT_EQ(i.last_event_ns, kLast);

  const DatasetSummary w = BinaryLogReader::inspectWithSymbols(_dir);
  EXPECT_EQ(w.first_event_ns, kFirst);
  EXPECT_EQ(w.last_event_ns, kLast);
  EXPECT_EQ(w.symbols.size(), 2u);
}

// A segment without the Sorted flag proves nothing cheaply: timeRange() and
// summary() report the first and last events it wrote, as documented. The
// walk-based reports are exact.
TEST_F(ReportedTimeRangeTest, UnsortedSegmentIsReportedByItsFirstAndLastWrittenEvent)
{
  writeSegment(_dir, "a.lrvx", CompressionType::LZ4, arrivalOrder(true));
  const SegmentHeader hdr = readHeader(_dir / "a.lrvx");
  ASSERT_FALSE(hdr.isSorted());

  BinaryLogReader reader(ReaderConfig{.data_dir = _dir});
  const auto range = reader.timeRange();
  ASSERT_TRUE(range.has_value());
  EXPECT_EQ(range->first, hdr.first_event_ns);
  EXPECT_EQ(range->second, hdr.last_event_ns);

  const DatasetSummary w = BinaryLogReader::inspectWithSymbols(_dir);
  EXPECT_EQ(w.first_event_ns, kFirst);
  EXPECT_EQ(w.last_event_ns, kLast - 3 * kStep);
}

TEST_F(ReportedTimeRangeTest, FinalProgressTickIsTheLastEventDispatched)
{
  writeSegment(_dir, "a.lrvx", CompressionType::LZ4, arrivalOrder(false));
  for (const auto to : {std::optional<int64_t>{}, std::optional<int64_t>{kBase + 14 * kStep}})
  {
    SCOPED_TRACE(to ? "to_ns set" : "whole tape");
    BinaryLogReader reader(ReaderConfig{.data_dir = _dir, .to_ns = to});
    int64_t last_dispatched = 0;
    struct Last : IAggregator
    {
      int64_t* out{nullptr};
      void onEvent(const ReplayEvent& ev) override { *out = ev.timestamp_ns; }
      std::unique_ptr<IAggregator> cloneEmpty() const override { return std::make_unique<Last>(); }
      void merge(const IAggregator&) override {}
    } agg;
    agg.out = &last_dispatched;
    std::vector<std::pair<double, int64_t>> ticks;
    reader.setProgressCallback(
        [&](double pct, int64_t ts)
        {
          ticks.emplace_back(pct, ts);
          return true;
        },
        std::chrono::milliseconds(1000));
    std::array<IAggregator*, 1> panel{&agg};
    ASSERT_TRUE(reader.run(panel, 1));
    ASSERT_FALSE(ticks.empty());
    EXPECT_DOUBLE_EQ(ticks.back().first, 1.0);
    EXPECT_EQ(ticks.back().second, last_dispatched);
    EXPECT_EQ(last_dispatched, to ? *to : kLast);
  }
}

TEST_F(ReportedTimeRangeTest, ExtractTimeRangeJudgesTheRequestAgainstTheRealRange)
{
  for (const bool unsorted : {false, true})
  {
    SCOPED_TRACE(unsorted ? "unsorted" : "sorted");
    const auto dir = _dir / (unsorted ? "u" : "s");
    writeSegment(dir, "a.lrvx", CompressionType::LZ4, arrivalOrder(unsorted));
    const int64_t last = unsorted ? kLast - 3 * kStep : kLast;

    // Exactly the segment's range: nothing outside it.
    RangeExtractStats stats;
    const auto out = dir / "out";
    const auto n = SegmentOps::extractTimeRange(
        dir / "a.lrvx", out / "x.lrvx", kFirst, last,
        WriterConfig{.output_dir = out, .output_filename = "x.lrvx"}, &stats);
    EXPECT_EQ(n, 30u);
    EXPECT_EQ(stats.input_first_ns, kFirst);
    EXPECT_EQ(stats.input_last_ns, last);
    EXPECT_FALSE(stats.requested_from_before_data);
    EXPECT_FALSE(stats.requested_to_after_data);
    std::filesystem::remove_all(out);

    // One step past either end: both flagged.
    RangeExtractStats wide;
    SegmentOps::extractTimeRange(dir / "a.lrvx", out / "x.lrvx", kFirst - 1, last + 1,
                                 WriterConfig{.output_dir = out, .output_filename = "x.lrvx"},
                                 &wide);
    EXPECT_TRUE(wide.requested_from_before_data);
    EXPECT_TRUE(wide.requested_to_after_data);
    std::filesystem::remove_all(out);
  }
}

TEST_F(ReportedTimeRangeTest, ManifestAndGlobalIndexCarryTheRealRange)
{
  writeSegment(_dir, "a.lrvx", CompressionType::LZ4, arrivalOrder(false));

  const auto m = SegmentManifest::build(_dir);
  EXPECT_EQ(m.firstTimestamp(), kFirst);
  EXPECT_EQ(m.lastTimestamp(), kLast);
  ASSERT_EQ(m.segments().size(), 1u);
  EXPECT_EQ(m.segments()[0].first_event_ns, kFirst);
  EXPECT_EQ(m.segments()[0].last_event_ns, kLast);
  // A window holding only the segment's first event finds it.
  EXPECT_EQ(m.segmentsInRange(kFirst, kFirst).size(), 1u);
  EXPECT_EQ(m.segmentsInRange(kLast, kLast).size(), 1u);

  const auto g = GlobalIndexBuilder::build(_dir, _dir / "global.idx");
  ASSERT_TRUE(g.success) << g.error;
  const auto loaded = GlobalIndexBuilder::load(_dir / "global.idx");
  ASSERT_TRUE(loaded.has_value());
  ASSERT_EQ(loaded->size(), 1u);
  EXPECT_EQ((*loaded)[0].first_event_ns, kFirst);
  EXPECT_EQ((*loaded)[0].last_event_ns, kLast);
}

// An uncompressed Sorted segment's header is already exact; nothing changes.
TEST_F(ReportedTimeRangeTest, UncompressedSortedSegmentIsReportedFromItsHeader)
{
  std::vector<int64_t> ts;
  for (int i = 0; i < 30; ++i)
  {
    ts.push_back(kBase + i * kStep);
  }
  writeSegment(_dir, "a.lrvx", CompressionType::None, ts);
  BinaryLogReader reader(ReaderConfig{.data_dir = _dir});
  const auto range = reader.timeRange();
  ASSERT_TRUE(range.has_value());
  EXPECT_EQ(range->first, kFirst);
  EXPECT_EQ(range->second, kLast);
}
