/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// IndexBuilder::buildForSegment on a segment that mixes every record type.
//
// The builder read the timestamp of every frame that is not a trade through a
// 40-byte book header, so a frame shorter than that -- a pool state, whose
// fixed header is 32 bytes, carrying a payload under 8 bytes -- was indexed at
// timestamp 0, and a segment starting or ending with one got a header
// first/last_event_ns of 0. It now reads each frame's timestamp the way the
// reader parses the frame (frameTimestamp), so a rebuilt index gives back the
// writer's file byte for byte.

#include "lrvx/replay/binary_format_v1.h"
#include "lrvx/replay/ops/index_builder.h"
#include "lrvx/replay/readers/binary_log_reader.h"
#include "lrvx/replay/writers/binary_log_writer.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace lrvx::replay;

namespace
{

constexpr int64_t kBase = 1'700'000'000'000'000'000LL;
constexpr int64_t kStep = 1'000'000;
constexpr uint16_t kInterval = 3;
// Seven record kinds over an interval of three: index entries land on every
// kind in turn. The first and the last event are short pool states.
constexpr int kEvents = 7 * 30;

std::vector<std::byte> slurp(const std::filesystem::path& p)
{
  std::ifstream in(p, std::ios::binary);
  std::vector<char> raw((std::istreambuf_iterator<char>(in)), {});
  std::vector<std::byte> out(raw.size());
  std::memcpy(out.data(), raw.data(), raw.size());
  return out;
}

void writeMixed(BinaryLogWriter& w, int i)
{
  const int64_t ts = kBase + i * kStep;
  switch (i % 7)
  {
    case 0:  // pool state with an empty payload: a 32-byte frame
    case 6:  // pool state with a 4-byte payload: a 36-byte frame
    {
      PoolStateRecordHeader p{};
      p.exchange_ts_ns = ts;
      p.recv_ts_ns = ts + 2;
      p.symbol_id = 9;
      p.sub_type = 2;
      const std::byte payload[4]{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
      p.payload_len = (i % 7 == 0) ? 0 : 4;
      ASSERT_TRUE(w.writePoolState(p, payload, p.payload_len));
      break;
    }
    case 1:
    {
      TradeRecord t{};
      t.exchange_ts_ns = ts;
      t.recv_ts_ns = ts + 1;
      t.price_raw = 100 + i;
      t.qty_raw = 1;
      t.trade_id = static_cast<uint64_t>(i);
      t.symbol_id = 1;
      ASSERT_TRUE(w.writeTrade(t));
      break;
    }
    case 2:
    case 3:
    case 4:  // snapshot, delta, top of book
    {
      BookRecordHeader h{};
      h.exchange_ts_ns = ts;
      h.recv_ts_ns = ts + 5;
      h.seq = i;
      h.symbol_id = 2;
      h.type = static_cast<uint8_t>(i % 7 - 2);
      const BookLevel bids[2]{{100 - i, 3}, {99 - i, 4}};
      const BookLevel asks[1]{{101 + i, 5}};
      ASSERT_TRUE(w.writeBook(h, std::span<const BookLevel>(bids, h.type == 2 ? 1 : 2), asks));
      break;
    }
    case 5:
    {
      OptionQuoteRecord q{};
      q.exchange_ts_ns = ts;
      q.recv_ts_ns = ts + 3;
      q.mark_price_raw = 1000 + i;
      q.symbol_id = 7;
      ASSERT_TRUE(w.writeOptionQuote(q));
      break;
    }
  }
}

class IndexRebuildMixedTest : public ::testing::TestWithParam<CompressionType>
{
 protected:
  void SetUp() override
  {
    _dir = std::filesystem::temp_directory_path() /
           ("lrvx_index_rebuild_mixed_" + std::to_string(static_cast<int>(GetParam())));
    std::filesystem::remove_all(_dir);
    std::filesystem::create_directories(_dir);
  }
  void TearDown() override { std::filesystem::remove_all(_dir); }

  std::filesystem::path write(int events)
  {
    WriterConfig cfg{.output_dir = _dir,
                     .output_filename = "mixed.lrvx",
                     .max_segment_bytes = 1ull << 30,
                     .index_interval = kInterval,
                     .compression = GetParam()};
    BinaryLogWriter w(cfg);
    for (int i = 0; i < events; ++i)
    {
      writeMixed(w, i);
    }
    w.close();
    return _dir / "mixed.lrvx";
  }

  std::filesystem::path _dir;
};

}  // namespace

TEST(FrameTimestamp, ReadsEveryRecordTypeAtItsOwnLayout)
{
  std::vector<std::byte> buf(256);
  const int64_t ts = 0x0102030405060708LL;
  std::memcpy(buf.data(), &ts, sizeof(ts));
  int64_t got = 0;
  for (EventType t : {EventType::Trade, EventType::BookSnapshot, EventType::BookDelta,
                      EventType::BookTop, EventType::OptionQuote, EventType::PoolState})
  {
    const auto type = static_cast<uint8_t>(t);
    const size_t need = frameRecordSize(type);
    ASSERT_GT(need, 0u) << int(type);
    got = 0;
    EXPECT_TRUE(frameTimestamp(type, buf.data(), need, got)) << int(type);
    EXPECT_EQ(got, ts) << int(type);
    EXPECT_FALSE(frameTimestamp(type, buf.data(), need - 1, got)) << int(type);
  }
  EXPECT_EQ(frameRecordSize(static_cast<uint8_t>(EventType::PoolState)), 32u);
  // A type this build does not know leads with its timestamp too.
  got = 0;
  EXPECT_TRUE(frameTimestamp(200, buf.data(), 8, got));
  EXPECT_EQ(got, ts);
  EXPECT_FALSE(frameTimestamp(200, buf.data(), 7, got));
}

TEST_P(IndexRebuildMixedTest, RebuildGivesBackTheWritersFile)
{
  for (int events : {kEvents, kEvents + 1, 1})
  {
    SCOPED_TRACE(events);
    const auto seg = write(events);
    const auto written = slurp(seg);

    ASSERT_TRUE(IndexBuilder::removeIndex(seg));
    ASSERT_FALSE(IndexBuilder::hasIndex(seg));
    IndexBuilder builder(IndexBuilderConfig{.index_interval = kInterval});
    const auto r = builder.buildForSegment(seg);
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.events_scanned, static_cast<uint32_t>(events));
    EXPECT_EQ(slurp(seg), written);
  }
}

TEST_P(IndexRebuildMixedTest, NoEntryOrHeaderBoundIsZero)
{
  const auto seg = write(kEvents);
  ASSERT_TRUE(IndexBuilder::removeIndex(seg));
  IndexBuilder builder(IndexBuilderConfig{.index_interval = kInterval});
  ASSERT_TRUE(builder.buildForSegment(seg).success);

  BinaryLogIterator it(seg);
  ASSERT_TRUE(it.isValid());
  EXPECT_EQ(it.header().first_event_ns, kBase);
  EXPECT_EQ(it.header().last_event_ns, kBase + (kEvents - 1) * kStep);
  ASSERT_TRUE(it.loadIndex());
  ASSERT_FALSE(it.indexEntries().empty());
  for (const auto& e : it.indexEntries())
  {
    EXPECT_GE(e.timestamp_ns, kBase) << "entry at offset " << e.file_offset;
  }
}

INSTANTIATE_TEST_SUITE_P(Compression, IndexRebuildMixedTest,
                         ::testing::Values(CompressionType::None, CompressionType::LZ4),
                         [](const auto& info)
                         { return info.param == CompressionType::None ? "None" : "LZ4"; });
