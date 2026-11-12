/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// BacktestRunner::runTapeBarsOnTicks: the strategy decides on time bars built
// from the tape, the executor matches on the tape's own records. The rule
// pinned here: an order sent from onBar for bar n can first match against the
// first trade stamped at or after the end of bar n.

#include "lrvx/backtest/backtest_runner.h"
#include "lrvx/book/events/trade_event.h"
#include "lrvx/engine/symbol_registry.h"
#include "lrvx/replay/binary_format_v1.h"
#include "lrvx/replay/writers/binary_log_writer.h"
#include "lrvx/strategy/strategy.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <vector>

using namespace lrvx;

namespace
{

constexpr int64_t kMs = 1'000'000;
constexpr int64_t kSec = 1'000'000'000;
constexpr int64_t kT0 = 1'767'225'600LL * kSec;  // 2026-01-01T00:00:00Z, a minute boundary
constexpr auto kMinute = std::chrono::seconds(60);

struct T
{
  int64_t ts;
  double px;
  double qty{1.0};
  uint8_t side{1};
};

struct Book
{
  int64_t ts;
  double bid;
  double ask;
  double size{10.0};
};

class TapeDir
{
 public:
  explicit TapeDir(const std::string& tag)
  {
    _dir = std::filesystem::temp_directory_path() /
           ("lrvx_bars_on_ticks_" + tag + "_" + std::to_string(std::random_device{}()));
    std::filesystem::remove_all(_dir);
    std::filesystem::create_directories(_dir);
  }
  ~TapeDir() { std::filesystem::remove_all(_dir); }
  const std::filesystem::path& path() const { return _dir; }

  // One segment per call; records are written in timestamp order, trades
  // before books at equal timestamps.
  void segment(std::vector<T> trades, std::vector<Book> books = {}, SymbolId sym = 1)
  {
    std::stable_sort(trades.begin(), trades.end(),
                     [](const T& a, const T& b)
                     { return a.ts < b.ts; });
    std::stable_sort(books.begin(), books.end(),
                     [](const Book& a, const Book& b)
                     { return a.ts < b.ts; });
    replay::WriterConfig cfg;
    cfg.output_dir = _dir;
    replay::BinaryLogWriter writer(cfg);
    size_t ti = 0;
    size_t bi = 0;
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
        r.qty_raw = Quantity::fromDouble(t.qty).raw();
        r.symbol_id = sym;
        r.side = t.side;
        r.trade_id = ++_tradeId;
        writer.writeTrade(r);
      }
      else
      {
        const Book& b = books[bi++];
        replay::BookRecordHeader hdr{};
        hdr.exchange_ts_ns = b.ts;
        hdr.recv_ts_ns = b.ts;
        hdr.symbol_id = sym;
        hdr.type = 0;
        hdr.bid_count = 1;
        hdr.ask_count = 1;
        std::vector<replay::BookLevel> bids = {
            {Price::fromDouble(b.bid).raw(), Quantity::fromDouble(b.size).raw()}};
        std::vector<replay::BookLevel> asks = {
            {Price::fromDouble(b.ask).raw(), Quantity::fromDouble(b.size).raw()}};
        writer.writeBook(hdr, bids, asks);
      }
    }
    writer.close();
  }

 private:
  std::filesystem::path _dir;
  uint64_t _tradeId{0};
};

SymbolRegistry& registry()
{
  static SymbolRegistry* reg = []
  {
    auto* r = new SymbolRegistry();
    SymbolInfo info;
    info.exchange = "test";
    info.symbol = "BTCUSDT";
    info.type = InstrumentType::Spot;
    info.tickSize = Price::fromDouble(0.01);
    r->registerSymbol(info);
    return r;
  }();
  return *reg;
}

class Probe : public Strategy
{
 public:
  explicit Probe(SymbolId sym = 1) : Strategy(1, std::vector<SymbolId>{sym}, registry()), sym(sym) {}

  SymbolId sym;
  std::vector<BarEvent> bars;
  std::vector<int64_t> barClockNs;
  size_t tradesSeen{0};
  std::function<void(Probe&, const BarEvent&, size_t)> onBarHook;
  std::function<void(Probe&, const TradeEvent&)> onTradeHook;
  IClock* clock{nullptr};

  using Strategy::emitMarketBuy;
  using Strategy::emitMarketSell;
  using Strategy::emitStopMarket;

 protected:
  void onSymbolBar(SymbolContext&, const BarEvent& ev) override
  {
    bars.push_back(ev);
    if (clock != nullptr)
    {
      barClockNs.push_back(clock->nowNs().raw());
    }
    if (onBarHook)
    {
      onBarHook(*this, ev, bars.size() - 1);
    }
  }
  void onSymbolTrade(SymbolContext&, const TradeEvent& ev) override
  {
    ++tradesSeen;
    if (onTradeHook)
    {
      onTradeHook(*this, ev);
    }
  }
};

double px(const Fill& f) { return f.price.toDouble(); }

// Bar 0 is [kT0, kT0 + 60 s). Its last print is one nanosecond before the
// end; the next trade is stamped exactly at the end.
std::vector<T> boundaryTape(int64_t boundaryTs)
{
  return {
      {kT0 + 1 * kSec, 100.0},
      {kT0 + 30 * kSec, 100.5},
      {kT0 + 60 * kSec - 1, 101.0},
      {boundaryTs, 102.0},
      {boundaryTs + 500 * kMs, 103.0},
      {kT0 + 125 * kSec, 104.0},
  };
}

void buyOnFirstBar(Probe& p)
{
  p.onBarHook = [](Probe& s, const BarEvent&, size_t i)
  {
    if (i == 0)
    {
      s.emitMarketBuy(s.sym, Quantity::fromDouble(1.0));
    }
  };
}

}  // namespace

TEST(RunTapeBarsOnTicks, OrderFromBarCallbackFirstMatchesTheTradeStampedExactlyAtTheEnd)
{
  TapeDir tape("exact");
  tape.segment(boundaryTape(kT0 + 60 * kSec));

  Probe p;
  buyOnFirstBar(p);
  BacktestRunner runner;
  p.clock = &runner.clock();
  runner.setStrategy(&p);
  runner.runTapeBarsOnTicks(tape.path(), kMinute);

  // Bar 0 holds every print before the end, the one at end - 1 ns included,
  // and nothing from the boundary trade on.
  ASSERT_EQ(p.bars.size(), 2u);
  EXPECT_DOUBLE_EQ(p.bars[0].bar.close.toDouble(), 101.0);
  EXPECT_EQ(p.bars[0].bar.tradeCount.raw(), Quantity::fromRaw(3).raw());
  EXPECT_EQ(p.bars[0].bar.endTime.time_since_epoch().count(), kT0 + 60 * kSec);
  // The callback runs at the bar's end, not at the trade that revealed it.
  ASSERT_EQ(p.barClockNs.size(), 2u);
  EXPECT_EQ(p.barClockNs[0], kT0 + 60 * kSec);

  const auto& fills = runner.executor().fills();
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_DOUBLE_EQ(px(fills[0]), 102.0) << "matched against a print from inside the bar";
  EXPECT_EQ(fills[0].timestampNs.raw(), kT0 + 60 * kSec);
  EXPECT_EQ(p.tradesSeen, 0u) << "onTrade is not part of this mode";
}

TEST(RunTapeBarsOnTicks, OrderFromBarCallbackFirstMatchesTheFirstTradeAfterTheEnd)
{
  TapeDir tape("after");
  tape.segment(boundaryTape(kT0 + 60 * kSec + 1));

  Probe p;
  buyOnFirstBar(p);
  BacktestRunner runner;
  p.clock = &runner.clock();
  runner.setStrategy(&p);
  runner.runTapeBarsOnTicks(tape.path(), kMinute);

  ASSERT_GE(p.bars.size(), 1u);
  EXPECT_DOUBLE_EQ(p.bars[0].bar.close.toDouble(), 101.0);
  EXPECT_EQ(p.barClockNs[0], kT0 + 60 * kSec);
  const auto& fills = runner.executor().fills();
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_DOUBLE_EQ(px(fills[0]), 102.0);
  EXPECT_EQ(fills[0].timestampNs.raw(), kT0 + 60 * kSec + 1);
}

TEST(RunTapeBarsOnTicks, TheLastStillOpenBarIsNotDelivered)
{
  TapeDir tape("open");
  tape.segment(boundaryTape(kT0 + 60 * kSec));
  Probe p;
  BacktestRunner runner;
  runner.setStrategy(&p);
  runner.runTapeBarsOnTicks(tape.path(), kMinute);
  // Bars 0 and 1 close; bar 2 (the 125 s print) is still open at the end.
  ASSERT_EQ(p.bars.size(), 2u);
  EXPECT_EQ(p.bars[1].bar.startTime.time_since_epoch().count(), kT0 + 60 * kSec);
  EXPECT_DOUBLE_EQ(p.bars[1].bar.close.toDouble(), 103.0);
}

// A strategy that buys a bar which closed up and sells on the next bar. It
// makes money only if it trades inside the up-move it is reacting to, which it
// can do only by seeing the bar before the bar is over.
TEST(RunTapeBarsOnTicks, NoLookahead)
{
  TapeDir tape("lookahead");
  std::vector<T> trades;
  for (int i = 0; i < 59; ++i)
  {
    trades.push_back({kT0 + i * kSec, 100.0});  // bar 0, flat
  }
  for (int i = 0; i < 60; ++i)
  {
    trades.push_back({kT0 + 60 * kSec + i * kSec, 100.0 + i * (100.0 / 59)});  // bar 1, 100 -> 200
  }
  for (int i = 0; i < 120; ++i)
  {
    trades.push_back({kT0 + 120 * kSec + i * kSec, 200.0});  // bars 2-3, flat at the top
  }
  trades.push_back({kT0 + 240 * kSec, 200.0});
  tape.segment(trades);

  Probe p;
  bool holding = false;
  p.onBarHook = [&holding](Probe& s, const BarEvent& ev, size_t)
  {
    if (!holding && ev.bar.close > ev.bar.open)
    {
      s.emitMarketBuy(s.sym, Quantity::fromDouble(1.0));
      holding = true;
    }
    else if (holding)
    {
      s.emitMarketSell(s.sym, Quantity::fromDouble(1.0));
      holding = false;
    }
  };
  BacktestConfig cfg;
  cfg.feeRate = 0.0;
  BacktestRunner runner(cfg);
  runner.setStrategy(&p);
  runner.runTapeBarsOnTicks(tape.path(), kMinute);

  const auto& fills = runner.executor().fills();
  ASSERT_EQ(fills.size(), 2u);
  EXPECT_DOUBLE_EQ(px(fills[0]), 200.0) << "bought inside the bar it reacted to";
  EXPECT_DOUBLE_EQ(px(fills[1]), 200.0);
  EXPECT_LE(px(fills[1]) - px(fills[0]), 0.0);
}

// A stop armed from one bar callback fires on the print that crosses it inside
// a later bar and fills at that print, not at the bar's low. A conditional
// never fills better than its trigger: here the book still shows a bid above
// the trigger when the print crosses it.
TEST(RunTapeBarsOnTicks, ArmedStopFillsOnThePrintAndNeverBetterThanItsTrigger)
{
  TapeDir tape("stop");
  tape.segment(
      {
          {kT0 + 10 * kSec, 100.0},
          {kT0 + 60 * kSec, 100.0},  // closes bar 0; the entry and the stop meet it
          {kT0 + 70 * kSec, 96.0},
          {kT0 + 80 * kSec, 94.0},  // crosses the 95 trigger
          {kT0 + 90 * kSec, 90.0},  // bar 1's low
          {kT0 + 125 * kSec, 91.0},
      },
      {
          {kT0 + 65 * kSec, 97.0, 97.5},  // stale bid above the trigger
      });

  Probe p;
  p.onBarHook = [](Probe& s, const BarEvent&, size_t i)
  {
    if (i == 0)
    {
      s.emitMarketBuy(s.sym, Quantity::fromDouble(1.0));
      s.emitStopMarket(s.sym, Side::SELL, Price::fromDouble(95.0), Quantity::fromDouble(1.0));
    }
  };
  BacktestRunner runner;
  runner.setStrategy(&p);
  runner.runTapeBarsOnTicks(tape.path(), kMinute);

  const auto& fills = runner.executor().fills();
  ASSERT_EQ(fills.size(), 2u);
  EXPECT_DOUBLE_EQ(px(fills[0]), 100.0);
  EXPECT_EQ(fills[1].side, Side::SELL);
  EXPECT_DOUBLE_EQ(px(fills[1]), 95.0) << "a stop must not fill better than its trigger";
  EXPECT_EQ(fills[1].timestampNs.raw(), kT0 + 80 * kSec) << "the stop fires on the crossing print";
}

TEST(RunTapeBarsOnTicks, ArmedStopOnATradeOnlyTapePaysThePrintThatCrossedIt)
{
  TapeDir tape("stop_trades");
  tape.segment({
      {kT0 + 10 * kSec, 100.0},
      {kT0 + 60 * kSec, 100.0},
      {kT0 + 70 * kSec, 96.0},
      {kT0 + 80 * kSec, 94.5},
      {kT0 + 90 * kSec, 90.0},
      {kT0 + 125 * kSec, 91.0},
  });
  auto entryAndStop = [](Probe& s, const BarEvent&, size_t i)
  {
    if (i == 0)
    {
      s.emitMarketBuy(s.sym, Quantity::fromDouble(1.0));
      s.emitStopMarket(s.sym, Side::SELL, Price::fromDouble(95.0), Quantity::fromDouble(1.0));
    }
  };

  Probe onTicks;
  onTicks.onBarHook = entryAndStop;
  BacktestRunner tickRunner;
  tickRunner.setStrategy(&onTicks);
  tickRunner.runTapeBarsOnTicks(tape.path(), kMinute);
  ASSERT_EQ(tickRunner.executor().fills().size(), 2u);
  EXPECT_DOUBLE_EQ(px(tickRunner.executor().fills()[1]), 94.5);

  // Bar replay of the same strategy books the stop at the bar's low.
  Probe onBars;
  onBars.onBarHook = entryAndStop;
  BacktestRunner barRunner;
  barRunner.setStrategy(&onBars);
  barRunner.runTapeBars(tape.path(), kMinute);
  ASSERT_EQ(barRunner.executor().fills().size(), 2u);
  EXPECT_DOUBLE_EQ(px(barRunner.executor().fills()[1]), 90.0);
}

// Time exists inside a bar here: a 5 ms submit latency moves the fill to the
// first print 5 ms after the bar's end, not to the next bar.
TEST(RunTapeBarsOnTicks, SubmitLatencyMovesTheFillByMilliseconds)
{
  TapeDir tape("latency");
  tape.segment({
      {kT0 + 10 * kSec, 100.0},
      {kT0 + 60 * kSec, 101.0},            // boundary trade
      {kT0 + 60 * kSec + 4 * kMs, 102.0},  // inside the latency window
      {kT0 + 60 * kSec + 5 * kMs, 103.0},  // first print at end + 5 ms
      {kT0 + 61 * kSec, 104.0},
      {kT0 + 125 * kSec, 105.0},
  });
  BacktestConfig cfg;
  cfg.submitAckLatencyNs = 5 * kMs;
  Probe p;
  buyOnFirstBar(p);
  BacktestRunner runner(cfg);
  runner.setStrategy(&p);
  runner.runTapeBarsOnTicks(tape.path(), kMinute);

  const auto& fills = runner.executor().fills();
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_DOUBLE_EQ(px(fills[0]), 103.0);
  EXPECT_EQ(fills[0].timestampNs.raw(), kT0 + 60 * kSec + 5 * kMs);
}

namespace
{

std::vector<T> randomTrades(uint64_t seed, int64_t start, int n)
{
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<int64_t> gap(1, 4 * kSec);
  std::uniform_int_distribution<int> step(-20, 20);
  std::uniform_int_distribution<int> qty(1, 50);
  std::vector<T> out;
  int64_t t = start;
  int64_t cents = 10'000;
  for (int i = 0; i < n; ++i)
  {
    t += gap(rng);
    cents += step(rng);
    out.push_back({t, cents / 100.0, qty(rng) / 10.0, static_cast<uint8_t>(i % 3 == 0 ? 0 : 1)});
  }
  return out;
}

// Bars in the order trades closed them, as onBar sees them, compared field by
// field.
void expectSameBars(const std::vector<BarEvent>& a, const std::vector<BarEvent>& b)
{
  ASSERT_EQ(a.size(), b.size());
  for (size_t i = 0; i < a.size(); ++i)
  {
    SCOPED_TRACE(i);
    EXPECT_EQ(a[i].symbol, b[i].symbol);
    EXPECT_EQ(a[i].instrument, b[i].instrument);
    EXPECT_EQ(a[i].barType, b[i].barType);
    EXPECT_EQ(a[i].barTypeParam, b[i].barTypeParam);
    EXPECT_EQ(a[i].bar.open.raw(), b[i].bar.open.raw());
    EXPECT_EQ(a[i].bar.high.raw(), b[i].bar.high.raw());
    EXPECT_EQ(a[i].bar.low.raw(), b[i].bar.low.raw());
    EXPECT_EQ(a[i].bar.close.raw(), b[i].bar.close.raw());
    EXPECT_EQ(a[i].bar.volume.raw(), b[i].bar.volume.raw());
    EXPECT_EQ(a[i].bar.buyVolume.raw(), b[i].bar.buyVolume.raw());
    EXPECT_EQ(a[i].bar.tradeCount.raw(), b[i].bar.tradeCount.raw());
    EXPECT_EQ(a[i].bar.startTime, b[i].bar.startTime);
    EXPECT_EQ(a[i].bar.endTime, b[i].bar.endTime);
    EXPECT_EQ(a[i].bar.reason, b[i].bar.reason);
  }
}

void expectSameFills(const std::vector<Fill>& a, const std::vector<Fill>& b)
{
  ASSERT_EQ(a.size(), b.size());
  for (size_t i = 0; i < a.size(); ++i)
  {
    SCOPED_TRACE(i);
    EXPECT_EQ(a[i].side, b[i].side);
    EXPECT_EQ(a[i].price.raw(), b[i].price.raw());
    EXPECT_EQ(a[i].quantity.raw(), b[i].quantity.raw());
    EXPECT_EQ(a[i].timestampNs.raw(), b[i].timestampNs.raw());
    EXPECT_EQ(a[i].isMaker, b[i].isMaker);
  }
}

// Market orders on a fixed bar schedule: buy on bars 1, 7, 13, ..., sell on
// bars 4, 10, 16, ...
void scheduledOrder(Probe& s, size_t barIndex)
{
  if (barIndex % 6 == 1)
  {
    s.emitMarketBuy(s.sym, Quantity::fromDouble(0.5));
  }
  else if (barIndex % 6 == 4)
  {
    s.emitMarketSell(s.sym, Quantity::fromDouble(0.5));
  }
}

}  // namespace

TEST(RunTapeBarsOnTicks, BarsAreTheBarsRunTapeBarsDelivers)
{
  TapeDir tape("same_bars");
  for (int k : {2, 0, 1})
  {
    tape.segment(randomTrades(30 + k, kT0 + k * 3600 * kSec, 1500));
  }
  Probe a;
  BacktestRunner ra;
  ra.setStrategy(&a);
  ra.runTapeBars(tape.path(), kMinute);

  Probe b;
  BacktestRunner rb;
  rb.setStrategy(&b);
  rb.runTapeBarsOnTicks(tape.path(), kMinute);

  ASSERT_GT(a.bars.size(), 100u);
  expectSameBars(a.bars, b.bars);
}

// The same market orders, submitted by a tick strategy from onTrade of the
// first trade at or after each bar's end, fill identically under tick replay.
TEST(RunTapeBarsOnTicks, MarketOrdersFillAsTickReplayFillsThemAtTheBoundaryTrade)
{
  TapeDir tape("equiv");
  tape.segment(randomTrades(7, kT0, 4000));

  Probe onBars;
  onBars.onBarHook = [](Probe& s, const BarEvent&, size_t i)
  { scheduledOrder(s, i); };
  BacktestConfig cfg;
  cfg.defaultSlippage.model = SlippageModel::FIXED_BPS;
  cfg.defaultSlippage.bps = 3.0;
  BacktestRunner rb(cfg);
  rb.setStrategy(&onBars);
  rb.runTapeBarsOnTicks(tape.path(), kMinute);

  // The tick strategy closes its own buckets: when a trade lands in a later
  // minute than the live bucket, the live bucket is bar n and this trade is
  // the first one at or after its end.
  Probe onTicks;
  int64_t liveBucket = -1;
  size_t barIndex = 0;
  onTicks.onTradeHook = [&](Probe& s, const TradeEvent& ev)
  {
    const int64_t bucket = ev.trade.exchangeTsNs.raw() / (60 * kSec);
    if (liveBucket < 0)
    {
      liveBucket = bucket;
      return;
    }
    if (bucket > liveBucket)
    {
      scheduledOrder(s, barIndex++);
      liveBucket = bucket;
    }
  };
  BacktestRunner rt(cfg);
  rt.setStrategy(&onTicks);
  rt.runTape(tape.path());

  ASSERT_GT(onBars.bars.size(), 50u);
  EXPECT_EQ(barIndex, onBars.bars.size());
  ASSERT_GT(rb.executor().fills().size(), 10u);
  expectSameFills(rb.executor().fills(), rt.executor().fills());
}

TEST(RunTapeBarsOnTicks, SegmentsOutOfFileNameOrderGiveTheSameRun)
{
  std::vector<std::vector<T>> parts;
  for (int k = 0; k < 3; ++k)
  {
    parts.push_back(randomTrades(50 + k, kT0 + k * 3600 * kSec, 1200));
  }
  TapeDir inOrder("in_order");
  for (int k : {0, 1, 2})
  {
    inOrder.segment(parts[k]);
  }
  TapeDir shuffled("shuffled");
  for (int k : {2, 0, 1})
  {
    shuffled.segment(parts[k]);
  }

  auto run = [](const std::filesystem::path& dir, Probe& p)
  {
    p.onBarHook = [](Probe& s, const BarEvent&, size_t i)
    { scheduledOrder(s, i); };
    BacktestRunner r;
    r.setStrategy(&p);
    r.runTapeBarsOnTicks(dir, kMinute);
    return r.executor().fills();
  };
  Probe a;
  Probe b;
  const auto fa = run(inOrder.path(), a);
  const auto fb = run(shuffled.path(), b);
  expectSameBars(a.bars, b.bars);
  ASSERT_GT(fa.size(), 10u);
  expectSameFills(fa, fb);
}

TEST(RunTapeBarsOnTicks, ResultShapeAndRepeatRun)
{
  TapeDir tape("shape");
  tape.segment(randomTrades(9, kT0, 2000));
  Probe p;
  p.onBarHook = [](Probe& s, const BarEvent&, size_t i)
  { scheduledOrder(s, i); };
  BacktestRunner r;
  r.setStrategy(&p);
  const BacktestResult first = r.runTapeBarsOnTicks(tape.path(), kMinute);
  const BacktestStats s1 = first.computeStats();
  EXPECT_GT(s1.totalTrades, 0u);
  EXPECT_FALSE(first.trades().empty());
  EXPECT_FALSE(first.equityCurve().empty());
  EXPECT_EQ(r.state().tradeCount, 2000u);

  p.bars.clear();
  const BacktestStats s2 = r.runTapeBarsOnTicks(tape.path(), kMinute).computeStats();
  EXPECT_EQ(s1.totalTrades, s2.totalTrades);
  EXPECT_DOUBLE_EQ(s1.netPnl, s2.netPnl);
}

TEST(RunTapeBarsOnTicks, RefusesABadIntervalAndASecondSymbol)
{
  TapeDir tape("two");
  tape.segment(randomTrades(1, kT0, 50), {}, 1);
  tape.segment(randomTrades(2, kT0, 50), {}, 2);
  Probe p;
  BacktestRunner r;
  r.setStrategy(&p);
  EXPECT_THROW(r.runTapeBarsOnTicks(tape.path(), std::chrono::nanoseconds{0}),
               std::invalid_argument);
  EXPECT_THROW(r.runTapeBarsOnTicks(tape.path(), kMinute), std::invalid_argument);
  EXPECT_NO_THROW(r.runTapeBarsOnTicks(tape.path(), kMinute, SymbolId{1}));
  EXPECT_THROW(r.runTapeBarsOnTicks(tape.path(), kMinute, SymbolId{7}), std::invalid_argument);
  EXPECT_THROW(r.runTapeBarsOnTicks(tape.path() / "missing", kMinute), std::runtime_error);
}
