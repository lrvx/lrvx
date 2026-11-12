/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Segments with no events.
//
// A compressed writer rotates after the block that takes the segment past
// max_segment_bytes. It used to open the next segment right there, so a
// recording whose last block landed on the bound closed with an extra file
// holding only a header. The writer now opens the next segment with the next
// event, and closes no empty segment.
//
// Tapes already on disk keep theirs, and a collector killed right after
// opening a segment leaves the same thing. Every tool that walks a directory
// has to take such a file in its stride: the readers deliver the tape as if it
// were not there (an empty segment must not split a run of overlapping Sorted
// segments into two that are no longer merged), and the validator, the
// segment operations and the index builders accept it.

#include "lrvx/replay/aggregators/event_type_stats.h"
#include "lrvx/replay/binary_format_v1.h"
#include "lrvx/replay/ops/index_builder.h"
#include "lrvx/replay/ops/manifest.h"
#include "lrvx/replay/ops/segment_ops.h"
#include "lrvx/replay/ops/validator.h"
#include "lrvx/replay/readers/binary_log_reader.h"
#include "lrvx/replay/readers/parallel_reader.h"
#include "lrvx/replay/writers/binary_log_writer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

using namespace lrvx::replay;

namespace
{

constexpr int64_t kBase = 1'700'000'000'000'000'000LL;
constexpr int64_t kStep = 1'000'000;

TradeRecord trade(int64_t ts, int i)
{
  TradeRecord t{};
  t.exchange_ts_ns = ts;
  t.recv_ts_ns = ts;
  t.price_raw = 100'000 + i;
  t.qty_raw = 1 + i % 5;
  t.trade_id = static_cast<uint64_t>(i);
  t.symbol_id = 1;
  return t;
}

SegmentHeader readHeader(const std::filesystem::path& path)
{
  SegmentHeader hdr{};
  std::ifstream in(path, std::ios::binary);
  in.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
  return hdr;
}

std::vector<std::filesystem::path> segmentPaths(const std::filesystem::path& dir)
{
  std::vector<std::filesystem::path> out;
  for (const auto& e : std::filesystem::directory_iterator(dir))
  {
    if (isSegmentExtension(e.path().extension().string()))
    {
      out.push_back(e.path());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

// A segment as the compressed writer used to leave it after rotating on its
// last block: a header and nothing else, flagged Compressed and Sorted.
void writeEmptyCompressedSegment(const std::filesystem::path& path)
{
  SegmentHeader hdr{};
  hdr.flags = SegmentFlags::Compressed | SegmentFlags::Sorted;
  hdr.compression = static_cast<uint8_t>(CompressionType::LZ4);
  hdr.created_ns = kBase;
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
}

// A segment a collector opened and was killed in before writing a frame.
void writeEmptyUncompressedSegment(const std::filesystem::path& path)
{
  SegmentHeader hdr{};
  hdr.created_ns = kBase;
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
}

void writeSegment(const std::filesystem::path& dir, const std::string& name,
                  CompressionType compression, int64_t first_ts, int n, int64_t step)
{
  WriterConfig cfg{.output_dir = dir,
                   .output_filename = name,
                   .max_segment_bytes = 1ull << 30,
                   .index_interval = 20,
                   .compression = compression};
  BinaryLogWriter w(cfg);
  for (int i = 0; i < n; ++i)
  {
    ASSERT_TRUE(w.writeTrade(trade(first_ts + i * step, i)));
  }
  w.close();
}

class EmptySegmentTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    _dir = std::filesystem::temp_directory_path() /
           (std::string("lrvx_empty_segment_") + info->name());
    std::filesystem::remove_all(_dir);
    std::filesystem::create_directories(_dir);
  }
  void TearDown() override { std::filesystem::remove_all(_dir); }

  std::filesystem::path _dir;
};

std::vector<int64_t> collect(const std::filesystem::path& dir,
                             const std::function<bool(BinaryLogReader&, BinaryLogReader::EventCallback)>& read)
{
  BinaryLogReader reader(ReaderConfig{.data_dir = dir});
  std::vector<int64_t> ts;
  read(reader,
       [&](const ReplayEvent& ev)
       {
         ts.push_back(ev.timestamp_ns);
         return true;
       });
  return ts;
}

}  // namespace

TEST_F(EmptySegmentTest, CompressedWriterClosesNoEmptySegmentWhenRotationFallsOnTheLastBlock)
{
  // Every block crosses the bound, so the writer rotates after each one; the
  // last event fills the last block exactly.
  WriterConfig cfg{.output_dir = _dir,
                   .max_segment_bytes = 1,
                   .index_interval = 50,
                   .compression = CompressionType::LZ4};
  {
    BinaryLogWriter w(cfg);
    for (int i = 0; i < 150; ++i)
    {
      ASSERT_TRUE(w.writeTrade(trade(kBase + i * kStep, i)));
    }
    w.close();
  }
  const auto paths = segmentPaths(_dir);
  ASSERT_EQ(paths.size(), 3u) << "one segment per block, and no trailing empty one";
  for (const auto& p : paths)
  {
    EXPECT_EQ(readHeader(p).event_count, 50u) << p.filename();
  }

  BinaryLogReader reader(ReaderConfig{.data_dir = _dir});
  EXPECT_EQ(reader.count(), 150u);
}

TEST_F(EmptySegmentTest, UncompressedWriterClosesNoEmptySegment)
{
  WriterConfig cfg{.output_dir = _dir, .max_segment_bytes = 60 * 10 + sizeof(SegmentHeader)};
  {
    BinaryLogWriter w(cfg);
    for (int i = 0; i < 30; ++i)
    {
      ASSERT_TRUE(w.writeTrade(trade(kBase + i * kStep, i)));
    }
    w.close();
  }
  uint32_t total = 0;
  for (const auto& p : segmentPaths(_dir))
  {
    EXPECT_GT(readHeader(p).event_count, 0u) << p.filename();
    total += readHeader(p).event_count;
  }
  EXPECT_EQ(total, 30u);
}

// After a rotation on a block boundary the next event opens the next segment,
// so a writer that goes on writing is unaffected.
TEST_F(EmptySegmentTest, CompressedWriterKeepsWritingAfterABoundaryRotation)
{
  WriterConfig cfg{.output_dir = _dir,
                   .max_segment_bytes = 1,
                   .index_interval = 10,
                   .compression = CompressionType::LZ4};
  {
    BinaryLogWriter w(cfg);
    for (int i = 0; i < 35; ++i)
    {
      ASSERT_TRUE(w.writeTrade(trade(kBase + i * kStep, i)));
    }
    w.close();
    EXPECT_EQ(w.stats().segments_created, 4u);
  }
  const auto paths = segmentPaths(_dir);
  ASSERT_EQ(paths.size(), 4u);
  EXPECT_EQ(readHeader(paths.back()).event_count, 5u);
  const auto ts = collect(_dir, [](BinaryLogReader& r, BinaryLogReader::EventCallback cb)
                          { return r.forEach(cb); });
  ASSERT_EQ(ts.size(), 35u);
  EXPECT_TRUE(std::is_sorted(ts.begin(), ts.end()));
}

// Two overlapping Sorted segments with an empty one between them in file
// order, and empty ones at either end. Every read path delivers exactly what
// it delivers with the empty files removed, and the overlap is still merged.
TEST_F(EmptySegmentTest, ReadersDeliverTheTapeAsIfEmptySegmentsWereNotThere)
{
  const auto with = _dir / "with";
  const auto without = _dir / "without";
  for (const auto& dir : {with, without})
  {
    std::filesystem::create_directories(dir);
    writeSegment(dir, "b.lrvx", CompressionType::LZ4, kBase, 200, 2 * kStep);
    writeSegment(dir, "d.lrvx", CompressionType::None, kBase + kStep, 200, 2 * kStep);
  }
  writeEmptyCompressedSegment(with / "a.lrvx");
  writeEmptyCompressedSegment(with / "c.lrvx");
  writeEmptyUncompressedSegment(with / "e.lrvx");

  using Read = std::function<bool(BinaryLogReader&, BinaryLogReader::EventCallback)>;
  auto run_n = [](std::size_t n)
  {
    return Read(
        [n](BinaryLogReader& r, BinaryLogReader::EventCallback cb)
        {
          struct Forward : IAggregator
          {
            BinaryLogReader::EventCallback* cb{nullptr};
            void onEvent(const ReplayEvent& ev) override { (*cb)(ev); }
            std::unique_ptr<IAggregator> cloneEmpty() const override
            {
              auto c = std::make_unique<Forward>();
              c->cb = cb;
              return c;
            }
            void merge(const IAggregator&) override {}
          } fwd;
          fwd.cb = &cb;
          std::array<IAggregator*, 1> panel{&fwd};
          return r.run(panel, n);
        });
  };
  const std::vector<std::pair<std::string, Read>> paths{
      {"forEach", [](BinaryLogReader& r, BinaryLogReader::EventCallback cb)
       { return r.forEach(cb); }},
      {"forEachFrom",
       [](BinaryLogReader& r, BinaryLogReader::EventCallback cb)
       { return r.forEachFrom(kBase + 51 * kStep, cb); }},
      {"streamForEach",
       [](BinaryLogReader& r, BinaryLogReader::EventCallback cb)
       { return r.streamForEach(cb); }},
      {"streamForEachFrom",
       [](BinaryLogReader& r, BinaryLogReader::EventCallback cb)
       { return r.streamForEachFrom(kBase + 51 * kStep, cb); }},
      {"run(1)", run_n(1)},
  };
  for (const auto& [name, read] : paths)
  {
    SCOPED_TRACE(name);
    const auto expected = collect(without, read);
    const auto got = collect(with, read);
    ASSERT_FALSE(expected.empty());
    EXPECT_TRUE(std::is_sorted(expected.begin(), expected.end()));
    EXPECT_EQ(got, expected);
  }

  BinaryLogReader reader(ReaderConfig{.data_dir = with});
  reader.forEach([](const ReplayEvent&)
                 { return true; });
  EXPECT_EQ(reader.stats().segments_interleaved, 2u);
  EXPECT_EQ(reader.count(), 400u);
  const auto range = reader.timeRange();
  ASSERT_TRUE(range.has_value());
  EXPECT_EQ(range->first, kBase);
  EXPECT_EQ(range->second, kBase + kStep + 199 * 2 * kStep);

  ParallelReader parallel(ParallelReaderConfig{.data_dir = with, .num_threads = 4});
  std::vector<int64_t> merged;
  parallel.forEach(
      [&](const ReplayEvent& ev)
      {
        merged.push_back(ev.timestamp_ns);
        return true;
      });
  EXPECT_EQ(merged, collect(without, paths[0].second));
}

TEST_F(EmptySegmentTest, ToolsAcceptEmptySegments)
{
  writeSegment(_dir, "a.lrvx", CompressionType::LZ4, kBase, 100, kStep);
  writeEmptyCompressedSegment(_dir / "b.lrvx");
  writeSegment(_dir, "c.lrvx", CompressionType::None, kBase + 200 * kStep, 100, kStep);
  writeEmptyUncompressedSegment(_dir / "d.lrvx");

  {
    SCOPED_TRACE("validator");
    DatasetValidator validator;
    const auto result = validator.validate(_dir);
    EXPECT_EQ(result.segments.size(), 4u);
    for (const auto& issue : result.issues)
    {
      EXPECT_LT(issue.severity, IssueSeverity::Error) << issue.message;
    }
    for (const auto& seg : result.segments)
    {
      for (const auto& issue : seg.issues)
      {
        EXPECT_LT(issue.severity, IssueSeverity::Error) << seg.path.filename() << ": " << issue.message;
      }
    }
    EXPECT_EQ(result.total_events, 200u);
  }
  {
    SCOPED_TRACE("merge");
    const auto out = _dir / "merged";
    const auto r = SegmentOps::mergeDirectory(_dir, MergeConfig{.output_dir = out});
    EXPECT_TRUE(r.success);
    EXPECT_EQ(r.events_written, 200u);
    std::filesystem::remove_all(out);
  }
  {
    SCOPED_TRACE("split");
    const auto out = _dir / "split";
    const auto r = SegmentOps::splitDirectory(
        _dir, SplitConfig{.output_dir = out, .mode = SplitMode::ByEventCount, .events_per_file = 70});
    EXPECT_TRUE(r.success);
    EXPECT_EQ(r.events_written, 200u);
    std::filesystem::remove_all(out);
  }
  {
    SCOPED_TRACE("extractTimeRange on an empty segment");
    RangeExtractStats stats;
    const auto n = SegmentOps::extractTimeRange(_dir / "b.lrvx", _dir / "x" / "out.lrvx", kBase,
                                                kBase + 10 * kStep,
                                                WriterConfig{.output_dir = _dir / "x", .output_filename = "out.lrvx"},
                                                &stats);
    EXPECT_EQ(n, 0u);
    EXPECT_FALSE(stats.requested_from_before_data);
    EXPECT_FALSE(stats.requested_to_after_data);
    std::filesystem::remove_all(_dir / "x");
  }
  {
    SCOPED_TRACE("index builder");
    IndexBuilder builder;
    for (const auto& name : {"b.lrvx", "d.lrvx"})
    {
      const auto r = builder.buildForSegment(_dir / name);
      EXPECT_TRUE(r.success) << name << ": " << r.error;
      EXPECT_EQ(r.events_scanned, 0u);
    }
    const auto g = GlobalIndexBuilder::build(_dir, _dir / "index.lrvxidx");
    EXPECT_TRUE(g.success) << g.error;
    EXPECT_EQ(g.total_events, 200u);
    const auto loaded = GlobalIndexBuilder::load(_dir / "index.lrvxidx");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->size(), 4u);
    std::filesystem::remove(_dir / "index.lrvxidx");
  }
  {
    SCOPED_TRACE("manifest");
    const auto m = SegmentManifest::build(_dir);
    EXPECT_EQ(m.totalEvents(), 200u);
    EXPECT_EQ(m.firstTimestamp(), kBase);
    EXPECT_EQ(m.lastTimestamp(), kBase + 299 * kStep);
  }
  {
    SCOPED_TRACE("summary");
    const auto s = BinaryLogReader::inspect(_dir);
    EXPECT_EQ(s.segment_count, 4u);
    EXPECT_EQ(s.total_events, 200u);
    EXPECT_EQ(s.first_event_ns, kBase);
    EXPECT_EQ(s.last_event_ns, kBase + 299 * kStep);
  }
}
