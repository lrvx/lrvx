/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// What a BinaryLogReader reports about itself before and after a read.
//
// Its accessors answer from the segment list, which is built by a scan on
// first use. An accessor that skipped the scan answered "no segments" on a
// fresh reader, the same answer an empty directory gives.
//
// ReaderStats::bytes_read is the number of segment-file bytes the read
// consumed as event data: every frame (header and payload) of an
// uncompressed segment, every block (header and compressed payload) of a
// compressed one. Segment headers, the index and the scan are not counted,
// and a segment the range skips contributes nothing. A full read therefore
// counts, per segment, everything between the segment header and the index,
// whichever path delivered it.

#include "lrvx/replay/aggregators/event_type_stats.h"
#include "lrvx/replay/binary_format_v1.h"
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
#include <optional>
#include <string>
#include <vector>

using namespace lrvx::replay;

namespace
{

constexpr int kTrades = 2000;
constexpr int64_t kBase = 1'700'000'000'000'000'000LL;
constexpr int64_t kStep = 1'000'000;  // 1 ms

struct TapeKind
{
  const char* name;
  CompressionType compression;
  bool sorted;
};

constexpr std::array<TapeKind, 4> kKinds{{
    {"uncompressed_sorted", CompressionType::None, true},
    {"uncompressed_unsorted", CompressionType::None, false},
    {"lz4_sorted", CompressionType::LZ4, true},
    {"lz4_unsorted", CompressionType::LZ4, false},
}};

class ReaderAccessorsTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    _dir = std::filesystem::temp_directory_path() /
           (std::string("lrvx_reader_accessors_") + info->name());
    std::filesystem::remove_all(_dir);
  }

  void TearDown() override { std::filesystem::remove_all(_dir); }

  // Several segments of 50-event blocks. An unsorted tape moves the first
  // trade of every block 3 ms back, behind the end of the block before it and
  // inside the reorder window, so its segments are written without the Sorted
  // flag (an inversion inside one block would be sorted away by the writer).
  std::filesystem::path writeTape(const TapeKind& kind)
  {
    const auto dir = _dir / kind.name;
    std::filesystem::create_directories(dir);
    WriterConfig config{
        .output_dir = dir,
        .max_segment_bytes = kind.compression == CompressionType::None ? 32 * 1024u : 8 * 1024u,
        .index_interval = 50,
        .compression = kind.compression,
    };
    BinaryLogWriter writer(config);
    for (int i = 0; i < kTrades; ++i)
    {
      TradeRecord trade{};
      trade.exchange_ts_ns = kBase + i * kStep;
      if (!kind.sorted && i % 50 == 0 && i > 0)
      {
        trade.exchange_ts_ns -= 3 * kStep;
      }
      trade.recv_ts_ns = trade.exchange_ts_ns;
      trade.price_raw = 50000LL * 100000000LL + i * 7919LL;
      trade.qty_raw = 100000000LL + (i * 104729LL) % 99991LL;
      trade.trade_id = static_cast<uint64_t>(i) * 2654435761ULL;
      trade.symbol_id = 1 + i % 3;
      trade.side = static_cast<uint8_t>(i % 2);
      EXPECT_TRUE(writer.writeTrade(trade));
    }
    writer.close();
    return dir;
  }

  std::filesystem::path _dir;
};

SegmentHeader readHeader(const std::filesystem::path& path)
{
  SegmentHeader hdr{};
  std::ifstream in(path, std::ios::binary);
  in.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
  return hdr;
}

// Event-data bytes of one segment: from the end of its header to its index
// (or to the end of the file when it has none).
uint64_t dataBytes(const std::filesystem::path& path)
{
  const SegmentHeader hdr = readHeader(path);
  const uint64_t end = hdr.hasIndex() ? hdr.index_offset : std::filesystem::file_size(path);
  return end - sizeof(SegmentHeader);
}

uint64_t dataBytes(const std::vector<SegmentInfo>& segments)
{
  uint64_t total = 0;
  for (const auto& seg : segments)
  {
    total += dataBytes(seg.path);
  }
  return total;
}

size_t segmentFileCount(const std::filesystem::path& dir)
{
  size_t n = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir))
  {
    if (entry.path().extension() == ".lrvx")
    {
      ++n;
    }
  }
  return n;
}

bool runPanel(BinaryLogReader& reader, std::size_t n_threads)
{
  EventTypeStatsAggregator agg;
  std::array<IAggregator*, 1> panel{&agg};
  return reader.run(panel, n_threads);
}

using ReadPath = std::function<bool(BinaryLogReader&)>;

std::vector<std::pair<std::string, ReadPath>> readPaths(int64_t start)
{
  auto drain = [](const ReplayEvent&)
  {
    return true;
  };
  return {
      {"forEach", [=](BinaryLogReader& r)
       { return r.forEach(drain); }},
      {"streamForEach", [=](BinaryLogReader& r)
       { return r.streamForEach(drain); }},
      {"forEachFrom", [=](BinaryLogReader& r)
       { return r.forEachFrom(start, drain); }},
      {"streamForEachFrom", [=](BinaryLogReader& r)
       { return r.streamForEachFrom(start, drain); }},
      {"run(1)", [](BinaryLogReader& r)
       { return runPanel(r, 1); }},
      {"run(4)", [](BinaryLogReader& r)
       { return runPanel(r, 4); }},
  };
}

}  // namespace

TEST_F(ReaderAccessorsTest, AccessorsScanBeforeAnswering)
{
  const auto dir = writeTape(kKinds[0]);
  const size_t files = segmentFileCount(dir);
  ASSERT_GT(files, 2u);

  {
    BinaryLogReader reader(ReaderConfig{.data_dir = dir});
    EXPECT_EQ(reader.segmentFiles().size(), files) << "segmentFiles() on a fresh reader";
  }
  {
    BinaryLogReader reader(ReaderConfig{.data_dir = dir});
    EXPECT_EQ(reader.segments().size(), files) << "segments() on a fresh reader";
  }
  {
    BinaryLogReader reader(ReaderConfig{.data_dir = dir});
    EXPECT_EQ(reader.segmentsInReadOrder().size(), files)
        << "segmentsInReadOrder() on a fresh reader";
  }
  {
    BinaryLogReader reader(ReaderConfig{.data_dir = dir});
    const auto range = reader.timeRange();
    ASSERT_TRUE(range.has_value()) << "timeRange() on a fresh reader";
    EXPECT_EQ(range->first, kBase);
    EXPECT_EQ(range->second, kBase + (kTrades - 1) * kStep);
  }
  {
    BinaryLogReader reader(ReaderConfig{.data_dir = dir});
    EXPECT_EQ(reader.count(), static_cast<uint64_t>(kTrades)) << "count() on a fresh reader";
  }
  {
    BinaryLogReader reader(ReaderConfig{.data_dir = dir});
    EXPECT_EQ(reader.summary().segment_count, files) << "summary() on a fresh reader";
  }
}

TEST_F(ReaderAccessorsTest, SegmentFilesAreInFileNameOrder)
{
  const auto dir = writeTape(kKinds[0]);
  BinaryLogReader reader(ReaderConfig{.data_dir = dir});
  const auto paths = reader.segmentFiles();
  ASSERT_FALSE(paths.empty());
  EXPECT_TRUE(std::is_sorted(paths.begin(), paths.end()));
  const auto& segs = reader.segments();
  ASSERT_EQ(segs.size(), paths.size());
  for (size_t i = 0; i < paths.size(); ++i)
  {
    EXPECT_EQ(paths[i], segs[i].path);
  }
}

TEST_F(ReaderAccessorsTest, BytesReadCountsEventDataOnEveryPath)
{
  for (const auto& kind : kKinds)
  {
    const auto dir = writeTape(kind);
    BinaryLogReader scanner(ReaderConfig{.data_dir = dir});
    const auto& segs = scanner.segments();
    ASSERT_GT(segs.size(), 2u) << kind.name;
    for (const auto& seg : segs)
    {
      if (seg.event_count > 0)
      {
        ASSERT_EQ(seg.sorted, kind.sorted) << kind.name << " " << seg.path;
      }
    }
    const uint64_t expected = dataBytes(segs);
    ASSERT_GT(expected, 0u);

    for (const auto& [name, read] : readPaths(kBase - 10 * kStep))
    {
      BinaryLogReader reader(ReaderConfig{.data_dir = dir});
      ASSERT_TRUE(read(reader)) << kind.name << " " << name;
      const ReaderStats stats = reader.stats();
      EXPECT_EQ(stats.events_read + stats.late_dropped, static_cast<uint64_t>(kTrades))
          << kind.name << " " << name;
      EXPECT_EQ(stats.bytes_read, expected) << kind.name << " " << name;
    }
  }
}

// The intra-segment parallel path splits a compressed segment's blocks across
// workers; their counts must add up to what one thread reads.
TEST_F(ReaderAccessorsTest, BytesReadSumsAcrossWorkersOfOneSegment)
{
  const auto dir = _dir / "one_segment";
  std::filesystem::create_directories(dir);
  {
    WriterConfig config{.output_dir = dir, .index_interval = 50, .compression = CompressionType::LZ4};
    BinaryLogWriter writer(config);
    for (int i = 0; i < kTrades; ++i)
    {
      TradeRecord trade{};
      trade.exchange_ts_ns = kBase + i * kStep;
      trade.price_raw = 50000LL * 100000000LL + i;
      trade.qty_raw = 100000000LL;
      trade.trade_id = static_cast<uint64_t>(i);
      trade.symbol_id = 1;
      ASSERT_TRUE(writer.writeTrade(trade));
    }
    writer.close();
  }
  ASSERT_EQ(segmentFileCount(dir), 1u);

  BinaryLogReader single(ReaderConfig{.data_dir = dir});
  ASSERT_TRUE(runPanel(single, 1));
  BinaryLogReader parallel(ReaderConfig{.data_dir = dir});
  ASSERT_TRUE(runPanel(parallel, 8));

  const uint64_t expected = dataBytes(single.segments());
  EXPECT_EQ(single.stats().bytes_read, expected);
  EXPECT_EQ(parallel.stats().bytes_read, expected);
}

std::vector<std::pair<std::string, ReadPath>> rangedReadPaths()
{
  std::vector<std::pair<std::string, ReadPath>> out;
  for (auto& entry : readPaths(0))
  {
    if (entry.first != "forEachFrom" && entry.first != "streamForEachFrom")
    {
      out.push_back(std::move(entry));
    }
  }
  return out;
}

// A range that covers exactly one uncompressed segment reads that segment
// whole and skips the rest, which contribute nothing.
TEST_F(ReaderAccessorsTest, BytesReadOfARangedReadCountsOnlyTheSegmentRead)
{
  const auto dir = writeTape(kKinds[0]);
  BinaryLogReader scanner(ReaderConfig{.data_dir = dir});
  const auto segs = scanner.segmentsInReadOrder();
  ASSERT_GT(segs.size(), 2u);
  const SegmentInfo& target = segs[1];

  ReaderConfig config{.data_dir = dir, .from_ns = target.min_event_ns, .to_ns = target.last_event_ns};
  for (const auto& [name, read] : rangedReadPaths())
  {
    BinaryLogReader reader(config);
    ASSERT_TRUE(read(reader)) << name;
    const ReaderStats stats = reader.stats();
    EXPECT_EQ(stats.files_read, 1u) << name;
    EXPECT_EQ(stats.segments_skipped, segs.size() - 1) << name;
    EXPECT_EQ(stats.events_read, target.event_count) << name;
    EXPECT_EQ(stats.bytes_read, dataBytes(target.path)) << name;
  }
}

// On a compressed tape the index bounds each block only from below, so the
// last block of the segment before the range is read as well. Every path
// reads the same blocks and reports the same count.
TEST_F(ReaderAccessorsTest, BytesReadOfARangedCompressedReadAgreesAcrossPaths)
{
  const auto dir = writeTape(kKinds[2]);
  BinaryLogReader scanner(ReaderConfig{.data_dir = dir});
  const auto segs = scanner.segmentsInReadOrder();
  ASSERT_GT(segs.size(), 3u);
  const SegmentInfo& target = segs[2];

  ReaderConfig config{.data_dir = dir, .from_ns = target.min_event_ns, .to_ns = target.last_event_ns};
  std::optional<uint64_t> first;
  for (const auto& [name, read] : rangedReadPaths())
  {
    BinaryLogReader reader(config);
    ASSERT_TRUE(read(reader)) << name;
    const ReaderStats stats = reader.stats();
    EXPECT_EQ(stats.events_read, target.event_count) << name;
    EXPECT_GE(stats.bytes_read, dataBytes(target.path)) << name;
    EXPECT_LT(stats.bytes_read, dataBytes(target.path) + dataBytes(segs[1].path)) << name;
    if (!first)
    {
      first = stats.bytes_read;
    }
    EXPECT_EQ(stats.bytes_read, *first) << name;
  }
}

// A range before the start of the tape decodes nothing: every segment is
// proven to lie after it and is skipped.
TEST_F(ReaderAccessorsTest, BytesReadIsZeroWhenTheRangeSkipsEverySegment)
{
  for (const auto& kind : {kKinds[0], kKinds[2]})
  {
    const auto dir = writeTape(kind);
    ReaderConfig config{.data_dir = dir,
                        .from_ns = kBase - 3 * kTrades * kStep,
                        .to_ns = kBase - 2 * kTrades * kStep};
    for (const auto& [name, read] : rangedReadPaths())
    {
      BinaryLogReader reader(config);
      ASSERT_TRUE(read(reader)) << kind.name << " " << name;
      EXPECT_GT(reader.stats().segments_skipped, 0u) << kind.name << " " << name;
      EXPECT_EQ(reader.stats().events_read, 0u) << kind.name << " " << name;
      EXPECT_EQ(reader.stats().bytes_read, 0u) << kind.name << " " << name;
    }
  }
}

TEST_F(ReaderAccessorsTest, BytesReadAccumulatesAcrossReads)
{
  const auto dir = writeTape(kKinds[2]);
  BinaryLogReader reader(ReaderConfig{.data_dir = dir});
  const uint64_t once = dataBytes(reader.segments());
  ASSERT_TRUE(reader.forEach([](const ReplayEvent&)
                             { return true; }));
  ASSERT_TRUE(reader.streamForEach([](const ReplayEvent&)
                                   { return true; }));
  EXPECT_EQ(reader.stats().bytes_read, 2 * once);
}

TEST_F(ReaderAccessorsTest, ParallelReaderCountsBytesReadOnEveryPath)
{
  for (const auto& kind : {kKinds[0], kKinds[2]})
  {
    const auto dir = writeTape(kind);
    BinaryLogReader scanner(ReaderConfig{.data_dir = dir});
    ASSERT_GT(scanner.segments().size(), 2u);
    const uint64_t expected = dataBytes(scanner.segments());

    for (uint32_t threads : {1u, 4u})
    {
      for (bool sorted_output : {true, false})
      {
        ParallelReader reader(ParallelReaderConfig{
            .data_dir = dir, .num_threads = threads, .sort_output = sorted_output});
        EXPECT_EQ(reader.forEach([](const ReplayEvent&)
                                 { return true; }),
                  static_cast<uint64_t>(kTrades));
        EXPECT_EQ(reader.stats().bytes_read, expected)
            << kind.name << " threads=" << threads << " sorted=" << sorted_output;
      }
    }

    ParallelReader batches(ParallelReaderConfig{.data_dir = dir, .num_threads = 4});
    EXPECT_EQ(batches.forEachBatch([](const std::vector<ReplayEvent>&)
                                   { return true; }),
              static_cast<uint64_t>(kTrades));
    EXPECT_EQ(batches.stats().bytes_read, expected) << kind.name << " forEachBatch";
  }
}
