/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// ReaderStats::segments_truncated: data the reader cannot decode ends the
// segment, and says so.
//
// A compressed block that fails to decompress, carries a bad header or holds
// fewer frames than its header counts, and an uncompressed frame claiming more
// than 10 MiB, leave the reader no way to find the next frame. It stops the
// segment there and goes on with the next one; nothing is thrown and the read
// reports success. That used to happen without a trace (the oversized frame
// was counted as a CRC error). Now every read path counts the segment in
// segments_truncated, delivers exactly the events before the damage --
// run(n) included, whose workers split a segment into block ranges -- and the
// validator reports the segment as damaged. A segment still being written (no
// index) whose last frame or block is cut short is the live tail, not damage.

#include "lrvx/replay/aggregator.h"
#include "lrvx/replay/binary_format_v1.h"
#include "lrvx/replay/ops/index_builder.h"
#include "lrvx/replay/ops/validator.h"
#include "lrvx/replay/readers/binary_log_reader.h"
#include "lrvx/replay/readers/parallel_reader.h"
#include "lrvx/replay/writers/binary_log_writer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

using namespace lrvx::replay;

namespace
{

constexpr int kSegments = 3;
constexpr int kPerSegment = 1200;
constexpr int kInterval = 50;  // events per block
constexpr int kBlocks = kPerSegment / kInterval;
constexpr int kDamagedBlock = 10;   // of the middle segment, compressed
constexpr int kDamagedFrame = 500;  // of the middle segment, uncompressed
constexpr int64_t kBase = 1'700'000'000'000'000'000LL;
constexpr int64_t kStep = 1'000'000;

TradeRecord trade(int i)
{
  TradeRecord t{};
  t.exchange_ts_ns = kBase + i * kStep;
  t.recv_ts_ns = t.exchange_ts_ns + 1;
  t.price_raw = 100'000 + i;
  t.qty_raw = 1 + i % 7;
  t.trade_id = static_cast<uint64_t>(i);
  t.symbol_id = 1;
  return t;
}

enum class Damage
{
  Decompress,      // block's original_size no longer matches its payload
  BlockMagic,      // block header magic overwritten
  FrameCount,      // block header counts one frame more than the block holds
  OversizedFrame,  // uncompressed frame length above 10 MiB
};

struct Case
{
  const char* name;
  Damage damage;
  CompressionType compression;
  // Events of the middle segment delivered before the damage.
  int delivered;
};

constexpr std::array<Case, 4> kCases{{
    {"decompress", Damage::Decompress, CompressionType::LZ4, kDamagedBlock* kInterval},
    {"block_magic", Damage::BlockMagic, CompressionType::LZ4, kDamagedBlock* kInterval},
    {"frame_count", Damage::FrameCount, CompressionType::LZ4, (kDamagedBlock + 1) * kInterval},
    {"oversized_frame", Damage::OversizedFrame, CompressionType::None, kDamagedFrame},
}};

// File offset of block `n` of a compressed segment, or of frame `n` of an
// uncompressed one.
uint64_t offsetOf(std::FILE* f, const SegmentHeader& hdr, int n)
{
  uint64_t pos = sizeof(SegmentHeader);
  for (int i = 0; i < n; ++i)
  {
    std::fseek(f, static_cast<long>(pos), SEEK_SET);
    if (hdr.isCompressed())
    {
      CompressedBlockHeader bh{};
      std::fread(&bh, sizeof(bh), 1, f);
      pos += sizeof(bh) + bh.compressed_size;
    }
    else
    {
      FrameHeader fh{};
      std::fread(&fh, sizeof(fh), 1, f);
      pos += sizeof(fh) + fh.size;
    }
  }
  return pos;
}

void damage(const std::filesystem::path& path, Damage d)
{
  std::FILE* f = std::fopen(path.string().c_str(), "r+b");
  ASSERT_NE(f, nullptr);
  SegmentHeader hdr{};
  ASSERT_EQ(std::fread(&hdr, sizeof(hdr), 1, f), 1u);
  if (d == Damage::OversizedFrame)
  {
    const uint64_t at = offsetOf(f, hdr, kDamagedFrame);
    FrameHeader fh{};
    std::fseek(f, static_cast<long>(at), SEEK_SET);
    ASSERT_EQ(std::fread(&fh, sizeof(fh), 1, f), 1u);
    fh.size = 11u * 1024u * 1024u;
    std::fseek(f, static_cast<long>(at), SEEK_SET);
    ASSERT_EQ(std::fwrite(&fh, sizeof(fh), 1, f), 1u);
    std::fclose(f);
    return;
  }
  const uint64_t at = offsetOf(f, hdr, kDamagedBlock);
  CompressedBlockHeader bh{};
  std::fseek(f, static_cast<long>(at), SEEK_SET);
  ASSERT_EQ(std::fread(&bh, sizeof(bh), 1, f), 1u);
  switch (d)
  {
    case Damage::Decompress:
      bh.original_size += 64;
      break;
    case Damage::BlockMagic:
      bh.magic = 0;
      break;
    case Damage::FrameCount:
      bh.event_count += 1;
      break;
    case Damage::OversizedFrame:
      break;
  }
  std::fseek(f, static_cast<long>(at), SEEK_SET);
  ASSERT_EQ(std::fwrite(&bh, sizeof(bh), 1, f), 1u);
  std::fclose(f);
}

class ReplayUndecodableDataTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    _dir = std::filesystem::temp_directory_path() /
           (std::string("lrvx_undecodable_") + info->name());
    std::filesystem::remove_all(_dir);
  }
  void TearDown() override { std::filesystem::remove_all(_dir); }

  std::filesystem::path writeTape(const std::string& name, CompressionType compression)
  {
    const auto dir = _dir / name;
    std::filesystem::create_directories(dir);
    for (int s = 0; s < kSegments; ++s)
    {
      WriterConfig cfg{.output_dir = dir,
                       .output_filename = "seg" + std::to_string(s) + ".lrvx",
                       .max_segment_bytes = 1ull << 30,
                       .index_interval = kInterval,
                       .compression = compression};
      BinaryLogWriter w(cfg);
      for (int i = 0; i < kPerSegment; ++i)
      {
        EXPECT_TRUE(w.writeTrade(trade(s * kPerSegment + i)));
      }
      w.close();
    }
    return dir;
  }

  std::filesystem::path _dir;
};

using ReadPath = std::function<bool(BinaryLogReader&, std::vector<int64_t>&)>;

std::vector<std::pair<std::string, ReadPath>> readPaths()
{
  // Workers feed their clones while they run, and run(n) merges only the
  // clones it keeps; collect per clone so a dropped range is dropped here too.
  struct Keep : IAggregator
  {
    std::vector<int64_t> ts;
    void onEvent(const ReplayEvent& ev) override { ts.push_back(ev.timestamp_ns); }
    std::unique_ptr<IAggregator> cloneEmpty() const override { return std::make_unique<Keep>(); }
    void merge(const IAggregator& o) override
    {
      const auto& k = static_cast<const Keep&>(o);
      ts.insert(ts.end(), k.ts.begin(), k.ts.end());
    }
  };
  auto run = [](std::size_t n) -> ReadPath
  {
    return [n](BinaryLogReader& r, std::vector<int64_t>& ts)
    {
      Keep k;
      std::array<IAggregator*, 1> panel{&k};
      const bool ok = r.run(panel, n);
      ts = k.ts;
      return ok;
    };
  };
  auto into = [](std::vector<int64_t>& ts)
  {
    return [&ts](const ReplayEvent& ev)
    {
      ts.push_back(ev.timestamp_ns);
      return true;
    };
  };
  return {
      {"forEach", [=](BinaryLogReader& r, std::vector<int64_t>& ts)
       { return r.forEach(into(ts)); }},
      {"forEachFrom", [=](BinaryLogReader& r, std::vector<int64_t>& ts)
       { return r.forEachFrom(kBase, into(ts)); }},
      {"streamForEach", [=](BinaryLogReader& r, std::vector<int64_t>& ts)
       { return r.streamForEach(into(ts)); }},
      {"streamForEachFrom", [=](BinaryLogReader& r, std::vector<int64_t>& ts)
       { return r.streamForEachFrom(kBase, into(ts)); }},
      {"run(1)", run(1)},
      {"run(4)", run(4)},
  };
}

// What a read of the damaged tape must deliver: segments 0 and 2 whole, and
// the middle segment up to the damage.
std::vector<int64_t> expected(int delivered)
{
  std::vector<int64_t> ts;
  for (int i = 0; i < kSegments * kPerSegment; ++i)
  {
    const int in_segment = i - kPerSegment;
    if (in_segment >= delivered && in_segment < kPerSegment)
    {
      continue;
    }
    ts.push_back(kBase + i * kStep);
  }
  return ts;
}

}  // namespace

TEST_F(ReplayUndecodableDataTest, EndsTheSegmentAndIsCountedOnEveryPath)
{
  for (const auto& c : kCases)
  {
    const auto dir = writeTape(c.name, c.compression);
    damage(dir / "seg1.lrvx", c.damage);
    const auto want = expected(c.delivered);
    for (const auto& [name, read] : readPaths())
    {
      SCOPED_TRACE(std::string(c.name) + " " + name);
      BinaryLogReader reader(ReaderConfig{.data_dir = dir});
      std::vector<int64_t> ts;
      bool ok = false;
      ASSERT_NO_THROW(ok = read(reader, ts));
      ASSERT_TRUE(ok);
      std::sort(ts.begin(), ts.end());
      EXPECT_EQ(ts, want);
      const ReaderStats stats = reader.stats();
      EXPECT_EQ(stats.segments_truncated, 1u);
      EXPECT_EQ(stats.crc_errors, 0u);
      EXPECT_EQ(stats.events_read, want.size());
    }
  }
}

TEST_F(ReplayUndecodableDataTest, ParallelReaderCountsIt)
{
  for (const auto& c : kCases)
  {
    const auto dir = writeTape(c.name, c.compression);
    damage(dir / "seg1.lrvx", c.damage);
    for (uint32_t threads : {1u, 4u})
    {
      for (bool sorted : {true, false})
      {
        SCOPED_TRACE(std::string(c.name) + " threads=" + std::to_string(threads) +
                     (sorted ? " sorted" : " unsorted"));
        ParallelReader reader(ParallelReaderConfig{
            .data_dir = dir, .num_threads = threads, .sort_output = sorted});
        std::mutex mu;
        uint64_t seen = 0;
        reader.forEach(
            [&](const ReplayEvent&)
            {
              std::lock_guard g(mu);
              ++seen;
              return true;
            });
        EXPECT_EQ(seen, expected(c.delivered).size());
        EXPECT_EQ(reader.stats().segments_truncated, 1u);
        EXPECT_EQ(reader.stats().crc_errors, 0u);
      }
    }
  }
}

TEST_F(ReplayUndecodableDataTest, CleanTapeCountsNothing)
{
  for (auto compression : {CompressionType::None, CompressionType::LZ4})
  {
    const auto dir = writeTape(compression == CompressionType::None ? "clean_unc" : "clean_lz4",
                               compression);
    for (const auto& [name, read] : readPaths())
    {
      SCOPED_TRACE(name);
      BinaryLogReader reader(ReaderConfig{.data_dir = dir});
      std::vector<int64_t> ts;
      ASSERT_TRUE(read(reader, ts));
      EXPECT_EQ(ts.size(), static_cast<size_t>(kSegments * kPerSegment));
      EXPECT_EQ(reader.stats().segments_truncated, 0u);
    }
  }
}

// A segment without an index is still being written, so its last frame or
// block may be on disk only in part. That is where its data ends, not damage.
TEST_F(ReplayUndecodableDataTest, LiveTailIsNotDamage)
{
  for (auto compression : {CompressionType::None, CompressionType::LZ4})
  {
    SCOPED_TRACE(compression == CompressionType::None ? "uncompressed" : "lz4");
    const auto dir = writeTape(compression == CompressionType::None ? "tail_unc" : "tail_lz4",
                               compression);
    const auto seg = dir / "seg2.lrvx";
    ASSERT_TRUE(IndexBuilder::removeIndex(seg));
    std::filesystem::resize_file(seg, std::filesystem::file_size(seg) - 5);

    BinaryLogReader reader(ReaderConfig{.data_dir = dir});
    uint64_t n = 0;
    ASSERT_TRUE(reader.forEach(
        [&](const ReplayEvent&)
        {
          ++n;
          return true;
        }));
    const uint64_t lost = compression == CompressionType::None ? 1 : kInterval;
    EXPECT_EQ(n, kSegments * kPerSegment - lost);
    EXPECT_EQ(reader.stats().segments_truncated, 0u);
  }
}

// With its index in place the data must reach it: a file cut short before
// that is damage.
TEST_F(ReplayUndecodableDataTest, ClosedSegmentCutShortIsDamage)
{
  for (auto compression : {CompressionType::None, CompressionType::LZ4})
  {
    SCOPED_TRACE(compression == CompressionType::None ? "uncompressed" : "lz4");
    const auto dir = writeTape(compression == CompressionType::None ? "cut_unc" : "cut_lz4",
                               compression);
    const auto seg = dir / "seg2.lrvx";
    SegmentHeader hdr{};
    {
      std::FILE* f = std::fopen(seg.string().c_str(), "rb");
      ASSERT_EQ(std::fread(&hdr, sizeof(hdr), 1, f), 1u);
      std::fclose(f);
    }
    std::filesystem::resize_file(seg, hdr.index_offset - 5);

    BinaryLogReader reader(ReaderConfig{.data_dir = dir});
    ASSERT_TRUE(reader.forEach([](const ReplayEvent&)
                               { return true; }));
    EXPECT_EQ(reader.stats().segments_truncated, 1u);
  }
}

TEST_F(ReplayUndecodableDataTest, ValidatorReportsTheSegmentAsDamaged)
{
  for (const auto& c : kCases)
  {
    SCOPED_TRACE(c.name);
    const auto dir = writeTape(c.name, c.compression);
    EXPECT_TRUE(isValidSegment(dir / "seg1.lrvx"));
    damage(dir / "seg1.lrvx", c.damage);
    SegmentValidator validator;
    const auto result = validator.validate(dir / "seg1.lrvx");
    EXPECT_FALSE(result.valid);
    EXPECT_TRUE(result.hasErrors());
    EXPECT_FALSE(isValidSegment(dir / "seg1.lrvx"));

    DatasetValidator dataset;
    const auto all = dataset.validate(dir);
    EXPECT_FALSE(all.valid);
    EXPECT_EQ(all.corrupted_segments, 1u);
  }
}

// The counter accumulates across walks like every other ReaderStats field.
TEST_F(ReplayUndecodableDataTest, CounterAccumulatesAcrossWalks)
{
  const auto dir = writeTape("accumulate", CompressionType::LZ4);
  damage(dir / "seg1.lrvx", Damage::Decompress);
  BinaryLogReader reader(ReaderConfig{.data_dir = dir});
  ASSERT_TRUE(reader.forEach([](const ReplayEvent&)
                             { return true; }));
  ASSERT_TRUE(reader.streamForEach([](const ReplayEvent&)
                                   { return true; }));
  EXPECT_EQ(reader.stats().segments_truncated, 2u);
  static_assert(kBlocks > kDamagedBlock + 2);
}
