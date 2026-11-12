/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// IndexBuilder::buildForSegment on compressed segments.
//
// The builder used to walk a compressed segment as a stream of frames, read a
// block header as a frame header and stop with an error, so neither it nor
// SegmentRepairer (which calls it to replace a damaged index) could index a
// compressed segment. It now indexes one entry per block holding the block's
// smallest exchange_ts_ns, which is what the writer stores: rebuilding the
// index of a segment the writer closed gives back the writer's file byte for
// byte. The header's first/last_event_ns keep what the writer recorded
// (arrival order); a header left at zero is filled from the stored events.

#include "lrvx/replay/binary_format_v1.h"
#include "lrvx/replay/ops/index_builder.h"
#include "lrvx/replay/ops/validator.h"
#include "lrvx/replay/readers/binary_log_reader.h"
#include "lrvx/replay/writers/binary_log_writer.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

using namespace lrvx::replay;

namespace
{

constexpr int64_t kBase = 1'700'000'000'000'000'000LL;
constexpr int64_t kStep = 1'000'000;
constexpr uint16_t kInterval = 64;
constexpr int kEvents = 1000;  // 15 full blocks and a short one

std::vector<std::byte> slurp(const std::filesystem::path& p)
{
  std::ifstream in(p, std::ios::binary);
  std::vector<char> raw((std::istreambuf_iterator<char>(in)), {});
  std::vector<std::byte> out(raw.size());
  std::memcpy(out.data(), raw.data(), raw.size());
  return out;
}

SegmentHeader readHeader(const std::filesystem::path& p)
{
  SegmentHeader h{};
  std::FILE* f = std::fopen(p.string().c_str(), "rb");
  EXPECT_NE(f, nullptr);
  EXPECT_EQ(std::fread(&h, sizeof(h), 1, f), 1u);
  std::fclose(f);
  return h;
}

void writeHeader(const std::filesystem::path& p, const SegmentHeader& h)
{
  std::FILE* f = std::fopen(p.string().c_str(), "r+b");
  ASSERT_NE(f, nullptr);
  ASSERT_EQ(std::fwrite(&h, sizeof(h), 1, f), 1u);
  std::fclose(f);
}

struct Shape
{
  const char* name;
  // Arrival-order timestamp of event i.
  int64_t (*ts)(int i);
};

int64_t inOrder(int i) { return kBase + i * kStep; }
// Jitter inside blocks: the writer sorts each block, so the segment stays
// Sorted although arrival order is not.
int64_t jitter(int i) { return kBase + (i % 7 == 3 && i % kInterval >= 2 ? i - 2 : i) * kStep; }
// The first and last events to arrive are not their blocks' extremes.
int64_t endsSwapped(int i)
{
  if (i == 0 || i == 1)
  {
    return kBase + (1 - i) * kStep;
  }
  if (i == kEvents - 1)
  {
    return kBase + (kEvents - 2) * kStep - kStep / 2;
  }
  return kBase + i * kStep;
}
// An inversion across blocks: not Sorted.
int64_t crossBlock(int i) { return kBase + (i == 300 ? 10 : i) * kStep; }

constexpr Shape kShapes[] = {
    {"in_order", inOrder},
    {"jitter", jitter},
    {"ends_swapped", endsSwapped},
    {"cross_block", crossBlock},
};

class IndexRebuildCompressedTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    _dir = std::filesystem::temp_directory_path() /
           (std::string("lrvx_index_rebuild_") + info->name());
    std::filesystem::remove_all(_dir);
    std::filesystem::create_directories(_dir);
  }
  void TearDown() override { std::filesystem::remove_all(_dir); }

  // Trades and books on two symbols, as a collector writes them.
  std::filesystem::path write(const Shape& shape)
  {
    const std::string name = std::string(shape.name) + ".lrvx";
    WriterConfig cfg{.output_dir = _dir,
                     .output_filename = name,
                     .max_segment_bytes = 1ull << 30,
                     .index_interval = kInterval,
                     .compression = CompressionType::LZ4};
    BinaryLogWriter w(cfg);
    for (int i = 0; i < kEvents; ++i)
    {
      if (i % 4 == 1)
      {
        BookRecordHeader h{};
        h.exchange_ts_ns = shape.ts(i);
        h.recv_ts_ns = h.exchange_ts_ns + 5;
        h.seq = i;
        h.symbol_id = 2;
        h.type = 1;
        const BookLevel bids[2]{{100 - i, 3}, {99 - i, 4}};
        const BookLevel asks[1]{{101 + i, 5}};
        EXPECT_TRUE(w.writeBook(h, bids, asks));
      }
      else
      {
        TradeRecord t{};
        t.exchange_ts_ns = shape.ts(i);
        t.recv_ts_ns = t.exchange_ts_ns + 1;
        t.price_raw = 100 + i;
        t.qty_raw = 1;
        t.trade_id = static_cast<uint64_t>(i);
        t.symbol_id = 1;
        EXPECT_TRUE(w.writeTrade(t));
      }
    }
    w.close();
    return _dir / name;
  }

  std::filesystem::path _dir;
};

}  // namespace

TEST_F(IndexRebuildCompressedTest, RebuildGivesBackTheWritersFile)
{
  for (const auto& shape : kShapes)
  {
    SCOPED_TRACE(shape.name);
    const auto seg = write(shape);
    const auto written = slurp(seg);
    ASSERT_TRUE(readHeader(seg).isCompressed());

    ASSERT_TRUE(IndexBuilder::removeIndex(seg));
    ASSERT_FALSE(IndexBuilder::hasIndex(seg));
    IndexBuilder builder(IndexBuilderConfig{.index_interval = kInterval});
    const auto r = builder.buildForSegment(seg);
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.events_scanned, static_cast<uint32_t>(kEvents));
    EXPECT_EQ(r.index_entries_created, (kEvents + kInterval - 1) / kInterval);
    EXPECT_EQ(slurp(seg), written);
  }
}

// Rebuilding in place over the writer's own index changes nothing either.
TEST_F(IndexRebuildCompressedTest, RebuildOverAnExistingIndexChangesNothing)
{
  for (const auto& shape : kShapes)
  {
    SCOPED_TRACE(shape.name);
    const auto seg = write(shape);
    const auto written = slurp(seg);
    IndexBuilder builder(IndexBuilderConfig{.index_interval = kInterval});
    const auto r = builder.buildForSegment(seg);
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(slurp(seg), written);
  }
}

// SegmentRepairer replaces a damaged index through the builder.
TEST_F(IndexRebuildCompressedTest, RepairerRebuildsADamagedIndex)
{
  const auto seg = write(kShapes[0]);
  const auto written = slurp(seg);
  const SegmentHeader hdr = readHeader(seg);
  {
    std::FILE* f = std::fopen(seg.string().c_str(), "r+b");
    ASSERT_NE(f, nullptr);
    // First entry's timestamp: the index CRC no longer matches.
    std::fseek(f, static_cast<long>(hdr.index_offset + sizeof(SegmentIndexHeader)), SEEK_SET);
    const int64_t junk = 42;
    ASSERT_EQ(std::fwrite(&junk, sizeof(junk), 1, f), 1u);
    std::fclose(f);
  }
  SegmentValidator validator;
  const auto before = validator.validate(seg);
  ASSERT_TRUE(before.has_index);
  ASSERT_FALSE(before.index_valid);

  SegmentRepairer repairer(RepairConfig{.backup_before_repair = false});
  const auto result = repairer.repair(seg, before);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.errors.empty()) << (result.errors.empty() ? "" : result.errors.front());
  // The default interval differs from the writer's here, so only the header's
  // interval field may differ from the writer's file.
  auto repaired = slurp(seg);
  ASSERT_EQ(repaired.size(), written.size());
  const size_t interval_at = hdr.index_offset + offsetof(SegmentIndexHeader, interval);
  std::memcpy(repaired.data() + interval_at, written.data() + interval_at, sizeof(uint16_t));
  EXPECT_EQ(repaired, written);
  EXPECT_TRUE(validator.validate(seg).index_valid);
}

// A ranged read through a rebuilt index finds what the writer's index found.
TEST_F(IndexRebuildCompressedTest, RangedReadThroughARebuiltIndex)
{
  const auto seg = write(kShapes[1]);
  auto ranged = [&]
  {
    BinaryLogReader reader(ReaderConfig{.data_dir = _dir,
                                        .from_ns = kBase + 400 * kStep,
                                        .to_ns = kBase + 450 * kStep});
    std::vector<int64_t> ts;
    reader.forEach(
        [&](const ReplayEvent& ev)
        {
          ts.push_back(ev.timestamp_ns);
          return true;
        });
    return std::make_pair(ts, reader.stats().bytes_read);
  };
  const auto want = ranged();
  ASSERT_FALSE(want.first.empty());
  ASSERT_TRUE(IndexBuilder::removeIndex(seg));
  IndexBuilder builder(IndexBuilderConfig{.index_interval = kInterval});
  ASSERT_TRUE(builder.buildForSegment(seg).success);
  const auto got = ranged();
  EXPECT_EQ(got.first, want.first);
  // The rebuilt index narrows the read exactly as the writer's did.
  EXPECT_EQ(got.second, want.second);
}

// The header records first and last events in arrival order, which the
// sorted blocks no longer show. The builder keeps what the writer stored and
// fills a header a killed writer left at zero from the stored events.
TEST_F(IndexRebuildCompressedTest, HeaderFirstAndLastFollowTheSpec)
{
  const auto seg = write(kShapes[2]);
  const SegmentHeader written = readHeader(seg);
  ASSERT_TRUE(written.isSorted());
  EXPECT_EQ(written.first_event_ns, endsSwapped(0));
  EXPECT_EQ(written.last_event_ns, endsSwapped(kEvents - 1));

  IndexBuilder builder(IndexBuilderConfig{.index_interval = kInterval});
  ASSERT_TRUE(IndexBuilder::removeIndex(seg));
  ASSERT_TRUE(builder.buildForSegment(seg).success);
  EXPECT_EQ(readHeader(seg).first_event_ns, written.first_event_ns);
  EXPECT_EQ(readHeader(seg).last_event_ns, written.last_event_ns);

  SegmentHeader zeroed = readHeader(seg);
  zeroed.first_event_ns = 0;
  zeroed.last_event_ns = 0;
  zeroed.event_count = 0;
  writeHeader(seg, zeroed);
  ASSERT_TRUE(IndexBuilder::removeIndex(seg));
  ASSERT_TRUE(builder.buildForSegment(seg).success);
  const SegmentHeader filled = readHeader(seg);
  EXPECT_EQ(filled.event_count, static_cast<uint32_t>(kEvents));
  // Stored order: each block sorted, so the first block's smallest event
  // first and the last block's largest event last.
  EXPECT_EQ(filled.first_event_ns, kBase);
  EXPECT_EQ(filled.last_event_ns, kBase + (kEvents - 2) * kStep);
}

// A block that cannot be decoded stops the build with an error rather than
// indexing part of the segment.
TEST_F(IndexRebuildCompressedTest, UndecodableBlockIsAnError)
{
  const auto seg = write(kShapes[0]);
  ASSERT_TRUE(IndexBuilder::removeIndex(seg));
  {
    std::FILE* f = std::fopen(seg.string().c_str(), "r+b");
    ASSERT_NE(f, nullptr);
    CompressedBlockHeader bh{};
    std::fseek(f, sizeof(SegmentHeader), SEEK_SET);
    ASSERT_EQ(std::fread(&bh, sizeof(bh), 1, f), 1u);
    bh.original_size += 16;
    std::fseek(f, sizeof(SegmentHeader), SEEK_SET);
    ASSERT_EQ(std::fwrite(&bh, sizeof(bh), 1, f), 1u);
    std::fclose(f);
  }
  const auto size_before = std::filesystem::file_size(seg);
  const auto r = IndexBuilder().buildForSegment(seg);
  EXPECT_FALSE(r.success);
  EXPECT_NE(r.error.find("Decompression failed"), std::string::npos) << r.error;
  EXPECT_EQ(std::filesystem::file_size(seg), size_before);
  EXPECT_FALSE(IndexBuilder::hasIndex(seg));
}
