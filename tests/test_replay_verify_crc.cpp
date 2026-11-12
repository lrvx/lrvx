/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// ReaderConfig::verify_crc and ParallelReaderConfig::verify_crc on every read
// path.
//
// With the check on (the default) a frame whose payload does not match its
// CRC is skipped and counted in crc_errors. With it off the frame is not
// checked: it is delivered as it parses, and nothing is counted. The tape here
// carries one frame whose CRC field was altered and whose payload is intact,
// so the flag decides whether that one event is delivered.

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
constexpr int kInterval = 50;
constexpr int64_t kBase = 1'700'000'000'000'000'000LL;
constexpr int64_t kStep = 1'000'000;
constexpr int kDamagedFrame = 333;  // in the middle segment
constexpr uint64_t kTotal = static_cast<uint64_t>(kSegments) * kPerSegment;

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

// Flips the CRC field of frame `ordinal` (file order) and leaves its payload
// alone. A compressed segment has the block holding it recompressed; the frame
// sizes are unchanged, so the block's compressed length may move and the
// index offsets are rebuilt with it.
void corruptCrc(const std::filesystem::path& path, int ordinal)
{
  auto bytes = slurp(path);
  SegmentHeader hdr{};
  std::memcpy(&hdr, bytes.data(), sizeof(hdr));
  const size_t data_end = hdr.hasIndex() ? hdr.index_offset : bytes.size();
  auto touch = [&](std::byte* at, int n)
  {
    FrameHeader fh{};
    std::memcpy(&fh, at, sizeof(fh));
    if (n == ordinal)
    {
      fh.crc32 ^= 0x5A5A5A5Au;
      std::memcpy(at, &fh, sizeof(fh));
    }
    return sizeof(FrameHeader) + fh.size;
  };
  if (!hdr.isCompressed())
  {
    size_t pos = sizeof(SegmentHeader);
    for (int n = 0; pos < data_end; ++n)
    {
      pos += touch(bytes.data() + pos, n);
    }
    spit(path, bytes);
    return;
  }
  std::vector<std::byte> out(bytes.begin(), bytes.begin() + sizeof(SegmentHeader));
  SegmentIndexHeader ih{};
  std::memcpy(&ih, bytes.data() + hdr.index_offset, sizeof(ih));
  std::vector<IndexEntry> index(ih.entry_count);
  std::memcpy(index.data(), bytes.data() + hdr.index_offset + sizeof(ih),
              index.size() * sizeof(IndexEntry));
  size_t pos = sizeof(SegmentHeader);
  int n = 0;
  for (size_t block = 0; pos < data_end; ++block)
  {
    CompressedBlockHeader bh{};
    std::memcpy(&bh, bytes.data() + pos, sizeof(bh));
    const size_t stored = sizeof(bh) + bh.compressed_size;
    std::vector<std::byte> raw(bh.original_size);
    ASSERT_EQ(Compressor::decompress(hdr.compressionType(), bytes.data() + pos + sizeof(bh),
                                     bh.compressed_size, raw.data(), raw.size()),
              raw.size());
    for (size_t f = 0; f < raw.size(); ++n)
    {
      f += touch(raw.data() + f, n);
    }
    std::vector<std::byte> packed(Compressor::maxCompressedSize(hdr.compressionType(), raw.size()));
    const size_t packed_size = Compressor::compress(hdr.compressionType(), raw.data(), raw.size(),
                                                    packed.data(), packed.size());
    ASSERT_GT(packed_size, 0u);
    index[block].file_offset = out.size();
    bh.compressed_size = static_cast<uint32_t>(packed_size);
    const auto* p = reinterpret_cast<const std::byte*>(&bh);
    out.insert(out.end(), p, p + sizeof(bh));
    out.insert(out.end(), packed.begin(), packed.begin() + static_cast<std::ptrdiff_t>(packed_size));
    pos += stored;
  }
  ih.crc32 = Crc32::compute(index.data(), index.size() * sizeof(IndexEntry));
  hdr.index_offset = out.size();
  const auto* ip = reinterpret_cast<const std::byte*>(&ih);
  out.insert(out.end(), ip, ip + sizeof(ih));
  const auto* ep = reinterpret_cast<const std::byte*>(index.data());
  out.insert(out.end(), ep, ep + index.size() * sizeof(IndexEntry));
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

class ReplayVerifyCrcTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    _dir = std::filesystem::temp_directory_path() /
           (std::string("lrvx_verify_crc_") + info->name());
    std::filesystem::remove_all(_dir);
  }
  void TearDown() override { std::filesystem::remove_all(_dir); }

  std::filesystem::path writeTape(const Kind& kind)
  {
    const auto dir = _dir / kind.name;
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
    corruptCrc(dir / "seg1.lrvx", kDamagedFrame);
    return dir;
  }

  std::filesystem::path _dir;
};

// Every BinaryLogReader read path, collecting the timestamps it delivers.
std::vector<std::pair<std::string, std::function<bool(BinaryLogReader&, std::vector<int64_t>&)>>>
readPaths()
{
  struct Collect : IAggregator
  {
    std::vector<int64_t>* out;
    std::mutex* mu;
    void onEvent(const ReplayEvent& ev) override
    {
      std::lock_guard g(*mu);
      out->push_back(ev.timestamp_ns);
    }
    std::unique_ptr<IAggregator> cloneEmpty() const override
    {
      auto c = std::make_unique<Collect>();
      c->out = out;
      c->mu = mu;
      return c;
    }
    void merge(const IAggregator&) override {}
  };
  auto run = [](std::size_t n)
  {
    return [n](BinaryLogReader& r, std::vector<int64_t>& ts)
    {
      std::mutex mu;
      Collect c;
      c.out = &ts;
      c.mu = &mu;
      std::array<IAggregator*, 1> panel{&c};
      return r.run(panel, n);
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

constexpr int64_t kDamagedTs = kBase + (kPerSegment + kDamagedFrame) * kStep;

}  // namespace

TEST_F(ReplayVerifyCrcTest, CheckedByDefaultOnEveryPath)
{
  for (const auto& kind : kKinds)
  {
    const auto dir = writeTape(kind);
    for (const auto& [name, read] : readPaths())
    {
      SCOPED_TRACE(std::string(kind.name) + " " + name);
      BinaryLogReader reader(ReaderConfig{.data_dir = dir});
      std::vector<int64_t> ts;
      ASSERT_TRUE(read(reader, ts));
      EXPECT_EQ(ts.size(), kTotal - 1);
      EXPECT_EQ(std::count(ts.begin(), ts.end(), kDamagedTs), 0);
      EXPECT_EQ(reader.stats().crc_errors, 1u);
    }
  }
}

TEST_F(ReplayVerifyCrcTest, OffDeliversTheFrameOnEveryPath)
{
  for (const auto& kind : kKinds)
  {
    const auto dir = writeTape(kind);
    for (const auto& [name, read] : readPaths())
    {
      SCOPED_TRACE(std::string(kind.name) + " " + name);
      BinaryLogReader reader(ReaderConfig{.data_dir = dir, .verify_crc = false});
      std::vector<int64_t> ts;
      ASSERT_TRUE(read(reader, ts));
      EXPECT_EQ(ts.size(), kTotal);
      EXPECT_EQ(std::count(ts.begin(), ts.end(), kDamagedTs), 1);
      EXPECT_EQ(reader.stats().crc_errors, 0u);
      EXPECT_EQ(reader.stats().events_read, kTotal);
    }
  }
}

TEST_F(ReplayVerifyCrcTest, ParallelReaderHonoursTheFlag)
{
  for (const auto& kind : kKinds)
  {
    const auto dir = writeTape(kind);
    struct Mode
    {
      const char* name;
      uint32_t threads;
      bool sorted;
    };
    for (const Mode& m : {Mode{"nt1", 1, true}, Mode{"sorted", 4, true}, Mode{"unsorted", 4, false}})
    {
      for (bool verify : {true, false})
      {
        SCOPED_TRACE(std::string(kind.name) + " " + m.name + (verify ? " verify" : " no-verify"));
        ParallelReader reader(ParallelReaderConfig{.data_dir = dir,
                                                   .num_threads = m.threads,
                                                   .verify_crc = verify,
                                                   .sort_output = m.sorted});
        std::mutex mu;
        uint64_t seen = 0;
        reader.forEach(
            [&](const ReplayEvent&)
            {
              std::lock_guard g(mu);
              ++seen;
              return true;
            });
        EXPECT_EQ(seen, verify ? kTotal - 1 : kTotal);
        EXPECT_EQ(reader.stats().crc_errors, verify ? 1u : 0u);
      }
    }
  }
}

// The iterator on its own: the flag is per iterator and defaults to on.
TEST_F(ReplayVerifyCrcTest, IteratorDefaultsToChecking)
{
  for (const auto& kind : kKinds)
  {
    SCOPED_TRACE(kind.name);
    const auto dir = writeTape(kind);
    for (bool verify : {true, false})
    {
      BinaryLogIterator it(dir / "seg1.lrvx");
      ASSERT_TRUE(it.isValid());
      if (!verify)
      {
        it.setVerifyCrc(false);
      }
      uint64_t n = 0;
      ReplayEvent ev;
      while (it.next(ev))
      {
        ++n;
      }
      EXPECT_EQ(n, verify ? kPerSegment - 1u : static_cast<uint64_t>(kPerSegment));
      EXPECT_EQ(it.crcErrors(), verify ? 1u : 0u);
    }
  }
}
