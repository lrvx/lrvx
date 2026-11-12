/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Regression test for `BacktestRunner::runTape(path)` — the canonical
// recorded-tape → backtest path. Without `runTape` the only way to
// drive a strategy off a `.lrvx` was a sidecar (e.g. bars.npz) since
// `replay_tape` is callback-based and doesn't feed the runner.
//
// Test writes a tiny tape via `BinaryLogWriter`, then drives a
// counter strategy off it through `runTape` and asserts the
// strategy's `onSymbolTrade` fires the expected number of times.

#include "lrvx/backtest/backtest_runner.h"
#include "lrvx/book/events/trade_event.h"
#include "lrvx/engine/symbol_registry.h"
#include "lrvx/replay/binary_format_v1.h"
#include "lrvx/replay/writers/binary_log_writer.h"
#include "lrvx/strategy/strategy.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

using namespace lrvx;

namespace
{

class TradeCounter : public Strategy
{
 public:
  using Strategy::Strategy;
  size_t trades_seen{0};

 protected:
  void onSymbolTrade(SymbolContext& /*ctx*/, const TradeEvent& /*ev*/) override
  {
    ++trades_seen;
  }
};

SymbolId addSymbol(SymbolRegistry& reg, const std::string& name)
{
  SymbolInfo info;
  info.exchange = "test";
  info.symbol = name;
  info.type = InstrumentType::Spot;
  info.tickSize = Price::fromDouble(0.01);
  return reg.registerSymbol(info);
}

}  // namespace

TEST(BacktestRunTape, RoundTripsRecordedSession)
{
  auto td = std::filesystem::temp_directory_path() / "lrvx_run_tape_test";
  std::filesystem::remove_all(td);
  std::filesystem::create_directories(td);

  // Write a tiny tape: 50 trades on one symbol.
  SymbolRegistry reg;
  SymbolId sym = addSymbol(reg, "BTCUSDT");

  constexpr int64_t base_ns = 1'700'000'000'000'000'000;
  constexpr size_t kTradeCount = 50;
  {
    replay::WriterConfig cfg;
    cfg.output_dir = td;
    cfg.create_index = false;
    replay::BinaryLogWriter writer(cfg);
    for (size_t i = 0; i < kTradeCount; ++i)
    {
      replay::TradeRecord r{};
      r.exchange_ts_ns = base_ns + static_cast<int64_t>(i) * 1'000'000'000;
      r.recv_ts_ns = r.exchange_ts_ns;
      r.price_raw = Price::fromDouble(100.0 + i * 0.1).raw();
      r.qty_raw = Quantity::fromDouble(0.1).raw();
      r.symbol_id = sym;
      r.side = 1;
      r.trade_id = i + 1;
      writer.writeTrade(r);
    }
    writer.close();
  }

  TradeCounter strat(1, std::vector<SymbolId>{sym}, reg);
  BacktestRunner runner;
  runner.setStrategy(&strat);
  runner.runTape(td);

  EXPECT_EQ(strat.trades_seen, kTradeCount);

  std::filesystem::remove_all(td);
}

namespace
{

class BookWatcher : public Strategy
{
 public:
  using Strategy::ctx;
  using Strategy::Strategy;
  std::vector<std::string> calls;

 protected:
  void onSymbolBook(SymbolContext& /*c*/, const BookUpdateEvent& /*ev*/) override
  {
    calls.push_back("book");
  }

  void onSymbolTouch(SymbolContext& /*c*/, const BookUpdateEvent& /*ev*/) override
  {
    calls.push_back("touch");
  }
};

}  // namespace

TEST(BacktestRunTape, TopOfBookRecordsMoveTheTouchNotTheBook)
{
  auto td = std::filesystem::temp_directory_path() / "lrvx_run_tape_top_test";
  std::filesystem::remove_all(td);
  std::filesystem::create_directories(td);

  SymbolRegistry reg;
  SymbolId sym = addSymbol(reg, "BTCUSDT");

  constexpr int64_t base_ns = 1'700'000'000'000'000'000;
  {
    replay::WriterConfig cfg;
    cfg.output_dir = td;
    cfg.create_index = false;
    replay::BinaryLogWriter writer(cfg);

    replay::BookRecordHeader snap{};
    snap.exchange_ts_ns = base_ns;
    snap.recv_ts_ns = base_ns;
    snap.symbol_id = sym;
    snap.bid_count = 1;
    snap.ask_count = 1;
    snap.type = 0;
    std::vector<replay::BookLevel> bids = {{Price::fromDouble(100.0).raw(), Quantity::fromDouble(1.0).raw()}};
    std::vector<replay::BookLevel> asks = {{Price::fromDouble(101.0).raw(), Quantity::fromDouble(1.0).raw()}};
    writer.writeBook(snap, bids, asks);

    replay::BookRecordHeader top{};
    top.exchange_ts_ns = base_ns + 1'000'000'000;
    top.recv_ts_ns = top.exchange_ts_ns;
    top.symbol_id = sym;
    top.bid_count = 1;
    top.ask_count = 1;
    top.type = 2;
    std::vector<replay::BookLevel> tb = {{Price::fromDouble(100.4).raw(), Quantity::fromDouble(3.0).raw()}};
    std::vector<replay::BookLevel> ta = {{Price::fromDouble(100.6).raw(), Quantity::fromDouble(2.0).raw()}};
    writer.writeBook(top, tb, ta);
    writer.close();
  }

  BookWatcher strat(1, std::vector<SymbolId>{sym}, reg);
  BacktestRunner runner;
  runner.setStrategy(&strat);
  runner.runTape(td);

  // The snapshot fires the book and then the touch it set; the top fires the
  // touch alone and leaves the depth book where the snapshot put it.
  EXPECT_EQ(strat.calls, (std::vector<std::string>{"book", "touch", "touch"}));
  const auto& c = strat.ctx(sym);
  EXPECT_EQ(c.book.bestBid(), Price::fromDouble(100.0));
  ASSERT_TRUE(c.touchBid.has_value());
  EXPECT_EQ(c.touchBid->price, Price::fromDouble(100.4));
  EXPECT_EQ(c.touchAsk->price, Price::fromDouble(100.6));
  EXPECT_EQ(c.lastTouchNs, base_ns + 1'000'000'000);
  EXPECT_EQ(c.lastUpdateNs, base_ns);
}

// Option-quote and pool-state records have no consumer in the runner: no
// strategy callback, no subscriber, not the simulated executor. They are
// skipped, but counted per kind, and the counts are per run.
TEST(BacktestRunTape, CountsOptionQuoteAndPoolStateRecordsItDoesNotDeliver)
{
  auto td = std::filesystem::temp_directory_path() / "lrvx_run_tape_side_channel_test";
  std::filesystem::remove_all(td);
  std::filesystem::create_directories(td);

  SymbolRegistry reg;
  SymbolId sym = addSymbol(reg, "BTCUSDT");
  constexpr int64_t base_ns = 1'700'000'000'000'000'000;
  {
    replay::WriterConfig cfg;
    cfg.output_dir = td;
    cfg.create_index = false;
    replay::BinaryLogWriter writer(cfg);
    for (int i = 0; i < 6; ++i)
    {
      const int64_t ts = base_ns + static_cast<int64_t>(i) * 1'000'000'000;
      if (i % 2 == 0)
      {
        replay::TradeRecord r{};
        r.exchange_ts_ns = ts;
        r.recv_ts_ns = ts;
        r.price_raw = Price::fromDouble(100.0).raw();
        r.qty_raw = Quantity::fromDouble(0.1).raw();
        r.symbol_id = sym;
        r.side = 1;
        r.trade_id = static_cast<uint64_t>(i + 1);
        writer.writeTrade(r);
      }
      else if (i == 5)
      {
        const std::vector<std::byte> payload(64, std::byte{0x11});
        replay::PoolStateRecordHeader h{};
        h.exchange_ts_ns = ts;
        h.recv_ts_ns = ts;
        h.symbol_id = sym + 1;
        h.payload_len = static_cast<uint32_t>(payload.size());
        h.sub_type = static_cast<uint8_t>(replay::PoolStateKind::SwapDelta);
        writer.writePoolState(h, payload.data(), payload.size());
      }
      else
      {
        replay::OptionQuoteRecord q{};
        q.exchange_ts_ns = ts;
        q.recv_ts_ns = ts;
        q.symbol_id = sym + 2;
        q.mark_price_raw = Price::fromDouble(0.05).raw();
        writer.writeOptionQuote(q);
      }
    }
    writer.close();
  }

  TradeCounter strat(1, std::vector<SymbolId>{sym}, reg);
  BacktestRunner runner;
  runner.setStrategy(&strat);
  runner.runTape(td);

  EXPECT_EQ(strat.trades_seen, 3u);
  const BacktestState st = runner.state();
  EXPECT_EQ(st.optionQuoteCount, 2u);
  EXPECT_EQ(st.poolStateCount, 1u);
  EXPECT_EQ(st.tradeCount, 3u);
  EXPECT_EQ(st.eventCount, 6u);

  // A second run reports that run, not the sum.
  runner.runTape(td);
  EXPECT_EQ(runner.state().optionQuoteCount, 2u);
  EXPECT_EQ(runner.state().poolStateCount, 1u);

  std::filesystem::remove_all(td);
}

TEST(BacktestRunTape, ThrowsOnMissingDirectory)
{
  SymbolRegistry reg;
  SymbolId sym = addSymbol(reg, "BTCUSDT");
  TradeCounter strat(1, std::vector<SymbolId>{sym}, reg);
  BacktestRunner runner;
  runner.setStrategy(&strat);

  // The factory either returns nullptr (caught + thrown) or throws.
  // Either way the call must not silently produce zero-trade stats.
  auto missing = std::filesystem::temp_directory_path() / "lrvx_run_tape_does_not_exist";
  std::filesystem::remove_all(missing);
  EXPECT_ANY_THROW(runner.runTape(missing));
}
