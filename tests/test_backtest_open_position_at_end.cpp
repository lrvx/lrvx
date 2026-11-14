/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// A position still open when a backtest ends. net_pnl, total_pnl, trades()
// and the equity curve stay what they are (closed trades, every fill's fee);
// the stats also carry the open position, marked at the last price the run
// saw for its symbol:
//
//   unrealizedPnl  sum over symbols of (mark - entry) * qty for a long and
//                  (entry - mark) * qty for a short; no exit fee
//   finalEquity    initialCapital + netPnl + unrealizedPnl
//
// and BacktestResult::openPositions() lists each open position: symbol,
// side, quantity, average entry price, mark price and its unrealized P&L.
// The mark is the last price BacktestResult::markPrice() was given for the
// symbol, or its last fill price when it was never marked. The runners mark
// every price they replay: on bar replay a bar's close is the last price of
// that bar, on tick replay the last trade; a book record does not move it.

#include "lrvx/backtest/backtest_config.h"
#include "lrvx/backtest/backtest_result.h"
#include "lrvx/backtest/backtest_runner.h"
#include "lrvx/engine/symbol_registry.h"
#include "lrvx/replay/binary_format_v1.h"
#include "lrvx/replay/writers/binary_log_writer.h"
#include "lrvx/strategy/strategy.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <vector>

using namespace lrvx;

namespace
{

constexpr int64_t kSec = 1'000'000'000;
constexpr int64_t kMinuteNs = 60 * kSec;
constexpr int64_t kT0 = 1'767'225'600LL * kSec;  // 2026-01-01T00:00:00Z
constexpr SymbolId kSym = 1;
constexpr SymbolId kSym2 = 2;
constexpr double kEps = 1e-9;

Fill fill(SymbolId sym, Side side, double price, double qty, int64_t ts)
{
  static OrderId nextId = 1;
  Fill f{};
  f.orderId = nextId++;
  f.symbol = sym;
  f.side = side;
  f.price = Price::fromDouble(price);
  f.quantity = Quantity::fromDouble(qty);
  f.timestampNs = UnixNanos::fromRaw(ts);
  return f;
}

BacktestConfig makeConfig(double feeRate = 0.0004, double capital = 10'000.0)
{
  BacktestConfig cfg;
  cfg.initialCapital = capital;
  cfg.feeRate = feeRate;
  return cfg;
}

const OpenPosition* findOpen(const std::vector<OpenPosition>& open, SymbolId sym)
{
  for (const auto& p : open)
  {
    if (p.symbol == sym)
    {
      return &p;
    }
  }
  return nullptr;
}

SymbolRegistry& registry()
{
  static SymbolRegistry* reg = []
  {
    auto* r = new SymbolRegistry();
    for (const char* name : {"BTCUSDT", "ETHUSDT"})
    {
      SymbolInfo info;
      info.exchange = "test";
      info.symbol = name;
      info.type = InstrumentType::Spot;
      info.tickSize = Price::fromDouble(0.01);
      r->registerSymbol(info);
    }
    return r;
  }();
  return *reg;
}

BarEvent makeBar(SymbolId sym, double open, double high, double low, double close,
                 int64_t startNs)
{
  BarEvent ev{};
  ev.symbol = sym;
  ev.barType = BarType::Time;
  ev.barTypeParam = static_cast<uint64_t>(kMinuteNs);
  ev.bar.open = Price::fromDouble(open);
  ev.bar.high = Price::fromDouble(high);
  ev.bar.low = Price::fromDouble(low);
  ev.bar.close = Price::fromDouble(close);
  ev.bar.volume = Volume::fromDouble(1000.0);
  ev.bar.startTime = TimePoint{std::chrono::nanoseconds{startNs}};
  ev.bar.endTime = TimePoint{std::chrono::nanoseconds{startNs + kMinuteNs}};
  ev.bar.reason = BarCloseReason::Threshold;
  return ev;
}

class Probe : public Strategy
{
 public:
  explicit Probe(std::vector<SymbolId> syms = {kSym})
      : Strategy(1, std::move(syms), registry())
  {
  }

  std::function<void(Probe&, const BarEvent&, size_t)> onBarHook;
  std::function<void(Probe&, const TradeEvent&, size_t)> onTradeHook;
  size_t barCount{0};
  size_t tradeCount{0};

  using Strategy::emitMarketBuy;
  using Strategy::emitMarketSell;

 protected:
  void onSymbolBar(SymbolContext&, const BarEvent& ev) override
  {
    if (onBarHook)
    {
      onBarHook(*this, ev, barCount);
    }
    ++barCount;
  }
  void onSymbolTrade(SymbolContext&, const TradeEvent& ev) override
  {
    if (onTradeHook)
    {
      onTradeHook(*this, ev, tradeCount);
    }
    ++tradeCount;
  }
};

struct T
{
  int64_t ts;
  double px;
  SymbolId sym{kSym};
};

struct Book
{
  int64_t ts;
  double bid;
  double ask;
  SymbolId sym{kSym};
};

class TapeDir
{
 public:
  explicit TapeDir(const std::string& tag)
  {
    _dir = std::filesystem::temp_directory_path() /
           ("lrvx_open_at_end_" + tag + "_" + std::to_string(std::random_device{}()));
    std::filesystem::remove_all(_dir);
    std::filesystem::create_directories(_dir);
  }
  ~TapeDir() { std::filesystem::remove_all(_dir); }
  const std::filesystem::path& path() const { return _dir; }

  // One segment, records in the order given (callers pass them in time
  // order, books after trades at equal timestamps).
  void write(const std::vector<T>& trades, const std::vector<Book>& books = {})
  {
    replay::WriterConfig cfg;
    cfg.output_dir = _dir;
    replay::BinaryLogWriter writer(cfg);
    size_t ti = 0;
    size_t bi = 0;
    uint64_t tradeId = 0;
    while (ti < trades.size() || bi < books.size())
    {
      const bool takeTrade =
          bi >= books.size() || (ti < trades.size() && trades[ti].ts <= books[bi].ts);
      if (takeTrade)
      {
        const T& t = trades[ti++];
        replay::TradeRecord r{};
        r.exchange_ts_ns = t.ts;
        r.recv_ts_ns = t.ts;
        r.price_raw = Price::fromDouble(t.px).raw();
        r.qty_raw = Quantity::fromDouble(1.0).raw();
        r.symbol_id = t.sym;
        r.side = 1;
        r.trade_id = ++tradeId;
        writer.writeTrade(r);
      }
      else
      {
        const Book& b = books[bi++];
        replay::BookRecordHeader hdr{};
        hdr.exchange_ts_ns = b.ts;
        hdr.recv_ts_ns = b.ts;
        hdr.symbol_id = b.sym;
        hdr.type = 0;
        hdr.bid_count = 1;
        hdr.ask_count = 1;
        std::vector<replay::BookLevel> bids = {
            {Price::fromDouble(b.bid).raw(), Quantity::fromDouble(10.0).raw()}};
        std::vector<replay::BookLevel> asks = {
            {Price::fromDouble(b.ask).raw(), Quantity::fromDouble(10.0).raw()}};
        writer.writeBook(hdr, bids, asks);
      }
    }
    writer.close();
  }

 private:
  std::filesystem::path _dir;
};

// Bar 0 [kT0, +60 s): 100, 101. Bar 1 [+60 s, +120 s): 102, 103. Bar 2 holds
// the last trades, 106 and 110, and is still open when the tape ends, so bar
// replay never delivers it.
std::vector<T> threeMinuteTape()
{
  return {
      {kT0 + 1 * kSec, 100.0},
      {kT0 + 30 * kSec, 101.0},
      {kT0 + 61 * kSec, 102.0},
      {kT0 + 90 * kSec, 103.0},
      {kT0 + 121 * kSec, 106.0},
      {kT0 + 150 * kSec, 110.0},
  };
}

double fillPrice(const BacktestResult& r, size_t i) { return r.fills().at(i).price.toDouble(); }

}  // namespace

// ── BacktestResult: the accounting ─────────────────────────────────────

TEST(OpenPositionAtEnd, FlatRunHasNoUnrealizedAndFinalEquityIsFinalCapital)
{
  BacktestResult r(makeConfig());
  r.recordFill(fill(kSym, Side::BUY, 100.0, 1.0, kT0));
  r.recordFill(fill(kSym, Side::SELL, 110.0, 1.0, kT0 + kSec));
  r.markPrice(kSym, 120.0);

  const auto s = r.computeStats();
  EXPECT_EQ(s.unrealizedPnl, 0.0);
  EXPECT_NEAR(s.finalEquity, s.finalCapital, kEps);
  EXPECT_TRUE(r.openPositions().empty());
}

TEST(OpenPositionAtEnd, LongIsMarkedAtTheLastMarkAndTheOtherFieldsDoNotMove)
{
  BacktestResult r(makeConfig());
  r.recordFill(fill(kSym, Side::BUY, 100.0, 1.0, kT0));
  r.recordFill(fill(kSym, Side::SELL, 110.0, 1.0, kT0 + kSec));
  r.recordFill(fill(kSym, Side::BUY, 105.0, 2.0, kT0 + 2 * kSec));
  BacktestResult unmarked = r;
  r.markPrice(kSym, 112.0);
  r.markPrice(kSym, 108.5);  // the last mark wins

  const auto s = r.computeStats();
  // (108.5 - 105) * 2
  EXPECT_NEAR(s.unrealizedPnl, 7.0, kEps);
  EXPECT_NEAR(s.finalEquity, s.initialCapital + s.netPnl + 7.0, kEps);

  // Unchanged: closed trades only, every fill's fee.
  const auto base = unmarked.computeStats();
  EXPECT_EQ(s.totalTrades, 1u);
  EXPECT_DOUBLE_EQ(s.totalPnl, base.totalPnl);
  EXPECT_DOUBLE_EQ(s.totalFees, base.totalFees);
  EXPECT_DOUBLE_EQ(s.netPnl, base.netPnl);
  EXPECT_DOUBLE_EQ(s.finalCapital, base.finalCapital);
  EXPECT_DOUBLE_EQ(s.returnPct, base.returnPct);
  EXPECT_NEAR(s.totalFees, (100.0 + 110.0 + 210.0) * 0.0004, kEps);
  EXPECT_EQ(r.trades().size(), 1u);
  EXPECT_EQ(r.equityCurve().size(), 1u);

  const auto open = r.openPositions();
  ASSERT_EQ(open.size(), 1u);
  EXPECT_EQ(open[0].symbol, kSym);
  EXPECT_EQ(open[0].side, Side::BUY);
  EXPECT_NEAR(open[0].quantity.toDouble(), 2.0, kEps);
  EXPECT_NEAR(open[0].entryPrice.toDouble(), 105.0, kEps);
  EXPECT_NEAR(open[0].markPrice.toDouble(), 108.5, kEps);
  EXPECT_NEAR(open[0].unrealizedPnl.toDouble(), 7.0, kEps);
}

TEST(OpenPositionAtEnd, ShortGainsWhenTheMarkIsBelowTheEntry)
{
  BacktestResult r(makeConfig());
  r.recordFill(fill(kSym, Side::SELL, 200.0, 3.0, kT0));
  r.markPrice(kSym, 190.0);

  const auto s = r.computeStats();
  // (200 - 190) * 3
  EXPECT_NEAR(s.unrealizedPnl, 30.0, kEps);
  EXPECT_NEAR(s.finalEquity, s.initialCapital + s.netPnl + 30.0, kEps);

  const auto open = r.openPositions();
  ASSERT_EQ(open.size(), 1u);
  EXPECT_EQ(open[0].side, Side::SELL);
  EXPECT_NEAR(open[0].quantity.toDouble(), 3.0, kEps);
  EXPECT_NEAR(open[0].entryPrice.toDouble(), 200.0, kEps);
  EXPECT_NEAR(open[0].markPrice.toDouble(), 190.0, kEps);
  EXPECT_NEAR(open[0].unrealizedPnl.toDouble(), 30.0, kEps);
}

TEST(OpenPositionAtEnd, ShortLosesWhenTheMarkIsAboveTheEntry)
{
  BacktestResult r(makeConfig());
  r.recordFill(fill(kSym, Side::SELL, 200.0, 3.0, kT0));
  r.markPrice(kSym, 204.0);
  EXPECT_NEAR(r.computeStats().unrealizedPnl, -12.0, kEps);
}

// No trade closed: the existing statistics stay zero, as documented, but the
// open position is still valued.
TEST(OpenPositionAtEnd, NoClosedTradeStillValuesTheOpenPosition)
{
  BacktestResult r(makeConfig());
  r.recordFill(fill(kSym, Side::BUY, 100.0, 1.0, kT0));
  r.markPrice(kSym, 95.0);

  const auto s = r.computeStats();
  EXPECT_EQ(s.totalTrades, 0u);
  EXPECT_EQ(s.netPnl, 0.0);
  EXPECT_NEAR(s.unrealizedPnl, -5.0, kEps);
  EXPECT_NEAR(s.finalEquity, s.initialCapital + s.netPnl - 5.0, kEps);
  EXPECT_EQ(r.openPositions().size(), 1u);
}

// Adds average the entry; a partial close keeps the average and the rest open.
TEST(OpenPositionAtEnd, EntryIsTheAverageAfterAddsAndAPartialClose)
{
  BacktestResult r(makeConfig(0.0));
  r.recordFill(fill(kSym, Side::BUY, 100.0, 1.0, kT0));
  r.recordFill(fill(kSym, Side::BUY, 110.0, 1.0, kT0 + kSec));
  r.recordFill(fill(kSym, Side::SELL, 120.0, 0.5, kT0 + 2 * kSec));
  r.markPrice(kSym, 108.0);

  const auto open = r.openPositions();
  ASSERT_EQ(open.size(), 1u);
  EXPECT_NEAR(open[0].quantity.toDouble(), 1.5, kEps);
  EXPECT_NEAR(open[0].entryPrice.toDouble(), 105.0, kEps);
  // (108 - 105) * 1.5
  EXPECT_NEAR(r.computeStats().unrealizedPnl, 4.5, kEps);
}

TEST(OpenPositionAtEnd, UnmarkedSymbolIsValuedAtItsLastFill)
{
  BacktestResult r(makeConfig(0.0));
  r.recordFill(fill(kSym, Side::BUY, 100.0, 2.0, kT0));
  r.recordFill(fill(kSym, Side::SELL, 104.0, 1.0, kT0 + kSec));

  const auto open = r.openPositions();
  ASSERT_EQ(open.size(), 1u);
  EXPECT_NEAR(open[0].markPrice.toDouble(), 104.0, kEps);
  // (104 - 100) * 1
  EXPECT_NEAR(r.computeStats().unrealizedPnl, 4.0, kEps);
}

TEST(OpenPositionAtEnd, SymbolsAreSummedAndListedInSymbolOrder)
{
  constexpr SymbolId kSym3 = 3;
  BacktestResult r(makeConfig());
  r.recordFill(fill(kSym2, Side::SELL, 50.0, 2.0, kT0));
  r.recordFill(fill(kSym, Side::BUY, 100.0, 1.0, kT0 + kSec));
  r.recordFill(fill(kSym3, Side::BUY, 10.0, 1.0, kT0 + 2 * kSec));
  r.recordFill(fill(kSym3, Side::SELL, 11.0, 1.0, kT0 + 3 * kSec));
  r.markPrice(kSym, 110.0);   // +10
  r.markPrice(kSym2, 47.0);   // +6
  r.markPrice(kSym3, 999.0);  // flat: nothing

  const auto s = r.computeStats();
  EXPECT_NEAR(s.unrealizedPnl, 16.0, kEps);
  EXPECT_NEAR(s.finalEquity, s.initialCapital + s.netPnl + 16.0, kEps);

  const auto open = r.openPositions();
  ASSERT_EQ(open.size(), 2u);
  EXPECT_EQ(open[0].symbol, kSym);
  EXPECT_EQ(open[1].symbol, kSym2);
  EXPECT_NEAR(open[0].unrealizedPnl.toDouble(), 10.0, kEps);
  EXPECT_NEAR(open[1].unrealizedPnl.toDouble(), 6.0, kEps);
  EXPECT_EQ(open[1].side, Side::SELL);
}

// ── Runners: which price is the mark ────────────────────────────────────

// Bar replay: an order from bar 0 fills at bar 1's open; the mark is the
// last bar's close.
TEST(OpenPositionAtEnd, RunBarsMarksAtTheLastBarClose)
{
  for (const bool longSide : {true, false})
  {
    Probe p;
    p.onBarHook = [longSide](Probe& s, const BarEvent&, size_t i)
    {
      if (i == 0)
      {
        longSide ? s.emitMarketBuy(kSym, Quantity::fromDouble(2.0))
                 : s.emitMarketSell(kSym, Quantity::fromDouble(2.0));
      }
    };
    BacktestRunner runner(makeConfig());
    runner.setStrategy(&p);
    const std::vector<BarEvent> bars = {
        makeBar(kSym, 100.0, 100.5, 99.5, 100.0, kT0),
        makeBar(kSym, 101.0, 103.0, 100.0, 102.0, kT0 + kMinuteNs),
        makeBar(kSym, 103.0, 108.0, 96.0, 97.0, kT0 + 2 * kMinuteNs),
    };
    const BacktestResult r = runner.runBars(bars);
    ASSERT_EQ(r.fills().size(), 1u);
    EXPECT_NEAR(fillPrice(r, 0), 101.0, kEps);

    const auto s = r.computeStats();
    // long: (97 - 101) * 2; short: (101 - 97) * 2
    const double expected = longSide ? -8.0 : 8.0;
    EXPECT_NEAR(s.unrealizedPnl, expected, kEps) << (longSide ? "long" : "short");
    EXPECT_NEAR(s.finalEquity, s.initialCapital + s.netPnl + expected, kEps);
    const auto open = r.openPositions();
    ASSERT_EQ(open.size(), 1u);
    EXPECT_NEAR(open[0].markPrice.toDouble(), 97.0, kEps);
    EXPECT_EQ(open[0].side, longSide ? Side::BUY : Side::SELL);
  }
}

TEST(OpenPositionAtEnd, RunBarsEndingFlatHasNoUnrealized)
{
  Probe p;
  p.onBarHook = [](Probe& s, const BarEvent&, size_t i)
  {
    if (i == 0)
    {
      s.emitMarketBuy(kSym, Quantity::fromDouble(1.0));
    }
    else if (i == 1)
    {
      s.emitMarketSell(kSym, Quantity::fromDouble(1.0));
    }
  };
  BacktestRunner runner(makeConfig());
  runner.setStrategy(&p);
  const std::vector<BarEvent> bars = {
      makeBar(kSym, 100.0, 100.5, 99.5, 100.0, kT0),
      makeBar(kSym, 101.0, 103.0, 100.0, 102.0, kT0 + kMinuteNs),
      makeBar(kSym, 103.0, 108.0, 96.0, 97.0, kT0 + 2 * kMinuteNs),
  };
  const BacktestResult r = runner.runBars(bars);
  ASSERT_EQ(r.fills().size(), 2u);
  const auto s = r.computeStats();
  EXPECT_EQ(s.unrealizedPnl, 0.0);
  EXPECT_NEAR(s.finalEquity, s.finalCapital, kEps);
  EXPECT_TRUE(r.openPositions().empty());
}

// Tick replay: the mark is the last trade on the tape, and a book record
// after it does not move it.
TEST(OpenPositionAtEnd, RunTapeMarksAtTheLastTradeNotTheBook)
{
  TapeDir tape("tick");
  tape.write(threeMinuteTape(), {{kT0 + 151 * kSec, 199.0, 201.0}});

  Probe p;
  p.onTradeHook = [](Probe& s, const TradeEvent&, size_t i)
  {
    if (i == 0)
    {
      s.emitMarketBuy(kSym, Quantity::fromDouble(1.0));
    }
  };
  BacktestRunner runner(makeConfig());
  runner.setStrategy(&p);
  const BacktestResult r = runner.runTape(tape.path());
  ASSERT_EQ(r.fills().size(), 1u);
  const double entry = fillPrice(r, 0);

  const auto open = r.openPositions();
  ASSERT_EQ(open.size(), 1u);
  EXPECT_NEAR(open[0].markPrice.toDouble(), 110.0, kEps);
  EXPECT_NEAR(open[0].entryPrice.toDouble(), entry, kEps);
  EXPECT_NEAR(r.computeStats().unrealizedPnl, 110.0 - entry, kEps);
}

// runTapeBars feeds the executor bars: the bar still open when the tape ends
// is never replayed, so the run's last price is the close of bar 1 (103),
// not the tape's last trade (110).
TEST(OpenPositionAtEnd, RunTapeBarsMarksAtTheLastReplayedBarClose)
{
  TapeDir tape("bars");
  tape.write(threeMinuteTape());

  Probe p;
  p.onBarHook = [](Probe& s, const BarEvent&, size_t i)
  {
    if (i == 0)
    {
      s.emitMarketBuy(kSym, Quantity::fromDouble(1.0));
    }
  };
  BacktestRunner runner(makeConfig());
  runner.setStrategy(&p);
  const BacktestResult r = runner.runTapeBars(tape.path(), std::chrono::seconds(60));
  ASSERT_EQ(p.barCount, 2u);
  ASSERT_EQ(r.fills().size(), 1u);
  EXPECT_NEAR(fillPrice(r, 0), 102.0, kEps) << "bar 1's open";

  const auto open = r.openPositions();
  ASSERT_EQ(open.size(), 1u);
  EXPECT_NEAR(open[0].markPrice.toDouble(), 103.0, kEps);
  EXPECT_NEAR(r.computeStats().unrealizedPnl, 1.0, kEps);
}

// runTapeBarsOnTicks decides on the same bars but feeds the executor every
// trade, so the mark is the tape's last trade.
TEST(OpenPositionAtEnd, RunTapeBarsOnTicksMarksAtTheLastTrade)
{
  TapeDir tape("bars_on_ticks");
  tape.write(threeMinuteTape());

  Probe p;
  p.onBarHook = [](Probe& s, const BarEvent&, size_t i)
  {
    if (i == 0)
    {
      s.emitMarketBuy(kSym, Quantity::fromDouble(1.0));
    }
  };
  BacktestRunner runner(makeConfig());
  runner.setStrategy(&p);
  const BacktestResult r = runner.runTapeBarsOnTicks(tape.path(), std::chrono::seconds(60));
  ASSERT_EQ(r.fills().size(), 1u);
  EXPECT_NEAR(fillPrice(r, 0), 102.0, kEps) << "the first trade after bar 0's end";

  const auto open = r.openPositions();
  ASSERT_EQ(open.size(), 1u);
  EXPECT_NEAR(open[0].markPrice.toDouble(), 110.0, kEps);
  EXPECT_NEAR(r.computeStats().unrealizedPnl, 8.0, kEps);
}

// Two symbols on one tape: each is marked at its own last trade and the
// stats sum them.
TEST(OpenPositionAtEnd, RunTapeSumsOpenPositionsAcrossSymbols)
{
  TapeDir tape("two_symbols");
  tape.write({
      {kT0 + 1 * kSec, 100.0, kSym},
      {kT0 + 2 * kSec, 50.0, kSym2},
      {kT0 + 3 * kSec, 101.0, kSym},
      {kT0 + 4 * kSec, 51.0, kSym2},
      {kT0 + 5 * kSec, 104.0, kSym},
      {kT0 + 6 * kSec, 48.0, kSym2},
  });

  Probe p({kSym, kSym2});
  bool boughtBtc = false;
  bool soldEth = false;
  p.onTradeHook = [&](Probe& s, const TradeEvent& ev, size_t)
  {
    if (ev.trade.symbol == kSym && !boughtBtc)
    {
      boughtBtc = true;
      s.emitMarketBuy(kSym, Quantity::fromDouble(1.0));
    }
    else if (ev.trade.symbol == kSym2 && !soldEth)
    {
      soldEth = true;
      s.emitMarketSell(kSym2, Quantity::fromDouble(2.0));
    }
  };
  BacktestRunner runner(makeConfig());
  runner.setStrategy(&p);
  const BacktestResult r = runner.runTape(tape.path());
  ASSERT_EQ(r.fills().size(), 2u);

  double expected = 0.0;
  for (const auto& f : r.fills())
  {
    expected += f.symbol == kSym ? (104.0 - f.price.toDouble()) * 1.0
                                 : (f.price.toDouble() - 48.0) * 2.0;
  }
  const auto s = r.computeStats();
  EXPECT_NEAR(s.unrealizedPnl, expected, kEps);
  EXPECT_NEAR(s.finalEquity, s.initialCapital + s.netPnl + expected, kEps);

  const auto open = r.openPositions();
  ASSERT_EQ(open.size(), 2u);
  ASSERT_NE(findOpen(open, kSym), nullptr);
  ASSERT_NE(findOpen(open, kSym2), nullptr);
  EXPECT_NEAR(findOpen(open, kSym)->markPrice.toDouble(), 104.0, kEps);
  EXPECT_NEAR(findOpen(open, kSym2)->markPrice.toDouble(), 48.0, kEps);
  EXPECT_EQ(findOpen(open, kSym2)->side, Side::SELL);
}

// A second run on the same runner reports its own open position, not one
// left over from the first run's marks.
TEST(OpenPositionAtEnd, ASecondRunDoesNotInheritTheFirstRunsMarks)
{
  Probe p;
  p.onBarHook = [](Probe& s, const BarEvent&, size_t i)
  {
    if (i == 0)
    {
      s.emitMarketBuy(kSym, Quantity::fromDouble(1.0));
    }
  };
  BacktestRunner runner(makeConfig());
  runner.setStrategy(&p);
  runner.runBars({makeBar(kSym, 100.0, 100.0, 100.0, 100.0, kT0),
                  makeBar(kSym, 101.0, 130.0, 101.0, 130.0, kT0 + kMinuteNs)});
  p.barCount = 0;
  const BacktestResult r =
      runner.runBars({makeBar(kSym, 100.0, 100.0, 100.0, 100.0, kT0),
                      makeBar(kSym, 101.0, 102.0, 100.0, 102.0, kT0 + kMinuteNs)});
  const auto open = r.openPositions();
  ASSERT_EQ(open.size(), 1u);
  EXPECT_NEAR(open[0].markPrice.toDouble(), 102.0, kEps);
  EXPECT_NEAR(r.computeStats().unrealizedPnl, 1.0, kEps);
}
