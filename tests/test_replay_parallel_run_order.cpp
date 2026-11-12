/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// What a multi-threaded BinaryLogReader::run(panel, n) delivers, and in what
// order.
//
// The parallel path splits each compressed segment's blocks across workers.
// Every worker feeds a clone of the panel (IAggregator::cloneEmpty) a slice of
// the segment that is contiguous in time and in order; the clones are merged
// into the caller's panel (IAggregator::merge) segment by segment, in read
// order. Segments themselves were also taken one after the other, which on a
// tape with Sorted segments whose ranges overlap -- a tape the single-threaded
// reader merges event by event -- sent the panel back in time at every such
// segment boundary, with no warning. run() defaults to n_threads = 0, so that
// was the default behaviour.
//
// Now a run of overlapping Sorted segments is delivered through the ordered
// merge run(1) uses, into the caller's panel directly, and every other
// segment is partitioned as before: a tape recorded in order is read by the
// same workers in the same slices.

#include "lrvx/replay/aggregator.h"
#include "lrvx/replay/binary_format_v1.h"
#include "lrvx/replay/readers/binary_log_reader.h"
#include "lrvx/replay/writers/binary_log_writer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace lrvx::replay;

namespace
{

constexpr int64_t kBase = 1'700'000'000'000'000'000LL;
constexpr int64_t kStep = 1'000'000;

std::atomic<int> g_clones{0};

// Records what it is fed. A worker's clone records its slice; merge appends
// the other recorder's events, so the caller's recorder ends up holding the
// panel's view of the run: segment by segment, slice by slice.
struct Recorder : IAggregator
{
  std::vector<int64_t> ts;
  void onEvent(const ReplayEvent& ev) override { ts.push_back(ev.timestamp_ns); }
  std::unique_ptr<IAggregator> cloneEmpty() const override
  {
    ++g_clones;
    return std::make_unique<Recorder>();
  }
  void merge(const IAggregator& other) override
  {
    const auto& o = dynamic_cast<const Recorder&>(other);
    ts.insert(ts.end(), o.ts.begin(), o.ts.end());
  }
};

void writeSegment(const std::filesystem::path& dir, const std::string& name, int64_t first,
                  int n, int64_t step, CompressionType compression = CompressionType::LZ4)
{
  WriterConfig cfg{.output_dir = dir,
                   .output_filename = name,
                   .max_segment_bytes = 1ull << 30,
                   .index_interval = 20,
                   .compression = compression};
  BinaryLogWriter w(cfg);
  for (int i = 0; i < n; ++i)
  {
    TradeRecord t{};
    t.exchange_ts_ns = first + i * step;
    t.recv_ts_ns = t.exchange_ts_ns;
    t.price_raw = 100 + i;
    t.qty_raw = 1;
    t.symbol_id = 1;
    ASSERT_TRUE(w.writeTrade(t));
  }
  w.close();
}

class ParallelRunOrderTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    _dir = std::filesystem::temp_directory_path() /
           (std::string("lrvx_parallel_run_order_") + info->name());
    std::filesystem::remove_all(_dir);
    std::filesystem::create_directories(_dir);
    g_clones = 0;
  }
  void TearDown() override { std::filesystem::remove_all(_dir); }

  std::vector<int64_t> runAndRecord(std::size_t n_threads, ReaderStats* stats = nullptr)
  {
    BinaryLogReader reader(ReaderConfig{.data_dir = _dir});
    Recorder rec;
    std::array<IAggregator*, 1> panel{&rec};
    EXPECT_TRUE(reader.run(panel, n_threads));
    if (stats)
    {
      *stats = reader.stats();
    }
    return rec.ts;
  }

  std::filesystem::path _dir;
};

}  // namespace

// Sorted segments of many blocks whose ranges overlap: a.lrvx holds the even
// milliseconds, b.lrvx the odd ones of the first half, and d.lrvx odd ones
// after b has ended but before a has, so it overlaps a and not b. c.lrvx,
// later, touches nothing.
TEST_F(ParallelRunOrderTest, OverlappingSortedSegmentsArriveInTimeOrderByDefault)
{
  writeSegment(_dir, "a.lrvx", kBase, 1000, 2 * kStep);
  writeSegment(_dir, "b.lrvx", kBase + kStep, 500, 2 * kStep);
  writeSegment(_dir, "c.lrvx", kBase + 3000 * kStep, 1000, kStep);
  writeSegment(_dir, "d.lrvx", kBase + 1201 * kStep, 100, 2 * kStep);

  const auto single = runAndRecord(1);
  ASSERT_EQ(single.size(), 2600u);
  ASSERT_TRUE(std::is_sorted(single.begin(), single.end()));

  for (std::size_t n : {std::size_t{0}, std::size_t{4}})
  {
    SCOPED_TRACE("run(" + std::to_string(n) + ")");
    ReaderStats stats{};
    const auto got = runAndRecord(n, &stats);
    EXPECT_TRUE(std::is_sorted(got.begin(), got.end()))
        << "events went back in time on the multi-threaded path";
    EXPECT_EQ(got, single);
    EXPECT_EQ(stats.events_read, 2600u);
    EXPECT_EQ(stats.segments_interleaved, 3u);
  }
}

// The smallest case: two segments, the second inside the first.
TEST_F(ParallelRunOrderTest, TwoOverlappingSegmentsArriveInTimeOrder)
{
  writeSegment(_dir, "a.lrvx", kBase, 1000, 2 * kStep);
  writeSegment(_dir, "b.lrvx", kBase + 501 * kStep, 400, 2 * kStep);
  const auto single = runAndRecord(1);
  ASSERT_TRUE(std::is_sorted(single.begin(), single.end()));
  const auto got = runAndRecord(4);
  EXPECT_TRUE(std::is_sorted(got.begin(), got.end()));
  EXPECT_EQ(got, single);
}

// The default run() on a tape recorded in order is still partitioned: the
// workers clone the panel and the events arrive in order.
TEST_F(ParallelRunOrderTest, InOrderTapeIsStillPartitioned)
{
  writeSegment(_dir, "a.lrvx", kBase, 1000, kStep);
  writeSegment(_dir, "b.lrvx", kBase + 1000 * kStep, 1000, kStep);
  // Starts on b's last timestamp: segments that touch at a seam do not
  // overlap.
  writeSegment(_dir, "c.lrvx", kBase + 1999 * kStep, 1000, kStep);

  ReaderStats stats{};
  const auto got = runAndRecord(4, &stats);
  // 50 blocks per segment: each of the three is split four ways.
  EXPECT_EQ(g_clones.load(), 12) << "a segment of the in-order tape was not partitioned";
  EXPECT_EQ(got.size(), 3000u);
  EXPECT_TRUE(std::is_sorted(got.begin(), got.end()));
  EXPECT_EQ(stats.segments_interleaved, 0u);
}

// A segment with no events after the last one (an older compressed writer
// left one whenever it rotated on its last block) does not make the segment
// before it an overlapping run: the tape is still partitioned in full.
TEST_F(ParallelRunOrderTest, TrailingEmptySegmentDoesNotStopPartitioning)
{
  writeSegment(_dir, "a.lrvx", kBase, 1000, kStep);
  writeSegment(_dir, "b.lrvx", kBase + 1000 * kStep, 1000, kStep);
  {
    SegmentHeader hdr{};
    hdr.flags = SegmentFlags::Compressed | SegmentFlags::Sorted;
    hdr.compression = static_cast<uint8_t>(CompressionType::LZ4);
    std::ofstream out(_dir / "c.lrvx", std::ios::binary);
    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
  }
  ReaderStats stats{};
  const auto got = runAndRecord(4, &stats);
  EXPECT_EQ(g_clones.load(), 8) << "a segment was not partitioned";
  EXPECT_EQ(got.size(), 2000u);
  EXPECT_TRUE(std::is_sorted(got.begin(), got.end()));
}

// A segment without the Sorted flag that run(n) reads on one thread goes
// through the reorder buffer. A streaming walk before it on the same reader
// used to leave its watermark behind, so run(n) judged the new walk's first
// events against the end of the previous one and dropped them as late.
TEST_F(ParallelRunOrderTest, RunDoesNotInheritAPreviousWalksWatermark)
{
  {
    WriterConfig cfg{.output_dir = _dir, .output_filename = "u.lrvx", .create_index = false};
    BinaryLogWriter w(cfg);
    for (int i = 0; i < 100; ++i)
    {
      TradeRecord t{};
      // One step back every ten events withholds the Sorted flag.
      t.exchange_ts_ns = kBase + (i % 10 == 5 ? i - 2 : i) * 1'000'000'000LL;
      t.symbol_id = 1;
      ASSERT_TRUE(w.writeTrade(t));
    }
    w.close();
  }
  BinaryLogReader reader(ReaderConfig{.data_dir = _dir});
  ASSERT_TRUE(reader.streamForEach([](const ReplayEvent&)
                                   { return true; }));
  ASSERT_EQ(reader.stats().events_read, 100u);
  Recorder rec;
  std::array<IAggregator*, 1> panel{&rec};
  ASSERT_TRUE(reader.run(panel, 4));
  EXPECT_EQ(rec.ts.size(), 100u);
  EXPECT_EQ(reader.stats().late_dropped, 0u);
}
