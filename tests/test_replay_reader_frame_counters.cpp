/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// ReaderStats::crc_errors and ReaderStats::unknown_frames_skipped on every
// read path, and the counters ParallelReader keeps.
//
// The iterator counts both itself (a frame whose payload CRC does not match
// its header is skipped, a frame of a type this build does not know is
// stepped over by its length, and the read goes on in both cases). The
// reader's stats are what callers look at, so every path has to carry the
// iterator's counts into them, the way bytes_read is carried.

#include "lrvx/replay/aggregators/event_type_stats.h"
#include "lrvx/replay/binary_format_v1.h"
#include "lrvx/replay/ops/compression.h"
#include "lrvx/replay/readers/binary_log_reader.h"
#include "lrvx/replay/readers/parallel_reader.h"
#include "lrvx/replay/writers/binary_log_writer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

using namespace lrvx::replay;

namespace
{

constexpr int kSegments = 3;
constexpr int kPerSegment = 1200;
constexpr int kInterval = 50;  // events per block / index entry
constexpr int64_t kBase = 1'700'000'000'000'000'000LL;
constexpr int64_t kStep = 1'000'000;
constexpr uint8_t kFutureType = 0x7E;

TradeRecord trade(int i)
{
  TradeRecord t{};
  t.exchange_ts_ns = kBase + i * kStep;
  t.recv_ts_ns = t.exchange_ts_ns + 1;
  t.price_raw = 100'000 + i;
  t.qty_raw = 1 + i % 7;
  t.trade_id = static_cast<uint64_t>(i);
  t.symbol_id = 1 + i % 2;
  return t;
}

std::vector<std::byte> slurp(const std::filesystem::path& p)
{
  std::ifstream in(p, std::ios::binary);
  std::vector<char> raw((std::istreambuf_iterator<char>(in)), {});
  std::vector<std::byte> out(raw.size());
  std::memcpy(out.data(), raw.data(), raw.size());
  return out;
}

void spit(const std::filesystem::path& p, const std::vector<std::byte>& bytes)
{
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// Damages frames `crc_frame` and `unknown_frame` (0-based, counted in file
// order through the event data) of a segment: the first gets a CRC that does
// not match its payload, the second a record type this build does not know.
// Payload sizes are unchanged, so an uncompressed segment is edited in place;
// a compressed one has the touched block recompressed and the file, with its
// index offsets, rebuilt around it.
void damageFrames(const std::filesystem::path& path, int crc_frame, int unknown_frame)
{
  auto bytes = slurp(path);
  SegmentHeader hdr{};
  std::memcpy(&hdr, bytes.data(), sizeof(hdr));
  const size_t data_end = hdr.hasIndex() ? hdr.index_offset : bytes.size();

  auto damage = [&](std::byte* frame_at, int ordinal)
  {
    FrameHeader fh{};
    std::memcpy(&fh, frame_at, sizeof(fh));
    if (ordinal == crc_frame)
    {
      fh.crc32 ^= 0x5A5A5A5Au;
    }
    if (ordinal == unknown_frame)
    {
      fh.type = kFutureType;
    }
    std::memcpy(frame_at, &fh, sizeof(fh));
    return sizeof(FrameHeader) + fh.size;
  };

  if (!hdr.isCompressed())
  {
    size_t pos = sizeof(SegmentHeader);
    for (int ordinal = 0; pos < data_end; ++ordinal)
    {
      pos += damage(bytes.data() + pos, ordinal);
    }
    spit(path, bytes);
    return;
  }

  std::vector<std::byte> out(bytes.begin(), bytes.begin() + sizeof(SegmentHeader));
  std::vector<IndexEntry> index;
  if (hdr.hasIndex())
  {
    SegmentIndexHeader ih{};
    std::memcpy(&ih, bytes.data() + hdr.index_offset, sizeof(ih));
    index.resize(ih.entry_count);
    std::memcpy(index.data(), bytes.data() + hdr.index_offset + sizeof(ih),
                index.size() * sizeof(IndexEntry));
  }
  size_t pos = sizeof(SegmentHeader);
  int ordinal = 0;
  size_t block = 0;
  while (pos < data_end)
  {
    CompressedBlockHeader bh{};
    std::memcpy(&bh, bytes.data() + pos, sizeof(bh));
    std::vector<std::byte> raw(bh.original_size);
    ASSERT_EQ(Compressor::decompress(hdr.compressionType(), bytes.data() + pos + sizeof(bh),
                                     bh.compressed_size, raw.data(), raw.size()),
              raw.size());
    for (size_t f = 0; f < raw.size(); ++ordinal)
    {
      f += damage(raw.data() + f, ordinal);
    }
    std::vector<std::byte> packed(Compressor::maxCompressedSize(hdr.compressionType(), raw.size()));
    const size_t packed_size = Compressor::compress(hdr.compressionType(), raw.data(), raw.size(),
                                                    packed.data(), packed.size());
    ASSERT_GT(packed_size, 0u);
    if (block < index.size())
    {
      index[block].file_offset = out.size();
    }
    CompressedBlockHeader nh = bh;
    nh.compressed_size = static_cast<uint32_t>(packed_size);
    const auto* p = reinterpret_cast<const std::byte*>(&nh);
    out.insert(out.end(), p, p + sizeof(nh));
    out.insert(out.end(), packed.begin(), packed.begin() + static_cast<std::ptrdiff_t>(packed_size));
    pos += sizeof(bh) + bh.compressed_size;
    ++block;
  }
  if (hdr.hasIndex())
  {
    SegmentIndexHeader ih{};
    std::memcpy(&ih, bytes.data() + hdr.index_offset, sizeof(ih));
    ih.crc32 = Crc32::compute(index.data(), index.size() * sizeof(IndexEntry));
    hdr.index_offset = out.size();
    const auto* ip = reinterpret_cast<const std::byte*>(&ih);
    out.insert(out.end(), ip, ip + sizeof(ih));
    const auto* ep = reinterpret_cast<const std::byte*>(index.data());
    out.insert(out.end(), ep, ep + index.size() * sizeof(IndexEntry));
  }
  std::memcpy(out.data(), &hdr, sizeof(hdr));
  spit(path, out);
}

struct Kind
{
  const char* name;
  CompressionType compression;
};

constexpr std::array<Kind, 2> kKinds{{
    {"uncompressed", CompressionType::None},
    {"lz4", CompressionType::LZ4},
}};

class ReaderFrameCountersTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    _dir = std::filesystem::temp_directory_path() /
           (std::string("lrvx_frame_counters_") + info->name());
    std::filesystem::remove_all(_dir);
  }
  void TearDown() override { std::filesystem::remove_all(_dir); }

  // kSegments Sorted segments of kPerSegment trades, in time order, with one
  // CRC-damaged and one unknown-type frame in the middle segment when
  // `damaged`.
  std::filesystem::path writeTape(const Kind& kind, bool damaged)
  {
    const auto dir = _dir / (std::string(kind.name) + (damaged ? "_damaged" : "_clean"));
    std::filesystem::create_directories(dir);
    for (int s = 0; s < kSegments; ++s)
    {
      WriterConfig cfg{.output_dir = dir,
                       .output_filename = "seg" + std::to_string(s) + ".lrvx",
                       .max_segment_bytes = 1ull << 30,
                       .index_interval = kInterval,
                       .compression = kind.compression};
      BinaryLogWriter w(cfg);
      for (int i = 0; i < kPerSegment; ++i)
      {
        EXPECT_TRUE(w.writeTrade(trade(s * kPerSegment + i)));
      }
      w.close();
    }
    if (damaged)
    {
      damageFrames(dir / "seg1.lrvx", 333, 777);
    }
    return dir;
  }

  std::filesystem::path _dir;
};

bool runPanel(BinaryLogReader& reader, std::size_t n_threads)
{
  EventTypeStatsAggregator agg;
  std::array<IAggregator*, 1> panel{&agg};
  return reader.run(panel, n_threads);
}

std::vector<std::pair<std::string, std::function<bool(BinaryLogReader&)>>> readPaths()
{
  auto drain = [](const ReplayEvent&)
  {
    return true;
  };
  return {
      {"forEach", [=](BinaryLogReader& r)
       { return r.forEach(drain); }},
      {"forEachFrom", [=](BinaryLogReader& r)
       { return r.forEachFrom(kBase, drain); }},
      {"streamForEach", [=](BinaryLogReader& r)
       { return r.streamForEach(drain); }},
      {"streamForEachFrom", [=](BinaryLogReader& r)
       { return r.streamForEachFrom(kBase, drain); }},
      {"run(1)", [](BinaryLogReader& r)
       { return runPanel(r, 1); }},
      {"run(4)", [](BinaryLogReader& r)
       { return runPanel(r, 4); }},
  };
}

constexpr uint64_t kTotal = static_cast<uint64_t>(kSegments) * kPerSegment;

}  // namespace

TEST_F(ReaderFrameCountersTest, DamagedFramesAreCountedOnEveryPath)
{
  for (const auto& kind : kKinds)
  {
    const auto dir = writeTape(kind, /*damaged=*/true);
    for (const auto& [name, read] : readPaths())
    {
      SCOPED_TRACE(std::string(kind.name) + " " + name);
      BinaryLogReader reader(ReaderConfig{.data_dir = dir});
      ASSERT_TRUE(read(reader));
      const ReaderStats stats = reader.stats();
      EXPECT_EQ(stats.crc_errors, 1u);
      EXPECT_EQ(stats.unknown_frames_skipped, 1u);
      // Both damaged frames are skipped and the read goes on past them.
      EXPECT_EQ(stats.events_read, kTotal - 2);
    }
  }
}

TEST_F(ReaderFrameCountersTest, CleanTapeCountsNothing)
{
  for (const auto& kind : kKinds)
  {
    const auto dir = writeTape(kind, /*damaged=*/false);
    for (const auto& [name, read] : readPaths())
    {
      SCOPED_TRACE(std::string(kind.name) + " " + name);
      BinaryLogReader reader(ReaderConfig{.data_dir = dir});
      ASSERT_TRUE(read(reader));
      const ReaderStats stats = reader.stats();
      EXPECT_EQ(stats.crc_errors, 0u);
      EXPECT_EQ(stats.unknown_frames_skipped, 0u);
      EXPECT_EQ(stats.events_read, kTotal);
    }
  }
}

// The counters accumulate over the reader's lifetime, like every other field
// of ReaderStats: a second walk adds its own.
TEST_F(ReaderFrameCountersTest, CountersAccumulateAcrossWalks)
{
  const auto dir = writeTape(kKinds[1], /*damaged=*/true);
  BinaryLogReader reader(ReaderConfig{.data_dir = dir});
  ASSERT_TRUE(reader.forEach([](const ReplayEvent&)
                             { return true; }));
  ASSERT_TRUE(runPanel(reader, 4));
  EXPECT_EQ(reader.stats().crc_errors, 2u);
  EXPECT_EQ(reader.stats().unknown_frames_skipped, 2u);
}

TEST_F(ReaderFrameCountersTest, ParallelReaderCountsDamagedFrames)
{
  for (const auto& kind : kKinds)
  {
    const auto dir = writeTape(kind, /*damaged=*/true);
    struct Mode
    {
      const char* name;
      uint32_t threads;
      bool sorted;
      bool batch;
    };
    for (const Mode& m : {Mode{"nt1", 1, true, false}, Mode{"sorted", 4, true, false},
                          Mode{"unsorted", 4, false, false}, Mode{"batch", 4, true, true}})
    {
      SCOPED_TRACE(std::string(kind.name) + " " + m.name);
      ParallelReader reader(ParallelReaderConfig{
          .data_dir = dir, .num_threads = m.threads, .sort_output = m.sorted});
      std::mutex mu;
      uint64_t seen = 0;
      if (m.batch)
      {
        reader.forEachBatch(
            [&](const std::vector<ReplayEvent>& b)
            {
              std::lock_guard g(mu);
              seen += b.size();
              return true;
            });
      }
      else
      {
        reader.forEach(
            [&](const ReplayEvent&)
            {
              std::lock_guard g(mu);
              ++seen;
              return true;
            });
      }
      EXPECT_EQ(seen, kTotal - 2);
      const ParallelReaderStats stats = reader.stats();
      EXPECT_EQ(stats.crc_errors, 1u);
      EXPECT_EQ(stats.unknown_frames_skipped, 1u);
    }
  }
}

// ParallelReaderStats on every ParallelReader path: events, trades and book
// updates delivered and segments read, accumulated across calls.
TEST_F(ReaderFrameCountersTest, ParallelReaderStatsAccumulateOnEveryPath)
{
  const auto dir = writeTape(kKinds[1], /*damaged=*/false);
  struct Mode
  {
    const char* name;
    uint32_t threads;
    bool sorted;
    bool batch;
  };
  for (const Mode& m : {Mode{"nt1", 1, true, false}, Mode{"sorted", 4, true, false},
                        Mode{"unsorted", 4, false, false}, Mode{"batch", 4, true, true}})
  {
    SCOPED_TRACE(m.name);
    ParallelReader reader(ParallelReaderConfig{
        .data_dir = dir, .num_threads = m.threads, .sort_output = m.sorted});
    for (int pass = 1; pass <= 2; ++pass)
    {
      SCOPED_TRACE("pass " + std::to_string(pass));
      if (m.batch)
      {
        reader.forEachBatch([](const std::vector<ReplayEvent>&)
                            { return true; });
      }
      else
      {
        reader.forEach([](const ReplayEvent&)
                       { return true; });
      }
      const ParallelReaderStats stats = reader.stats();
      EXPECT_EQ(stats.events_read, pass * kTotal);
      EXPECT_EQ(stats.trades_read, pass * kTotal);
      EXPECT_EQ(stats.book_updates_read, 0u);
      EXPECT_EQ(stats.segments_processed, static_cast<uint64_t>(pass * kSegments));
    }
  }
}

// What the reader does with damaged frames, pinned: it skips them and reads
// on, delivering every other event in order; nothing is thrown.
TEST_F(ReaderFrameCountersTest, DamagedFramesAreSkippedAndTheReadGoesOn)
{
  for (const auto& kind : kKinds)
  {
    SCOPED_TRACE(kind.name);
    const auto dir = writeTape(kind, /*damaged=*/true);
    BinaryLogReader reader(ReaderConfig{.data_dir = dir});
    std::vector<int64_t> ts;
    ASSERT_NO_THROW(reader.forEach(
        [&](const ReplayEvent& ev)
        {
          ts.push_back(ev.timestamp_ns);
          return true;
        }));
    ASSERT_EQ(ts.size(), kTotal - 2);
    EXPECT_TRUE(std::is_sorted(ts.begin(), ts.end()));
    EXPECT_EQ(std::count(ts.begin(), ts.end(), kBase + (kPerSegment + 333) * kStep), 0);
    EXPECT_EQ(std::count(ts.begin(), ts.end(), kBase + (kPerSegment + 777) * kStep), 0);
    EXPECT_EQ(ts.back(), kBase + static_cast<int64_t>(kTotal - 1) * kStep);
  }
}

// Events the reorder buffer drops on a multi-threaded run reach ReaderStats
// as they do on a single-threaded one: every event is either delivered or
// counted as late.
TEST_F(ReaderFrameCountersTest, LateDropsReachStatsOnAMultiThreadedRun)
{
  const auto dir = _dir / "late";
  std::filesystem::create_directories(dir);
  constexpr int kEvents = 4000;
  {
    WriterConfig cfg{.output_dir = dir,
                     .output_filename = "late.lrvx",
                     .max_segment_bytes = 1ull << 30,
                     .index_interval = kInterval,
                     .compression = CompressionType::LZ4};
    BinaryLogWriter w(cfg);
    for (int i = 0; i < kEvents; ++i)
    {
      TradeRecord t = trade(i);
      // The first trade of every fourth block steps 20 s back, past the
      // default 10 s reorder window.
      if (i > 0 && i % (4 * kInterval) == 0)
      {
        t.exchange_ts_ns -= 20'000'000'000LL;
      }
      EXPECT_TRUE(w.writeTrade(t));
    }
    w.close();
  }
  for (std::size_t n : {std::size_t{1}, std::size_t{4}})
  {
    SCOPED_TRACE("run(" + std::to_string(n) + ")");
    BinaryLogReader reader(ReaderConfig{.data_dir = dir});
    ASSERT_TRUE(runPanel(reader, n));
    const ReaderStats stats = reader.stats();
    EXPECT_GT(stats.late_dropped, 0u);
    EXPECT_EQ(stats.events_read + stats.late_dropped, static_cast<uint64_t>(kEvents));
  }
}
