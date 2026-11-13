/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Segment rotation on the compressed writer.
//
// WriterConfig::max_segment_bytes is the only bound on how large a single
// .lrvx segment may grow. Every uncompressed write path consults it through
// maybeRotate(); the compressed path -- writeFrameToBlock, the single entry
// point for all compressed writes -- never does. A compressed recording
// therefore produces one unbounded segment no matter what the caller asked
// for, which defeats partial reads, rsync-as-you-record and per-segment
// recovery.

#include "lrvx/common.h"
#include "lrvx/replay/binary_format_v1.h"
#include "lrvx/replay/readers/binary_log_reader.h"
#include "lrvx/replay/writers/binary_log_writer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace lrvx;
using namespace lrvx::replay;

namespace
{

constexpr int64_t kBaseNs = 1'700'000'000'000'000'000;
// Small enough that a few hundred trades must cross it several times, large
// enough to hold a whole compressed block plus its headers.
constexpr uint64_t kMaxSegmentBytes = 8 * 1024;
constexpr size_t kTradeCount = 4000;

class ReplayWriterRotationTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    _dir = std::filesystem::temp_directory_path() / "lrvx_replay_writer_rotation";
    std::filesystem::remove_all(_dir);
    std::filesystem::create_directories(_dir);
  }

  void TearDown() override { std::filesystem::remove_all(_dir); }

  // Writes kTradeCount trades through a writer bounded at kMaxSegmentBytes.
  void writeBoundedTape(CompressionType compression)
  {
    WriterConfig cfg{};
    cfg.output_dir = _dir;
    // A fixed first name puts rotation on the deterministic "<stem>_NNNN"
    // branch; the default wall-clock names could collide when two rotations
    // land inside one clock tick.
    cfg.output_filename = "tape.lrvx";
    cfg.max_segment_bytes = kMaxSegmentBytes;
    cfg.compression = compression;
    // Blocks of 64 events, so the compressed path reaches flushBlock (and any
    // rotation check placed around it) many times over the run.
    cfg.index_interval = 64;

    BinaryLogWriter writer(cfg);
    for (size_t i = 0; i < kTradeCount; ++i)
    {
      TradeRecord r{};
      r.exchange_ts_ns = kBaseNs + static_cast<int64_t>(i) * 1'000'000;
      r.recv_ts_ns = r.exchange_ts_ns;
      // Incompressible payload: an LZ4 block of identical records would shrink
      // far enough to hide a missing rotation behind the compression ratio.
      r.price_raw = static_cast<int64_t>(i) * 2654435761LL + 1000003LL;
      r.qty_raw = static_cast<int64_t>(i) * 40503LL + 7919LL;
      r.trade_id = i + 1;
      r.symbol_id = 1;
      writer.writeTrade(r);
    }
    writer.close();
  }

  // A tape of byte-identical trade records. Identical records make every full
  // block compress to the same number of bytes, so the on-disk block size can
  // be read off the file and used as an exact bound; and they put the
  // compressed size far below the uncompressed one, which is what separates
  // "measured on the file" from "measured on the events".
  void writeUniformCompressibleTape(uint64_t max_segment_bytes, uint16_t index_interval,
                                    size_t trade_count)
  {
    WriterConfig cfg{};
    cfg.output_dir = _dir;
    cfg.output_filename = "tape.lrvx";
    cfg.max_segment_bytes = max_segment_bytes;
    cfg.index_interval = index_interval;
    cfg.compression = CompressionType::LZ4;

    BinaryLogWriter writer(cfg);
    TradeRecord r{};
    r.exchange_ts_ns = kBaseNs;
    r.recv_ts_ns = kBaseNs;
    r.price_raw = Price::fromDouble(100.0).raw();
    r.qty_raw = Quantity::fromDouble(1.0).raw();
    r.trade_id = 1;
    r.symbol_id = 1;
    for (size_t i = 0; i < trade_count; ++i)
    {
      writer.writeTrade(r);
    }
    writer.close();
  }

  // Uncompressed trades under an exact byte bound. One frame is
  // sizeof(FrameHeader) + sizeof(TradeRecord); a segment opens with a
  // SegmentHeader and nothing else, so the arithmetic is exact and the bound
  // can be put on a frame boundary.
  void writeUncompressedTrades(uint64_t max_segment_bytes, size_t count)
  {
    WriterConfig cfg{};
    cfg.output_dir = _dir;
    cfg.output_filename = "tape.lrvx";
    cfg.max_segment_bytes = max_segment_bytes;
    cfg.compression = CompressionType::None;

    BinaryLogWriter writer(cfg);
    for (size_t i = 0; i < count; ++i)
    {
      TradeRecord r{};
      r.exchange_ts_ns = kBaseNs + static_cast<int64_t>(i) * 1'000'000;
      r.recv_ts_ns = r.exchange_ts_ns;
      r.price_raw = Price::fromDouble(100.0).raw();
      r.qty_raw = Quantity::fromDouble(1.0).raw();
      r.trade_id = i + 1;
      r.symbol_id = 1;
      writer.writeTrade(r);
    }
    writer.close();
  }

  // Event counts of every non-empty segment, in the order a reader walks them.
  std::vector<uint32_t> segmentEventCounts() const
  {
    std::vector<uint32_t> counts;
    for (const auto& path : sortedSegments())
    {
      const uint32_t n = headerOf(path).event_count;
      if (n > 0)
      {
        counts.push_back(n);
      }
    }
    return counts;
  }

  // One compressed block of `index_interval` identical records, as it lands on
  // disk. Written into a throwaway directory so the tape under test stays the
  // only thing in _dir.
  uint64_t measureUniformBlockBytes(uint16_t index_interval)
  {
    const auto probe = _dir / "probe";
    std::filesystem::remove_all(probe);
    std::filesystem::create_directories(probe);

    {
      WriterConfig cfg{};
      cfg.output_dir = probe;
      cfg.output_filename = "probe.lrvx";
      cfg.index_interval = index_interval;
      cfg.compression = CompressionType::LZ4;
      BinaryLogWriter writer(cfg);
      TradeRecord r{};
      r.exchange_ts_ns = kBaseNs;
      r.recv_ts_ns = kBaseNs;
      r.price_raw = Price::fromDouble(100.0).raw();
      r.qty_raw = Quantity::fromDouble(1.0).raw();
      r.trade_id = 1;
      r.symbol_id = 1;
      for (uint16_t i = 0; i < index_interval; ++i)
      {
        writer.writeTrade(r);
      }
      writer.close();
    }

    const uint64_t bytes = firstBlockBytesOnDisk(probe / "probe.lrvx");
    std::filesystem::remove_all(probe);
    return bytes;
  }

  // Segment paths in the order a reader walks them (by filename).
  std::vector<std::filesystem::path> sortedSegments() const
  {
    auto paths = segments();
    std::sort(paths.begin(), paths.end());
    return paths;
  }

  static SegmentHeader headerOf(const std::filesystem::path& path)
  {
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in.good()) << "cannot open segment " << path;
    SegmentHeader header{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    EXPECT_EQ(in.gcount(), static_cast<std::streamsize>(sizeof(header)));
    return header;
  }

  // Bytes the first compressed block of `path` occupies in the file.
  static uint64_t firstBlockBytesOnDisk(const std::filesystem::path& path)
  {
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in.good()) << "cannot open segment " << path;
    in.seekg(sizeof(SegmentHeader));
    CompressedBlockHeader block{};
    in.read(reinterpret_cast<char*>(&block), sizeof(block));
    EXPECT_EQ(in.gcount(), static_cast<std::streamsize>(sizeof(block)));
    EXPECT_TRUE(block.isValid());
    return sizeof(CompressedBlockHeader) + block.compressed_size;
  }

  std::vector<std::filesystem::path> segments() const
  {
    std::vector<std::filesystem::path> out;
    for (const auto& entry : std::filesystem::directory_iterator(_dir))
    {
      if (entry.is_regular_file() && entry.path().extension() == ".lrvx")
      {
        out.push_back(entry.path());
      }
    }
    return out;
  }

  uint64_t largestSegmentBytes() const
  {
    uint64_t largest = 0;
    for (const auto& p : segments())
    {
      largest = std::max<uint64_t>(largest, std::filesystem::file_size(p));
    }
    return largest;
  }

  size_t countTrades() const
  {
    ReaderConfig cfg{};
    cfg.data_dir = _dir;
    BinaryLogReader reader(cfg);
    size_t n = 0;
    reader.forEach(
        [&](const ReplayEvent& ev)
        {
          if (ev.type == EventType::Trade)
          {
            ++n;
          }
          return true;
        });
    return n;
  }

  std::filesystem::path _dir;
};

}  // namespace

// The finding. A compressed writer bounded at kMaxSegmentBytes writes far past
// that bound and still produces exactly one segment.
TEST_F(ReplayWriterRotationTest, CompressedWriterRotatesAtTheConfiguredBound)
{
  writeBoundedTape(CompressionType::LZ4);

  EXPECT_GT(segments().size(), 1u)
      << "max_segment_bytes=" << kMaxSegmentBytes
      << " was configured but the compressed writer produced a single segment";
}

// A rotation that fires but writes segments of arbitrary size is not a
// rotation. The bound may be overshot by at most one block plus the index a
// closing segment appends, never by the whole recording.
TEST_F(ReplayWriterRotationTest, CompressedSegmentsStayNearTheConfiguredBound)
{
  writeBoundedTape(CompressionType::LZ4);

  EXPECT_LE(largestSegmentBytes(), kMaxSegmentBytes * 4)
      << "the largest compressed segment is far past max_segment_bytes=" << kMaxSegmentBytes;
}

// Rotation must not cost events: every trade written is still readable across
// the rotated set. Green on the untouched tree (one segment holds everything);
// it is here to fail a "fix" that rotates by dropping the tail of a segment.
TEST_F(ReplayWriterRotationTest, CompressedRotationKeepsEveryEvent)
{
  writeBoundedTape(CompressionType::LZ4);

  EXPECT_EQ(countTrades(), kTradeCount);
}

// Control. The uncompressed path already rotates; it must keep rotating, and
// keep every event, after the compressed path is fixed. Green on the untouched
// tree.
TEST_F(ReplayWriterRotationTest, UncompressedWriterRotatesAtTheConfiguredBound)
{
  writeBoundedTape(CompressionType::None);

  EXPECT_GT(segments().size(), 1u);
  EXPECT_LE(largestSegmentBytes(), kMaxSegmentBytes * 4);
  EXPECT_EQ(countTrades(), kTradeCount);
}

// The rule the fix states, pinned on the number it states it on: rotate on the
// compressed byte count that reached the file, checked after the block is
// flushed, at max_segment_bytes.
//
// The tape is byte-identical records, so one block costs ~60 bytes on disk and
// 3840 bytes of events -- the two numbers a rotation could be measured on are
// two orders of magnitude apart, and the bound sits between them. Every
// segment closed by the bound must therefore end in [max_segment_bytes,
// max_segment_bytes + one block): at or past the bound, because the check runs
// after the block reached the file, and less than a block past it, because the
// segment before that block was still under.
//
// Measuring the events instead of the file closes after the first block
// (3840 >= 2048) and lands far below the bound. Checking before the flush
// lands exactly one block above it. Adding a block of slack lands a whole
// uncompressed block above it. All three are outside the window.
TEST_F(ReplayWriterRotationTest, CompressedRotationMeasuresTheBytesThatReachTheFile)
{
  constexpr uint64_t kBound = 2048;
  constexpr uint16_t kBlockEvents = 64;
  // Not a multiple of the block size, so the run ends mid-block and leaves no
  // empty trailing segment behind the last rotation.
  constexpr size_t kTrades = 7960;

  writeUniformCompressibleTape(kBound, kBlockEvents, kTrades);

  auto paths = sortedSegments();
  ASSERT_GT(paths.size(), 2u) << "the tape did not rotate often enough to pin the bound";

  const uint64_t block_on_disk = firstBlockBytesOnDisk(paths.front());
  const uint64_t block_in_events =
      static_cast<uint64_t>(kBlockEvents) * (sizeof(FrameHeader) + sizeof(TradeRecord));

  // The window only means something while these hold; say so rather than fail
  // an assertion nobody can read.
  ASSERT_LT(block_on_disk, kBound)
      << "a single block already fills the segment, so no segment can hold more than one";
  ASSERT_GT(block_in_events, kBound)
      << "one block of events is smaller than the bound, so measuring the events "
         "and measuring the file would rotate at the same place";

  for (size_t i = 0; i + 1 < paths.size(); ++i)
  {
    const SegmentHeader header = headerOf(paths[i]);
    if (header.event_count == 0)
    {
      continue;
    }
    // index_offset is where the segment's data ends and its index begins: the
    // byte count the bound is supposed to be compared against.
    ASSERT_GT(header.index_offset, 0u) << "segment " << paths[i] << " carries no index";
    EXPECT_GE(header.index_offset, kBound)
        << "segment " << paths[i].filename() << " closed at " << header.index_offset
        << " bytes, below max_segment_bytes=" << kBound
        << " -- the bound was measured on something other than the file";
    EXPECT_LT(header.index_offset, kBound + block_on_disk)
        << "segment " << paths[i].filename() << " closed at " << header.index_offset
        << " bytes, a whole block past max_segment_bytes=" << kBound << " (block is "
        << block_on_disk << " bytes on disk) -- the bound was checked too late";
  }

  EXPECT_EQ(countTrades(), kTrades);
}

// max_segment_bytes is a size the segment may reach, not one it may not
// touch. maybeRotate asks whether the frame about to be written would take
// the segment *past* the bound; a segment filled to exactly the bound is full
// and its last frame belongs in it. Rotating one frame early is invisible at
// the 256 MB default and wrong everywhere the bound was chosen to match
// something -- a block device, a transfer unit, a retention quota. The
// arithmetic is exact on the uncompressed path, so the boundary can be named:
// header plus thirty-two frames, to the byte.
TEST_F(ReplayWriterRotationTest, AnUncompressedSegmentMayBeFilledExactlyToTheBound)
{
  constexpr uint64_t kFrameBytes = sizeof(FrameHeader) + sizeof(TradeRecord);
  constexpr uint32_t kFramesPerSegment = 32;
  constexpr uint64_t kBound = sizeof(SegmentHeader) + kFramesPerSegment * kFrameBytes;

  // Exactly enough to reach the bound: one segment, nothing rotated.
  writeUncompressedTrades(kBound, kFramesPerSegment);
  EXPECT_EQ(segmentEventCounts(), (std::vector<uint32_t>{kFramesPerSegment}))
      << "a segment filled to exactly max_segment_bytes=" << kBound
      << " rotated, so the last frame that fit was pushed into a new segment";

  // One frame more: the frame that does not fit opens the next segment, and it
  // is that frame and no earlier one.
  std::filesystem::remove_all(_dir);
  std::filesystem::create_directories(_dir);
  writeUncompressedTrades(kBound, kFramesPerSegment + 1);
  EXPECT_EQ(segmentEventCounts(), (std::vector<uint32_t>{kFramesPerSegment, 1u}))
      << "the segment did not close on the first frame that would have taken it "
         "past max_segment_bytes="
      << kBound;
}

// The same boundary on the compressed path, where the unit is a block rather
// than a frame. A compressed segment can only land on the bound when the
// bound sits on the block grid, so the test puts it there: measure what one
// block of identical records costs on disk, then ask for a segment of exactly
// four of them. Reaching the bound closes the segment; requiring the segment
// to exceed it carries a fifth block in.
TEST_F(ReplayWriterRotationTest, ACompressedSegmentClosesOnReachingTheBoundExactly)
{
  constexpr uint16_t kBlockEvents = 64;
  constexpr uint64_t kBlocksPerSegment = 4;

  const uint64_t block_on_disk = measureUniformBlockBytes(kBlockEvents);
  ASSERT_GT(block_on_disk, sizeof(CompressedBlockHeader));

  const uint64_t bound = sizeof(SegmentHeader) + kBlocksPerSegment * block_on_disk;
  const uint64_t events_per_segment = kBlocksPerSegment * kBlockEvents;

  // Three full segments plus a short tail, so the run does not end on a
  // rotation and leave an empty segment behind it.
  const size_t trades = static_cast<size_t>(3 * events_per_segment) + 40;
  writeUniformCompressibleTape(bound, kBlockEvents, trades);

  auto paths = sortedSegments();
  ASSERT_EQ(paths.size(), 4u) << "the bound did not land on the block grid as intended";

  for (size_t i = 0; i + 1 < paths.size(); ++i)
  {
    const SegmentHeader header = headerOf(paths[i]);
    EXPECT_EQ(header.index_offset, bound)
        << "segment " << paths[i].filename() << " ended at " << header.index_offset
        << " bytes instead of exactly max_segment_bytes=" << bound
        << " -- reaching the bound did not close it";
    EXPECT_EQ(header.event_count, events_per_segment)
        << "segment " << paths[i].filename() << " carries " << header.event_count
        << " events instead of " << events_per_segment << " (" << kBlocksPerSegment
        << " blocks of " << kBlockEvents << ")";
  }

  EXPECT_EQ(countTrades(), trades);
}

// A bound of 0 bytes is no bound anyone asked for: every event crossed it, so
// each got a segment of its own. 0 is the default bound.
TEST_F(ReplayWriterRotationTest, AZeroBoundIsTheDefaultBound)
{
  writeUncompressedTrades(0, 50);
  EXPECT_EQ(sortedSegments().size(), 1u);
  EXPECT_EQ(countTrades(), 50u);

  WriterConfig cfg{};
  EXPECT_EQ(cfg.max_segment_bytes, kDefaultMaxSegmentBytes);
}

TEST_F(ReplayWriterRotationTest, AZeroBoundIsTheDefaultBoundWhenCompressed)
{
  writeUniformCompressibleTape(0, 16, 200);
  EXPECT_EQ(sortedSegments().size(), 1u);
  EXPECT_EQ(countTrades(), 200u);
}
