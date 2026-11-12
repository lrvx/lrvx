/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// The C API data reader: LrvxReaderStats carries every ReaderStats counter
// (segments_skipped, segments_reordered and segments_interleaved arrived with
// ABI version 6), and lrvx_data_reader_create_ex reaches the two segment
// switches of ReaderConfig that no other create function sets.

#include "lrvx/capi/lrvx_capi.h"
#include "lrvx/replay/readers/binary_log_reader.h"
#include "lrvx/replay/writers/binary_log_writer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

using namespace lrvx::replay;

namespace
{

constexpr int64_t kBase = 1'700'000'000'000'000'000LL;
constexpr int64_t kMs = 1'000'000;

void writeSegment(const std::filesystem::path& dir, const std::string& name, int64_t first,
                  int n, int64_t step)
{
  WriterConfig cfg{.output_dir = dir, .output_filename = name};
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

class CapiReaderSegmentsTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    _dir = std::filesystem::temp_directory_path() /
           (std::string("lrvx_capi_reader_segments_") + info->name());
    std::filesystem::remove_all(_dir);
    std::filesystem::create_directories(_dir);
    // a: even milliseconds from 2; b: odd ones from 1, so it starts first and
    // overlaps a; c: later than both; d: between a and c, so read before c
    // without overlapping anything. Read order b a d c: four segments moved,
    // two interleaved.
    writeSegment(_dir, "a.lrvx", kBase + 2 * kMs, 100, 2 * kMs);
    writeSegment(_dir, "b.lrvx", kBase + kMs, 50, 2 * kMs);
    writeSegment(_dir, "c.lrvx", kBase + 1000 * kMs, 50, kMs);
    writeSegment(_dir, "d.lrvx", kBase + 500 * kMs, 50, kMs);
  }
  void TearDown() override { std::filesystem::remove_all(_dir); }

  // One walk through the C API, so every counter below is per walk.
  static std::vector<int64_t> walk(LrvxDataReaderHandle r)
  {
    std::vector<LrvxTradeRecord> out(1000);
    const uint64_t n = lrvx_data_reader_read_trades(r, out.data(), out.size());
    std::vector<int64_t> ts;
    for (uint64_t i = 0; i < n; ++i)
    {
      ts.push_back(out[i].exchange_ts_ns);
    }
    return ts;
  }

  std::filesystem::path _dir;
};

}  // namespace

TEST_F(CapiReaderSegmentsTest, StatsCarryTheSegmentCounters)
{
  LrvxDataReaderHandle r = lrvx_data_reader_create(_dir.string().c_str());
  ASSERT_NE(r, nullptr);
  const auto ts = walk(r);
  EXPECT_EQ(ts.size(), 250u);
  EXPECT_TRUE(std::is_sorted(ts.begin(), ts.end()));

  const LrvxReaderStats s = lrvx_data_reader_stats(r);
  EXPECT_EQ(s.files_read, 4u);
  EXPECT_EQ(s.events_read, 250u);
  EXPECT_EQ(s.segments_skipped, 0u);
  EXPECT_EQ(s.segments_reordered, 4u);
  EXPECT_EQ(s.segments_interleaved, 2u);

  // The pointer form copies the same struct, whole.
  LrvxReaderStats p{};
  lrvx_data_reader_stats_p(r, &p);
  EXPECT_EQ(p.segments_reordered, 4u);
  EXPECT_EQ(p.segments_interleaved, 2u);
  lrvx_data_reader_destroy(r);
}

TEST_F(CapiReaderSegmentsTest, CreateExOrdersSegmentsByFileNameOnRequest)
{
  LrvxDataReaderHandle r =
      lrvx_data_reader_create_ex(_dir.string().c_str(), 0, 0, nullptr, 0, 0, 0,
                                 /*order_segments_by_time=*/0, /*skip_segments_by_time=*/1);
  ASSERT_NE(r, nullptr);
  const auto ts = walk(r);
  ASSERT_EQ(ts.size(), 250u);
  EXPECT_EQ(ts.front(), kBase + 2 * kMs) << "a.lrvx is read first";
  EXPECT_FALSE(std::is_sorted(ts.begin(), ts.end()));
  const LrvxReaderStats s = lrvx_data_reader_stats(r);
  EXPECT_EQ(s.segments_reordered, 0u);
  EXPECT_EQ(s.segments_interleaved, 0u);
  lrvx_data_reader_destroy(r);

  // Both switches on is the default reader.
  r = lrvx_data_reader_create_ex(_dir.string().c_str(), 0, 0, nullptr, 0, 0, 0, 1, 1);
  ASSERT_NE(r, nullptr);
  const auto merged = walk(r);
  EXPECT_TRUE(std::is_sorted(merged.begin(), merged.end()));
  EXPECT_EQ(lrvx_data_reader_stats(r).segments_interleaved, 2u);
  lrvx_data_reader_destroy(r);
}

TEST_F(CapiReaderSegmentsTest, CreateExSkipsSegmentsOutsideTheRangeOnRequest)
{
  const int64_t from = kBase + 500 * kMs;
  LrvxDataReaderHandle skip =
      lrvx_data_reader_create_ex(_dir.string().c_str(), from, 0, nullptr, 0, 0, 0, 1, 1);
  LrvxDataReaderHandle walk_all =
      lrvx_data_reader_create_ex(_dir.string().c_str(), from, 0, nullptr, 0, 0, 0, 1, 0);
  ASSERT_NE(skip, nullptr);
  ASSERT_NE(walk_all, nullptr);
  const auto a = walk(skip);
  const auto b = walk(walk_all);
  EXPECT_EQ(a.size(), 100u);
  EXPECT_EQ(a, b);
  EXPECT_EQ(lrvx_data_reader_stats(skip).segments_skipped, 2u);
  EXPECT_EQ(lrvx_data_reader_stats(walk_all).segments_skipped, 0u);
  EXPECT_EQ(lrvx_data_reader_stats(walk_all).files_read, 4u);
  lrvx_data_reader_destroy(skip);
  lrvx_data_reader_destroy(walk_all);
}

TEST_F(CapiReaderSegmentsTest, CreateExKeepsTheOrderedReadersKnobs)
{
  // strict_ordering and a window pass through: a NULL directory fails cleanly.
  EXPECT_EQ(lrvx_data_reader_create_ex(nullptr, 0, 0, nullptr, 0, 0, 0, 1, 1), nullptr);
  const uint32_t sym = 1;
  LrvxDataReaderHandle r = lrvx_data_reader_create_ex(_dir.string().c_str(), 0, 0, &sym, 1,
                                                      5'000'000'000LL, 1, 1, 1);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(walk(r).size(), 250u);
  lrvx_data_reader_destroy(r);
}

TEST(CapiReaderStatsLayout, ElevenCountersSinceAbiSix)
{
  EXPECT_EQ(LRVX_CAPI_ABI_VERSION, 6);
  EXPECT_EQ(lrvx_capi_abi_version(), 6u);
  EXPECT_EQ(sizeof(LrvxReaderStats), 11 * sizeof(uint64_t));
  EXPECT_EQ(offsetof(LrvxReaderStats, unknown_frames_skipped), 7 * sizeof(uint64_t));
  EXPECT_EQ(offsetof(LrvxReaderStats, segments_skipped), 8 * sizeof(uint64_t));
  EXPECT_EQ(offsetof(LrvxReaderStats, segments_reordered), 9 * sizeof(uint64_t));
  EXPECT_EQ(offsetof(LrvxReaderStats, segments_interleaved), 10 * sizeof(uint64_t));
}
