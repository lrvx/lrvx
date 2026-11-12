/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// The Sorted flag on compressed segments with zero and negative timestamps.
//
// The writer sorts each block and sets Sorted when no block starts below the
// previous block's maximum. It used to make that comparison only once a block
// maximum was positive, so a segment whose early blocks sat at or below zero
// could be flagged Sorted with its blocks out of order. A zero or negative
// exchange_ts_ns is an ordinary value for ordering, on this path as on the
// uncompressed one: the flag is now set exactly when the stored order is
// non-decreasing.
//
// Readers do not trust the index bounds of a compressed segment whose first
// block minimum is not positive, because tapes the earlier writer produced may
// carry a false flag there. That defence stays: a tape with the flag forced on
// over out-of-order blocks still reads its ranges right.

#include "lrvx/replay/binary_format_v1.h"
#include "lrvx/replay/ops/compression.h"
#include "lrvx/replay/readers/binary_log_reader.h"
#include "lrvx/replay/writers/binary_log_writer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace lrvx::replay;

namespace
{

constexpr uint16_t kInterval = 4;

struct Case
{
  const char* name;
  std::vector<int64_t> ts;  // arrival order; blocks of kInterval
  bool sorted;              // stored order non-decreasing
};

const std::vector<Case>& cases()
{
  static const std::vector<Case> all{
      // Second block starts below the first block's (negative) maximum.
      {"negative_overlap", {-100, -90, -80, -70, -200, -60, -50, -40}, false},
      // Second block starts below the first block's maximum of zero.
      {"zero_then_negative", {0, 0, 0, 0, -5, 1, 2, 3}, false},
      // Negative block maximum, next block below it in a later position.
      {"negative_then_lower", {-10, -9, -8, -7, -6, -5, -4, -20}, false},
      // Zero maximum followed by a block that starts at zero: in order.
      {"zeros_then_positive", {0, 0, 0, 0, 0, 1, 2, 3}, true},
      {"negative_then_positive", {-40, -30, -20, -10, -10, 0, 10, 20}, true},
      // Inversions inside each block only: sorted away, flag stays.
      {"jitter_inside_blocks", {-3, -4, -1, -2, 5, 4, 7, 6, 9, 8}, true},
      {"positive_in_order", {1, 2, 3, 4, 5, 6, 7, 8, 9}, true},
      {"positive_overlap", {10, 20, 30, 40, 35, 50, 60, 70}, false},
      {"single_block_negative", {-3, -1, -2}, true},
  };
  return all;
}

std::vector<std::byte> slurp(const std::filesystem::path& p)
{
  std::ifstream in(p, std::ios::binary);
  std::vector<char> raw((std::istreambuf_iterator<char>(in)), {});
  std::vector<std::byte> out(raw.size());
  std::memcpy(out.data(), raw.data(), raw.size());
  return out;
}

// exchange_ts_ns of every frame in the order the blocks store them, decoded
// straight from the file.
std::vector<int64_t> storedOrder(const std::filesystem::path& path)
{
  const auto bytes = slurp(path);
  SegmentHeader hdr{};
  std::memcpy(&hdr, bytes.data(), sizeof(hdr));
  const size_t end = hdr.hasIndex() ? hdr.index_offset : bytes.size();
  std::vector<int64_t> ts;
  size_t pos = sizeof(SegmentHeader);
  while (pos < end)
  {
    CompressedBlockHeader bh{};
    std::memcpy(&bh, bytes.data() + pos, sizeof(bh));
    std::vector<std::byte> raw(bh.original_size);
    EXPECT_EQ(Compressor::decompress(hdr.compressionType(), bytes.data() + pos + sizeof(bh),
                                     bh.compressed_size, raw.data(), raw.size()),
              raw.size());
    for (size_t f = 0; f < raw.size();)
    {
      FrameHeader fh{};
      std::memcpy(&fh, raw.data() + f, sizeof(fh));
      int64_t t = 0;
      std::memcpy(&t, raw.data() + f + sizeof(fh), sizeof(t));
      ts.push_back(t);
      f += sizeof(fh) + fh.size;
    }
    pos += sizeof(bh) + bh.compressed_size;
  }
  return ts;
}

SegmentHeader header(const std::filesystem::path& p)
{
  SegmentHeader h{};
  std::FILE* f = std::fopen(p.string().c_str(), "rb");
  EXPECT_NE(f, nullptr);
  EXPECT_EQ(std::fread(&h, sizeof(h), 1, f), 1u);
  std::fclose(f);
  return h;
}

class CompressedSortedFlagTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    _dir = std::filesystem::temp_directory_path() /
           (std::string("lrvx_compressed_sorted_") + info->name());
    std::filesystem::remove_all(_dir);
  }
  void TearDown() override { std::filesystem::remove_all(_dir); }

  std::filesystem::path write(const Case& c)
  {
    const auto dir = _dir / c.name;
    std::filesystem::create_directories(dir);
    WriterConfig cfg{.output_dir = dir,
                     .output_filename = "seg.lrvx",
                     .max_segment_bytes = 1ull << 30,
                     .index_interval = kInterval,
                     .compression = CompressionType::LZ4};
    BinaryLogWriter w(cfg);
    int id = 0;
    for (int64_t t : c.ts)
    {
      TradeRecord r{};
      r.exchange_ts_ns = t;
      r.trade_id = static_cast<uint64_t>(id++);
      r.symbol_id = 1;
      EXPECT_TRUE(w.writeTrade(r));
    }
    w.close();
    return dir / "seg.lrvx";
  }

  std::filesystem::path _dir;
};

std::vector<int64_t> readAll(const ReaderConfig& cfg)
{
  BinaryLogReader reader(cfg);
  std::vector<int64_t> ts;
  reader.forEach(
      [&](const ReplayEvent& ev)
      {
        ts.push_back(ev.timestamp_ns);
        return true;
      });
  return ts;
}

}  // namespace

TEST_F(CompressedSortedFlagTest, FlagIsSetExactlyWhenStoredOrderIsNonDecreasing)
{
  for (const auto& c : cases())
  {
    SCOPED_TRACE(c.name);
    const auto seg = write(c);
    const auto stored = storedOrder(seg);
    ASSERT_EQ(stored.size(), c.ts.size());
    EXPECT_EQ(std::is_sorted(stored.begin(), stored.end()), c.sorted);
    EXPECT_EQ(header(seg).isSorted(), c.sorted);
  }
}

// What the flag promises is what readers rely on: a Sorted segment is
// delivered as stored, so the delivery is non-decreasing on every case, and a
// ranged read, whether the reader uses the index or turns it down, finds the
// events a filter over the whole segment finds.
TEST_F(CompressedSortedFlagTest, ReaderDeliveryAgreesWithTheFlag)
{
  for (const auto& c : cases())
  {
    SCOPED_TRACE(c.name);
    const auto seg = write(c);
    const auto dir = seg.parent_path();
    const auto all = readAll(ReaderConfig{.data_dir = dir});
    auto want = c.ts;
    std::sort(want.begin(), want.end());
    EXPECT_EQ(all, want);
    if (header(seg).isSorted())
    {
      EXPECT_EQ(all, storedOrder(seg));
    }

    for (size_t a = 0; a < want.size(); ++a)
    {
      const int64_t from = want[a];
      const int64_t to = want[std::min(want.size() - 1, a + 3)];
      std::vector<int64_t> in_range;
      std::copy_if(want.begin(), want.end(), std::back_inserter(in_range),
                   [&](int64_t t)
                   { return t >= from && t <= to; });
      EXPECT_EQ(readAll(ReaderConfig{.data_dir = dir, .from_ns = from, .to_ns = to}), in_range)
          << "[" << from << ", " << to << "]";
    }
  }
}

// A tape from the earlier writer: Sorted forced on over blocks that are out
// of order below zero. The reader turns its index down and the ranges come
// out right.
TEST_F(CompressedSortedFlagTest, ReaderStillDefendsAgainstAFalseFlag)
{
  const Case& c = cases().front();  // negative_overlap
  const auto seg = write(c);
  ASSERT_FALSE(header(seg).isSorted());
  SegmentHeader h = header(seg);
  h.flags |= SegmentFlags::Sorted;
  {
    std::FILE* f = std::fopen(seg.string().c_str(), "r+b");
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(std::fwrite(&h, sizeof(h), 1, f), 1u);
    std::fclose(f);
  }
  const auto dir = seg.parent_path();
  // [-200, -150] holds only the second block's first event; an index window
  // trusted from below would start at the second block anyway, but a window
  // trusted from above ([-80, -60]) must not stop at the second block.
  EXPECT_EQ(readAll(ReaderConfig{.data_dir = dir, .from_ns = -200, .to_ns = -150}),
            (std::vector<int64_t>{-200}));
  EXPECT_EQ(readAll(ReaderConfig{.data_dir = dir, .from_ns = -80, .to_ns = -60}),
            (std::vector<int64_t>{-80, -70, -60}));
}
