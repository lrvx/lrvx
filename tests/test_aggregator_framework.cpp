/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/error/lrvx_error.h"
#include "lrvx/replay/aggregator.h"
#include "lrvx/replay/aggregators/bin_count.h"
#include "lrvx/replay/aggregators/event_type_stats.h"
#include "lrvx/replay/aggregators/ohlc_bin.h"
#include "lrvx/replay/aggregators/peak.h"
#include "lrvx/replay/aggregators/quantile.h"
#include "lrvx/replay/aggregators/volume_bin.h"
#include "lrvx/replay/binary_format_v1.h"
#include "lrvx/replay/merged_tape_reader.h"
#include "lrvx/replay/readers/binary_log_reader.h"
#include "lrvx/replay/recording_metadata.h"
#include "lrvx/replay/writers/binary_log_writer.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <filesystem>

using namespace lrvx::replay;

namespace
{

// Test-only aggregator that just counts dispatched events and finalize() calls.
// Lets the framework test exercise the IAggregator contract end-to-end without
// pulling in any concrete aggregator implementation.
class CounterAggregator : public IAggregator
{
 public:
  void onEvent(const ReplayEvent& ev) override
  {
    ++_events;
    if (ev.type == EventType::Trade)
    {
      ++_trades;
    }
    else if (ev.type == EventType::BookSnapshot || ev.type == EventType::BookDelta)
    {
      ++_books;
    }
  }

  void finalize() override { ++_finalize_calls; }

  std::unique_ptr<IAggregator> cloneEmpty() const override
  {
    return std::make_unique<CounterAggregator>();
  }

  void merge(const IAggregator& other) override
  {
    const auto* o = dynamic_cast<const CounterAggregator*>(&other);
    ASSERT_NE(o, nullptr);
    _events += o->_events;
    _trades += o->_trades;
    _books += o->_books;
    // _finalize_calls is intentionally NOT merged — each instance's
    // finalize() counter is its own.
  }

  int events() const { return _events; }
  int trades() const { return _trades; }
  int books() const { return _books; }
  int finalize_calls() const { return _finalize_calls; }

 private:
  int _events{0};
  int _trades{0};
  int _books{0};
  int _finalize_calls{0};
};

}  // namespace

class AggregatorFrameworkTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    _test_dir = std::filesystem::temp_directory_path() / "lrvx_test_aggregator";
    std::filesystem::remove_all(_test_dir);
    std::filesystem::create_directories(_test_dir);
  }

  void TearDown() override { std::filesystem::remove_all(_test_dir); }

  // Write `num_trades` trades into _test_dir using LZ4 compression so the
  // run() path exercises decompression.
  void writeCompressedTrades(int num_trades)
  {
    WriterConfig config{.output_dir = _test_dir,
                        .index_interval = 100,
                        .compression = CompressionType::LZ4};
    BinaryLogWriter writer(config);

    for (int i = 0; i < num_trades; ++i)
    {
      TradeRecord trade{};
      trade.exchange_ts_ns = 1'000'000'000LL + i * 1'000'000LL;
      trade.symbol_id = 1;
      trade.trade_id = static_cast<uint64_t>(i);
      writer.writeTrade(trade);
    }
    writer.close();
  }

  std::filesystem::path _test_dir;
};

TEST_F(AggregatorFrameworkTest, RunEmptySpanIsNoOp)
{
  // Contract: run([]) does not walk the tape, does not decompress, and
  // returns true. Tested behaviourally: even on an empty data directory
  // (which would make forEach fail), run([]) still succeeds.
  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  std::array<IAggregator*, 0> none{};
  EXPECT_TRUE(reader.run(none));
}

TEST_F(AggregatorFrameworkTest, RunSingleAggregatorCountsAllEvents)
{
  constexpr int num_trades = 250;
  writeCompressedTrades(num_trades);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  CounterAggregator counter;
  std::array<IAggregator*, 1> aggregators{&counter};
  ASSERT_TRUE(reader.run(aggregators));

  EXPECT_EQ(counter.events(), num_trades);
  EXPECT_EQ(counter.trades(), num_trades);
  EXPECT_EQ(counter.books(), 0);
  EXPECT_EQ(counter.finalize_calls(), 1);
}

TEST_F(AggregatorFrameworkTest, RunMultipleAggregatorsSinglePass)
{
  // Functional single-pass guarantee: every aggregator sees every event
  // exactly once, finalize() fires exactly once per aggregator. This is
  // the contract that lets a 5-aggregator panel cost one decompression
  // scan, not five. The CI gate is functional (event-count parity);
  // wall-clock perf parity lives outside CI.
  constexpr int num_trades = 100;
  writeCompressedTrades(num_trades);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  CounterAggregator a;
  CounterAggregator b;
  CounterAggregator c;
  std::array<IAggregator*, 3> aggregators{&a, &b, &c};
  ASSERT_TRUE(reader.run(aggregators));

  EXPECT_EQ(a.events(), num_trades);
  EXPECT_EQ(b.events(), num_trades);
  EXPECT_EQ(c.events(), num_trades);
  EXPECT_EQ(a.finalize_calls(), 1);
  EXPECT_EQ(b.finalize_calls(), 1);
  EXPECT_EQ(c.finalize_calls(), 1);
}

TEST_F(AggregatorFrameworkTest, RunIgnoresNullAggregators)
{
  // The dispatch loop skips nullptrs. Useful for binding layers that
  // build the span from an optional or a map-by-name lookup that may
  // return missing entries — null-tolerance keeps the C++ side simple.
  constexpr int num_trades = 50;
  writeCompressedTrades(num_trades);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  CounterAggregator counter;
  std::array<IAggregator*, 3> aggregators{nullptr, &counter, nullptr};
  ASSERT_TRUE(reader.run(aggregators));

  EXPECT_EQ(counter.events(), num_trades);
  EXPECT_EQ(counter.finalize_calls(), 1);
}

// ───────────────────────────────────────────────────────────────────────
// EventTypeStatsAggregator
// ───────────────────────────────────────────────────────────────────────

namespace
{

// Mixed-symbol mixed-event tape fixture. Three symbols, each gets a
// distinct mix of trades / snapshots / deltas / tops so the per-symbol +
// per-event-kind counters can be verified independently.
void writeMixedTape(const std::filesystem::path& dir)
{
  WriterConfig config{.output_dir = dir,
                      .index_interval = 100,
                      .compression = CompressionType::LZ4};
  BinaryLogWriter writer(config);

  // symbol 1: 10 trades, 4 snapshots, 0 deltas
  for (int i = 0; i < 10; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1'000'000'000LL + i * 1'000LL;
    trade.symbol_id = 1;
    trade.trade_id = static_cast<uint64_t>(i);
    writer.writeTrade(trade);
  }
  for (int i = 0; i < 4; ++i)
  {
    BookRecordHeader hdr{};
    hdr.exchange_ts_ns = 2'000'000'000LL + i * 1'000LL;
    hdr.symbol_id = 1;
    hdr.type = 0;  // snapshot
    hdr.bid_count = 1;
    hdr.ask_count = 1;
    std::vector<BookLevel> bids = {{1, 1}};
    std::vector<BookLevel> asks = {{2, 1}};
    writer.writeBook(hdr, bids, asks);
  }

  // symbol 1 again: 3 tops (record byte 2)
  for (int i = 0; i < 3; ++i)
  {
    BookRecordHeader hdr{};
    hdr.exchange_ts_ns = 2'500'000'000LL + i * 1'000LL;
    hdr.symbol_id = 1;
    hdr.type = 2;  // top
    hdr.bid_count = 1;
    hdr.ask_count = 1;
    std::vector<BookLevel> bids = {{1, 1}};
    std::vector<BookLevel> asks = {{2, 1}};
    writer.writeBook(hdr, bids, asks);
  }

  // symbol 2: 5 trades, 0 snapshots, 7 deltas
  for (int i = 0; i < 5; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 3'000'000'000LL + i * 1'000LL;
    trade.symbol_id = 2;
    trade.trade_id = static_cast<uint64_t>(100 + i);
    writer.writeTrade(trade);
  }
  for (int i = 0; i < 7; ++i)
  {
    BookRecordHeader hdr{};
    hdr.exchange_ts_ns = 4'000'000'000LL + i * 1'000LL;
    hdr.symbol_id = 2;
    hdr.type = 1;  // delta
    hdr.bid_count = 1;
    hdr.ask_count = 0;
    std::vector<BookLevel> bids = {{3, 1}};
    std::vector<BookLevel> asks;
    writer.writeBook(hdr, bids, asks);
  }

  // symbol 3: 2 trades, 1 snapshot, 1 delta
  for (int i = 0; i < 2; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 5'000'000'000LL + i * 1'000LL;
    trade.symbol_id = 3;
    trade.trade_id = static_cast<uint64_t>(200 + i);
    writer.writeTrade(trade);
  }
  {
    BookRecordHeader hdr{};
    hdr.exchange_ts_ns = 6'000'000'000LL;
    hdr.symbol_id = 3;
    hdr.type = 0;
    hdr.bid_count = 1;
    hdr.ask_count = 1;
    std::vector<BookLevel> bids = {{4, 1}};
    std::vector<BookLevel> asks = {{5, 1}};
    writer.writeBook(hdr, bids, asks);
  }
  {
    BookRecordHeader hdr{};
    hdr.exchange_ts_ns = 6'001'000'000LL;
    hdr.symbol_id = 3;
    hdr.type = 1;
    hdr.bid_count = 0;
    hdr.ask_count = 1;
    std::vector<BookLevel> bids;
    std::vector<BookLevel> asks = {{6, 1}};
    writer.writeBook(hdr, bids, asks);
  }

  writer.close();
}

const EventTypeStatsAggregator::PerSymbolRow* findRow(
    const std::vector<EventTypeStatsAggregator::PerSymbolRow>& rows, uint32_t sid)
{
  auto it = std::find_if(rows.begin(), rows.end(),
                         [sid](const auto& r)
                         { return r.symbol_id == sid; });
  return it == rows.end() ? nullptr : &*it;
}

}  // namespace

TEST_F(AggregatorFrameworkTest, EventTypeStatsCountsPerSymbol)
{
  writeMixedTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  EventTypeStatsAggregator stats;
  std::array<IAggregator*, 1> aggregators{&stats};
  ASSERT_TRUE(reader.run(aggregators));

  const auto& rows = stats.result();
  ASSERT_EQ(rows.size(), 3u);
  // Sorted by symbol_id.
  EXPECT_EQ(rows[0].symbol_id, 1u);
  EXPECT_EQ(rows[1].symbol_id, 2u);
  EXPECT_EQ(rows[2].symbol_id, 3u);

  const auto* s1 = findRow(rows, 1);
  ASSERT_NE(s1, nullptr);
  EXPECT_EQ(s1->trades, 10u);
  EXPECT_EQ(s1->book_snapshots, 4u);
  EXPECT_EQ(s1->book_deltas, 0u);
  EXPECT_EQ(s1->book_tops, 3u);

  const auto* s2 = findRow(rows, 2);
  ASSERT_NE(s2, nullptr);
  EXPECT_EQ(s2->trades, 5u);
  EXPECT_EQ(s2->book_snapshots, 0u);
  EXPECT_EQ(s2->book_deltas, 7u);
  EXPECT_EQ(s2->book_tops, 0u);

  const auto* s3 = findRow(rows, 3);
  ASSERT_NE(s3, nullptr);
  EXPECT_EQ(s3->trades, 2u);
  EXPECT_EQ(s3->book_snapshots, 1u);
  EXPECT_EQ(s3->book_deltas, 1u);
}

TEST_F(AggregatorFrameworkTest, EventTypeStatsEventFilterTradesOnly)
{
  writeMixedTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  EventTypeStatsAggregator stats(AggregatorEventFilter::Trades);
  std::array<IAggregator*, 1> aggregators{&stats};
  ASSERT_TRUE(reader.run(aggregators));

  const auto& rows = stats.result();
  ASSERT_EQ(rows.size(), 3u);
  for (const auto& r : rows)
  {
    EXPECT_EQ(r.book_snapshots, 0u);
    EXPECT_EQ(r.book_deltas, 0u);
    EXPECT_EQ(r.book_tops, 0u);
  }
  EXPECT_EQ(findRow(rows, 1)->trades, 10u);
  EXPECT_EQ(findRow(rows, 2)->trades, 5u);
  EXPECT_EQ(findRow(rows, 3)->trades, 2u);
}

TEST_F(AggregatorFrameworkTest, EventTypeStatsEventFilterBooksOnly)
{
  writeMixedTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  EventTypeStatsAggregator stats(AggregatorEventFilter::BooksOnly);
  std::array<IAggregator*, 1> aggregators{&stats};
  ASSERT_TRUE(reader.run(aggregators));

  const auto& rows = stats.result();
  // Only symbols with book events appear. Symbol 1: 4 snapshots + 3
  // tops, symbol 2: 7 deltas, symbol 3: 1 + 1. All three have books, so
  // 3 rows.
  ASSERT_EQ(rows.size(), 3u);
  for (const auto& r : rows)
  {
    EXPECT_EQ(r.trades, 0u);
  }
  EXPECT_EQ(findRow(rows, 1)->book_snapshots, 4u);
  EXPECT_EQ(findRow(rows, 1)->book_tops, 3u);
  EXPECT_EQ(findRow(rows, 2)->book_deltas, 7u);
  EXPECT_EQ(findRow(rows, 3)->book_snapshots, 1u);
  EXPECT_EQ(findRow(rows, 3)->book_deltas, 1u);
}

TEST_F(AggregatorFrameworkTest, EventTypeStatsSymbolFilter)
{
  writeMixedTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  // Filter to symbols 1 and 3 only. Symbol 2 should not appear in the
  // result vector at all (its counters stay at zero and the row is
  // never inserted).
  EventTypeStatsAggregator stats(AggregatorEventFilter::Both, {1, 3});
  std::array<IAggregator*, 1> aggregators{&stats};
  ASSERT_TRUE(reader.run(aggregators));

  const auto& rows = stats.result();
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].symbol_id, 1u);
  EXPECT_EQ(rows[1].symbol_id, 3u);
  EXPECT_EQ(findRow(rows, 2), nullptr);
}

TEST_F(AggregatorFrameworkTest, EventTypeStatsResultEmptyBeforeRun)
{
  // The contract: result() returns an empty vector until run() has
  // finalised. Skipping run() (or never starting it) yields no rows
  // even if onEvent was somehow invoked manually — finalize() is the
  // step that publishes the result.
  EventTypeStatsAggregator stats;
  EXPECT_TRUE(stats.result().empty());
}

// ───────────────────────────────────────────────────────────────────────
// BinCountAggregator
// ───────────────────────────────────────────────────────────────────────

namespace
{

// Tape with predictable per-bucket counts. 10 buckets at 100 ms each,
// symbol 1 gets 3 BUY + 2 SELL per bucket = 5 trades, symbol 2 gets 1
// trade per bucket (all BUY). All trades.
void writeBucketedTape(const std::filesystem::path& dir)
{
  WriterConfig config{.output_dir = dir,
                      .index_interval = 100,
                      .compression = CompressionType::LZ4};
  BinaryLogWriter writer(config);

  constexpr int64_t bucket_ns = 100'000'000LL;  // 100 ms

  uint64_t tid = 0;
  for (int b = 0; b < 10; ++b)
  {
    const int64_t base = b * bucket_ns + 1;  // offset into bucket
    // symbol 1: 3 BUY, 2 SELL
    for (int i = 0; i < 5; ++i)
    {
      TradeRecord t{};
      t.exchange_ts_ns = base + i;
      t.symbol_id = 1;
      t.trade_id = tid++;
      t.side = (i < 3) ? 0 : 1;  // 0=BUY, 1=SELL
      writer.writeTrade(t);
    }
    // symbol 2: 1 BUY
    TradeRecord t{};
    t.exchange_ts_ns = base + 50;
    t.symbol_id = 2;
    t.trade_id = tid++;
    t.side = 0;
    writer.writeTrade(t);
  }

  writer.close();
}

const BinCountAggregator::Row* findBin(const std::vector<BinCountAggregator::Row>& rows,
                                       int64_t bucket, uint32_t sym, uint8_t side)
{
  auto it = std::find_if(rows.begin(), rows.end(),
                         [=](const auto& r)
                         {
                           return r.bucket_ts_ns == bucket && r.symbol_id == sym &&
                                  r.side == side;
                         });
  return it == rows.end() ? nullptr : &*it;
}

}  // namespace

TEST_F(AggregatorFrameworkTest, BinCountFlatNoSplit)
{
  // 100 ms buckets, no side/symbol split. Each bucket sees 6 trades
  // (5 from symbol 1 + 1 from symbol 2). 10 buckets total.
  writeBucketedTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  BinCountAggregator bins(100'000'000LL);
  std::array<IAggregator*, 1> aggregators{&bins};
  ASSERT_TRUE(reader.run(aggregators));

  const auto& rows = bins.result();
  ASSERT_EQ(rows.size(), 10u);
  for (size_t i = 0; i < 10; ++i)
  {
    EXPECT_EQ(rows[i].bucket_ts_ns, static_cast<int64_t>(i) * 100'000'000LL);
    EXPECT_EQ(rows[i].symbol_id, 0u);
    EXPECT_EQ(rows[i].side, 0u);
    EXPECT_EQ(rows[i].count, 6u);
  }
}

TEST_F(AggregatorFrameworkTest, BinCountBySide)
{
  // Same tape, by_side=true. Each bucket: BUY = 3 (sym1) + 1 (sym2) =
  // 4, SELL = 2 (sym1). Rows are (bucket, sym=0, side=1) and
  // (bucket, sym=0, side=2).
  writeBucketedTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  BinCountAggregator bins(100'000'000LL, /*by_side=*/true);
  std::array<IAggregator*, 1> aggregators{&bins};
  ASSERT_TRUE(reader.run(aggregators));

  const auto& rows = bins.result();
  ASSERT_EQ(rows.size(), 20u);  // 10 buckets × 2 sides

  for (int b = 0; b < 10; ++b)
  {
    const int64_t bucket = b * 100'000'000LL;
    const auto* buys = findBin(rows, bucket, 0, 1);
    const auto* sells = findBin(rows, bucket, 0, 2);
    ASSERT_NE(buys, nullptr);
    ASSERT_NE(sells, nullptr);
    EXPECT_EQ(buys->count, 4u);
    EXPECT_EQ(sells->count, 2u);
  }
}

TEST_F(AggregatorFrameworkTest, BinCountBySymbol)
{
  writeBucketedTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  BinCountAggregator bins(100'000'000LL, /*by_side=*/false, /*by_symbol=*/true);
  std::array<IAggregator*, 1> aggregators{&bins};
  ASSERT_TRUE(reader.run(aggregators));

  const auto& rows = bins.result();
  ASSERT_EQ(rows.size(), 20u);  // 10 buckets × 2 symbols

  for (int b = 0; b < 10; ++b)
  {
    const int64_t bucket = b * 100'000'000LL;
    const auto* s1 = findBin(rows, bucket, 1, 0);
    const auto* s2 = findBin(rows, bucket, 2, 0);
    ASSERT_NE(s1, nullptr);
    ASSERT_NE(s2, nullptr);
    EXPECT_EQ(s1->count, 5u);
    EXPECT_EQ(s2->count, 1u);
  }
}

TEST_F(AggregatorFrameworkTest, BinCountBySideAndBySymbol)
{
  writeBucketedTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  BinCountAggregator bins(100'000'000LL, /*by_side=*/true, /*by_symbol=*/true);
  std::array<IAggregator*, 1> aggregators{&bins};
  ASSERT_TRUE(reader.run(aggregators));

  const auto& rows = bins.result();
  // Symbol 1 has BUY and SELL → 2 rows/bucket. Symbol 2 has only BUY → 1.
  ASSERT_EQ(rows.size(), 30u);  // 10 buckets × 3 (sym1 buy + sym1 sell + sym2 buy)

  for (int b = 0; b < 10; ++b)
  {
    const int64_t bucket = b * 100'000'000LL;
    EXPECT_EQ(findBin(rows, bucket, 1, 1)->count, 3u);  // sym1 BUY
    EXPECT_EQ(findBin(rows, bucket, 1, 2)->count, 2u);  // sym1 SELL
    EXPECT_EQ(findBin(rows, bucket, 2, 1)->count, 1u);  // sym2 BUY
    EXPECT_EQ(findBin(rows, bucket, 2, 2), nullptr);    // sym2 has no SELL
  }
}

TEST_F(AggregatorFrameworkTest, BinCountSymbolFilter)
{
  writeBucketedTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  // Filter to symbol 2 only — should drop symbol 1 trades entirely.
  BinCountAggregator bins(100'000'000LL, /*by_side=*/false, /*by_symbol=*/false,
                          AggregatorEventFilter::Both, {2});
  std::array<IAggregator*, 1> aggregators{&bins};
  ASSERT_TRUE(reader.run(aggregators));

  const auto& rows = bins.result();
  ASSERT_EQ(rows.size(), 10u);
  for (const auto& r : rows)
  {
    EXPECT_EQ(r.count, 1u);  // only sym2's single trade per bucket
  }
}

TEST_F(AggregatorFrameworkTest, BinCountRejectsNonPositiveBucket)
{
  EXPECT_THROW(BinCountAggregator(0), std::invalid_argument);
  EXPECT_THROW(BinCountAggregator(-1'000'000LL), std::invalid_argument);
}

// ───────────────────────────────────────────────────────────────────────
// VolumeBinAggregator
// ───────────────────────────────────────────────────────────────────────

namespace
{

// Tape designed for volume math. 5 buckets at 100 ms each. In every
// bucket symbol 1 gets 3 BUY trades at qty 10 each + 2 SELL trades at
// qty 20 each → total qty = 70 (30 BUY + 40 SELL). Symbol 2 gets one
// BUY trade at qty 5 → total 5. Numbers are picked so every per-cell
// expected value is distinct and the sums catch off-by-side errors.
void writeVolumeTape(const std::filesystem::path& dir)
{
  WriterConfig config{.output_dir = dir,
                      .index_interval = 100,
                      .compression = CompressionType::LZ4};
  BinaryLogWriter writer(config);

  constexpr int64_t bucket_ns = 100'000'000LL;
  uint64_t tid = 0;
  for (int b = 0; b < 5; ++b)
  {
    const int64_t base = b * bucket_ns + 1;
    for (int i = 0; i < 3; ++i)
    {
      TradeRecord t{};
      t.exchange_ts_ns = base + i;
      t.symbol_id = 1;
      t.trade_id = tid++;
      t.side = 0;  // BUY
      t.qty_raw = 10;
      writer.writeTrade(t);
    }
    for (int i = 0; i < 2; ++i)
    {
      TradeRecord t{};
      t.exchange_ts_ns = base + 10 + i;
      t.symbol_id = 1;
      t.trade_id = tid++;
      t.side = 1;  // SELL
      t.qty_raw = 20;
      writer.writeTrade(t);
    }
    TradeRecord t{};
    t.exchange_ts_ns = base + 50;
    t.symbol_id = 2;
    t.trade_id = tid++;
    t.side = 0;
    t.qty_raw = 5;
    writer.writeTrade(t);
  }

  writer.close();
}

const VolumeBinAggregator::Row* findVol(const std::vector<VolumeBinAggregator::Row>& rows,
                                        int64_t bucket, uint32_t sym, uint8_t side)
{
  auto it = std::find_if(rows.begin(), rows.end(),
                         [=](const auto& r)
                         {
                           return r.bucket_ts_ns == bucket && r.symbol_id == sym &&
                                  r.side == side;
                         });
  return it == rows.end() ? nullptr : &*it;
}

}  // namespace

TEST_F(AggregatorFrameworkTest, VolumeBinFlatNoSplit)
{
  // Each bucket: 3×10 + 2×20 (sym1) + 5 (sym2) = 30 + 40 + 5 = 75.
  writeVolumeTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  VolumeBinAggregator vol(100'000'000LL);
  std::array<IAggregator*, 1> aggregators{&vol};
  ASSERT_TRUE(reader.run(aggregators));

  const auto& rows = vol.result();
  ASSERT_EQ(rows.size(), 5u);
  for (size_t i = 0; i < 5; ++i)
  {
    EXPECT_EQ(rows[i].bucket_ts_ns, static_cast<int64_t>(i) * 100'000'000LL);
    EXPECT_EQ(rows[i].symbol_id, 0u);
    EXPECT_EQ(rows[i].side, 0u);
    EXPECT_EQ(rows[i].qty_raw, 75);
  }
}

TEST_F(AggregatorFrameworkTest, VolumeBinBySide)
{
  // BUY qty per bucket: 3×10 (sym1) + 5 (sym2) = 35.
  // SELL qty per bucket: 2×20 (sym1) = 40.
  writeVolumeTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  VolumeBinAggregator vol(100'000'000LL, /*by_side=*/true);
  std::array<IAggregator*, 1> aggregators{&vol};
  ASSERT_TRUE(reader.run(aggregators));

  const auto& rows = vol.result();
  ASSERT_EQ(rows.size(), 10u);  // 5 buckets × 2 sides
  for (int b = 0; b < 5; ++b)
  {
    const int64_t bucket = b * 100'000'000LL;
    const auto* buys = findVol(rows, bucket, 0, 1);
    const auto* sells = findVol(rows, bucket, 0, 2);
    ASSERT_NE(buys, nullptr);
    ASSERT_NE(sells, nullptr);
    EXPECT_EQ(buys->qty_raw, 35);
    EXPECT_EQ(sells->qty_raw, 40);
  }
}

TEST_F(AggregatorFrameworkTest, VolumeBinBySymbolBySide)
{
  writeVolumeTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  VolumeBinAggregator vol(100'000'000LL, /*by_side=*/true, /*by_symbol=*/true);
  std::array<IAggregator*, 1> aggregators{&vol};
  ASSERT_TRUE(reader.run(aggregators));

  const auto& rows = vol.result();
  // sym1: BUY + SELL = 2 rows; sym2: BUY only = 1 row; per bucket → 3.
  ASSERT_EQ(rows.size(), 15u);
  for (int b = 0; b < 5; ++b)
  {
    const int64_t bucket = b * 100'000'000LL;
    EXPECT_EQ(findVol(rows, bucket, 1, 1)->qty_raw, 30);  // sym1 BUY 3×10
    EXPECT_EQ(findVol(rows, bucket, 1, 2)->qty_raw, 40);  // sym1 SELL 2×20
    EXPECT_EQ(findVol(rows, bucket, 2, 1)->qty_raw, 5);   // sym2 BUY 1×5
    EXPECT_EQ(findVol(rows, bucket, 2, 2), nullptr);
  }
}

TEST_F(AggregatorFrameworkTest, VolumeBinBooksOnlyIsEmpty)
{
  // Books carry no scalar qty for this aggregator; passing BooksOnly
  // produces no rows. Confirms the filter is honoured even though it
  // looks redundant given the trade-only nature of the aggregator.
  writeVolumeTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  VolumeBinAggregator vol(100'000'000LL, false, false,
                          AggregatorEventFilter::BooksOnly);
  std::array<IAggregator*, 1> aggregators{&vol};
  ASSERT_TRUE(reader.run(aggregators));

  EXPECT_TRUE(vol.result().empty());
}

TEST_F(AggregatorFrameworkTest, VolumeBinRejectsNonPositiveBucket)
{
  EXPECT_THROW(VolumeBinAggregator(0), std::invalid_argument);
  EXPECT_THROW(VolumeBinAggregator(-100), std::invalid_argument);
}

// ───────────────────────────────────────────────────────────────────────
// OHLCBinAggregator
// ───────────────────────────────────────────────────────────────────────

namespace
{

// Tape designed for OHLC math. 5 buckets at 100 ms each. Within each
// bucket b, symbol 1 gets four trades at prices
//   open  = 100 + 10·b
//   high  = open + 5
//   low   = open - 3
//   close = open + 2
// emitted in (open → high → low → close) order so that the timestamps
// inside the bucket are strictly ascending and the reorder buffer is a
// no-op. Symbol 2 gets a single trade at price 200 + b at ts mid-
// bucket. Prices are picked so every OHLC field is distinct from every
// neighbouring field — off-by-one mistakes (high↔close, open↔low)
// would show up as mismatched expectations.
void writeOhlcTape(const std::filesystem::path& dir)
{
  WriterConfig config{.output_dir = dir,
                      .index_interval = 100,
                      .compression = CompressionType::LZ4};
  BinaryLogWriter writer(config);

  constexpr int64_t bucket_ns = 100'000'000LL;
  uint64_t tid = 0;
  for (int b = 0; b < 5; ++b)
  {
    const int64_t base = b * bucket_ns;
    const int64_t open_price = 100 + 10 * b;
    const int64_t high_price = open_price + 5;
    const int64_t low_price = open_price - 3;
    const int64_t close_price = open_price + 2;
    const int64_t prices[4] = {open_price, high_price, low_price, close_price};
    for (int i = 0; i < 4; ++i)
    {
      TradeRecord t{};
      t.exchange_ts_ns = base + i + 1;
      t.symbol_id = 1;
      t.trade_id = tid++;
      t.side = (i % 2 == 0) ? 0 : 1;
      t.qty_raw = 1;
      t.price_raw = prices[i];
      writer.writeTrade(t);
    }
    TradeRecord t{};
    t.exchange_ts_ns = base + 50;
    t.symbol_id = 2;
    t.trade_id = tid++;
    t.side = 0;
    t.qty_raw = 1;
    t.price_raw = 200 + b;
    writer.writeTrade(t);
  }

  writer.close();
}

const OHLCBinAggregator::Row* findOhlc(const std::vector<OHLCBinAggregator::Row>& rows,
                                       int64_t bucket, uint32_t sym)
{
  auto it = std::find_if(rows.begin(), rows.end(),
                         [=](const auto& r)
                         { return r.bucket_ts_ns == bucket && r.symbol_id == sym; });
  return it == rows.end() ? nullptr : &*it;
}

}  // namespace

TEST_F(AggregatorFrameworkTest, OhlcBinFlatAcrossSymbols)
{
  // by_symbol=false: per bucket every trade folds into one cell, so
  // the aggregate OHLC covers symbol 1's quad AND symbol 2's single
  // print. Per construction:
  //   open  = 100 + 10·b       (sym1's first trade is earliest)
  //   high  = max(100+10b+5, 200+b) = 200 + b  (sym2 dominates highs)
  //   low   = (100+10b) - 3
  //   close = 200 + b          (sym2 prints last at ts = base+50)
  writeOhlcTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  OHLCBinAggregator ohlc(100'000'000LL);
  std::array<IAggregator*, 1> aggregators{&ohlc};
  ASSERT_TRUE(reader.run(aggregators));

  const auto& rows = ohlc.result();
  ASSERT_EQ(rows.size(), 5u);
  for (int b = 0; b < 5; ++b)
  {
    const int64_t open_price = 100 + 10 * b;
    EXPECT_EQ(rows[b].bucket_ts_ns, static_cast<int64_t>(b) * 100'000'000LL);
    EXPECT_EQ(rows[b].symbol_id, 0u);
    EXPECT_EQ(rows[b].open_raw, open_price);
    EXPECT_EQ(rows[b].high_raw, 200 + b);
    EXPECT_EQ(rows[b].low_raw, open_price - 3);
    EXPECT_EQ(rows[b].close_raw, 200 + b);
  }
}

TEST_F(AggregatorFrameworkTest, OhlcBinBySymbol)
{
  // by_symbol=true: separate cells for sym1 (the quad) and sym2 (one
  // print). Sym1's OHLC follows the constructed prices exactly; sym2
  // is degenerate (O=H=L=C = 200+b).
  writeOhlcTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  OHLCBinAggregator ohlc(100'000'000LL, /*by_symbol=*/true);
  std::array<IAggregator*, 1> aggregators{&ohlc};
  ASSERT_TRUE(reader.run(aggregators));

  const auto& rows = ohlc.result();
  ASSERT_EQ(rows.size(), 10u);  // 5 buckets × 2 symbols
  for (int b = 0; b < 5; ++b)
  {
    const int64_t bucket = b * 100'000'000LL;
    const int64_t open_price = 100 + 10 * b;
    const auto* s1 = findOhlc(rows, bucket, 1);
    const auto* s2 = findOhlc(rows, bucket, 2);
    ASSERT_NE(s1, nullptr);
    ASSERT_NE(s2, nullptr);
    EXPECT_EQ(s1->open_raw, open_price);
    EXPECT_EQ(s1->high_raw, open_price + 5);
    EXPECT_EQ(s1->low_raw, open_price - 3);
    EXPECT_EQ(s1->close_raw, open_price + 2);
    EXPECT_EQ(s2->open_raw, 200 + b);
    EXPECT_EQ(s2->high_raw, 200 + b);
    EXPECT_EQ(s2->low_raw, 200 + b);
    EXPECT_EQ(s2->close_raw, 200 + b);
  }
}

TEST_F(AggregatorFrameworkTest, OhlcBinBooksOnlyIsEmpty)
{
  writeOhlcTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  OHLCBinAggregator ohlc(100'000'000LL, false, AggregatorEventFilter::BooksOnly);
  std::array<IAggregator*, 1> aggregators{&ohlc};
  ASSERT_TRUE(reader.run(aggregators));

  EXPECT_TRUE(ohlc.result().empty());
}

TEST_F(AggregatorFrameworkTest, OhlcBinSymbolFilter)
{
  writeOhlcTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  OHLCBinAggregator ohlc(100'000'000LL, /*by_symbol=*/true,
                         AggregatorEventFilter::Trades, /*symbol_filter=*/{2});
  std::array<IAggregator*, 1> aggregators{&ohlc};
  ASSERT_TRUE(reader.run(aggregators));

  const auto& rows = ohlc.result();
  ASSERT_EQ(rows.size(), 5u);
  for (int b = 0; b < 5; ++b)
  {
    EXPECT_EQ(rows[b].symbol_id, 2u);
    EXPECT_EQ(rows[b].open_raw, 200 + b);
    EXPECT_EQ(rows[b].close_raw, 200 + b);
  }
}

TEST_F(AggregatorFrameworkTest, OhlcBinRejectsNonPositiveBucket)
{
  EXPECT_THROW(OHLCBinAggregator(0), std::invalid_argument);
  EXPECT_THROW(OHLCBinAggregator(-100), std::invalid_argument);
}

TEST_F(AggregatorFrameworkTest, OhlcBinMergePreservesOpenClose)
{
  // merge() is exercised by the parallel-run path; here we test it
  // directly to lock in the open-by-earliest-ts / close-by-latest-ts
  // invariant. Two clones receive disjoint, time-ordered halves of a
  // single bucket; the merge of the second into the first must yield
  // the same OHLC as a single aggregator over both halves.
  OHLCBinAggregator agg_a(1'000'000'000LL);
  OHLCBinAggregator agg_b(1'000'000'000LL);
  OHLCBinAggregator ref(1'000'000'000LL);

  auto trade = [](int64_t ts, int64_t price)
  {
    ReplayEvent ev{};
    ev.type = EventType::Trade;
    ev.timestamp_ns = ts;
    ev.trade.symbol_id = 1;
    ev.trade.exchange_ts_ns = ts;
    ev.trade.price_raw = price;
    ev.trade.qty_raw = 1;
    return ev;
  };

  // First half (earlier ts): prices that set the open and the low.
  agg_a.onEvent(trade(1'000, 100));
  agg_a.onEvent(trade(2'000, 95));
  ref.onEvent(trade(1'000, 100));
  ref.onEvent(trade(2'000, 95));

  // Second half (later ts): prices that set the high and the close.
  agg_b.onEvent(trade(3'000, 120));
  agg_b.onEvent(trade(4'000, 110));
  ref.onEvent(trade(3'000, 120));
  ref.onEvent(trade(4'000, 110));

  agg_a.merge(agg_b);
  agg_a.finalize();
  ref.finalize();

  const auto& rows = agg_a.result();
  const auto& ref_rows = ref.result();
  ASSERT_EQ(rows.size(), 1u);
  ASSERT_EQ(ref_rows.size(), 1u);
  EXPECT_EQ(rows[0].open_raw, ref_rows[0].open_raw);    // 100
  EXPECT_EQ(rows[0].high_raw, ref_rows[0].high_raw);    // 120
  EXPECT_EQ(rows[0].low_raw, ref_rows[0].low_raw);      // 95
  EXPECT_EQ(rows[0].close_raw, ref_rows[0].close_raw);  // 110
  EXPECT_EQ(rows[0].open_raw, 100);
  EXPECT_EQ(rows[0].high_raw, 120);
  EXPECT_EQ(rows[0].low_raw, 95);
  EXPECT_EQ(rows[0].close_raw, 110);
}

// ───────────────────────────────────────────────────────────────────────
// PeakAggregator
// ───────────────────────────────────────────────────────────────────────

namespace
{

// Tape with two distinct, well-separated bursts so the dedup logic
// can be checked: 50 trades within 100 µs at the 10s mark (the
// dominant burst at small scales), then 30 trades within 100 µs at
// the 60s mark (second-place at small scales), separated by ≈ 50 s
// of sparse activity (one trade per second between the bursts).
void writePeakTape(const std::filesystem::path& dir)
{
  WriterConfig config{.output_dir = dir,
                      .index_interval = 1000,
                      .compression = CompressionType::LZ4};
  BinaryLogWriter writer(config);

  uint64_t tid = 0;
  auto pushTrade = [&](int64_t ts)
  {
    TradeRecord t{};
    t.exchange_ts_ns = ts;
    t.symbol_id = 1;
    t.trade_id = tid++;
    t.side = 0;
    t.qty_raw = 1;
    writer.writeTrade(t);
  };

  // Burst 1 at 10 s: 50 trades within 100 µs.
  const int64_t b1 = 10'000'000'000LL;
  for (int i = 0; i < 50; ++i)
  {
    pushTrade(b1 + i * 1'000LL);  // 1 µs apart → all in first 50 µs
  }

  // Sparse: 1 trade per second from t=11s to t=59s.
  for (int s = 11; s < 60; ++s)
  {
    pushTrade(static_cast<int64_t>(s) * 1'000'000'000LL);
  }

  // Burst 2 at 60 s: 30 trades within 100 µs.
  const int64_t b2 = 60'000'000'000LL;
  for (int i = 0; i < 30; ++i)
  {
    pushTrade(b2 + i * 1'000LL);
  }

  writer.close();
}

const PeakAggregator::Row* findPeak(const std::vector<PeakAggregator::Row>& rows,
                                    int64_t window_ns, std::size_t rank)
{
  std::size_t r = 0;
  for (const auto& row : rows)
  {
    if (row.window_ns == window_ns)
    {
      if (r == rank)
      {
        return &row;
      }
      ++r;
    }
  }
  return nullptr;
}

}  // namespace

TEST_F(AggregatorFrameworkTest, PeakSingleScaleFindsBothBurstsDeduped)
{
  // 1 ms window: burst 1 has 50 trades in 100 µs (fully contained
  // within any 1 ms window starting between b1-900µs and b1). Burst 2
  // has 30 trades in 100 µs. Sparse region has 1 trade per second
  // (count = 1 in a 1 ms window). top_n=2 with 3 ms suppression
  // separates the two bursts cleanly.
  writePeakTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  PeakAggregator peaks({1'000'000LL}, /*top_n=*/2);
  std::array<IAggregator*, 1> aggregators{&peaks};
  ASSERT_TRUE(reader.run(aggregators));

  const auto& rows = peaks.result();
  ASSERT_EQ(rows.size(), 2u);

  // Strongest peak should be the 50-trade burst at t≈10s.
  EXPECT_EQ(rows[0].window_ns, 1'000'000LL);
  EXPECT_EQ(rows[0].count, 50u);
  EXPECT_GE(rows[0].start_ns, 9'000'000'000LL);
  EXPECT_LT(rows[0].start_ns, 11'000'000'000LL);

  // Second peak should be the 30-trade burst at t≈60s, NOT another
  // copy of the first burst's neighborhood (which the 3*window
  // dedup must have suppressed).
  EXPECT_EQ(rows[1].count, 30u);
  EXPECT_GE(rows[1].start_ns, 59'000'000'000LL);
  EXPECT_LT(rows[1].start_ns, 61'000'000'000LL);
}

TEST_F(AggregatorFrameworkTest, PeakMultipleScalesEachReportsOwnTop)
{
  // Three scales: 1 ms (catches each burst fully — 50 / 30), 1 s
  // (still distinguishes bursts but with more nearby sparse trades
  // included), 10 s (windows long enough to potentially merge bursts;
  // the 10 s window starting near burst 1 catches just burst 1, the
  // window near burst 2 catches just burst 2 — they're 50 s apart).
  writePeakTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  PeakAggregator peaks({1'000'000LL, 1'000'000'000LL, 10'000'000'000LL},
                       /*top_n=*/2);
  std::array<IAggregator*, 1> aggregators{&peaks};
  ASSERT_TRUE(reader.run(aggregators));

  const auto& rows = peaks.result();
  // 3 scales × 2 peaks = 6 rows expected.
  ASSERT_EQ(rows.size(), 6u);

  // 1 ms scale: 50, 30.
  EXPECT_EQ(findPeak(rows, 1'000'000LL, 0)->count, 50u);
  EXPECT_EQ(findPeak(rows, 1'000'000LL, 1)->count, 30u);

  // 1 s scale: burst 1 (50) and burst 2 (30) still dominate vs the
  // 1-per-second sparse activity (count = 1 in a 1 s window).
  EXPECT_GE(findPeak(rows, 1'000'000'000LL, 0)->count, 50u);
  EXPECT_GE(findPeak(rows, 1'000'000'000LL, 1)->count, 30u);

  // 10 s scale: each burst is the dominant feature within its
  // ±10 s neighborhood; the second peak is still on burst 2 (the
  // 3*window = 30 s suppression keeps it from collapsing into
  // burst 1's neighborhood).
  const auto* p10_top = findPeak(rows, 10'000'000'000LL, 0);
  const auto* p10_second = findPeak(rows, 10'000'000'000LL, 1);
  ASSERT_NE(p10_top, nullptr);
  ASSERT_NE(p10_second, nullptr);
  EXPECT_GT(p10_top->count, p10_second->count);
}

TEST_F(AggregatorFrameworkTest, PeakBooksOnlyFilterIsEmpty)
{
  writePeakTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  PeakAggregator peaks({1'000'000LL}, 5, 100, AggregatorEventFilter::BooksOnly);
  std::array<IAggregator*, 1> aggregators{&peaks};
  ASSERT_TRUE(reader.run(aggregators));

  EXPECT_TRUE(peaks.result().empty());
}

TEST_F(AggregatorFrameworkTest, PeakConstructorRejectsBadArgs)
{
  EXPECT_THROW(PeakAggregator({}), std::invalid_argument);
  EXPECT_THROW(PeakAggregator({0}), std::invalid_argument);
  EXPECT_THROW(PeakAggregator({-1'000'000LL}), std::invalid_argument);
  EXPECT_THROW(PeakAggregator({1'000'000LL}, 0), std::invalid_argument);
}

// ───────────────────────────────────────────────────────────────────────
// QuantileAggregator
// ───────────────────────────────────────────────────────────────────────

namespace
{

// Tape with a known event-time distribution: 100 trades evenly spaced
// at 1 ms intervals → a 1 ms window catches exactly 1 trade most of
// the time. We then add a single 5-trade burst (5 trades within
// 200 µs) somewhere in the middle. With 1 ms windows, observations
// during the burst region see counts 2..5 briefly, but the vast
// majority of windows still see count 1. So quantile(0.5)=1 and
// quantile(0.99) might be 1 or 2 depending on how many burst-region
// observations slip past the dedup threshold.
void writeQuantileTape(const std::filesystem::path& dir)
{
  WriterConfig config{.output_dir = dir,
                      .index_interval = 1000,
                      .compression = CompressionType::LZ4};
  BinaryLogWriter writer(config);

  uint64_t tid = 0;
  // Sparse: 100 trades, 1 ms apart, starting at t=1s.
  for (int i = 0; i < 100; ++i)
  {
    TradeRecord t{};
    t.exchange_ts_ns = 1'000'000'000LL + static_cast<int64_t>(i) * 1'000'000LL;
    t.symbol_id = 1;
    t.trade_id = tid++;
    writer.writeTrade(t);
  }
  writer.close();
}

const QuantileAggregator::Row* findQuant(
    const std::vector<QuantileAggregator::Row>& rows, int64_t window_ns,
    double quantile)
{
  auto it = std::find_if(rows.begin(), rows.end(),
                         [=](const auto& r)
                         {
                           return r.window_ns == window_ns && r.quantile == quantile;
                         });
  return it == rows.end() ? nullptr : &*it;
}

}  // namespace

TEST_F(AggregatorFrameworkTest, QuantileSparseTapeMostlyOneEventPerWindow)
{
  // 100 trades 1 ms apart, 1 ms sliding window → at every event
  // arrival the sliding window contains exactly that event (the
  // previous event was 1 ms ago, which is on the boundary —
  // pruning is at `front <= t - w` so prior event is dropped).
  // Therefore every observation is count=1, all quantiles → 1.
  writeQuantileTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  QuantileAggregator quant({1'000'000LL}, {0.5, 0.95, 0.99});
  std::array<IAggregator*, 1> aggregators{&quant};
  ASSERT_TRUE(reader.run(aggregators));

  const auto& rows = quant.result();
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(findQuant(rows, 1'000'000LL, 0.5)->count, 1u);
  EXPECT_EQ(findQuant(rows, 1'000'000LL, 0.95)->count, 1u);
  EXPECT_EQ(findQuant(rows, 1'000'000LL, 0.99)->count, 1u);
}

TEST_F(AggregatorFrameworkTest, QuantileLargerWindowSeesMoreEvents)
{
  // Same tape, but with a 5 ms window: each event arrival sees the
  // previous ~5 events in its sliding window. The first few events
  // see 1, 2, 3, 4, 5 counts; the rest of the tape sees count=5
  // (the steady-state sliding-window depth). The median should
  // be 5.
  writeQuantileTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  QuantileAggregator quant({5'000'000LL}, {0.5, 0.95});
  std::array<IAggregator*, 1> aggregators{&quant};
  ASSERT_TRUE(reader.run(aggregators));

  const auto& rows = quant.result();
  EXPECT_EQ(findQuant(rows, 5'000'000LL, 0.5)->count, 5u);
  EXPECT_EQ(findQuant(rows, 5'000'000LL, 0.95)->count, 5u);
}

TEST_F(AggregatorFrameworkTest, QuantileMultipleScales)
{
  writeQuantileTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  QuantileAggregator quant({1'000'000LL, 5'000'000LL, 10'000'000LL},
                           {0.5, 0.99});
  std::array<IAggregator*, 1> aggregators{&quant};
  ASSERT_TRUE(reader.run(aggregators));

  const auto& rows = quant.result();
  ASSERT_EQ(rows.size(), 6u);
  // Larger windows → higher median window count.
  EXPECT_LT(findQuant(rows, 1'000'000LL, 0.5)->count,
            findQuant(rows, 5'000'000LL, 0.5)->count);
  EXPECT_LE(findQuant(rows, 5'000'000LL, 0.5)->count,
            findQuant(rows, 10'000'000LL, 0.5)->count);
}

TEST_F(AggregatorFrameworkTest, QuantileBooksOnlyEmitsZeroThresholds)
{
  // BooksOnly with a trade-only tape → no observations. Result rows
  // still appear (one per (scale, quantile)) with count=0 so the
  // shape stays stable for downstream consumers.
  writeQuantileTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  QuantileAggregator quant({1'000'000LL}, {0.5, 0.95},
                           AggregatorEventFilter::BooksOnly);
  std::array<IAggregator*, 1> aggregators{&quant};
  ASSERT_TRUE(reader.run(aggregators));

  const auto& rows = quant.result();
  ASSERT_EQ(rows.size(), 2u);
  for (const auto& r : rows)
  {
    EXPECT_EQ(r.count, 0u);
  }
}

TEST_F(AggregatorFrameworkTest, QuantileRejectsBadArgs)
{
  EXPECT_THROW(QuantileAggregator({}, {0.5}), std::invalid_argument);
  EXPECT_THROW(QuantileAggregator({1'000'000LL}, {}), std::invalid_argument);
  EXPECT_THROW(QuantileAggregator({0}, {0.5}), std::invalid_argument);
  EXPECT_THROW(QuantileAggregator({-1}, {0.5}), std::invalid_argument);
  EXPECT_THROW(QuantileAggregator({1'000'000LL}, {0.0}), std::invalid_argument);
  EXPECT_THROW(QuantileAggregator({1'000'000LL}, {1.5}), std::invalid_argument);
  EXPECT_THROW(QuantileAggregator({1'000'000LL}, {-0.1}), std::invalid_argument);
}

TEST_F(AggregatorFrameworkTest, PeakStreamForEachParityWithForEach)
{
  // Correctness gate for the streaming-run memory fix: PeakAggregator
  // results via DataReader.run() (which routes through streamForEach
  // for O(1) per-segment memory) must equal results via forEach
  // (which buffers + sorts unsorted segments) on a monotonic tape.
  //
  // The two-burst writePeakTape fixture is monotonic by construction
  // (timestamps emitted in ascending order); both paths must deliver
  // the same event stream to PeakAggregator and therefore the same
  // top-N. This is the unit-level guard for the prod observation
  // that md_collector tapes are monotonic in practice even when the
  // Sorted flag is not set.
  writePeakTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};

  BinaryLogReader reader_run(rconfig);
  PeakAggregator peak_run({1'000'000LL, 1'000'000'000LL}, /*top_n=*/3);
  std::array<IAggregator*, 1> aggregators{&peak_run};
  ASSERT_TRUE(reader_run.run(aggregators));

  BinaryLogReader reader_foreach(rconfig);
  PeakAggregator peak_foreach({1'000'000LL, 1'000'000'000LL}, /*top_n=*/3);
  ASSERT_TRUE(reader_foreach.forEach(
      [&peak_foreach](const ReplayEvent& ev)
      {
        peak_foreach.onEvent(ev);
        return true;
      }));
  peak_foreach.finalize();

  const auto& rows_run = peak_run.result();
  const auto& rows_fe = peak_foreach.result();
  ASSERT_EQ(rows_run.size(), rows_fe.size());
  for (size_t i = 0; i < rows_run.size(); ++i)
  {
    EXPECT_EQ(rows_run[i].window_ns, rows_fe[i].window_ns) << "row " << i;
    EXPECT_EQ(rows_run[i].count, rows_fe[i].count) << "row " << i;
    EXPECT_EQ(rows_run[i].start_ns, rows_fe[i].start_ns) << "row " << i;
  }
}

TEST_F(AggregatorFrameworkTest, StreamForEachVisitsEveryEvent)
{
  // Direct exercise of the new public streamForEach. Counts events
  // and verifies the totals match what scanSegments saw at open.
  writePeakTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);
  const uint64_t expected = reader.count();

  int seen = 0;
  ASSERT_TRUE(reader.streamForEach(
      [&seen](const ReplayEvent& /*ev*/)
      {
        ++seen;
        return true;
      }));
  EXPECT_EQ(static_cast<uint64_t>(seen), expected);
}

TEST_F(AggregatorFrameworkTest, PeakHeapBudgetBoundedPathKeepsLargestCounts)
{
  // Regression for the budget-bound replace path bug: with
  // oversample_factor=1 and top_n=2 the heap fills at 2 candidates,
  // every subsequent event hits the branch that replaces the heap
  // minimum if the new candidate beats it.
  //
  // Fixture: 100 trades 1ms apart, 100ms sliding window. The
  // observed window count grows 1, 2, 3, ..., 100 across event
  // arrivals — every candidate after the second is strictly larger
  // than the current heap minimum and MUST replace it. Correct
  // top-1 count is in the 90s. The arg-swapped version of the heap
  // check would freeze top-1 at 2 (the largest count before the
  // budget filled), so any drift back below ~50 means the bug
  // returned.
  WriterConfig cfg{.output_dir = _test_dir,
                   .index_interval = 1000,
                   .compression = CompressionType::LZ4};
  BinaryLogWriter writer(cfg);
  for (int i = 0; i < 100; ++i)
  {
    TradeRecord t{};
    t.exchange_ts_ns = 1'000'000LL * (i + 1);  // 1ms..100ms
    t.symbol_id = 1;
    t.trade_id = static_cast<uint64_t>(i);
    writer.writeTrade(t);
  }
  writer.close();

  ReaderConfig rcfg{.data_dir = _test_dir};
  BinaryLogReader reader(rcfg);

  PeakAggregator peak({100'000'000LL}, /*top_n=*/2, /*oversample_factor=*/1);
  std::array<IAggregator*, 1> aggs{&peak};
  ASSERT_TRUE(reader.run(aggs));

  const auto& rows = peak.result();
  ASSERT_FALSE(rows.empty());
  EXPECT_GE(rows[0].count, 90u)
      << "top-1 count should reflect the steady-state full window (~100); "
      << "got " << rows[0].count
      << " — heap comparator likely keeping smallest instead of largest";
}

TEST_F(AggregatorFrameworkTest, PeakSymbolFilterScopesToListedSymbols)
{
  // Add a second symbol that has its own larger burst; filter to
  // symbol 1 only and confirm peaks come from symbol 1's bursts, not
  // the (larger) symbol 2 burst.
  WriterConfig config{.output_dir = _test_dir,
                      .index_interval = 1000,
                      .compression = CompressionType::LZ4};
  BinaryLogWriter writer(config);

  uint64_t tid = 0;
  // symbol 1: 20-trade burst at 5 s
  for (int i = 0; i < 20; ++i)
  {
    TradeRecord t{};
    t.exchange_ts_ns = 5'000'000'000LL + i * 1'000LL;
    t.symbol_id = 1;
    t.trade_id = tid++;
    writer.writeTrade(t);
  }
  // symbol 2: 100-trade "noise" burst at 30 s (much bigger; would
  // dominate if not filtered out)
  for (int i = 0; i < 100; ++i)
  {
    TradeRecord t{};
    t.exchange_ts_ns = 30'000'000'000LL + i * 1'000LL;
    t.symbol_id = 2;
    t.trade_id = tid++;
    writer.writeTrade(t);
  }
  writer.close();

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  PeakAggregator peaks({1'000'000LL}, /*top_n=*/1, 100,
                       AggregatorEventFilter::Trades, {1});
  std::array<IAggregator*, 1> aggregators{&peaks};
  ASSERT_TRUE(reader.run(aggregators));

  const auto& rows = peaks.result();
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].count, 20u);
  EXPECT_GE(rows[0].start_ns, 4'000'000'000LL);
  EXPECT_LT(rows[0].start_ns, 6'000'000'000LL);
}

// ───────────────────────────────────────────────────────────────────────
// Bounded reorder buffer (cross-block OOO recovery)
// ───────────────────────────────────────────────────────────────────────

namespace
{

// Writes a tape with deliberate cross-block timestamp inversion so the
// segment ends up with Sorted=false at close. BinaryLogWriter sorts
// inside each block but tracks (and does not fix) cross-block inversion,
// so any block whose first event timestamp is < previous block's last
// event timestamp leaves _segment_has_cross_block_inversion=true.
//
// Layout: block 0 events at t = 1ms, 2ms, ..., 5ms; block 1 events at
// t = 2.5ms, 3.5ms, ..., 6.5ms. block 1's first event (2.5ms) < block 0's
// last (5ms) → cross-block inversion of magnitude 2.5ms.
void writeCrossBlockInvertedTape(const std::filesystem::path& dir)
{
  WriterConfig config{.output_dir = dir,
                      .index_interval = 5,  // tiny blocks to force the inversion
                      .compression = CompressionType::LZ4};
  BinaryLogWriter writer(config);

  uint64_t tid = 0;
  for (int i = 0; i < 5; ++i)
  {
    TradeRecord t{};
    t.exchange_ts_ns = static_cast<int64_t>(1'000'000) * (i + 1);  // 1ms..5ms
    t.symbol_id = 1;
    t.trade_id = tid++;
    writer.writeTrade(t);
  }
  for (int i = 0; i < 5; ++i)
  {
    TradeRecord t{};
    t.exchange_ts_ns =
        static_cast<int64_t>(2'500'000) + static_cast<int64_t>(1'000'000) * i;  // 2.5ms..6.5ms
    t.symbol_id = 1;
    t.trade_id = tid++;
    writer.writeTrade(t);
  }
  writer.close();
}

}  // namespace

TEST_F(AggregatorFrameworkTest, ReorderBufferDeliversCrossBlockInOrder)
{
  // Wide reorder window relative to the 2.5ms inversion → heap can
  // hold both blocks at once, drain at end emits the merged stream
  // in timestamp order.
  writeCrossBlockInvertedTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir, .reorder_window_ns = 10'000'000};  // 10ms
  BinaryLogReader reader(rconfig);

  std::vector<int64_t> timestamps;
  ASSERT_TRUE(reader.streamForEach(
      [&timestamps](const ReplayEvent& ev)
      {
        timestamps.push_back(ev.timestamp_ns);
        return true;
      }));

  ASSERT_EQ(timestamps.size(), 10u);
  for (size_t i = 1; i < timestamps.size(); ++i)
  {
    EXPECT_LE(timestamps[i - 1], timestamps[i])
        << "monotonic violation at index " << i;
  }

  // Sanity: the merge interleaves block 0 (1-5ms) and block 1 (2.5-6.5ms)
  // — first three timestamps should be 1ms, 2ms, 2.5ms.
  EXPECT_EQ(timestamps[0], 1'000'000);
  EXPECT_EQ(timestamps[1], 2'000'000);
  EXPECT_EQ(timestamps[2], 2'500'000);
}

TEST_F(AggregatorFrameworkTest, ReorderBufferDropsAndCountsLateEventByDefault)
{
  // Same tape (2.5ms inversion) but reorder_window=1ms. The first event from
  // block 1 (t=2.5ms) arrives when watermark=5ms; delta = 2.5ms > 1ms. The
  // default is to drop that event and count it: a walk that loses a few
  // frames and reports them beats one that dies mid-run with aggregators
  // already half fed.
  writeCrossBlockInvertedTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir, .reorder_window_ns = 1'000'000};  // 1ms
  BinaryLogReader reader(rconfig);

  std::vector<int64_t> timestamps;
  ASSERT_NO_THROW({
    reader.streamForEach(
        [&timestamps](const ReplayEvent& ev)
        {
          timestamps.push_back(ev.timestamp_ns);
          return true;
        });
  });

  EXPECT_GT(reader.stats().late_dropped, 0u);
  EXPECT_EQ(timestamps.size() + reader.stats().late_dropped, 10u);
  for (size_t i = 1; i < timestamps.size(); ++i)
  {
    EXPECT_LE(timestamps[i - 1], timestamps[i]) << "monotonic violation at index " << i;
  }
}

TEST_F(AggregatorFrameworkTest, ReorderBufferThrowsOnWindowOverflowInStrictMode)
{
  // strict_ordering brings back the throw for reproducibility work.
  writeCrossBlockInvertedTape(_test_dir);

  ReaderConfig rconfig{
      .data_dir = _test_dir, .reorder_window_ns = 1'000'000, .strict_ordering = true};
  BinaryLogReader reader(rconfig);

  bool threw = false;
  std::string err_code;
  try
  {
    reader.streamForEach(
        [](const ReplayEvent&)
        { return true; });
  }
  catch (const lrvx::LrvxError& e)
  {
    threw = true;
    err_code = e.code();
  }
  EXPECT_TRUE(threw);
  EXPECT_EQ(err_code, "E_DATA_002");
  EXPECT_EQ(reader.stats().late_dropped, 0u);
}

TEST_F(AggregatorFrameworkTest, ReorderBufferBypassedForSortedSegment)
{
  // Writer with sane monotonic input → segment ends up Sorted=true →
  // streamForEach takes the fast path, the reorder window is ignored.
  // Setting reorder_window=0 would normally throw on any OOO; here we
  // verify it does NOT, because the fast path bypasses the buffer
  // entirely.
  writeCompressedTrades(50);  // monotonic from the helper

  ReaderConfig rconfig{.data_dir = _test_dir, .reorder_window_ns = 0};
  BinaryLogReader reader(rconfig);

  int seen = 0;
  ASSERT_NO_THROW({
    reader.streamForEach(
        [&seen](const ReplayEvent&)
        {
          ++seen;
          return true;
        });
  });
  EXPECT_EQ(seen, 50);
}

namespace
{

// Write a metadata.json for a single-symbol tape directory so
// MergedTapeReader's loadManifests() accepts it.
void writeTapeMetadata(const std::filesystem::path& dir, const std::string& exchange,
                       uint32_t symbol_id, const std::string& symbol_name)
{
  RecordingMetadata meta;
  meta.recording_id = "test-" + exchange + "-" + symbol_name;
  meta.exchange = exchange;
  meta.exchange_type = "cex";
  meta.instrument_type = "perpetual";
  meta.has_trades = true;
  SymbolInfo sym;
  sym.symbol_id = symbol_id;
  sym.name = symbol_name;
  sym.base_asset = symbol_name.substr(0, 3);
  sym.quote_asset = "USDT";
  sym.price_precision = 2;
  sym.qty_precision = 4;
  meta.symbols.push_back(sym);
  meta.save(RecordingMetadata::metadataPath(dir));
}

void writeCrossBlockInvertedTapeNamed(const std::filesystem::path& dir,
                                      uint32_t symbol_id)
{
  WriterConfig config{.output_dir = dir,
                      .index_interval = 5,
                      .compression = CompressionType::LZ4};
  BinaryLogWriter writer(config);

  uint64_t tid = 0;
  for (int i = 0; i < 5; ++i)
  {
    TradeRecord t{};
    t.exchange_ts_ns = static_cast<int64_t>(1'000'000) * (i + 1);
    t.symbol_id = symbol_id;
    t.trade_id = tid++;
    writer.writeTrade(t);
  }
  for (int i = 0; i < 5; ++i)
  {
    TradeRecord t{};
    t.exchange_ts_ns = static_cast<int64_t>(2'500'000) +
                       static_cast<int64_t>(1'000'000) * i;
    t.symbol_id = symbol_id;
    t.trade_id = tid++;
    writer.writeTrade(t);
  }
  writer.close();
}

}  // namespace

TEST_F(AggregatorFrameworkTest, MergedReorderBufferDeliversCrossBlockInOrder)
{
  // Two tapes, each with the same 2.5 ms cross-block inversion as the
  // single-tape fixture. Without per-tape reorder, MergedTapeReader's
  // N-way heap would see out-of-order events from each tape and the
  // merged stream would have monotonicity gaps. With ReorderingWalker
  // (the reordering fix), each per-tape input is monotonic before merging.
  auto dir_a = _test_dir / "tape_a";
  auto dir_b = _test_dir / "tape_b";
  std::filesystem::create_directories(dir_a);
  std::filesystem::create_directories(dir_b);
  writeCrossBlockInvertedTapeNamed(dir_a, /*symbol_id=*/1);
  writeCrossBlockInvertedTapeNamed(dir_b, /*symbol_id=*/1);
  writeTapeMetadata(dir_a, "bitget", 1, "BTCUSDT");
  writeTapeMetadata(dir_b, "bybit", 1, "BTCUSDT");

  MergedTapeReaderConfig cfg{};
  cfg.tape_dirs = {dir_a, dir_b};
  cfg.reorder_window_ns = 10'000'000;  // 10 ms — well above the 2.5 ms inversion.
  MergedTapeReader merged(cfg);

  std::vector<int64_t> timestamps;
  ASSERT_TRUE(merged.streamEvents(
      [&timestamps](uint32_t /*tape_index*/, const ReplayEvent& ev)
      {
        timestamps.push_back(ev.timestamp_ns);
        return true;
      }));

  ASSERT_EQ(timestamps.size(), 20u);  // 10 per tape × 2 tapes
  for (size_t i = 1; i < timestamps.size(); ++i)
  {
    EXPECT_LE(timestamps[i - 1], timestamps[i])
        << "merged stream non-monotonic at index " << i;
  }
}

TEST_F(AggregatorFrameworkTest, MergedReorderBufferThrowsOnOverflow)
{
  auto dir_a = _test_dir / "tape_a2";
  auto dir_b = _test_dir / "tape_b2";
  std::filesystem::create_directories(dir_a);
  std::filesystem::create_directories(dir_b);
  writeCrossBlockInvertedTapeNamed(dir_a, /*symbol_id=*/1);
  writeCrossBlockInvertedTapeNamed(dir_b, /*symbol_id=*/1);
  writeTapeMetadata(dir_a, "bitget", 1, "BTCUSDT");
  writeTapeMetadata(dir_b, "bybit", 1, "BTCUSDT");

  MergedTapeReaderConfig cfg{};
  cfg.tape_dirs = {dir_a, dir_b};
  cfg.reorder_window_ns = 1'000'000;  // 1 ms < 2.5 ms inversion → overflow
  MergedTapeReader merged(cfg);

  bool threw = false;
  std::string code;
  try
  {
    merged.streamEvents(
        [](uint32_t, const ReplayEvent&)
        { return true; });
  }
  catch (const lrvx::LrvxError& e)
  {
    threw = true;
    code = e.code();
  }
  EXPECT_TRUE(threw);
  EXPECT_EQ(code, "E_DATA_002");
}

namespace
{

// Write a multi-segment fixture by rotating at small byte budget. Each
// trade is ~48 bytes payload + 12B frame header ≈ 60B; 8 KB rotation
// gives roughly 100-200 events per segment, so 1000 trades land in
// ~6-10 segments — enough to spread across 4 workers.
void writeMultiSegmentTrades(const std::filesystem::path& dir, int count)
{
  WriterConfig cfg{.output_dir = dir,
                   .max_segment_bytes = 8 * 1024,
                   .index_interval = 50,
                   .compression = CompressionType::LZ4};
  BinaryLogWriter writer(cfg);
  for (int i = 0; i < count; ++i)
  {
    TradeRecord t{};
    t.exchange_ts_ns = 1'000'000LL * (i + 1);  // 1ms apart
    t.symbol_id = 1;
    t.trade_id = static_cast<uint64_t>(i);
    t.side = (i % 2);
    t.qty_raw = 100 + (i % 10);
    writer.writeTrade(t);
  }
  writer.close();
}

}  // namespace

TEST_F(AggregatorFrameworkTest, ParallelRunMatchesSingleThreadResults)
{
  // Multi-segment fixture run through single-threaded and parallel
  // panels with identical inputs. Result vectors / per-cell values
  // must match exactly for the merge-friendly aggregators
  // (EventTypeStats, BinCount, VolumeBin, Quantile). Peak's
  // post-merge top-N is unordered by start_ns within a count tier so
  // we compare the multiset of counts.
  constexpr int N = 1000;
  writeMultiSegmentTrades(_test_dir, N);

  ReaderConfig rcfg{.data_dir = _test_dir};

  auto runWith = [&](std::size_t n_threads)
  {
    BinaryLogReader r(rcfg);
    auto stats = std::make_unique<EventTypeStatsAggregator>();
    auto bins = std::make_unique<BinCountAggregator>(10'000'000LL);
    auto vol = std::make_unique<VolumeBinAggregator>(10'000'000LL);
    auto quant = std::make_unique<QuantileAggregator>(
        std::vector<int64_t>{10'000'000LL}, std::vector<double>{0.5, 0.99});
    std::array<IAggregator*, 4> panel{stats.get(), bins.get(), vol.get(), quant.get()};
    EXPECT_TRUE(r.run(panel, n_threads));
    return std::make_tuple(std::move(stats), std::move(bins), std::move(vol),
                           std::move(quant));
  };

  auto [s1, b1, v1, q1] = runWith(1);
  auto [s4, b4, v4, q4] = runWith(4);

  // EventTypeStats parity
  ASSERT_EQ(s1->result().size(), s4->result().size());
  for (size_t i = 0; i < s1->result().size(); ++i)
  {
    EXPECT_EQ(s1->result()[i].symbol_id, s4->result()[i].symbol_id);
    EXPECT_EQ(s1->result()[i].trades, s4->result()[i].trades);
    EXPECT_EQ(s1->result()[i].book_snapshots, s4->result()[i].book_snapshots);
    EXPECT_EQ(s1->result()[i].book_deltas, s4->result()[i].book_deltas);
  }
  EXPECT_EQ(s1->result()[0].trades, static_cast<uint64_t>(N));

  // BinCount parity
  ASSERT_EQ(b1->result().size(), b4->result().size());
  for (size_t i = 0; i < b1->result().size(); ++i)
  {
    EXPECT_EQ(b1->result()[i].bucket_ts_ns, b4->result()[i].bucket_ts_ns);
    EXPECT_EQ(b1->result()[i].count, b4->result()[i].count);
  }

  // VolumeBin parity
  ASSERT_EQ(v1->result().size(), v4->result().size());
  for (size_t i = 0; i < v1->result().size(); ++i)
  {
    EXPECT_EQ(v1->result()[i].bucket_ts_ns, v4->result()[i].bucket_ts_ns);
    EXPECT_EQ(v1->result()[i].qty_raw, v4->result()[i].qty_raw);
  }

  // Quantile parity — histogram sums merge associatively so threshold
  // counts must match exactly.
  ASSERT_EQ(q1->result().size(), q4->result().size());
  for (size_t i = 0; i < q1->result().size(); ++i)
  {
    EXPECT_EQ(q1->result()[i].window_ns, q4->result()[i].window_ns);
    EXPECT_EQ(q1->result()[i].quantile, q4->result()[i].quantile);
    EXPECT_EQ(q1->result()[i].count, q4->result()[i].count);
  }
}

TEST_F(AggregatorFrameworkTest, ParallelRunEmptySpanNoOp)
{
  writeMultiSegmentTrades(_test_dir, 100);
  ReaderConfig rcfg{.data_dir = _test_dir};
  BinaryLogReader reader(rcfg);
  std::array<IAggregator*, 0> empty{};
  EXPECT_TRUE(reader.run(empty, 4));
}

TEST_F(AggregatorFrameworkTest, IntraSegmentParallelMatchesSingleThread)
{
  // Single huge segment with many blocks — exercises intra-segment
  // partition (per-segment blocks distributed across workers). The
  // md_collector workload looks like this: one active segment that
  // grows for weeks before rotation. Segment-level partition would
  // pin all work on one worker; intra-segment must split it.
  //
  // Config: no segment rotation (huge max_segment_bytes), tiny
  // index_interval so we get many blocks. 2000 trades at 5 events
  // per block → 400 blocks in one segment.
  WriterConfig cfg{.output_dir = _test_dir,
                   .max_segment_bytes = 1024ull * 1024 * 1024,  // 1 GB cap
                   .index_interval = 5,
                   .compression = CompressionType::LZ4};
  BinaryLogWriter writer(cfg);
  constexpr int N = 2000;
  for (int i = 0; i < N; ++i)
  {
    TradeRecord t{};
    t.exchange_ts_ns = 1'000'000LL * (i + 1);
    t.symbol_id = 1;
    t.trade_id = static_cast<uint64_t>(i);
    t.side = (i % 2);
    t.qty_raw = 100 + (i % 7);
    writer.writeTrade(t);
  }
  writer.close();

  ReaderConfig rcfg{.data_dir = _test_dir};

  auto runWith = [&](std::size_t n_threads)
  {
    BinaryLogReader r(rcfg);
    auto stats = std::make_unique<EventTypeStatsAggregator>();
    auto bins = std::make_unique<BinCountAggregator>(50'000'000LL);
    auto vol = std::make_unique<VolumeBinAggregator>(50'000'000LL);
    auto quant = std::make_unique<QuantileAggregator>(
        std::vector<int64_t>{10'000'000LL}, std::vector<double>{0.5, 0.99});
    std::array<IAggregator*, 4> panel{stats.get(), bins.get(), vol.get(), quant.get()};
    EXPECT_TRUE(r.run(panel, n_threads));
    const auto segs = r.summary().segment_count;
    return std::make_tuple(std::move(stats), std::move(bins), std::move(vol),
                           std::move(quant), segs);
  };

  auto [s1, b1, v1, q1, segs1] = runWith(1);
  auto [s4, b4, v4, q4, segs4] = runWith(4);

  // Sanity: this fixture really is a single segment (the whole point
  // of the test). If the writer rotated for some reason, the test
  // doesn't exercise intra-segment partition — fail loud rather than
  // pass quietly.
  EXPECT_EQ(segs1, 1u);
  EXPECT_EQ(segs4, 1u);

  ASSERT_EQ(s1->result().size(), s4->result().size());
  EXPECT_EQ(s1->result()[0].trades, static_cast<uint64_t>(N));
  EXPECT_EQ(s4->result()[0].trades, static_cast<uint64_t>(N));

  ASSERT_EQ(b1->result().size(), b4->result().size());
  for (size_t i = 0; i < b1->result().size(); ++i)
  {
    EXPECT_EQ(b1->result()[i].count, b4->result()[i].count);
  }

  ASSERT_EQ(v1->result().size(), v4->result().size());
  for (size_t i = 0; i < v1->result().size(); ++i)
  {
    EXPECT_EQ(v1->result()[i].qty_raw, v4->result()[i].qty_raw);
  }

  ASSERT_EQ(q1->result().size(), q4->result().size());
  for (size_t i = 0; i < q1->result().size(); ++i)
  {
    EXPECT_EQ(q1->result()[i].count, q4->result()[i].count);
  }
}

TEST_F(AggregatorFrameworkTest, OhlcBinParallelMatchesSingleThread)
{
  // Single huge segment with many blocks + monotonically drifting
  // price. Parallel intra-segment partition splits the bucket that
  // straddles a worker seam; OHLCBinAggregator::merge() has to
  // recombine per-cell open/high/low/close exactly. Drift makes the
  // open and close of the merged cell differ, so a broken merge that
  // (say) picks dst.open unconditionally would show up here.
  WriterConfig cfg{.output_dir = _test_dir,
                   .max_segment_bytes = 1024ull * 1024 * 1024,
                   .index_interval = 5,
                   .compression = CompressionType::LZ4};
  BinaryLogWriter writer(cfg);
  constexpr int N = 2000;
  for (int i = 0; i < N; ++i)
  {
    TradeRecord t{};
    t.exchange_ts_ns = 1'000'000LL * (i + 1);
    t.symbol_id = 1;
    t.trade_id = static_cast<uint64_t>(i);
    t.side = (i % 2);
    t.qty_raw = 1;
    // Drift + sawtooth: distinct open/high/low/close per bucket.
    t.price_raw = 1000 + i + (i % 11) * 17 - (i % 7) * 13;
    writer.writeTrade(t);
  }
  writer.close();

  ReaderConfig rcfg{.data_dir = _test_dir};
  auto runWith = [&](std::size_t n_threads)
  {
    BinaryLogReader r(rcfg);
    auto ohlc = std::make_unique<OHLCBinAggregator>(50'000'000LL);
    std::array<IAggregator*, 1> panel{ohlc.get()};
    EXPECT_TRUE(r.run(panel, n_threads));
    return ohlc;
  };

  auto o1 = runWith(1);
  auto o4 = runWith(4);

  ASSERT_EQ(o1->result().size(), o4->result().size());
  for (size_t i = 0; i < o1->result().size(); ++i)
  {
    EXPECT_EQ(o1->result()[i].bucket_ts_ns, o4->result()[i].bucket_ts_ns);
    EXPECT_EQ(o1->result()[i].open_raw, o4->result()[i].open_raw);
    EXPECT_EQ(o1->result()[i].high_raw, o4->result()[i].high_raw);
    EXPECT_EQ(o1->result()[i].low_raw, o4->result()[i].low_raw);
    EXPECT_EQ(o1->result()[i].close_raw, o4->result()[i].close_raw);
  }
}
