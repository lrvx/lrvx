/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/replay/abstract_event_reader.h"
#include "lrvx/replay/binary_format_v1.h"
#include "lrvx/replay/binary_log_recorder_hook.h"
#include "lrvx/replay/ops/index_builder.h"
#include "lrvx/replay/readers/binary_log_reader.h"
#include "lrvx/replay/time_utils.h"
#include "lrvx/replay/writers/binary_log_writer.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

using namespace lrvx::replay;

class BinaryLogTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    _test_dir = std::filesystem::temp_directory_path() / "lrvx_test_log";
    std::filesystem::remove_all(_test_dir);
    std::filesystem::create_directories(_test_dir);
  }

  void TearDown() override { std::filesystem::remove_all(_test_dir); }

  std::filesystem::path _test_dir;
};

TEST_F(BinaryLogTest, Crc32BasicValues)
{
  const char* data = "hello";
  uint32_t crc = Crc32::compute(data, 5);
  // Known CRC32 for "hello"
  EXPECT_EQ(crc, 0x3610A686);
}

TEST_F(BinaryLogTest, SegmentHeaderValidation)
{
  SegmentHeader header;
  EXPECT_TRUE(header.isValid());

  header.magic = 0;
  EXPECT_FALSE(header.isValid());

  header.magic = kMagic;
  header.version = 99;
  EXPECT_FALSE(header.isValid());
}

TEST_F(BinaryLogTest, WriteSingleTrade)
{
  WriterConfig config{.output_dir = _test_dir};
  BinaryLogWriter writer(config);

  TradeRecord trade{};
  trade.exchange_ts_ns = 1000000000;
  trade.recv_ts_ns = 1000000100;
  trade.price_raw = 50000LL * 1000000LL;  // $50,000
  trade.qty_raw = 1LL * 1000000LL;        // 1.0
  trade.trade_id = 12345;
  trade.symbol_id = 1;
  trade.side = 1;  // buy

  EXPECT_TRUE(writer.writeTrade(trade));
  writer.close();

  auto stats = writer.stats();
  EXPECT_EQ(stats.trades_written, 1);
  EXPECT_EQ(stats.segments_created, 1);
}

TEST_F(BinaryLogTest, WriteSingleBookUpdate)
{
  WriterConfig config{.output_dir = _test_dir};
  BinaryLogWriter writer(config);

  BookRecordHeader header{};
  header.exchange_ts_ns = 2000000000;
  header.recv_ts_ns = 2000000100;
  header.seq = 1;
  header.symbol_id = 1;
  header.bid_count = 2;
  header.ask_count = 2;
  header.type = 0;  // snapshot

  std::vector<BookLevel> bids = {{50000LL * 1000000LL, 10LL * 1000000LL}, {49999LL * 1000000LL, 5LL * 1000000LL}};

  std::vector<BookLevel> asks = {{50001LL * 1000000LL, 8LL * 1000000LL}, {50002LL * 1000000LL, 12LL * 1000000LL}};

  EXPECT_TRUE(writer.writeBook(header, bids, asks));
  writer.close();

  auto stats = writer.stats();
  EXPECT_EQ(stats.book_updates_written, 1);
}

TEST_F(BinaryLogTest, TopOfBookRoundTripsAsItsOwnFrameType)
{
  // Write: a snapshot, then a one-level top-of-book refresh.
  {
    WriterConfig config{.output_dir = _test_dir};
    BinaryLogWriter writer(config);

    BookRecordHeader snap{};
    snap.exchange_ts_ns = 1000;
    snap.symbol_id = 7;
    snap.bid_count = 1;
    snap.ask_count = 1;
    snap.type = 0;  // snapshot
    std::vector<BookLevel> bids = {{100LL * 1000000LL, 1LL * 1000000LL}};
    std::vector<BookLevel> asks = {{101LL * 1000000LL, 1LL * 1000000LL}};
    EXPECT_TRUE(writer.writeBook(snap, bids, asks));

    BookRecordHeader top{};
    top.exchange_ts_ns = 2000;
    top.symbol_id = 7;
    top.bid_count = 1;
    top.ask_count = 1;
    top.type = 2;  // top-of-book
    std::vector<BookLevel> tb = {{100LL * 1000000LL + 500000LL, 2LL * 1000000LL}};
    std::vector<BookLevel> ta = {{101LL * 1000000LL - 500000LL, 2LL * 1000000LL}};
    EXPECT_TRUE(writer.writeBook(top, tb, ta));
    writer.close();
  }

  // Read: the top comes back as a BookTop frame with its record type and
  // levels intact, after the snapshot.
  {
    ReaderConfig config{.data_dir = _test_dir};
    BinaryLogReader reader(config);

    std::vector<EventType> frames;
    std::vector<uint8_t> recordTypes;
    reader.forEach([&](const ReplayEvent& event)
                   {
      frames.push_back(event.type);
      recordTypes.push_back(event.book_header.type);
      if (event.type == EventType::BookTop)
      {
        EXPECT_EQ(event.book_header.exchange_ts_ns, 2000);
        EXPECT_EQ(event.bids.size(), 1u);
        EXPECT_EQ(event.asks.size(), 1u);
        EXPECT_EQ(event.bids[0].price_raw, 100LL * 1000000LL + 500000LL);
      }
      return true; });

    ASSERT_EQ(frames.size(), 2u);
    EXPECT_EQ(frames[0], EventType::BookSnapshot);
    EXPECT_EQ(frames[1], EventType::BookTop);
    EXPECT_EQ(recordTypes[0], 0);
    EXPECT_EQ(recordTypes[1], 2);
  }
}

TEST_F(BinaryLogTest, RecorderHookByteTwoLandsAsBookTopWithMetadata)
{
  // The hook's update_type byte: 1 snapshot, 0 delta, 2 top-of-book.
  {
    BinaryLogRecorderHookConfig cfg{};
    cfg.output_dir = _test_dir;
    BinaryLogRecorderHook hook(cfg);
    hook.start();

    BookLevel bid{100LL * 1000000LL, 1LL * 1000000LL};
    BookLevel ask{101LL * 1000000LL, 1LL * 1000000LL};
    hook.onBookUpdate(7, /*update_type=*/1, &bid, 1, &ask, 1, 1000, 1000);
    hook.onBookUpdate(7, /*update_type=*/2, &bid, 1, &ask, 1, 2000, 2000);
    hook.stop();
  }

  {
    ReaderConfig config{.data_dir = _test_dir};
    BinaryLogReader reader(config);

    std::vector<EventType> frames;
    reader.forEach([&](const ReplayEvent& event)
                   {
      frames.push_back(event.type);
      return true; });
    ASSERT_EQ(frames.size(), 2u);
    EXPECT_EQ(frames[0], EventType::BookSnapshot);
    EXPECT_EQ(frames[1], EventType::BookTop);
  }

  std::ifstream meta(std::filesystem::path(_test_dir) / "metadata.json");
  ASSERT_TRUE(meta.good());
  std::stringstream buf;
  buf << meta.rdbuf();
  const std::string metaText = buf.str();
  EXPECT_NE(metaText.find("\"has_book_snapshots\": true"), std::string::npos);
  EXPECT_NE(metaText.find("\"has_book_tops\": true"), std::string::npos);
}

TEST_F(BinaryLogTest, WriteAndReadTrades)
{
  // Write
  {
    WriterConfig config{.output_dir = _test_dir};
    BinaryLogWriter writer(config);

    for (int i = 0; i < 100; ++i)
    {
      TradeRecord trade{};
      trade.exchange_ts_ns = 1000000000 + i * 1000000;
      trade.price_raw = (50000LL + i) * 1000000LL;
      trade.qty_raw = 1LL * 1000000LL;
      trade.trade_id = i;
      trade.symbol_id = 1;
      trade.side = i % 2;

      EXPECT_TRUE(writer.writeTrade(trade));
    }
    writer.close();
  }

  // Read
  {
    ReaderConfig config{.data_dir = _test_dir};
    BinaryLogReader reader(config);

    int count = 0;
    reader.forEach([&](const ReplayEvent& event)
                   {
      EXPECT_EQ(event.type, EventType::Trade);
      EXPECT_EQ(event.trade.symbol_id, 1u);
      EXPECT_EQ(event.trade.trade_id, static_cast<uint64_t>(count));
      ++count;
      return true; });

    EXPECT_EQ(count, 100);
  }
}

TEST_F(BinaryLogTest, WriteAndReadBookUpdates)
{
  // Write
  {
    WriterConfig config{.output_dir = _test_dir};
    BinaryLogWriter writer(config);

    for (int i = 0; i < 50; ++i)
    {
      BookRecordHeader header{};
      header.exchange_ts_ns = 2000000000LL + i * 1000000LL;
      header.seq = i;
      header.symbol_id = 2;
      header.bid_count = 5;
      header.ask_count = 5;
      header.type = (i == 0) ? 0 : 1;  // First is snapshot, rest are deltas

      std::vector<BookLevel> bids(5);
      std::vector<BookLevel> asks(5);
      for (int j = 0; j < 5; ++j)
      {
        bids[j] = {(50000LL - j) * 1000000LL, (j + 1LL) * 1000000LL};
        asks[j] = {(50001LL + j) * 1000000LL, (j + 1LL) * 1000000LL};
      }

      EXPECT_TRUE(writer.writeBook(header, bids, asks));
    }
    writer.close();
  }

  // Read
  {
    ReaderConfig config{.data_dir = _test_dir};
    BinaryLogReader reader(config);

    int count = 0;
    reader.forEach([&](const ReplayEvent& event)
                   {
      EXPECT_NE(event.type, EventType::Trade);
      EXPECT_EQ(event.book_header.symbol_id, 2u);
      EXPECT_EQ(event.bids.size(), 5u);
      EXPECT_EQ(event.asks.size(), 5u);
      ++count;
      return true; });

    EXPECT_EQ(count, 50);
  }
}

TEST_F(BinaryLogTest, MixedEventsTimeOrdering)
{
  // Write interleaved trades and book updates
  {
    WriterConfig config{.output_dir = _test_dir};
    BinaryLogWriter writer(config);

    for (int i = 0; i < 100; ++i)
    {
      if (i % 2 == 0)
      {
        TradeRecord trade{};
        trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
        trade.symbol_id = 1;
        writer.writeTrade(trade);
      }
      else
      {
        BookRecordHeader header{};
        header.exchange_ts_ns = 1000000000LL + i * 1000000LL;
        header.symbol_id = 1;
        header.bid_count = 1;
        header.ask_count = 1;

        std::vector<BookLevel> bids = {{50000000000, 1000000}};
        std::vector<BookLevel> asks = {{50001000000, 1000000}};
        writer.writeBook(header, bids, asks);
      }
    }
    writer.close();
  }

  // Read and verify time ordering
  {
    ReaderConfig config{.data_dir = _test_dir};
    BinaryLogReader reader(config);

    int64_t last_ts = 0;
    int count = 0;
    reader.forEach([&](const ReplayEvent& event)
                   {
      EXPECT_GE(event.timestamp_ns, last_ts);
      last_ts = event.timestamp_ns;
      ++count;
      return true; });

    EXPECT_EQ(count, 100);
  }
}

TEST_F(BinaryLogTest, TimeRangeFilter)
{
  // Write events over a time range
  {
    WriterConfig config{.output_dir = _test_dir};
    BinaryLogWriter writer(config);

    for (int i = 0; i < 1000; ++i)
    {
      TradeRecord trade{};
      trade.exchange_ts_ns = i * 1000000LL;  // 0 to 999ms in ns
      trade.symbol_id = 1;
      writer.writeTrade(trade);
    }
    writer.close();
  }

  // Read with time filter (500-700ms)
  {
    ReaderConfig config{
        .data_dir = _test_dir,
        .from_ns = 500LL * 1000000LL,
        .to_ns = 700LL * 1000000LL,
    };
    BinaryLogReader reader(config);

    int count = 0;
    reader.forEach([&](const ReplayEvent& event)
                   {
      EXPECT_GE(event.timestamp_ns, 500LL * 1000000LL);
      EXPECT_LE(event.timestamp_ns, 700LL * 1000000LL);
      ++count;
      return true; });

    EXPECT_EQ(count, 201);  // 500 to 700 inclusive
  }
}

TEST_F(BinaryLogTest, SymbolFilter)
{
  // Write events for multiple symbols
  {
    WriterConfig config{.output_dir = _test_dir};
    BinaryLogWriter writer(config);

    for (int i = 0; i < 300; ++i)
    {
      TradeRecord trade{};
      trade.exchange_ts_ns = i * 1000000LL;
      trade.symbol_id = (i % 3) + 1;  // Symbols 1, 2, 3
      writer.writeTrade(trade);
    }
    writer.close();
  }

  // Read only symbol 2
  {
    ReaderConfig config{
        .data_dir = _test_dir,
        .symbols = {2},
    };
    BinaryLogReader reader(config);

    int count = 0;
    reader.forEach([&](const ReplayEvent& event)
                   {
      EXPECT_EQ(event.trade.symbol_id, 2u);
      ++count;
      return true; });

    EXPECT_EQ(count, 100);  // 1/3 of 300
  }
}

TEST_F(BinaryLogTest, SegmentRotation)
{
  WriterConfig config{
      .output_dir = _test_dir,
      .max_segment_bytes = 2048,  // Small enough to trigger rotation with 100 trades
  };
  BinaryLogWriter writer(config);

  // Write enough data to trigger rotation
  // Each trade ~60 bytes, 2048/60 = ~34 trades per segment
  // 100 trades should create ~3 segments
  for (int i = 0; i < 100; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = i * 1000000LL;
    trade.symbol_id = 1;
    writer.writeTrade(trade);
  }
  writer.close();

  auto stats = writer.stats();
  EXPECT_GT(stats.segments_created, 1u);  // Should have rotated
}

TEST_F(BinaryLogTest, EmptyBookUpdate)
{
  WriterConfig config{.output_dir = _test_dir};
  BinaryLogWriter writer(config);

  BookRecordHeader header{};
  header.exchange_ts_ns = 1000000000;
  header.symbol_id = 1;
  header.bid_count = 0;
  header.ask_count = 0;
  header.type = 1;  // delta

  std::vector<BookLevel> empty;
  EXPECT_TRUE(writer.writeBook(header, empty, empty));
  writer.close();

  // Read it back
  ReaderConfig reader_config{.data_dir = _test_dir};
  BinaryLogReader reader(reader_config);

  int count = 0;
  reader.forEach([&](const ReplayEvent& event)
                 {
    EXPECT_TRUE(event.bids.empty());
    EXPECT_TRUE(event.asks.empty());
    ++count;
    return true; });

  EXPECT_EQ(count, 1);
}

TEST_F(BinaryLogTest, LargeBookUpdate)
{
  WriterConfig config{.output_dir = _test_dir};
  BinaryLogWriter writer(config);

  const int kLevels = 1000;

  BookRecordHeader header{};
  header.exchange_ts_ns = 1000000000;
  header.symbol_id = 1;
  header.bid_count = kLevels;
  header.ask_count = kLevels;
  header.type = 0;

  std::vector<BookLevel> bids(kLevels);
  std::vector<BookLevel> asks(kLevels);
  for (int i = 0; i < kLevels; ++i)
  {
    bids[i] = {(50000LL - i) * 1000000LL, static_cast<int64_t>(i) * 1000000LL};
    asks[i] = {(50001LL + i) * 1000000LL, static_cast<int64_t>(i) * 1000000LL};
  }

  EXPECT_TRUE(writer.writeBook(header, bids, asks));
  writer.close();

  // Read it back
  ReaderConfig reader_config{.data_dir = _test_dir};
  BinaryLogReader reader(reader_config);

  int count = 0;
  reader.forEach([&](const ReplayEvent& event)
                 {
    EXPECT_EQ(event.bids.size(), kLevels);
    EXPECT_EQ(event.asks.size(), kLevels);
    ++count;
    return true; });

  EXPECT_EQ(count, 1);
}

TEST_F(BinaryLogTest, EarlyExitFromCallback)
{
  // Write 100 events
  {
    WriterConfig config{.output_dir = _test_dir};
    BinaryLogWriter writer(config);

    for (int i = 0; i < 100; ++i)
    {
      TradeRecord trade{};
      trade.exchange_ts_ns = i * 1000000LL;
      trade.symbol_id = 1;
      writer.writeTrade(trade);
    }
    writer.close();
  }

  // Read but exit after 10
  {
    ReaderConfig config{.data_dir = _test_dir};
    BinaryLogReader reader(config);

    int count = 0;
    reader.forEach([&](const ReplayEvent&)
                   {
                     ++count;
                     return count < 10;  // Stop after 10
                   });

    EXPECT_EQ(count, 10);
  }
}

TEST_F(BinaryLogTest, TimeRangeQuery)
{
  // Write events
  {
    WriterConfig config{.output_dir = _test_dir};
    BinaryLogWriter writer(config);

    TradeRecord trade{};
    trade.exchange_ts_ns = 1000;
    writer.writeTrade(trade);

    trade.exchange_ts_ns = 5000;
    writer.writeTrade(trade);

    writer.close();
  }

  // Query time range
  ReaderConfig config{.data_dir = _test_dir};
  BinaryLogReader reader(config);

  // Need to scan first
  reader.forEach([](const ReplayEvent&)
                 { return true; });

  auto range = reader.timeRange();
  ASSERT_TRUE(range.has_value());
  EXPECT_EQ(range->first, 1000);
  EXPECT_EQ(range->second, 5000);
}

TEST_F(BinaryLogTest, IndexCreation)
{
  // Write events with small index interval to ensure index is created
  WriterConfig config{
      .output_dir = _test_dir,
      .create_index = true,
      .index_interval = 10,  // Create index entry every 10 events
  };
  BinaryLogWriter writer(config);

  // Write 100 events (should create ~10 index entries)
  for (int i = 0; i < 100; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = (i + 1) * 1000000000LL;  // 1s, 2s, ..., 100s
    trade.symbol_id = 1;
    writer.writeTrade(trade);
  }
  writer.close();

  // Read segment info and verify index exists
  ReaderConfig reader_config{.data_dir = _test_dir};
  BinaryLogReader reader(reader_config);

  // Trigger scan
  reader.forEach([](const ReplayEvent&)
                 { return false; });

  const auto& segments = reader.segments();
  ASSERT_EQ(segments.size(), 1u);
  EXPECT_TRUE(segments[0].has_index);
  EXPECT_GT(segments[0].index_offset, 0u);
}

TEST_F(BinaryLogTest, IndexSeekForward)
{
  // Write events with index
  WriterConfig config{
      .output_dir = _test_dir,
      .create_index = true,
      .index_interval = 100,
  };
  BinaryLogWriter writer(config);

  // Write 1000 events spanning 1000 seconds
  for (int i = 0; i < 1000; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = (i + 1) * 1000000000LL;
    trade.symbol_id = 1;
    trade.price_raw = (i + 1) * 1000000LL;
    writer.writeTrade(trade);
  }
  writer.close();

  // Read from timestamp 500s - should skip ~500 events using index
  ReaderConfig reader_config{.data_dir = _test_dir};
  BinaryLogReader reader(reader_config);

  int64_t start_ts = 500LL * 1000000000LL;  // 500s
  int count = 0;
  int64_t first_ts = 0;

  reader.forEachFrom(start_ts, [&](const ReplayEvent& event)
                     {
    if (count == 0){ first_ts = event.timestamp_ns;
}
    ++count;
    return true; });

  // Should have read approximately 500 events (from 500s to 1000s)
  EXPECT_GE(count, 500);
  EXPECT_LE(count, 510);  // Allow small variance due to index granularity
  EXPECT_GE(first_ts, start_ts);
}

TEST_F(BinaryLogTest, IndexLoadAndSeek)
{
  // Write events with small index interval
  WriterConfig config{
      .output_dir = _test_dir,
      .create_index = true,
      .index_interval = 10,
  };
  BinaryLogWriter writer(config);

  for (int i = 0; i < 100; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = (i + 1) * 1000000000LL;
    trade.symbol_id = 1;
    writer.writeTrade(trade);
  }
  writer.close();

  // Use iterator directly to test index loading
  auto files = std::filesystem::directory_iterator(_test_dir);
  std::filesystem::path segment_path;
  for (const auto& entry : files)
  {
    if (entry.path().extension() == ".lrvx")
    {
      segment_path = entry.path();
      break;
    }
  }

  BinaryLogIterator iter(segment_path);
  ASSERT_TRUE(iter.isValid());
  EXPECT_TRUE(iter.header().hasIndex());

  EXPECT_TRUE(iter.loadIndex());
  EXPECT_TRUE(iter.hasIndex());

  // Seek to event at ~50s
  int64_t target_ts = 50LL * 1000000000LL;
  EXPECT_TRUE(iter.seekToTimestamp(target_ts));

  // Read first event after seek
  ReplayEvent event;
  ASSERT_TRUE(iter.next(event));

  // Should be close to target (within index interval)
  EXPECT_LE(event.timestamp_ns, target_ts + 10LL * 1000000000LL);
}

TEST_F(BinaryLogTest, NoIndexWhenDisabled)
{
  // Write events without index
  WriterConfig config{
      .output_dir = _test_dir,
      .create_index = false,
  };
  BinaryLogWriter writer(config);

  for (int i = 0; i < 100; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = i * 1000000000LL;
    trade.symbol_id = 1;
    writer.writeTrade(trade);
  }
  writer.close();

  // Verify no index
  ReaderConfig reader_config{.data_dir = _test_dir};
  BinaryLogReader reader(reader_config);

  reader.forEach([](const ReplayEvent&)
                 { return false; });

  const auto& segments = reader.segments();
  ASSERT_EQ(segments.size(), 1u);
  EXPECT_FALSE(segments[0].has_index);
}

TEST_F(BinaryLogTest, IndexBuilderAddToExisting)
{
  // Write events WITHOUT index
  {
    WriterConfig config{
        .output_dir = _test_dir,
        .create_index = false,
    };
    BinaryLogWriter writer(config);

    for (int i = 0; i < 100; ++i)
    {
      TradeRecord trade{};
      trade.exchange_ts_ns = (i + 1) * 1000000000LL;
      trade.symbol_id = 1;
      trade.price_raw = (i + 1) * 1000000LL;
      writer.writeTrade(trade);
    }
    writer.close();
  }

  // Verify no index initially
  auto files = std::filesystem::directory_iterator(_test_dir);
  std::filesystem::path segment_path;
  for (const auto& entry : files)
  {
    if (entry.path().extension() == ".lrvx")
    {
      segment_path = entry.path();
      break;
    }
  }

  EXPECT_FALSE(IndexBuilder::hasIndex(segment_path));

  // Build index
  IndexBuilderConfig builder_config{
      .index_interval = 10,
      .verify_crc = true,
  };
  IndexBuilder builder(builder_config);

  auto result = builder.buildForSegment(segment_path);
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.events_scanned, 100u);
  EXPECT_GE(result.index_entries_created, 10u);  // At least 10 entries (100/10)

  // Verify index now exists
  EXPECT_TRUE(IndexBuilder::hasIndex(segment_path));

  // Read and seek using new index
  BinaryLogIterator iter(segment_path);
  ASSERT_TRUE(iter.isValid());
  EXPECT_TRUE(iter.header().hasIndex());

  EXPECT_TRUE(iter.loadIndex());
  EXPECT_TRUE(iter.hasIndex());

  // Seek to ~50s
  EXPECT_TRUE(iter.seekToTimestamp(50LL * 1000000000LL));

  ReplayEvent event;
  ASSERT_TRUE(iter.next(event));
  EXPECT_GE(event.timestamp_ns, 40LL * 1000000000LL);  // Within index interval
}

TEST_F(BinaryLogTest, IndexBuilderRemoveIndex)
{
  // Write events WITH index
  {
    WriterConfig config{
        .output_dir = _test_dir,
        .create_index = true,
        .index_interval = 10,
    };
    BinaryLogWriter writer(config);

    for (int i = 0; i < 100; ++i)
    {
      TradeRecord trade{};
      trade.exchange_ts_ns = (i + 1) * 1000000000LL;
      trade.symbol_id = 1;
      writer.writeTrade(trade);
    }
    writer.close();
  }

  auto files = std::filesystem::directory_iterator(_test_dir);
  std::filesystem::path segment_path;
  for (const auto& entry : files)
  {
    if (entry.path().extension() == ".lrvx")
    {
      segment_path = entry.path();
      break;
    }
  }

  EXPECT_TRUE(IndexBuilder::hasIndex(segment_path));

  auto size_with_index = std::filesystem::file_size(segment_path);

  // Remove index
  EXPECT_TRUE(IndexBuilder::removeIndex(segment_path));
  EXPECT_FALSE(IndexBuilder::hasIndex(segment_path));

  auto size_without_index = std::filesystem::file_size(segment_path);
  EXPECT_LT(size_without_index, size_with_index);

  // Data should still be readable
  ReaderConfig reader_config{.data_dir = _test_dir};
  BinaryLogReader reader(reader_config);

  int count = 0;
  reader.forEach([&](const ReplayEvent&)
                 {
    ++count;
    return true; });

  EXPECT_EQ(count, 100);
}

TEST_F(BinaryLogTest, IndexBuilderForDirectory)
{
  // Write multiple segments without index
  {
    WriterConfig config{
        .output_dir = _test_dir,
        .max_segment_bytes = 2048,  // Force rotation
        .create_index = false,
    };
    BinaryLogWriter writer(config);

    for (int i = 0; i < 200; ++i)
    {
      TradeRecord trade{};
      trade.exchange_ts_ns = (i + 1) * 1000000000LL;
      trade.symbol_id = 1;
      writer.writeTrade(trade);
    }
    writer.close();
  }

  // Build index for entire directory
  IndexBuilderConfig builder_config{
      .index_interval = 10,
  };
  IndexBuilder builder(builder_config);

  auto results = builder.buildForDirectory(_test_dir);
  EXPECT_GT(results.size(), 1u);  // Multiple segments

  // Verify all segments now have index
  for (const auto& result : results)
  {
    EXPECT_TRUE(result.success);
  }

  ReaderConfig reader_config{.data_dir = _test_dir};
  BinaryLogReader reader(reader_config);
  reader.forEach([](const ReplayEvent&)
                 { return false; });

  for (const auto& seg : reader.segments())
  {
    EXPECT_TRUE(seg.has_index);
  }
}

TEST_F(BinaryLogTest, GlobalIndexBuilder)
{
  // Write multiple segments with index
  {
    WriterConfig config{
        .output_dir = _test_dir,
        .max_segment_bytes = 2048,
        .create_index = true,
        .index_interval = 10,
    };
    BinaryLogWriter writer(config);

    for (int i = 0; i < 200; ++i)
    {
      TradeRecord trade{};
      trade.exchange_ts_ns = (i + 1) * 1000000000LL;
      trade.symbol_id = 1;
      writer.writeTrade(trade);
    }
    writer.close();
  }

  // Build global index
  auto result = GlobalIndexBuilder::build(_test_dir);
  EXPECT_TRUE(result.success);
  EXPECT_GT(result.segments_indexed, 1u);
  EXPECT_EQ(result.total_events, 200u);

  // Verify global index file exists
  auto index_path = _test_dir / "index.lrvxidx";
  EXPECT_TRUE(std::filesystem::exists(index_path));

  // Load and verify global index
  auto segments = GlobalIndexBuilder::load(index_path);
  ASSERT_TRUE(segments.has_value());
  EXPECT_GT(segments->size(), 1u);

  // Verify segments are sorted by timestamp
  for (size_t i = 1; i < segments->size(); ++i)
  {
    EXPECT_GE((*segments)[i].first_event_ns, (*segments)[i - 1].first_event_ns);
  }
}

TEST_F(BinaryLogTest, StaticInspect)
{
  // Write some data
  {
    WriterConfig config{.output_dir = _test_dir, .create_index = true};
    BinaryLogWriter writer(config);

    for (int i = 0; i < 500; ++i)
    {
      TradeRecord trade{};
      trade.exchange_ts_ns = 1000000000LL + static_cast<int64_t>(i) * 1000000LL;
      trade.symbol_id = (i % 3) + 1;  // 3 different symbols
      writer.writeTrade(trade);
    }
    writer.close();
  }

  // Use static inspect
  auto summary = BinaryLogReader::inspect(_test_dir);

  EXPECT_FALSE(summary.empty());
  EXPECT_EQ(summary.total_events, 500u);
  EXPECT_EQ(summary.segment_count, 1u);
  EXPECT_GT(summary.total_bytes, 0u);
  EXPECT_EQ(summary.first_event_ns, 1000000000LL);
  EXPECT_EQ(summary.last_event_ns, 1000000000LL + 499LL * 1000000LL);
  EXPECT_TRUE(summary.fullyIndexed());
  EXPECT_EQ(summary.segments_with_index, 1u);
  EXPECT_EQ(summary.segments_without_index, 0u);

  // Duration should be ~499ms
  EXPECT_GT(summary.durationSeconds(), 0.0);
  EXPECT_LT(summary.durationSeconds(), 1.0);
}

TEST_F(BinaryLogTest, StaticInspectWithSymbols)
{
  // Write data with multiple symbols
  {
    WriterConfig config{.output_dir = _test_dir};
    BinaryLogWriter writer(config);

    for (int i = 0; i < 300; ++i)
    {
      TradeRecord trade{};
      trade.exchange_ts_ns = i * 1000000LL;
      trade.symbol_id = (i % 5) + 1;  // 5 different symbols (1-5)
      writer.writeTrade(trade);
    }
    writer.close();
  }

  // Use inspectWithSymbols
  auto summary = BinaryLogReader::inspectWithSymbols(_test_dir);

  EXPECT_EQ(summary.total_events, 300u);
  EXPECT_EQ(summary.symbols.size(), 5u);

  // Verify all expected symbols are present
  for (uint32_t sym = 1; sym <= 5; ++sym)
  {
    EXPECT_NE(summary.symbols.find(sym), summary.symbols.end());
  }
}

TEST_F(BinaryLogTest, InstanceSummary)
{
  // Write data
  {
    WriterConfig config{
        .output_dir = _test_dir,
        .max_segment_bytes = 2048,  // Force multiple segments
        .create_index = true,
    };
    BinaryLogWriter writer(config);

    for (int i = 0; i < 200; ++i)
    {
      TradeRecord trade{};
      trade.exchange_ts_ns = static_cast<int64_t>(i) * 1000000000LL;
      trade.symbol_id = 1;
      writer.writeTrade(trade);
    }
    writer.close();
  }

  // Get summary via instance
  ReaderConfig config{.data_dir = _test_dir};
  BinaryLogReader reader(config);

  auto summary = reader.summary();

  EXPECT_EQ(summary.total_events, 200u);
  EXPECT_GT(summary.segment_count, 1u);  // Multiple segments
  EXPECT_TRUE(summary.fullyIndexed());
}

TEST_F(BinaryLogTest, CountMethod)
{
  // Write data
  {
    WriterConfig config{.output_dir = _test_dir};
    BinaryLogWriter writer(config);

    for (int i = 0; i < 123; ++i)
    {
      TradeRecord trade{};
      trade.exchange_ts_ns = i * 1000000LL;
      trade.symbol_id = 1;
      writer.writeTrade(trade);
    }
    writer.close();
  }

  ReaderConfig config{.data_dir = _test_dir};
  BinaryLogReader reader(config);

  EXPECT_EQ(reader.count(), 123u);
}

TEST_F(BinaryLogTest, AvailableSymbols)
{
  // Write data with multiple symbols
  {
    WriterConfig config{.output_dir = _test_dir};
    BinaryLogWriter writer(config);

    // Trades with symbols 1, 2, 3
    for (int i = 0; i < 30; ++i)
    {
      TradeRecord trade{};
      trade.exchange_ts_ns = i * 1000000LL;
      trade.symbol_id = (i % 3) + 1;
      writer.writeTrade(trade);
    }

    // Book updates with symbols 4, 5
    for (int i = 0; i < 20; ++i)
    {
      BookRecordHeader header{};
      header.exchange_ts_ns = 30000000LL + i * 1000000LL;
      header.symbol_id = (i % 2) + 4;
      header.bid_count = 1;
      header.ask_count = 1;

      std::vector<BookLevel> bids = {{50000000000, 1000000}};
      std::vector<BookLevel> asks = {{50001000000, 1000000}};
      writer.writeBook(header, bids, asks);
    }
    writer.close();
  }

  ReaderConfig config{.data_dir = _test_dir};
  BinaryLogReader reader(config);

  auto symbols = reader.availableSymbols();

  EXPECT_EQ(symbols.size(), 5u);
  for (uint32_t sym = 1; sym <= 5; ++sym)
  {
    EXPECT_NE(symbols.find(sym), symbols.end());
  }
}

TEST_F(BinaryLogTest, InspectEmptyDirectory)
{
  // Empty directory
  auto summary = BinaryLogReader::inspect(_test_dir);

  EXPECT_TRUE(summary.empty());
  EXPECT_EQ(summary.total_events, 0u);
  EXPECT_EQ(summary.segment_count, 0u);
}

TEST_F(BinaryLogTest, InspectNonExistentDirectory)
{
  auto summary = BinaryLogReader::inspect("/nonexistent/path/that/does/not/exist");

  EXPECT_TRUE(summary.empty());
}

TEST_F(BinaryLogTest, TimeRangeStruct)
{
  TimeRange range{.start_ns = 1000000000LL, .end_ns = 2000000000LL};

  EXPECT_FALSE(range.empty());
  EXPECT_EQ(range.duration().count(), 1000000000LL);
  EXPECT_DOUBLE_EQ(range.durationSeconds(), 1.0);
  EXPECT_TRUE(range.contains(1500000000LL));
  EXPECT_FALSE(range.contains(500000000LL));
  EXPECT_FALSE(range.contains(2500000000LL));
}

TEST_F(BinaryLogTest, TimeUtils)
{
  using namespace time_utils;

  // secondsToNanos
  EXPECT_EQ(secondsToNanos(1), 1'000'000'000LL);
  EXPECT_EQ(secondsToNanos(10), 10'000'000'000LL);

  // millisToNanos
  EXPECT_EQ(millisToNanos(1), 1'000'000LL);
  EXPECT_EQ(millisToNanos(1000), 1'000'000'000LL);

  // microsToNanos
  EXPECT_EQ(microsToNanos(1), 1'000LL);
  EXPECT_EQ(microsToNanos(1000), 1'000'000LL);

  // nanosToSeconds
  EXPECT_DOUBLE_EQ(nanosToSeconds(1'000'000'000LL), 1.0);
  EXPECT_DOUBLE_EQ(nanosToSeconds(500'000'000LL), 0.5);

  // nowNanos - just check it returns something reasonable
  auto now = nowNanos();
  EXPECT_GT(now, 0LL);

  // toNanos/fromNanos round trip
  auto tp = std::chrono::system_clock::now();
  auto ns = toNanos(tp);
  auto tp2 = fromNanos(ns);
  auto diff = std::chrono::duration_cast<std::chrono::nanoseconds>(tp2 - tp).count();
  EXPECT_EQ(diff, 0);  // Should round-trip exactly
}

TEST_F(BinaryLogTest, DatasetSummaryHelperMethods)
{
  DatasetSummary summary;
  summary.first_event_ns = 1000000000LL;    // 1s
  summary.last_event_ns = 3600000000000LL;  // 3600s = 1 hour

  EXPECT_DOUBLE_EQ(summary.durationSeconds(), 3599.0);
  EXPECT_NEAR(summary.durationMinutes(), 59.98, 0.01);
  EXPECT_NEAR(summary.durationHours(), 0.999, 0.001);
}

#include "lrvx/replay/ops/compression.h"

TEST_F(BinaryLogTest, CompressionAvailability)
{
  // CompressionType::None should always be available
  EXPECT_TRUE(isCompressionAvailable(CompressionType::None));

#if LRVX_LZ4_ENABLED
  EXPECT_TRUE(isCompressionAvailable(CompressionType::LZ4));
  EXPECT_TRUE(isCompressionAvailable());
#else
  EXPECT_FALSE(isCompressionAvailable(CompressionType::LZ4));
  EXPECT_FALSE(isCompressionAvailable());
#endif
}

TEST_F(BinaryLogTest, CompressorNonePassthrough)
{
  // Test that CompressionType::None just copies data
  std::vector<std::byte> input(1000);
  for (size_t i = 0; i < input.size(); ++i)
  {
    input[i] = static_cast<std::byte>(i & 0xFF);
  }

  std::vector<std::byte> output(1000);
  size_t compressed_size =
      Compressor::compress(CompressionType::None, input.data(), input.size(), output.data(), output.size());

  EXPECT_EQ(compressed_size, input.size());
  EXPECT_EQ(std::memcmp(input.data(), output.data(), input.size()), 0);

  // Decompress
  std::vector<std::byte> decompressed(1000);
  size_t decompressed_size =
      Compressor::decompress(CompressionType::None, output.data(), compressed_size, decompressed.data(), input.size());

  EXPECT_EQ(decompressed_size, input.size());
  EXPECT_EQ(std::memcmp(input.data(), decompressed.data(), input.size()), 0);
}

#if LRVX_LZ4_ENABLED
TEST_F(BinaryLogTest, CompressorLZ4RoundTrip)
{
  // Test LZ4 compression round-trip
  std::vector<std::byte> input(10000);
  // Create compressible data (repeated patterns)
  for (size_t i = 0; i < input.size(); ++i)
  {
    input[i] = static_cast<std::byte>((i % 16) & 0xFF);
  }

  size_t max_size = Compressor::maxCompressedSize(CompressionType::LZ4, input.size());
  EXPECT_GT(max_size, 0u);

  std::vector<std::byte> compressed(max_size);
  size_t compressed_size =
      Compressor::compress(CompressionType::LZ4, input.data(), input.size(), compressed.data(), max_size);

  EXPECT_GT(compressed_size, 0u);
  EXPECT_LT(compressed_size, input.size());  // Should compress well due to repeated patterns

  // Decompress
  std::vector<std::byte> decompressed(input.size());
  size_t decompressed_size =
      Compressor::decompress(CompressionType::LZ4, compressed.data(), compressed_size, decompressed.data(), input.size());

  EXPECT_EQ(decompressed_size, input.size());
  EXPECT_EQ(std::memcmp(input.data(), decompressed.data(), input.size()), 0);
}

TEST_F(BinaryLogTest, CompressedWriteAndReadTrades)
{
  // Write trades with compression enabled
  WriterConfig config{
      .output_dir = _test_dir,
      .index_interval = 100,  // Small interval for testing
      .compression = CompressionType::LZ4};
  BinaryLogWriter writer(config);

  constexpr int num_trades = 500;
  for (int i = 0; i < num_trades; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
    trade.recv_ts_ns = trade.exchange_ts_ns + 100;
    trade.price_raw = 50000LL * 1000000LL + i;
    trade.qty_raw = 1LL * 1000000LL;
    trade.trade_id = static_cast<uint64_t>(i);
    trade.symbol_id = 1;
    trade.side = (i % 2);
    EXPECT_TRUE(writer.writeTrade(trade));
  }
  writer.close();

  auto wstats = writer.stats();
  EXPECT_EQ(wstats.trades_written, num_trades);
  EXPECT_EQ(wstats.events_written, num_trades);
  EXPECT_GT(wstats.blocks_written, 0u);  // Should have written some blocks
  EXPECT_GT(wstats.uncompressed_bytes, 0u);
  EXPECT_GT(wstats.compressed_bytes, 0u);
  EXPECT_LT(wstats.compressed_bytes, wstats.uncompressed_bytes);  // Should compress

  // Read back and verify
  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  int read_count = 0;
  reader.forEach([&read_count, num_trades](const ReplayEvent& event)
                 {
    EXPECT_EQ(event.type, EventType::Trade);
    EXPECT_EQ(event.trade.exchange_ts_ns, 1000000000LL + read_count * 1000000LL);
    EXPECT_EQ(event.trade.trade_id, static_cast<uint64_t>(read_count));
    ++read_count;
    return true; });

  EXPECT_EQ(read_count, num_trades);
}

TEST_F(BinaryLogTest, CompressedWriteAndReadMixedEvents)
{
  // Write mixed events with compression
  WriterConfig config{
      .output_dir = _test_dir,
      .index_interval = 50,
      .compression = CompressionType::LZ4};
  BinaryLogWriter writer(config);

  constexpr int num_events = 200;
  for (int i = 0; i < num_events; ++i)
  {
    if (i % 3 == 0)
    {
      // Write book update
      BookRecordHeader header{};
      header.exchange_ts_ns = 1000000000LL + i * 1000000LL;
      header.recv_ts_ns = header.exchange_ts_ns + 100;
      header.seq = i;
      header.symbol_id = 1;
      header.bid_count = 2;
      header.ask_count = 2;
      header.type = 0;

      std::vector<BookLevel> bids = {{50000LL * 1000000LL, 10LL * 1000000LL},
                                     {49999LL * 1000000LL, 5LL * 1000000LL}};
      std::vector<BookLevel> asks = {{50001LL * 1000000LL, 8LL * 1000000LL},
                                     {50002LL * 1000000LL, 12LL * 1000000LL}};
      EXPECT_TRUE(writer.writeBook(header, bids, asks));
    }
    else
    {
      // Write trade
      TradeRecord trade{};
      trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
      trade.recv_ts_ns = trade.exchange_ts_ns + 100;
      trade.price_raw = 50000LL * 1000000LL;
      trade.qty_raw = 1LL * 1000000LL;
      trade.trade_id = static_cast<uint64_t>(i);
      trade.symbol_id = 1;
      trade.side = 1;
      EXPECT_TRUE(writer.writeTrade(trade));
    }
  }
  writer.close();

  // Read back and verify order and types
  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  int idx = 0;
  reader.forEach([&idx](const ReplayEvent& event)
                 {
    int64_t expected_ts = 1000000000LL + idx * 1000000LL;
    EXPECT_EQ(event.timestamp_ns, expected_ts);

    if (idx % 3 == 0)
    {
      EXPECT_EQ(event.type, EventType::BookSnapshot);
      EXPECT_EQ(event.book_header.bid_count, 2);
      EXPECT_EQ(event.book_header.ask_count, 2);
    }
    else
    {
      EXPECT_EQ(event.type, EventType::Trade);
      EXPECT_EQ(event.trade.trade_id, static_cast<uint64_t>(idx));
    }
    ++idx;
    return true; });

  EXPECT_EQ(idx, num_events);
}

TEST_F(BinaryLogTest, CompressedSegmentHeader)
{
  // Verify segment header indicates compression
  WriterConfig config{
      .output_dir = _test_dir,
      .compression = CompressionType::LZ4};
  BinaryLogWriter writer(config);

  TradeRecord trade{};
  trade.exchange_ts_ns = 1000000000;
  trade.symbol_id = 1;
  writer.writeTrade(trade);
  writer.close();

  // Read segment header directly
  auto files = std::filesystem::directory_iterator(_test_dir);
  for (const auto& entry : files)
  {
    if (entry.path().extension() == ".lrvx")
    {
      std::FILE* f = std::fopen(entry.path().string().c_str(), "rb");
      ASSERT_NE(f, nullptr);

      SegmentHeader header;
      EXPECT_EQ(std::fread(&header, sizeof(header), 1, f), 1u);
      std::fclose(f);

      EXPECT_TRUE(header.isValid());
      EXPECT_TRUE(header.isCompressed());
      EXPECT_EQ(header.compressionType(), CompressionType::LZ4);
      EXPECT_TRUE((header.flags & SegmentFlags::Compressed) != 0);
    }
  }
}

TEST_F(BinaryLogTest, CompressedWithIndex)
{
  // Test that index works correctly with compressed data
  WriterConfig config{
      .output_dir = _test_dir,
      .create_index = true,
      .index_interval = 100,
      .compression = CompressionType::LZ4};
  BinaryLogWriter writer(config);

  constexpr int num_trades = 500;
  for (int i = 0; i < num_trades; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
    trade.symbol_id = 1;
    trade.trade_id = static_cast<uint64_t>(i);
    writer.writeTrade(trade);
  }
  writer.close();

  // Use forEachFrom to test index seeking
  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  // Seek to middle
  int64_t seek_ts = 1000000000LL + 250 * 1000000LL;  // Middle of data
  int count = 0;
  reader.forEachFrom(seek_ts, [&count, seek_ts](const ReplayEvent& event)
                     {
    EXPECT_GE(event.timestamp_ns, seek_ts);
    ++count;
    return true; });

  // Should read approximately half the events (250 remaining)
  EXPECT_GT(count, 200);
  EXPECT_LE(count, 300);  // Allow some tolerance due to block boundaries
}

TEST_F(BinaryLogTest, LiveTapeTruncatedTailNoOOM)
{
  // Regression: a live-writing compressed tape can have a complete
  // CompressedBlockHeader on disk while its payload is still being
  // written. Worse, the 16-byte header region may have been seeked-into
  // before any bytes were flushed, so `compressed_size` reads as prior
  // disk garbage — e.g. 0x7FFFFFFF (~2 GB). Without bounds-checking the
  // recovery path would `std::vector<byte>(compressed_size)` and OOM.
  //
  // This test writes a well-formed no-index compressed segment, then
  // appends a fake tail block header whose `compressed_size` points far
  // past EOF, and confirms reads complete cleanly: the bogus tail is
  // treated as the live-writer tail and skipped, surfacing exactly the
  // events that were durably written.

  WriterConfig config{.output_dir = _test_dir,
                      .create_index = false,
                      .compression = CompressionType::LZ4};
  BinaryLogWriter writer(config);

  constexpr int num_trades = 200;
  for (int i = 0; i < num_trades; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
    trade.symbol_id = 1;
    trade.trade_id = static_cast<uint64_t>(i);
    writer.writeTrade(trade);
  }
  writer.close();

  std::filesystem::path tape_path;
  for (const auto& entry : std::filesystem::directory_iterator(_test_dir))
  {
    if (entry.path().extension() == ".lrvx")
    {
      tape_path = entry.path();
      break;
    }
  }
  ASSERT_FALSE(tape_path.empty());

  {
    std::FILE* f = std::fopen(tape_path.string().c_str(), "ab");
    ASSERT_NE(f, nullptr);
    CompressedBlockHeader bogus{};
    bogus.compressed_size = 0x7FFFFFFFu;
    bogus.original_size = 4096;
    bogus.event_count = 999;
    ASSERT_EQ(std::fwrite(&bogus, sizeof(bogus), 1, f), 1u);
    std::fclose(f);
  }

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  int read_count = 0;
  reader.forEach(
      [&read_count](const ReplayEvent& event)
      {
        EXPECT_EQ(event.type, EventType::Trade);
        EXPECT_EQ(event.trade.trade_id, static_cast<uint64_t>(read_count));
        ++read_count;
        return true;
      });

  EXPECT_EQ(read_count, num_trades);
}
#endif  // LRVX_LZ4_ENABLED

#include "lrvx/replay/ops/validator.h"

TEST_F(BinaryLogTest, ValidatorValidSegment)
{
  // Write a valid segment
  WriterConfig config{.output_dir = _test_dir, .index_interval = 100};
  BinaryLogWriter writer(config);

  for (int i = 0; i < 500; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
    trade.symbol_id = 1;
    trade.trade_id = static_cast<uint64_t>(i);
    writer.writeTrade(trade);
  }
  writer.close();

  // Validate it
  auto files = std::filesystem::directory_iterator(_test_dir);
  for (const auto& entry : files)
  {
    if (entry.path().extension() == ".lrvx")
    {
      SegmentValidator validator;
      auto result = validator.validate(entry.path());

      EXPECT_TRUE(result.valid);
      EXPECT_TRUE(result.header_valid);
      EXPECT_EQ(result.actual_event_count, 500u);
      EXPECT_EQ(result.trades_found, 500u);
      EXPECT_EQ(result.crc_errors, 0u);
      EXPECT_TRUE(result.has_index);
      EXPECT_TRUE(result.index_valid);
    }
  }
}

// SegmentRepairer::rebuildIndex must actually rebuild (and verify) the index,
// not fake success. Corrupt the index region so validation reports a present
// but invalid index; repair must regenerate it and report the real action.
TEST_F(BinaryLogTest, RepairerRebuildsCorruptIndex)
{
  {
    WriterConfig config{.output_dir = _test_dir, .index_interval = 50};
    BinaryLogWriter writer(config);
    for (int i = 0; i < 300; ++i)
    {
      TradeRecord trade{};
      trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
      trade.symbol_id = 1;
      trade.trade_id = static_cast<uint64_t>(i);
      writer.writeTrade(trade);
    }
    writer.close();
  }

  std::filesystem::path segment_path;
  for (const auto& entry : std::filesystem::directory_iterator(_test_dir))
  {
    if (entry.path().extension() == ".lrvx")
    {
      segment_path = entry.path();
      break;
    }
  }
  ASSERT_FALSE(segment_path.empty());

  // Read the header to find the index offset, then flip a byte inside the index.
  SegmentHeader header{};
  {
    std::FILE* f = std::fopen(segment_path.string().c_str(), "rb");
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(std::fread(&header, sizeof(header), 1, f), 1u);
    std::fclose(f);
  }
  ASSERT_TRUE(header.hasIndex());
  {
    std::FILE* f = std::fopen(segment_path.string().c_str(), "r+b");
    ASSERT_NE(f, nullptr);
    std::fseek(f, static_cast<long>(header.index_offset), SEEK_SET);
    uint8_t b = 0;
    ASSERT_EQ(std::fread(&b, 1, 1, f), 1u);
    b ^= 0xFF;
    std::fseek(f, static_cast<long>(header.index_offset), SEEK_SET);
    ASSERT_EQ(std::fwrite(&b, 1, 1, f), 1u);
    std::fclose(f);
  }

  // Now the index is present but invalid.
  SegmentValidator validator;
  auto before = validator.validate(segment_path);
  EXPECT_TRUE(before.has_index);
  EXPECT_FALSE(before.index_valid);

  // Repair: rebuild_index is on by default; it must call the real IndexBuilder.
  RepairConfig rc;
  rc.backup_before_repair = false;
  SegmentRepairer repairer(rc);
  auto rr = repairer.repair(segment_path);

  bool rebuilt = false;
  for (const auto& a : rr.actions_taken)
  {
    if (a.find("Rebuilt index") != std::string::npos)
    {
      rebuilt = true;
    }
  }
  EXPECT_TRUE(rebuilt);  // the real action was reported, not a fake "requested"

  auto after = validator.validate(segment_path);
  EXPECT_TRUE(after.has_index);
  EXPECT_TRUE(after.index_valid);  // index actually usable again
}

TEST_F(BinaryLogTest, ValidatorNonExistentFile)
{
  SegmentValidator validator;
  auto result = validator.validate(_test_dir / "nonexistent.lrvx");

  EXPECT_FALSE(result.valid);
  EXPECT_TRUE(result.hasCritical());
  EXPECT_EQ(result.issues.size(), 1u);
  EXPECT_EQ(result.issues[0].type, IssueType::FileNotFound);
}

TEST_F(BinaryLogTest, ValidatorDataset)
{
  // Write multiple segments (force rotation)
  WriterConfig config{
      .output_dir = _test_dir,
      .max_segment_bytes = 10 * 1024,  // Small segments for testing
      .index_interval = 50};
  BinaryLogWriter writer(config);

  for (int i = 0; i < 1000; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
    trade.symbol_id = 1;
    trade.trade_id = static_cast<uint64_t>(i);
    writer.writeTrade(trade);
  }
  writer.close();

  // Should have multiple segments
  auto wstats = writer.stats();
  EXPECT_GT(wstats.segments_created, 1u);

  // Validate entire dataset
  DatasetValidator validator;
  auto result = validator.validate(_test_dir);

  EXPECT_TRUE(result.valid);
  EXPECT_EQ(result.total_segments, wstats.segments_created);
  EXPECT_EQ(result.valid_segments, result.total_segments);
  EXPECT_EQ(result.corrupted_segments, 0u);
  EXPECT_EQ(result.total_events, 1000u);
}

TEST_F(BinaryLogTest, ValidatorConvenienceFunctions)
{
  // Write a valid segment
  WriterConfig config{.output_dir = _test_dir};
  BinaryLogWriter writer(config);

  TradeRecord trade{};
  trade.exchange_ts_ns = 1000000000;
  trade.symbol_id = 1;
  writer.writeTrade(trade);
  writer.close();

  // Use convenience functions
  auto files = std::filesystem::directory_iterator(_test_dir);
  for (const auto& entry : files)
  {
    if (entry.path().extension() == ".lrvx")
    {
      EXPECT_TRUE(isValidSegment(entry.path()));
    }
  }

  EXPECT_TRUE(isValidDataset(_test_dir));
  EXPECT_FALSE(isValidDataset(_test_dir / "nonexistent"));
}

TEST_F(BinaryLogTest, RepairerFixEventCount)
{
  // Write a segment
  WriterConfig config{.output_dir = _test_dir};
  BinaryLogWriter writer(config);

  for (int i = 0; i < 100; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
    trade.symbol_id = 1;
    writer.writeTrade(trade);
  }
  writer.close();

  // Corrupt the event count in header
  auto files = std::filesystem::directory_iterator(_test_dir);
  std::filesystem::path segment_path;
  for (const auto& entry : files)
  {
    if (entry.path().extension() == ".lrvx")
    {
      segment_path = entry.path();
      break;
    }
  }

  // Manually corrupt the header
  {
    std::FILE* f = std::fopen(segment_path.string().c_str(), "r+b");
    ASSERT_NE(f, nullptr);

    SegmentHeader header;
    std::fread(&header, sizeof(header), 1, f);
    header.event_count = 999;  // Wrong count
    std::fseek(f, 0, SEEK_SET);
    std::fwrite(&header, sizeof(header), 1, f);
    std::fclose(f);
  }

  // Validate - should show mismatch
  SegmentValidator validator;
  auto val_result = validator.validate(segment_path);
  EXPECT_EQ(val_result.reported_event_count, 999u);
  EXPECT_EQ(val_result.actual_event_count, 100u);

  // Repair
  RepairConfig repair_config{.backup_before_repair = true, .fix_event_count = true};
  SegmentRepairer repairer(repair_config);
  auto repair_result = repairer.repair(segment_path, val_result);

  EXPECT_TRUE(repair_result.success);
  EXPECT_TRUE(repair_result.backup_created);

  // Validate again - should be fixed
  auto val_result2 = validator.validate(segment_path);
  EXPECT_EQ(val_result2.reported_event_count, 100u);
  EXPECT_EQ(val_result2.actual_event_count, 100u);
}

#include "lrvx/replay/ops/segment_ops.h"

TEST_F(BinaryLogTest, MergeSegments)
{
  // Create two separate segments
  auto dir1 = _test_dir / "seg1";
  auto dir2 = _test_dir / "seg2";
  auto merged_dir = _test_dir / "merged";
  std::filesystem::create_directories(dir1);
  std::filesystem::create_directories(dir2);

  // Write first segment
  {
    WriterConfig config{.output_dir = dir1};
    BinaryLogWriter writer(config);
    for (int i = 0; i < 100; ++i)
    {
      TradeRecord trade{};
      trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
      trade.symbol_id = 1;
      trade.trade_id = static_cast<uint64_t>(i);
      writer.writeTrade(trade);
    }
    writer.close();
  }

  // Write second segment with later timestamps
  {
    WriterConfig config{.output_dir = dir2};
    BinaryLogWriter writer(config);
    for (int i = 0; i < 100; ++i)
    {
      TradeRecord trade{};
      trade.exchange_ts_ns = 2000000000LL + i * 1000000LL;
      trade.symbol_id = 1;
      trade.trade_id = static_cast<uint64_t>(100 + i);
      writer.writeTrade(trade);
    }
    writer.close();
  }

  // Collect paths
  std::vector<std::filesystem::path> paths;
  for (const auto& entry : std::filesystem::directory_iterator(dir1))
  {
    if (entry.path().extension() == ".lrvx")
    {
      paths.push_back(entry.path());
    }
  }
  for (const auto& entry : std::filesystem::directory_iterator(dir2))
  {
    if (entry.path().extension() == ".lrvx")
    {
      paths.push_back(entry.path());
    }
  }

  // Merge
  MergeConfig config{.output_dir = merged_dir, .sort_by_timestamp = true};
  auto result = SegmentOps::merge(paths, config);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.segments_merged, 2u);
  EXPECT_EQ(result.events_written, 200u);
  EXPECT_TRUE(std::filesystem::exists(result.output_path));

  // Read and verify order
  ReaderConfig rconfig{.data_dir = merged_dir};
  BinaryLogReader reader(rconfig);

  int64_t last_ts = 0;
  int count = 0;
  reader.forEach([&](const ReplayEvent& event)
                 {
    EXPECT_GE(event.timestamp_ns, last_ts);  // Sorted order
    last_ts = event.timestamp_ns;
    ++count;
    return true; });
  EXPECT_EQ(count, 200);
}

TEST_F(BinaryLogTest, SplitByEventCount)
{
  // Write a segment with many events
  WriterConfig wconfig{.output_dir = _test_dir};
  BinaryLogWriter writer(wconfig);
  for (int i = 0; i < 500; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
    trade.symbol_id = 1;
    trade.trade_id = static_cast<uint64_t>(i);
    writer.writeTrade(trade);
  }
  writer.close();

  // Get the segment path
  std::filesystem::path segment_path;
  for (const auto& entry : std::filesystem::directory_iterator(_test_dir))
  {
    if (entry.path().extension() == ".lrvx")
    {
      segment_path = entry.path();
      break;
    }
  }

  // Split into chunks of 100 events
  auto split_dir = _test_dir / "split";
  SplitConfig config{
      .output_dir = split_dir, .mode = SplitMode::ByEventCount, .events_per_file = 100};
  auto result = SegmentOps::split(segment_path, config);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.events_written, 500u);
  EXPECT_EQ(result.segments_created, 5u);  // 500 / 100 = 5
  EXPECT_EQ(result.output_paths.size(), 5u);
}

TEST_F(BinaryLogTest, SplitBySymbolInterleaved)
{
  // Write interleaved data - simulates real multi-symbol feed
  // sym1, sym2, sym1, sym2, ... - this is how real connectors work
  WriterConfig wconfig{.output_dir = _test_dir};
  BinaryLogWriter writer(wconfig);

  for (int i = 0; i < 200; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
    trade.symbol_id = (i % 2 == 0) ? 1 : 2;  // Alternating symbols
    trade.trade_id = static_cast<uint64_t>(i);
    writer.writeTrade(trade);
  }
  writer.close();

  // Get segment path
  std::filesystem::path segment_path;
  for (const auto& entry : std::filesystem::directory_iterator(_test_dir))
  {
    if (entry.path().extension() == ".lrvx")
    {
      segment_path = entry.path();
      break;
    }
  }

  // Split by symbol
  auto split_dir = _test_dir / "split_by_symbol";
  SplitConfig config{.output_dir = split_dir, .mode = SplitMode::BySymbol};
  auto result = SegmentOps::split(segment_path, config);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.events_written, 200u);
  EXPECT_EQ(result.segments_created, 2u);  // Must be exactly 2, not 200!
  EXPECT_EQ(result.output_paths.size(), 2u);

  // Verify each output file contains only one symbol
  for (const auto& path : result.output_paths)
  {
    auto temp_dir = _test_dir / "verify_temp";
    std::filesystem::create_directories(temp_dir);
    std::filesystem::copy_file(path, temp_dir / path.filename(),
                               std::filesystem::copy_options::overwrite_existing);

    auto summary = BinaryLogReader::inspectWithSymbols(temp_dir);
    EXPECT_EQ(summary.symbols.size(), 1u) << "File should contain only one symbol";

    std::filesystem::remove_all(temp_dir);
  }

  // Verify total event count matches
  auto verify = BinaryLogReader::inspect(split_dir);
  EXPECT_EQ(verify.total_events, 200u);
}

TEST_F(BinaryLogTest, WriterConfigOutputFilename)
{
  // Test that output_filename is respected
  std::string custom_name = "my_custom_segment.lrvx";
  WriterConfig wconfig{
      .output_dir = _test_dir,
      .output_filename = custom_name,
  };
  BinaryLogWriter writer(wconfig);

  for (int i = 0; i < 10; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
    trade.symbol_id = 1;
    writer.writeTrade(trade);
  }
  writer.close();

  // Verify file was created with exact name
  auto expected_path = _test_dir / custom_name;
  EXPECT_TRUE(std::filesystem::exists(expected_path))
      << "Expected file: " << expected_path;

  // Count files - should be exactly one
  int file_count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(_test_dir))
  {
    if (entry.path().extension() == ".lrvx")
    {
      ++file_count;
      EXPECT_EQ(entry.path().filename(), custom_name);
    }
  }
  EXPECT_EQ(file_count, 1);
}

TEST_F(BinaryLogTest, RecompressToSpecificPath)
{
  // Write uncompressed segment
  WriterConfig wconfig{.output_dir = _test_dir};
  BinaryLogWriter writer(wconfig);

  for (int i = 0; i < 100; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
    trade.symbol_id = 1;
    trade.trade_id = static_cast<uint64_t>(i);
    writer.writeTrade(trade);
  }
  writer.close();

  // Get source segment
  std::filesystem::path source_path;
  for (const auto& entry : std::filesystem::directory_iterator(_test_dir))
  {
    if (entry.path().extension() == ".lrvx")
    {
      source_path = entry.path();
      break;
    }
  }

  // Recompress to specific output path
  auto output_dir = _test_dir / "recompressed";
  std::filesystem::create_directories(output_dir);
  auto output_path = output_dir / "recompressed_output.lrvx";

  bool success = SegmentOps::recompress(source_path, output_path, CompressionType::None);
  EXPECT_TRUE(success);

  // Verify output file exists at exact path
  EXPECT_TRUE(std::filesystem::exists(output_path))
      << "Expected recompressed file at: " << output_path;

  // Verify content
  auto verify = BinaryLogReader::inspect(output_dir);
  EXPECT_EQ(verify.total_events, 100u);
}

TEST_F(BinaryLogTest, ExportToCSV)
{
  // Write a segment
  WriterConfig wconfig{.output_dir = _test_dir};
  BinaryLogWriter writer(wconfig);
  for (int i = 0; i < 50; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
    trade.price_raw = 50000LL * 1000000LL;
    trade.qty_raw = 1LL * 1000000LL;
    trade.symbol_id = 1;
    trade.side = 1;
    trade.trade_id = static_cast<uint64_t>(i);
    writer.writeTrade(trade);
  }
  writer.close();

  // Get segment path
  std::filesystem::path segment_path;
  for (const auto& entry : std::filesystem::directory_iterator(_test_dir))
  {
    if (entry.path().extension() == ".lrvx")
    {
      segment_path = entry.path();
      break;
    }
  }

  // Export to CSV
  auto csv_path = _test_dir / "export.csv";
  ExportConfig config{.output_path = csv_path, .format = ExportFormat::CSV, .include_header = true};
  auto result = SegmentOps::exportData(segment_path, config);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.events_exported, 50u);
  EXPECT_TRUE(std::filesystem::exists(csv_path));
  EXPECT_GT(result.bytes_written, 0u);

  // Verify CSV has header + 50 lines
  std::FILE* f = std::fopen(csv_path.string().c_str(), "r");
  ASSERT_NE(f, nullptr);
  int lines = 0;
  char buf[1024];
  while (std::fgets(buf, sizeof(buf), f))
  {
    ++lines;
  }
  std::fclose(f);
  EXPECT_EQ(lines, 51);  // 1 header + 50 data lines
}

TEST_F(BinaryLogTest, FilterBySymbol)
{
  // Write events for multiple symbols
  WriterConfig wconfig{.output_dir = _test_dir};
  BinaryLogWriter writer(wconfig);
  for (int i = 0; i < 100; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
    trade.symbol_id = static_cast<uint32_t>(i % 5);  // 5 different symbols
    trade.trade_id = static_cast<uint64_t>(i);
    writer.writeTrade(trade);
  }
  writer.close();

  // Get segment path
  std::filesystem::path segment_path;
  for (const auto& entry : std::filesystem::directory_iterator(_test_dir))
  {
    if (entry.path().extension() == ".lrvx")
    {
      segment_path = entry.path();
      break;
    }
  }

  // Extract only symbol 1
  auto filtered_dir = _test_dir / "filtered";
  std::filesystem::create_directories(filtered_dir);

  WriterConfig out_config{.output_dir = filtered_dir};
  uint64_t count = SegmentOps::extractSymbols(segment_path, filtered_dir / "sym1.lrvx", {1},
                                              out_config);

  EXPECT_EQ(count, 20u);  // 100 / 5 = 20 events for symbol 1

  // Verify all events are symbol 1
  ReaderConfig rconfig{.data_dir = filtered_dir};
  BinaryLogReader reader(rconfig);
  reader.forEach([](const ReplayEvent& event)
                 {
    EXPECT_EQ(event.trade.symbol_id, 1u);
    return true; });
}

#include "lrvx/replay/readers/parallel_reader.h"

TEST_F(BinaryLogTest, ParallelReaderBasic)
{
  // Create multiple segments
  WriterConfig wconfig{
      .output_dir = _test_dir,
      .max_segment_bytes = 5 * 1024,  // Small segments
  };
  BinaryLogWriter writer(wconfig);

  constexpr int num_trades = 500;
  for (int i = 0; i < num_trades; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
    trade.symbol_id = 1;
    trade.trade_id = static_cast<uint64_t>(i);
    writer.writeTrade(trade);
  }
  writer.close();

  // Should have multiple segments
  auto wstats = writer.stats();
  EXPECT_GT(wstats.segments_created, 1u);

  // Read with parallel reader
  ParallelReaderConfig config{
      .data_dir = _test_dir,
      .num_threads = 4,
      .sort_output = true,
  };
  ParallelReader preader(config);

  int64_t last_ts = 0;
  uint64_t count = preader.forEach([&last_ts](const ReplayEvent& event)
                                   {
    // Verify sorted order
    EXPECT_GE(event.timestamp_ns, last_ts);
    last_ts = event.timestamp_ns;
    return true; });

  EXPECT_EQ(count, num_trades);

  auto stats = preader.stats();
  EXPECT_EQ(stats.events_read, num_trades);
}

TEST_F(BinaryLogTest, ParallelReaderWithFilters)
{
  // Create segments with multiple symbols
  WriterConfig wconfig{
      .output_dir = _test_dir,
      .max_segment_bytes = 10 * 1024,
  };
  BinaryLogWriter writer(wconfig);

  for (int i = 0; i < 300; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
    trade.symbol_id = static_cast<uint32_t>(i % 3);  // 3 symbols
    trade.trade_id = static_cast<uint64_t>(i);
    writer.writeTrade(trade);
  }
  writer.close();

  // Read with symbol filter
  ParallelReaderConfig config{
      .data_dir = _test_dir,
      .num_threads = 2,
      .symbols = {1},  // Only symbol 1
      .sort_output = true,
  };
  ParallelReader preader(config);

  uint64_t count = preader.forEach([](const ReplayEvent& event)
                                   {
    EXPECT_EQ(event.trade.symbol_id, 1u);
    return true; });

  EXPECT_EQ(count, 100u);  // 300 / 3 = 100
}

TEST_F(BinaryLogTest, ParallelReaderTimeFilter)
{
  // Create segments
  WriterConfig wconfig{
      .output_dir = _test_dir,
      .max_segment_bytes = 5 * 1024,
  };
  BinaryLogWriter writer(wconfig);

  for (int i = 0; i < 500; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
    trade.symbol_id = 1;
    trade.trade_id = static_cast<uint64_t>(i);
    writer.writeTrade(trade);
  }
  writer.close();

  // Read with time filter - only events from i=200 to i=300
  int64_t from_ts = 1000000000LL + 200 * 1000000LL;
  int64_t to_ts = 1000000000LL + 300 * 1000000LL;

  ParallelReaderConfig config{
      .data_dir = _test_dir,
      .num_threads = 2,
      .from_ns = from_ts,
      .to_ns = to_ts,
      .sort_output = true,
  };
  ParallelReader preader(config);

  uint64_t count = preader.forEach([from_ts, to_ts](const ReplayEvent& event)
                                   {
    EXPECT_GE(event.timestamp_ns, from_ts);
    EXPECT_LE(event.timestamp_ns, to_ts);
    return true; });

  EXPECT_EQ(count, 101u);  // 200 to 300 inclusive = 101 events
}

TEST_F(BinaryLogTest, ParallelReaderBatch)
{
  // Create segments
  WriterConfig wconfig{
      .output_dir = _test_dir,
      .max_segment_bytes = 5 * 1024,
  };
  BinaryLogWriter writer(wconfig);

  for (int i = 0; i < 300; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
    trade.symbol_id = 1;
    trade.trade_id = static_cast<uint64_t>(i);
    writer.writeTrade(trade);
  }
  writer.close();

  // Read using batch interface
  ParallelReaderConfig config{
      .data_dir = _test_dir,
      .num_threads = 4,
  };
  ParallelReader preader(config);

  std::atomic<uint64_t> batch_count{0};
  uint64_t total = preader.forEachBatch([&batch_count](const std::vector<ReplayEvent>& events)
                                        {
    batch_count.fetch_add(1, std::memory_order_relaxed);
    // Each batch should have events
    EXPECT_GT(events.size(), 0u);
    return true; });

  EXPECT_EQ(total, 300u);
  EXPECT_GT(batch_count.load(), 0u);  // Should have at least one batch
}

TEST_F(BinaryLogTest, ParallelReaderSingleThread)
{
  // Create a single small segment
  WriterConfig wconfig{.output_dir = _test_dir};
  BinaryLogWriter writer(wconfig);

  for (int i = 0; i < 50; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
    trade.symbol_id = 1;
    trade.trade_id = static_cast<uint64_t>(i);
    writer.writeTrade(trade);
  }
  writer.close();

  // Force single-threaded mode
  ParallelReaderConfig config{
      .data_dir = _test_dir,
      .num_threads = 1,
  };
  ParallelReader preader(config);

  uint64_t count = preader.forEach([](const ReplayEvent&)
                                   { return true; });
  EXPECT_EQ(count, 50u);
}

#include "lrvx/replay/readers/mmap_reader.h"

TEST_F(BinaryLogTest, MmapReaderBasic)
{
  // Write uncompressed segment
  WriterConfig wconfig{
      .output_dir = _test_dir,
      .compression = CompressionType::None,
  };
  BinaryLogWriter writer(wconfig);

  constexpr int num_trades = 200;
  for (int i = 0; i < num_trades; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
    trade.symbol_id = 1;
    trade.trade_id = static_cast<uint64_t>(i);
    trade.price_raw = 50000LL * 1000000LL;
    trade.qty_raw = 1LL * 1000000LL;
    writer.writeTrade(trade);
  }
  writer.close();

  // Read with mmap reader
  MmapReader::Config config{.data_dir = _test_dir};
  MmapReader reader(config);

  EXPECT_EQ(reader.totalEvents(), num_trades);

  int64_t last_ts = 0;
  uint64_t count = reader.forEach([&last_ts](const ReplayEvent& event)
                                  {
    EXPECT_GE(event.timestamp_ns, last_ts);
    last_ts = event.timestamp_ns;
    return true; });

  EXPECT_EQ(count, num_trades);

  auto stats = reader.stats();
  EXPECT_EQ(stats.events_read, num_trades);
  EXPECT_GT(stats.bytes_mapped, 0u);
}

TEST_F(BinaryLogTest, MmapReaderRawTrades)
{
  // Write uncompressed segment
  WriterConfig wconfig{
      .output_dir = _test_dir,
      .compression = CompressionType::None,
  };
  BinaryLogWriter writer(wconfig);

  for (int i = 0; i < 100; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
    trade.symbol_id = static_cast<uint32_t>(i % 5);
    trade.trade_id = static_cast<uint64_t>(i);
    trade.price_raw = (50000LL + i) * 1000000LL;
    trade.qty_raw = 1LL * 1000000LL;
    writer.writeTrade(trade);
  }
  writer.close();

  // Read using zero-copy raw trade interface
  MmapReader::Config config{
      .data_dir = _test_dir,
      .symbols = {1},  // Only symbol 1
  };
  MmapReader reader(config);

  uint64_t count = reader.forEachRawTrade([](const TradeRecord* trade)
                                          {
    EXPECT_EQ(trade->symbol_id, 1u);
    return true; });

  EXPECT_EQ(count, 20u);  // 100 / 5 = 20
}

TEST_F(BinaryLogTest, MmapSegmentReader)
{
  // Write a segment
  WriterConfig wconfig{
      .output_dir = _test_dir,
      .compression = CompressionType::None,
  };
  BinaryLogWriter writer(wconfig);

  for (int i = 0; i < 50; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
    trade.symbol_id = 1;
    trade.trade_id = static_cast<uint64_t>(i);
    writer.writeTrade(trade);
  }
  writer.close();

  // Get segment path
  std::filesystem::path segment_path;
  for (const auto& entry : std::filesystem::directory_iterator(_test_dir))
  {
    if (entry.path().extension() == ".lrvx")
    {
      segment_path = entry.path();
      break;
    }
  }

  // Use MmapSegmentReader directly
  MmapSegmentReader seg_reader(segment_path);
  EXPECT_TRUE(seg_reader.isValid());
  EXPECT_FALSE(seg_reader.isCompressed());
  EXPECT_GT(seg_reader.totalSize(), 0u);

  int count = 0;
  ReplayEvent event;
  while (seg_reader.next(event))
  {
    EXPECT_EQ(event.type, EventType::Trade);
    ++count;
  }
  EXPECT_EQ(count, 50);

  // Test reset
  seg_reader.reset();
  EXPECT_TRUE(seg_reader.next(event));
}

TEST_F(BinaryLogTest, ISegmentReaderFactoryMmap)
{
  // Write uncompressed data
  WriterConfig wconfig{
      .output_dir = _test_dir,
      .compression = CompressionType::None,
  };
  BinaryLogWriter writer(wconfig);

  for (int i = 0; i < 25; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
    trade.symbol_id = 1;
    trade.trade_id = static_cast<uint64_t>(i);
    writer.writeTrade(trade);
  }
  writer.close();

  // Get segment path
  std::filesystem::path segment_path;
  for (const auto& entry : std::filesystem::directory_iterator(_test_dir))
  {
    if (entry.path().extension() == ".lrvx")
    {
      segment_path = entry.path();
      break;
    }
  }

  // Use factory to create reader (should prefer mmap for uncompressed)
  auto reader = createSegmentReader(segment_path, true);
  EXPECT_TRUE(reader->isValid());
  EXPECT_FALSE(reader->isCompressed());

  int count = 0;
  ReplayEvent event;
  while (reader->next(event))
  {
    EXPECT_EQ(event.type, EventType::Trade);
    ++count;
  }
  EXPECT_EQ(count, 25);
}

TEST_F(BinaryLogTest, ISegmentReaderFactoryIterator)
{
#if !LRVX_LZ4_ENABLED
  GTEST_SKIP() << "LZ4 compression not enabled";
#endif

  // Write compressed data
  WriterConfig wconfig{
      .output_dir = _test_dir,
      .compression = CompressionType::LZ4,
  };
  BinaryLogWriter writer(wconfig);

  for (int i = 0; i < 30; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
    trade.symbol_id = 2;
    trade.trade_id = static_cast<uint64_t>(i);
    writer.writeTrade(trade);
  }
  writer.close();

  // Get segment path
  std::filesystem::path segment_path;
  for (const auto& entry : std::filesystem::directory_iterator(_test_dir))
  {
    if (entry.path().extension() == ".lrvx")
    {
      segment_path = entry.path();
      break;
    }
  }

  // Use factory - should use iterator for compressed data
  auto reader = createSegmentReader(segment_path, true);
  EXPECT_TRUE(reader->isValid());
  EXPECT_TRUE(reader->isCompressed());

  int count = 0;
  ReplayEvent event;
  while (reader->next(event))
  {
    EXPECT_EQ(event.type, EventType::Trade);
    EXPECT_EQ(event.trade.symbol_id, 2u);
    ++count;
  }
  EXPECT_EQ(count, 30);
}

// reset() on the iterator adapter must rewind to the first event, so a second
// pass yields the same sequence -- not a silent no-op that continues from the
// current position (which returned nothing on the second pass).
TEST_F(BinaryLogTest, IteratorAdapterResetRewinds)
{
  {
    WriterConfig config{.output_dir = _test_dir};
    BinaryLogWriter writer(config);
    for (int i = 0; i < 20; ++i)
    {
      TradeRecord trade{};
      trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
      trade.symbol_id = 3;
      trade.trade_id = static_cast<uint64_t>(i);
      writer.writeTrade(trade);
    }
    writer.close();
  }

  std::filesystem::path segment_path;
  for (const auto& entry : std::filesystem::directory_iterator(_test_dir))
  {
    if (entry.path().extension() == ".lrvx")
    {
      segment_path = entry.path();
      break;
    }
  }

  // prefer_mmap=false forces the BinaryLogIteratorAdapter (the one whose reset
  // was a no-op).
  auto reader = createSegmentReader(segment_path, /*prefer_mmap*/ false);
  ASSERT_TRUE(reader->isValid());

  auto drain = [&]
  {
    std::vector<uint64_t> ids;
    ReplayEvent event;
    while (reader->next(event))
    {
      ids.push_back(event.trade.trade_id);
    }
    return ids;
  };

  const auto first = drain();
  EXPECT_EQ(first.size(), 20u);

  reader->reset();
  const auto second = drain();
  EXPECT_EQ(second, first);  // identical, non-empty second pass
}

TEST_F(BinaryLogTest, IMultiSegmentReaderFactory)
{
  // Write data across multiple segments
  WriterConfig wconfig{
      .output_dir = _test_dir,
      .max_segment_bytes = 1024,  // Small segments
      .compression = CompressionType::None,
  };
  BinaryLogWriter writer(wconfig);

  for (int i = 0; i < 100; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
    trade.symbol_id = static_cast<uint32_t>(i % 3);  // 3 symbols
    trade.trade_id = static_cast<uint64_t>(i);
    writer.writeTrade(trade);
  }
  writer.close();

  // Test sequential reader
  {
    ReaderFilter filter;
    auto reader = createMultiSegmentReader(_test_dir, filter, false, false);
    uint64_t count = reader->forEach([](const ReplayEvent&)
                                     { return true; });
    EXPECT_EQ(count, 100u);
  }

  // Test parallel reader
  {
    ReaderFilter filter;
    auto reader = createMultiSegmentReader(_test_dir, filter, true, false);
    uint64_t count = reader->forEach([](const ReplayEvent&)
                                     { return true; });
    EXPECT_EQ(count, 100u);
  }

  // Test mmap reader
  {
    ReaderFilter filter;
    auto reader = createMultiSegmentReader(_test_dir, filter, false, true);
    uint64_t count = reader->forEach([](const ReplayEvent&)
                                     { return true; });
    EXPECT_EQ(count, 100u);
  }
}

TEST_F(BinaryLogTest, IMultiSegmentReaderWithFilter)
{
  // Write data
  WriterConfig wconfig{
      .output_dir = _test_dir,
      .compression = CompressionType::None,
  };
  BinaryLogWriter writer(wconfig);

  for (int i = 0; i < 50; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
    trade.symbol_id = static_cast<uint32_t>(i % 5);
    trade.trade_id = static_cast<uint64_t>(i);
    writer.writeTrade(trade);
  }
  writer.close();

  // Filter by symbol
  ReaderFilter filter;
  filter.symbols = {1, 3};

  auto reader = createMultiSegmentReader(_test_dir, filter, false, false);
  uint64_t count = reader->forEach([](const ReplayEvent& event)
                                   {
    EXPECT_TRUE(event.trade.symbol_id == 1 || event.trade.symbol_id == 3);
    return true; });

  // 50 events / 5 symbols * 2 = 20 events for symbols 1 and 3
  EXPECT_EQ(count, 20u);
}

TEST_F(BinaryLogTest, ReaderFilterPasses)
{
  ReaderFilter filter;
  filter.from_ns = 1000;
  filter.to_ns = 5000;
  filter.symbols = {1, 2};

  // Timestamp in range, symbol in set
  EXPECT_TRUE(filter.passes(2000, 1));
  EXPECT_TRUE(filter.passes(3000, 2));

  // Timestamp out of range
  EXPECT_FALSE(filter.passes(500, 1));
  EXPECT_FALSE(filter.passes(6000, 1));

  // Symbol not in set
  EXPECT_FALSE(filter.passes(2000, 3));
  EXPECT_FALSE(filter.passes(2000, 99));

  // Empty filter passes all
  ReaderFilter empty_filter;
  EXPECT_TRUE(empty_filter.passes(100, 50));
}

#include "lrvx/replay/ops/manifest.h"

TEST_F(BinaryLogTest, ManifestBuildAndSave)
{
  // Write some data
  WriterConfig wconfig{
      .output_dir = _test_dir,
      .compression = CompressionType::None,
  };
  BinaryLogWriter writer(wconfig);

  for (int i = 0; i < 100; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
    trade.symbol_id = static_cast<uint32_t>(i % 3);
    trade.trade_id = static_cast<uint64_t>(i);
    writer.writeTrade(trade);
  }
  writer.close();

  // Build manifest
  auto manifest = SegmentManifest::build(_test_dir);
  EXPECT_FALSE(manifest.empty());
  EXPECT_EQ(manifest.totalEvents(), 100u);
  EXPECT_EQ(manifest.symbols().size(), 3u);  // symbols 0, 1, 2

  // Save and reload
  EXPECT_TRUE(manifest.save());
  EXPECT_TRUE(std::filesystem::exists(manifestPath(_test_dir)));

  auto loaded = SegmentManifest::load(manifestPath(_test_dir));
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->totalEvents(), 100u);
  EXPECT_EQ(loaded->symbols().size(), 3u);
}

TEST_F(BinaryLogTest, ManifestGetOrBuild)
{
  // Write data
  WriterConfig wconfig{.output_dir = _test_dir};
  BinaryLogWriter writer(wconfig);

  for (int i = 0; i < 50; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
    trade.symbol_id = 1;
    writer.writeTrade(trade);
  }
  writer.close();

  // First call builds
  auto manifest1 = getOrBuildManifest(_test_dir);
  EXPECT_EQ(manifest1.totalEvents(), 50u);

  // Second call loads from cache
  auto manifest2 = getOrBuildManifest(_test_dir);
  EXPECT_EQ(manifest2.totalEvents(), 50u);
}

TEST_F(BinaryLogTest, ManifestSegmentsInRange)
{
  // Write data with specific time range
  WriterConfig wconfig{
      .output_dir = _test_dir,
      .max_segment_bytes = 512,  // Small segments
  };
  BinaryLogWriter writer(wconfig);

  for (int i = 0; i < 100; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
    trade.symbol_id = 1;
    writer.writeTrade(trade);
  }
  writer.close();

  auto manifest = SegmentManifest::build(_test_dir);
  EXPECT_GT(manifest.segmentCount(), 1u);  // Should have multiple segments

  // Query time range in middle
  int64_t mid_start = 1000000000LL + 25 * 1000000LL;
  int64_t mid_end = 1000000000LL + 75 * 1000000LL;
  auto segs = manifest.segmentsInRange(mid_start, mid_end);

  // Should return segments overlapping this range
  EXPECT_GT(segs.size(), 0u);
  EXPECT_LE(segs.size(), manifest.segmentCount());
}

#include "lrvx/replay/ops/partitioner.h"

TEST_F(BinaryLogTest, PartitionerByTime)
{
  // Write data
  WriterConfig wconfig{.output_dir = _test_dir};
  BinaryLogWriter writer(wconfig);

  for (int i = 0; i < 100; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
    trade.symbol_id = 1;
    writer.writeTrade(trade);
  }
  writer.close();

  Partitioner partitioner(_test_dir);
  auto partitions = partitioner.partitionByTime(4, 0);

  EXPECT_EQ(partitions.size(), 4u);

  // Check partition ordering
  for (size_t i = 1; i < partitions.size(); ++i)
  {
    EXPECT_GE(partitions[i].from_ns, partitions[i - 1].to_ns);
  }

  // First partition starts at data start
  EXPECT_EQ(partitions[0].from_ns, partitioner.manifest().firstTimestamp());

  // Last partition ends at data end
  EXPECT_EQ(partitions.back().to_ns, partitioner.manifest().lastTimestamp());
}

TEST_F(BinaryLogTest, PartitionerByTimeWithWarmup)
{
  // Write data
  WriterConfig wconfig{.output_dir = _test_dir};
  BinaryLogWriter writer(wconfig);

  for (int i = 0; i < 100; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
    trade.symbol_id = 1;
    writer.writeTrade(trade);
  }
  writer.close();

  int64_t warmup_ns = 10 * 1000000LL;  // 10ms warmup
  Partitioner partitioner(_test_dir);
  auto partitions = partitioner.partitionByTime(2, warmup_ns);

  EXPECT_EQ(partitions.size(), 2u);

  // Second partition should have warmup_from_ns < from_ns
  EXPECT_LT(partitions[1].warmup_from_ns, partitions[1].from_ns);
  EXPECT_TRUE(partitions[1].hasWarmup());
}

TEST_F(BinaryLogTest, PartitionerBySymbol)
{
  // Write data with multiple symbols
  WriterConfig wconfig{.output_dir = _test_dir};
  BinaryLogWriter writer(wconfig);

  for (int i = 0; i < 100; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
    trade.symbol_id = static_cast<uint32_t>(i % 5);  // 5 symbols
    writer.writeTrade(trade);
  }
  writer.close();

  Partitioner partitioner(_test_dir);
  auto partitions = partitioner.partitionBySymbol(2);

  EXPECT_EQ(partitions.size(), 2u);

  // Each partition should have symbols
  for (const auto& p : partitions)
  {
    EXPECT_FALSE(p.symbols.empty());
    EXPECT_TRUE(p.hasSymbolFilter());
  }

  // All symbols should be covered
  std::set<uint32_t> all_symbols;
  for (const auto& p : partitions)
  {
    all_symbols.insert(p.symbols.begin(), p.symbols.end());
  }
  EXPECT_EQ(all_symbols.size(), 5u);
}

TEST_F(BinaryLogTest, PartitionSerialization)
{
  // Create a partition
  Partition p;
  p.partition_id = 42;
  p.from_ns = 1000000;
  p.to_ns = 2000000;
  p.warmup_from_ns = 900000;
  p.symbols = {1, 2, 3};
  p.estimated_events = 1000;

  SegmentInfo seg;
  seg.path = "/data/segment.lrvx";
  seg.first_event_ns = 1000000;
  seg.last_event_ns = 2000000;
  seg.event_count = 1000;
  p.segments.push_back(seg);

  // Serialize
  auto data = serializePartition(p);
  EXPECT_GT(data.size(), 0u);

  // Deserialize
  auto result = deserializePartition(data);
  ASSERT_TRUE(result.has_value());

  EXPECT_EQ(result->partition_id, 42u);
  EXPECT_EQ(result->from_ns, 1000000);
  EXPECT_EQ(result->to_ns, 2000000);
  EXPECT_EQ(result->warmup_from_ns, 900000);
  EXPECT_EQ(result->symbols.size(), 3u);
  EXPECT_EQ(result->segments.size(), 1u);
}

TEST_F(BinaryLogTest, PartitionToJson)
{
  Partition p;
  p.partition_id = 1;
  p.from_ns = 1000;
  p.to_ns = 2000;
  p.symbols = {10, 20};

  std::string json = partitionToJson(p);
  EXPECT_TRUE(json.find("\"partition_id\": 1") != std::string::npos);
  EXPECT_TRUE(json.find("\"from_ns\": 1000") != std::string::npos);
}

TEST_F(BinaryLogTest, ProgressReporting)
{
  // Write data
  WriterConfig wconfig{
      .output_dir = _test_dir,
      .compression = CompressionType::None,
  };
  BinaryLogWriter writer(wconfig);

  for (int i = 0; i < 100; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1000000000LL + i * 1000000LL;
    trade.symbol_id = 1;
    writer.writeTrade(trade);
  }
  writer.close();

  // Read with progress
  ReaderFilter filter;
  auto reader = createMultiSegmentReader(_test_dir, filter, false, false);

  std::vector<ReadProgress> progress_reports;
  uint64_t count = reader->forEachWithProgress(
      [](const ReplayEvent&)
      { return true; },
      [&progress_reports](const ReadProgress& p)
      { progress_reports.push_back(p); },
      25  // Report every 25 events
  );

  EXPECT_EQ(count, 100u);
  EXPECT_GT(progress_reports.size(), 0u);

  // Last report should show completion
  EXPECT_EQ(progress_reports.back().events_processed, 100u);
}

TEST_F(BinaryLogTest, ReadProgressHelpers)
{
  ReadProgress p;
  p.events_processed = 50;
  p.total_events = 100;
  p.segments_processed = 1;
  p.total_segments = 4;

  EXPECT_DOUBLE_EQ(p.percentComplete(), 50.0);
  EXPECT_DOUBLE_EQ(p.segmentProgress(), 25.0);

  // Edge case: empty
  ReadProgress empty;
  EXPECT_DOUBLE_EQ(empty.percentComplete(), 0.0);
  EXPECT_DOUBLE_EQ(empty.segmentProgress(), 0.0);
}

TEST_F(BinaryLogTest, TimeUtilsParseISO8601)
{
  // Basic date
  auto ns = TimeUtils::parseISO8601("2024-01-15");
  ASSERT_TRUE(ns.has_value());
  EXPECT_GT(*ns, 0);

  // Date with time
  ns = TimeUtils::parseISO8601("2024-01-15T09:30:00");
  ASSERT_TRUE(ns.has_value());

  // Date with time and space separator
  ns = TimeUtils::parseISO8601("2024-01-15 09:30:00");
  ASSERT_TRUE(ns.has_value());

  // With Z suffix
  ns = TimeUtils::parseISO8601("2024-01-15T09:30:00Z");
  ASSERT_TRUE(ns.has_value());

  // With fractional seconds
  ns = TimeUtils::parseISO8601("2024-01-15T09:30:00.123456789");
  ASSERT_TRUE(ns.has_value());
  // Verify fractional part
  EXPECT_EQ(*ns % TimeUtils::kNsPerSec, 123456789);

  // With timezone
  auto ns_utc = TimeUtils::parseISO8601("2024-01-15T12:00:00Z");
  auto ns_plus3 = TimeUtils::parseISO8601("2024-01-15T15:00:00+03:00");
  ASSERT_TRUE(ns_utc.has_value() && ns_plus3.has_value());
  EXPECT_EQ(*ns_utc, *ns_plus3);  // Should be same UTC time

  // Invalid
  EXPECT_FALSE(TimeUtils::parseISO8601("invalid").has_value());
  EXPECT_FALSE(TimeUtils::parseISO8601("").has_value());
}

TEST_F(BinaryLogTest, TimeUtilsFormatISO8601)
{
  // Known timestamp: 2024-01-15T00:00:00Z
  int64_t ns = 1705276800LL * TimeUtils::kNsPerSec;
  std::string formatted = TimeUtils::toISO8601(ns);
  EXPECT_TRUE(formatted.find("2024-01-15") != std::string::npos);
  EXPECT_TRUE(formatted.find("00:00:00") != std::string::npos);

  // With nanoseconds
  int64_t ns_frac = ns + 123456789;
  formatted = TimeUtils::toISO8601(ns_frac, true);
  EXPECT_TRUE(formatted.find(".123456789") != std::string::npos);
}

TEST_F(BinaryLogTest, TimeUtilsParseDuration)
{
  // Hours
  auto dur = TimeUtils::parseDuration("1h");
  ASSERT_TRUE(dur.has_value());
  EXPECT_EQ(*dur, TimeUtils::kNsPerHour);

  // Minutes
  dur = TimeUtils::parseDuration("30m");
  ASSERT_TRUE(dur.has_value());
  EXPECT_EQ(*dur, 30 * TimeUtils::kNsPerMin);

  // Combined
  dur = TimeUtils::parseDuration("1h30m");
  ASSERT_TRUE(dur.has_value());
  EXPECT_EQ(*dur, TimeUtils::kNsPerHour + 30 * TimeUtils::kNsPerMin);

  // Days
  dur = TimeUtils::parseDuration("2d");
  ASSERT_TRUE(dur.has_value());
  EXPECT_EQ(*dur, 2 * TimeUtils::kNsPerDay);

  // Milliseconds
  dur = TimeUtils::parseDuration("500ms");
  ASSERT_TRUE(dur.has_value());
  EXPECT_EQ(*dur, 500 * TimeUtils::kNsPerMs);

  // Complex
  dur = TimeUtils::parseDuration("1d2h30m15s");
  ASSERT_TRUE(dur.has_value());
  EXPECT_EQ(*dur, TimeUtils::kNsPerDay + 2 * TimeUtils::kNsPerHour +
                      30 * TimeUtils::kNsPerMin + 15 * TimeUtils::kNsPerSec);

  // Invalid
  EXPECT_FALSE(TimeUtils::parseDuration("").has_value());
  EXPECT_FALSE(TimeUtils::parseDuration("abc").has_value());
}

TEST_F(BinaryLogTest, TimeUtilsFormatDuration)
{
  EXPECT_EQ(TimeUtils::formatDuration(TimeUtils::kNsPerHour), "1h");
  EXPECT_EQ(TimeUtils::formatDuration(30 * TimeUtils::kNsPerMin), "30m");
  EXPECT_EQ(TimeUtils::formatDuration(TimeUtils::kNsPerDay), "1d");
  EXPECT_EQ(TimeUtils::formatDuration(500 * TimeUtils::kNsPerMs), "500ms");
  EXPECT_EQ(TimeUtils::formatDuration(0), "0ms");
}

TEST_F(BinaryLogTest, TimeUtilsLiterals)
{
  using namespace lrvx::replay::literals;

  EXPECT_EQ(1_h, TimeUtils::kNsPerHour);
  EXPECT_EQ(30_min, 30 * TimeUtils::kNsPerMin);
  EXPECT_EQ(1_s, TimeUtils::kNsPerSec);
  EXPECT_EQ(500_ms, 500 * TimeUtils::kNsPerMs);
  EXPECT_EQ(1_d, TimeUtils::kNsPerDay);
  EXPECT_EQ(1000_us, TimeUtils::kNsPerMs);
  EXPECT_EQ(1000000_ns, TimeUtils::kNsPerMs);
}

TEST_F(BinaryLogTest, TimeRangeEx)
{
  // Basic construction
  TimeRangeEx r1(1000, 2000);
  EXPECT_EQ(r1.start_ns, 1000);
  EXPECT_EQ(r1.end_ns, 2000);
  EXPECT_TRUE(r1.isValid());
  EXPECT_EQ(r1.durationNs(), 1000);

  // Factory methods
  auto r2 = TimeRangeEx::fromTime(5000);
  EXPECT_EQ(r2.start_ns, 5000);
  EXPECT_EQ(r2.end_ns, INT64_MAX);

  auto r3 = TimeRangeEx::toTime(3000);
  EXPECT_EQ(r3.start_ns, 0);
  EXPECT_EQ(r3.end_ns, 3000);

  auto r4 = TimeRangeEx::between("2024-01-15", "2024-01-16");
  EXPECT_TRUE(r4.isValid());
  EXPECT_GT(r4.durationNs(), 0);

  // Contains/overlaps
  TimeRangeEx r5(1000, 2000);
  EXPECT_TRUE(r5.contains(1500));
  EXPECT_FALSE(r5.contains(500));
  EXPECT_FALSE(r5.contains(2500));

  TimeRangeEx r6(1500, 2500);
  EXPECT_TRUE(r5.overlaps(r6));
  TimeRangeEx r7(3000, 4000);
  EXPECT_FALSE(r5.overlaps(r7));
}

// A directory holding `.floxlog` segments next to `.lrvx` ones is one
// tape: every scanner reads both, in filename (= time) order.
TEST_F(BinaryLogTest, LegacyAndCurrentSegmentExtensionsReadAsOneTape)
{
  constexpr int kTrades = 300;
  {
    WriterConfig config{
        .output_dir = _test_dir,
        .max_segment_bytes = 2048,
        .create_index = false,
    };
    BinaryLogWriter writer(config);
    for (int i = 0; i < kTrades; ++i)
    {
      TradeRecord trade{};
      trade.exchange_ts_ns = (i + 1) * 1000000LL;
      trade.symbol_id = 1;
      trade.trade_id = static_cast<uint64_t>(i);
      writer.writeTrade(trade);
    }
    writer.close();
  }

  std::vector<std::filesystem::path> written;
  for (const auto& entry : std::filesystem::directory_iterator(_test_dir))
  {
    if (entry.path().extension() == kSegmentExtension)
    {
      written.push_back(entry.path());
    }
  }
  std::sort(written.begin(), written.end());
  ASSERT_GE(written.size(), 4u);

  // The earlier half carries the legacy name.
  const size_t legacy = written.size() / 2;
  for (size_t i = 0; i < legacy; ++i)
  {
    auto renamed = written[i];
    renamed.replace_extension(kLegacySegmentExtension);
    std::filesystem::rename(written[i], renamed);
  }
  const auto segments = static_cast<uint32_t>(written.size());

  {
    ReaderConfig config{.data_dir = _test_dir};
    BinaryLogReader reader(config);
    int count = 0;
    int64_t last_ts = 0;
    reader.forEach(
        [&](const ReplayEvent& event)
        {
          EXPECT_GE(event.timestamp_ns, last_ts);
          last_ts = event.timestamp_ns;
          ++count;
          return true;
        });
    EXPECT_EQ(count, kTrades);
    EXPECT_EQ(reader.segments().size(), written.size());
  }

  auto summary = BinaryLogReader::inspect(_test_dir);
  EXPECT_EQ(summary.total_events, static_cast<uint64_t>(kTrades));
  EXPECT_EQ(summary.segment_count, segments);

  auto validation = DatasetValidator{}.validate(_test_dir);
  EXPECT_TRUE(validation.valid);
  EXPECT_EQ(validation.total_segments, segments);
  EXPECT_EQ(validation.total_events, static_cast<uint64_t>(kTrades));

  auto manifest = SegmentManifest::build(_test_dir);
  EXPECT_EQ(manifest.segmentCount(), written.size());
  EXPECT_EQ(manifest.totalEvents(), static_cast<uint64_t>(kTrades));

  auto merged = SegmentOps::mergeDirectory(_test_dir, MergeConfig{.output_dir = _test_dir / "merged"});
  EXPECT_TRUE(merged.success);
  EXPECT_EQ(merged.segments_merged, segments);
  EXPECT_EQ(merged.events_written, static_cast<uint64_t>(kTrades));
  EXPECT_EQ(merged.output_path.extension(), kSegmentExtension);

  auto built = IndexBuilder(IndexBuilderConfig{.index_interval = 10}).buildForDirectory(_test_dir);
  EXPECT_EQ(built.size(), written.size());
  for (const auto& result : built)
  {
    EXPECT_TRUE(result.success);
  }

  auto global = GlobalIndexBuilder::build(_test_dir);
  EXPECT_TRUE(global.success);
  EXPECT_EQ(global.segments_indexed, segments);
  EXPECT_EQ(global.total_events, static_cast<uint64_t>(kTrades));
}
