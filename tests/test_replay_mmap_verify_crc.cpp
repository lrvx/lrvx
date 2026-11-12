/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// MmapReader and MmapSegmentReader check frame CRCs the way BinaryLogReader
// does: MmapReader::Config::verify_crc defaults to true, a frame that fails
// is skipped and counted in MmapReaderStats::crc_errors, and a segment read
// that stops on data it cannot walk past is counted in segments_truncated.
// The mmap readers used to deliver a damaged frame as whatever it parsed to,
// and to end a segment silently on a short or overrunning frame.

#include "lrvx/replay/abstract_event_reader.h"
#include "lrvx/replay/binary_format_v1.h"
#include "lrvx/replay/readers/binary_log_reader.h"
#include "lrvx/replay/readers/mmap_reader.h"
#include "lrvx/replay/writers/binary_log_writer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace lrvx::replay;

namespace
{

constexpr int kSegments = 3;
constexpr int kPerSegment = 400;
constexpr int64_t kBase = 1'700'000'000'000'000'000LL;
constexpr int64_t kStep = 1'000'000;
constexpr uint64_t kTotal = static_cast<uint64_t>(kSegments) * kPerSegment;

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

// File offset of frame `ordinal` of an uncompressed segment.
size_t frameOffset(const std::vector<std::byte>& bytes, int ordinal)
{
  size_t pos = sizeof(SegmentHeader);
  for (int n = 0; n < ordinal; ++n)
  {
    FrameHeader fh{};
    std::memcpy(&fh, bytes.data() + pos, sizeof(fh));
    pos += sizeof(FrameHeader) + fh.size;
  }
  return pos;
}

class MmapVerifyCrcTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    _dir = std::filesystem::temp_directory_path() / (std::string("lrvx_mmap_crc_") + info->name());
    std::filesystem::remove_all(_dir);
    std::filesystem::create_directories(_dir);
    for (int s = 0; s < kSegments; ++s)
    {
      const std::string name = "seg" + std::to_string(s) + ".lrvx";
      WriterConfig cfg{.output_dir = _dir, .output_filename = name, .max_segment_bytes = 1ull << 30};
      BinaryLogWriter w(cfg);
      for (int i = 0; i < kPerSegment; ++i)
      {
        const int k = s * kPerSegment + i;
        if (k % 3 == 2)
        {
          BookRecordHeader h{};
          h.exchange_ts_ns = kBase + k * kStep;
          h.recv_ts_ns = h.exchange_ts_ns + 1;
          h.seq = k;
          h.symbol_id = 2;
          h.type = 1;
          const BookLevel bids[1]{{100 + k, 1}};
          const BookLevel asks[1]{{101 + k, 1}};
          ASSERT_TRUE(w.writeBook(h, bids, asks));
        }
        else
        {
          TradeRecord t{};
          t.exchange_ts_ns = kBase + k * kStep;
          t.recv_ts_ns = t.exchange_ts_ns + 1;
          t.price_raw = 100'000 + k;
          t.qty_raw = 1;
          t.trade_id = static_cast<uint64_t>(k);
          t.symbol_id = 1;
          ASSERT_TRUE(w.writeTrade(t));
        }
      }
      w.close();
    }
  }
  void TearDown() override { std::filesystem::remove_all(_dir); }

  std::filesystem::path seg(int s) const { return _dir / ("seg" + std::to_string(s) + ".lrvx"); }

  // Flips the CRC field of trade frame `ordinal` of segment 1, payload intact.
  void damageCrc(int ordinal)
  {
    auto bytes = slurp(seg(1));
    const size_t at = frameOffset(bytes, ordinal);
    FrameHeader fh{};
    std::memcpy(&fh, bytes.data() + at, sizeof(fh));
    ASSERT_EQ(fh.type, static_cast<uint8_t>(EventType::Trade));
    fh.crc32 ^= 0x5A5A5A5Au;
    std::memcpy(bytes.data() + at, &fh, sizeof(fh));
    spit(seg(1), bytes);
  }

  std::filesystem::path _dir;
};

uint64_t countAll(MmapReader& r)
{
  uint64_t n = 0;
  r.forEach([&](const ReplayEvent&)
            { return ++n, true; });
  return n;
}

}  // namespace

TEST_F(MmapVerifyCrcTest, DefaultIsToCheck)
{
  EXPECT_TRUE(MmapReader::Config{}.verify_crc);
  EXPECT_TRUE(ReaderConfig{}.verify_crc);
}

TEST_F(MmapVerifyCrcTest, DamagedFrameSkippedAndCountedOnEveryReadPath)
{
  damageCrc(30);

  MmapReader r(MmapReader::Config{.data_dir = _dir});
  EXPECT_EQ(countAll(r), kTotal - 1);
  EXPECT_EQ(r.stats().crc_errors, 1u);

  uint64_t from = 0;
  r.forEachFrom(kBase, [&](const ReplayEvent&)
                { return ++from, true; });
  EXPECT_EQ(from, kTotal - 1);
  EXPECT_EQ(r.stats().crc_errors, 2u);

  uint64_t trades = 0;
  r.forEachRawTrade([&](const TradeRecord*)
                    { return ++trades, true; });
  EXPECT_EQ(trades, kTotal - kTotal / 3 - 1);
  EXPECT_EQ(r.stats().crc_errors, 3u);
  EXPECT_EQ(r.stats().segments_truncated, 0u);

  // The counts BinaryLogReader reports for the same tape.
  BinaryLogReader blr(ReaderConfig{.data_dir = _dir});
  uint64_t n = 0;
  blr.forEach([&](const ReplayEvent&)
              { return ++n, true; });
  EXPECT_EQ(n, kTotal - 1);
  EXPECT_EQ(blr.stats().crc_errors, 1u);
}

TEST_F(MmapVerifyCrcTest, UncheckedDeliversTheFrame)
{
  damageCrc(30);
  MmapReader::Config cfg{.data_dir = _dir};
  cfg.verify_crc = false;
  MmapReader r(cfg);
  EXPECT_EQ(countAll(r), kTotal);
  EXPECT_EQ(r.stats().crc_errors, 0u);
}

TEST_F(MmapVerifyCrcTest, FactoryReadersCheck)
{
  damageCrc(30);
  auto multi = createMultiSegmentReader(_dir, {}, /*parallel=*/false, /*mmap=*/true);
  uint64_t n = 0;
  multi->forEach([&](const ReplayEvent&)
                 { return ++n, true; });
  EXPECT_EQ(n, kTotal - 1);

  auto single = createSegmentReader(seg(1), /*prefer_mmap=*/true);
  uint64_t m = 0;
  ReplayEvent ev;
  while (single->next(ev))
  {
    ++m;
  }
  EXPECT_EQ(m, static_cast<uint64_t>(kPerSegment) - 1);

  MmapSegmentReader direct(seg(1));
  uint64_t d = 0;
  while (direct.next(ev))
  {
    ++d;
  }
  EXPECT_EQ(d, static_cast<uint64_t>(kPerSegment) - 1);
  EXPECT_EQ(direct.crcErrors(), 1u);
  direct.reset();
  EXPECT_EQ(direct.crcErrors(), 0u);
}

// A frame too short for its record type is walked past and counted, as
// BinaryLogReader does, instead of ending the segment there.
TEST_F(MmapVerifyCrcTest, ShortFrameSkippedNotEndOfSegment)
{
  auto bytes = slurp(seg(1));
  const size_t at = frameOffset(bytes, 30);
  FrameHeader fh{};
  std::memcpy(&fh, bytes.data() + at, sizeof(fh));
  ASSERT_EQ(fh.type, static_cast<uint8_t>(EventType::Trade));
  // Relabel the 48-byte trade as a 40+-byte book frame whose level counts
  // overrun it, with a CRC that matches: it passes the check and fails to
  // parse.
  BookRecordHeader bh{};
  std::memcpy(&bh, bytes.data() + at + sizeof(FrameHeader), sizeof(bh));
  bh.bid_count = 50;
  std::memcpy(bytes.data() + at + sizeof(FrameHeader), &bh, sizeof(bh));
  fh.type = static_cast<uint8_t>(EventType::BookDelta);
  fh.crc32 = Crc32::compute(bytes.data() + at + sizeof(FrameHeader), fh.size);
  std::memcpy(bytes.data() + at, &fh, sizeof(fh));
  spit(seg(1), bytes);

  MmapReader r(MmapReader::Config{.data_dir = _dir});
  EXPECT_EQ(countAll(r), kTotal - 1);
  EXPECT_EQ(r.stats().crc_errors, 1u);

  BinaryLogReader blr(ReaderConfig{.data_dir = _dir});
  uint64_t n = 0;
  blr.forEach([&](const ReplayEvent&)
              { return ++n, true; });
  EXPECT_EQ(n, kTotal - 1);
  EXPECT_EQ(blr.stats().crc_errors, 1u);
}

// An indexed segment whose data stops short of its index has lost bytes: the
// read stops and counts the segment, in both readers.
TEST_F(MmapVerifyCrcTest, DataEndingBeforeTheIndexIsTruncated)
{
  const auto written = slurp(seg(1));
  SegmentHeader hdr{};
  std::memcpy(&hdr, written.data(), sizeof(hdr));
  ASSERT_TRUE(hdr.hasIndex());
  // Cut halfway into frame 100, then exactly between frames 99 and 100: the
  // header still points at the index past the end of the file either way.
  for (size_t into : {size_t{20}, size_t{0}})
  {
    SCOPED_TRACE(into);
    auto bytes = written;
    bytes.resize(frameOffset(bytes, 100) + into);
    spit(seg(1), bytes);

    MmapReader r(MmapReader::Config{.data_dir = _dir});
    EXPECT_EQ(countAll(r), kTotal - kPerSegment + 100);
    EXPECT_EQ(r.stats().segments_truncated, 1u);

    BinaryLogReader blr(ReaderConfig{.data_dir = _dir});
    uint64_t n = 0;
    blr.forEach([&](const ReplayEvent&)
                { return ++n, true; });
    EXPECT_EQ(n, kTotal - kPerSegment + 100);
    EXPECT_EQ(blr.stats().segments_truncated, 1u);
  }
}

// A frame length past the 10 MiB ceiling is a damaged header: stop there.
TEST_F(MmapVerifyCrcTest, ImplausibleFrameLengthIsTruncated)
{
  auto bytes = slurp(seg(1));
  const size_t at = frameOffset(bytes, 200);
  FrameHeader fh{};
  std::memcpy(&fh, bytes.data() + at, sizeof(fh));
  fh.size = 64u * 1024u * 1024u;
  std::memcpy(bytes.data() + at, &fh, sizeof(fh));
  spit(seg(1), bytes);

  MmapReader r(MmapReader::Config{.data_dir = _dir});
  EXPECT_EQ(countAll(r), kTotal - kPerSegment + 200);
  EXPECT_EQ(r.stats().segments_truncated, 1u);
}

// Without an index the data runs to the end of the file, whose tail may still
// be being written: a partial last frame ends the segment uncounted.
TEST_F(MmapVerifyCrcTest, PartialTailOfALiveSegmentIsNotTruncation)
{
  auto bytes = slurp(seg(1));
  SegmentHeader hdr{};
  std::memcpy(&hdr, bytes.data(), sizeof(hdr));
  const size_t cut = frameOffset(bytes, 100) + 20;
  hdr.flags &= static_cast<uint8_t>(~SegmentFlags::HasIndex);
  hdr.index_offset = 0;
  bytes.resize(cut);
  std::memcpy(bytes.data(), &hdr, sizeof(hdr));
  spit(seg(1), bytes);

  MmapReader r(MmapReader::Config{.data_dir = _dir});
  EXPECT_EQ(countAll(r), kTotal - kPerSegment + 100);
  EXPECT_EQ(r.stats().segments_truncated, 0u);
  EXPECT_EQ(r.stats().crc_errors, 0u);
}
