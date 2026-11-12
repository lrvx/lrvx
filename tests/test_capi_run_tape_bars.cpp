/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// lrvx_backtest_runner_run_tape_bars replays a tape as time bars in one call.
// The explicit route is the trade arrays -> lrvx_aggregate_time_bars ->
// lrvx_backtest_runner_run_bars; the one-call route must land on the same
// bars, trades and equity curve. lrvx_backtest_runner_create_with_fill_settings
// puts BacktestConfig's slippage and latency on the built-in executor.

#include "lrvx/capi/lrvx_capi.h"
#include "lrvx/replay/binary_format_v1.h"
#include "lrvx/replay/writers/binary_log_writer.h"
#include "lrvx/util/base/decimal.h"

#include <gtest/gtest.h>
#include <cmath>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace
{

constexpr int64_t kSec = 1'000'000'000;
constexpr int64_t kT0 = 1'767'225'600LL * kSec;  // 2026-01-01T00:00:00Z

struct Trade
{
  int64_t ts;
  int64_t priceRaw;
  int64_t qtyRaw;
  uint32_t symbol;
  uint8_t side;
};

std::vector<Trade> makeTrades(uint32_t symbol, uint64_t seed, int64_t start, int n, double base)
{
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<int64_t> gap(1, 5 * kSec);
  std::uniform_int_distribution<int> step(-25, 25);
  std::uniform_int_distribution<int> qty(1, 2000);
  std::vector<Trade> out;
  int64_t t = start;
  int64_t px = static_cast<int64_t>(base * 100);
  for (int i = 0; i < n; ++i)
  {
    t += gap(rng);
    px += step(rng);
    out.push_back({t, px * 1'000'000, static_cast<int64_t>(qty(rng)) * 100'000, symbol,
                   static_cast<uint8_t>(i % 3 == 0 ? 0 : 1)});
  }
  return out;
}

void writeSegment(const std::filesystem::path& dir, std::vector<Trade> trades)
{
  std::sort(trades.begin(), trades.end(),
            [](const Trade& a, const Trade& b)
            { return a.ts < b.ts; });
  lrvx::replay::WriterConfig cfg;
  cfg.output_dir = dir;
  lrvx::replay::BinaryLogWriter writer(cfg);
  uint64_t id = 1;
  for (const auto& t : trades)
  {
    lrvx::replay::TradeRecord r{};
    r.exchange_ts_ns = t.ts;
    r.recv_ts_ns = t.ts;
    r.price_raw = t.priceRaw;
    r.qty_raw = t.qtyRaw;
    r.symbol_id = t.symbol;
    r.side = t.side;
    r.trade_id = id++;
    writer.writeTrade(r);
  }
  writer.close();
}

struct Seen
{
  LrvxStrategyHandle strategy{nullptr};
  uint32_t symbol{0};
  std::vector<LrvxBarData> bars;
};

void onBar(void* ud, const LrvxSymbolContext*, const LrvxBarData* bar)
{
  auto* s = static_cast<Seen*>(ud);
  s->bars.push_back(*bar);
  const size_t i = s->bars.size();
  if (i % 6 == 1)
  {
    lrvx_emit_market_buy(s->strategy, s->symbol, 50'000'000);
  }
  else if (i % 6 == 4)
  {
    lrvx_emit_market_sell(s->strategy, s->symbol, 50'000'000);
  }
}

struct Outcome
{
  int rc{0};
  LrvxBacktestStats stats{};
  std::vector<LrvxBacktestTrade> trades;
  std::vector<LrvxEquityPoint> equity;
  std::vector<LrvxBarData> bars;
};

class TapeBars : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    _dir = std::filesystem::temp_directory_path() /
           (std::string("lrvx_capi_tape_bars_") +
            ::testing::UnitTest::GetInstance()->current_test_info()->name() + "_" +
            std::to_string(std::random_device{}()));
    std::filesystem::remove_all(_dir);
    std::filesystem::create_directories(_dir);
    // Two symbols, three segments, the latest written first so the file
    // names do not list them in time order.
    for (int k : {2, 0, 1})
    {
      auto a = makeTrades(1, 10 + k, kT0 + k * 3600 * kSec, 600, 100.0 + k);
      auto b = makeTrades(2, 20 + k, kT0 + k * 3600 * kSec, 400, 50.0 + k);
      a.insert(a.end(), b.begin(), b.end());
      _all.insert(_all.end(), a.begin(), a.end());
      writeSegment(_dir, a);
    }
    std::sort(_all.begin(), _all.end(),
              [](const Trade& a, const Trade& b)
              { return a.ts < b.ts; });
    _reg = lrvx_registry_create();
    ASSERT_EQ(lrvx_registry_add_symbol(_reg, "test", "AAA", 0.01), 1u);
    ASSERT_EQ(lrvx_registry_add_symbol(_reg, "test", "BBB", 0.01), 2u);
  }

  void TearDown() override
  {
    lrvx_registry_destroy(_reg);
    std::filesystem::remove_all(_dir);
  }

  template <typename Run>
  Outcome run(uint32_t symbol, LrvxBacktestRunnerHandle btr, Run&& go)
  {
    Seen seen;
    seen.symbol = symbol;
    LrvxStrategyCallbacks cb{};
    cb.on_bar = onBar;
    cb.user_data = &seen;
    uint32_t syms[] = {symbol};
    seen.strategy = lrvx_strategy_create(1, syms, 1, _reg, cb);
    lrvx_backtest_runner_set_strategy(btr, seen.strategy);
    Outcome o;
    o.rc = go(btr, &o.stats);
    if (o.rc == 1)
    {
      auto* res = lrvx_backtest_runner_take_result(btr);
      o.trades.resize(10'000);
      o.trades.resize(lrvx_backtest_result_trades(res, o.trades.data(), 10'000));
      o.equity.resize(10'000);
      o.equity.resize(lrvx_backtest_result_equity_curve(res, o.equity.data(), 10'000));
      lrvx_backtest_result_destroy(res);
    }
    o.bars = std::move(seen.bars);
    lrvx_strategy_destroy(seen.strategy);
    lrvx_backtest_runner_destroy(btr);
    return o;
  }

  Outcome threeStep(uint32_t symbol, const char* name, double intervalSeconds)
  {
    std::vector<int64_t> ts;
    std::vector<double> px, qty;
    std::vector<uint8_t> buy;
    for (const auto& t : _all)
    {
      if (t.symbol != symbol)
      {
        continue;
      }
      ts.push_back(t.ts);
      px.push_back(static_cast<double>(t.priceRaw) / 1e8);
      qty.push_back(static_cast<double>(t.qtyRaw) / 1e8);
      buy.push_back(t.side == 0 ? 1 : 0);
    }
    std::vector<LrvxBar> bars(ts.size());
    bars.resize(lrvx_aggregate_time_bars(ts.data(), px.data(), qty.data(), buy.data(), ts.size(),
                                         intervalSeconds, bars.data(),
                                         static_cast<uint32_t>(bars.size())));
    std::vector<int64_t> s, e;
    std::vector<double> o, h, l, c, v;
    for (const auto& b : bars)
    {
      s.push_back(b.start_time_ns);
      e.push_back(b.end_time_ns);
      o.push_back(static_cast<double>(b.open_raw) / 1e8);
      h.push_back(static_cast<double>(b.high_raw) / 1e8);
      l.push_back(static_cast<double>(b.low_raw) / 1e8);
      c.push_back(static_cast<double>(b.close_raw) / 1e8);
      v.push_back(static_cast<double>(b.volume_raw) / 1e8);
    }
    const auto paramNs = static_cast<uint64_t>(intervalSeconds * 1e9);
    return run(symbol, lrvx_backtest_runner_create(_reg, 0.0004, 10'000.0),
               [&](LrvxBacktestRunnerHandle btr, LrvxBacktestStats* st)
               {
                 return lrvx_backtest_runner_run_bars(
                     btr, s.data(), e.data(), o.data(), h.data(), l.data(), c.data(), v.data(),
                     static_cast<uint32_t>(s.size()), name, 0, paramNs, st);
               });
  }

  Outcome oneCall(uint32_t symbol, const char* name, double intervalSeconds,
                  LrvxBacktestRunnerHandle btr = nullptr)
  {
    const std::string dir = _dir.string();
    const auto ns = static_cast<int64_t>(intervalSeconds * 1e9);
    return run(symbol, btr != nullptr ? btr : lrvx_backtest_runner_create(_reg, 0.0004, 10'000.0),
               [&](LrvxBacktestRunnerHandle h, LrvxBacktestStats* st)
               { return lrvx_backtest_runner_run_tape_bars(h, dir.c_str(), ns, name, st); });
  }

  std::filesystem::path _dir;
  std::vector<Trade> _all;
  LrvxRegistryHandle _reg{nullptr};
};

void expectSame(const Outcome& a, const Outcome& b)
{
  ASSERT_EQ(a.rc, 1);
  ASSERT_EQ(b.rc, 1);
  EXPECT_EQ(std::memcmp(&a.stats, &b.stats, sizeof a.stats), 0);
  ASSERT_EQ(a.trades.size(), b.trades.size());
  for (size_t i = 0; i < a.trades.size(); ++i)
  {
    EXPECT_EQ(a.trades[i].entry_price, b.trades[i].entry_price) << i;
    EXPECT_EQ(a.trades[i].exit_price, b.trades[i].exit_price) << i;
    EXPECT_EQ(a.trades[i].entry_time_ns, b.trades[i].entry_time_ns) << i;
    EXPECT_EQ(a.trades[i].exit_time_ns, b.trades[i].exit_time_ns) << i;
    EXPECT_EQ(a.trades[i].pnl, b.trades[i].pnl) << i;
    EXPECT_EQ(a.trades[i].fee, b.trades[i].fee) << i;
  }
  ASSERT_EQ(a.equity.size(), b.equity.size());
  for (size_t i = 0; i < a.equity.size(); ++i)
  {
    EXPECT_EQ(a.equity[i].timestamp_ns, b.equity[i].timestamp_ns) << i;
    EXPECT_EQ(a.equity[i].equity, b.equity[i].equity) << i;
  }
  ASSERT_EQ(a.bars.size(), b.bars.size());
  for (size_t i = 0; i < a.bars.size(); ++i)
  {
    EXPECT_EQ(a.bars[i].start_time_ns, b.bars[i].start_time_ns) << i;
    EXPECT_EQ(a.bars[i].end_time_ns, b.bars[i].end_time_ns) << i;
    EXPECT_EQ(a.bars[i].open_raw, b.bars[i].open_raw) << i;
    EXPECT_EQ(a.bars[i].high_raw, b.bars[i].high_raw) << i;
    EXPECT_EQ(a.bars[i].low_raw, b.bars[i].low_raw) << i;
    EXPECT_EQ(a.bars[i].close_raw, b.bars[i].close_raw) << i;
    EXPECT_EQ(a.bars[i].volume_raw, b.bars[i].volume_raw) << i;
    EXPECT_EQ(a.bars[i].bar_type_param, b.bars[i].bar_type_param) << i;
  }
}

}  // namespace

TEST_F(TapeBars, EqualsTheExplicitRouteForBothSymbolsAndIntervals)
{
  for (double interval : {60.0, 300.0})
  {
    for (uint32_t sym : {1u, 2u})
    {
      const char* name = sym == 1 ? "AAA" : "BBB";
      auto ref = threeStep(sym, name, interval);
      auto got = oneCall(sym, name, interval);
      SCOPED_TRACE(std::string(name) + " " + std::to_string(interval));
      ASSERT_GT(ref.bars.size(), 10u);
      ASSERT_GE(ref.trades.size(), 2u);
      expectSame(got, ref);
      for (const auto& b : got.bars)
      {
        EXPECT_EQ(b.symbol, sym);
        EXPECT_EQ(b.start_time_ns % static_cast<int64_t>(interval * 1e9), 0);
      }
    }
  }
}

TEST_F(TapeBars, MultiSymbolTapeWithoutSymbolFails)
{
  auto o = oneCall(1, nullptr, 60.0);
  EXPECT_EQ(o.rc, 0);
  EXPECT_NE(std::string(lrvx_last_error_message()).find("more than one symbol"), std::string::npos)
      << lrvx_last_error_message();
}

TEST_F(TapeBars, BadArgumentsFail)
{
  const std::string dir = _dir.string();
  auto* btr = lrvx_backtest_runner_create(_reg, 0.0, 1000.0);
  LrvxBacktestStats st{};
  EXPECT_EQ(lrvx_backtest_runner_run_tape_bars(btr, dir.c_str(), 0, "AAA", &st), 0);
  EXPECT_NE(std::string(lrvx_last_error_message()).find("interval_ns"), std::string::npos);
  EXPECT_EQ(lrvx_backtest_runner_run_tape_bars(btr, dir.c_str(), 60 * kSec, "ZZZ", &st), 0);
  EXPECT_NE(std::string(lrvx_last_error_message()).find("ZZZ"), std::string::npos);
  EXPECT_EQ(lrvx_backtest_runner_run_tape_bars(btr, (dir + "/nope").c_str(), 60 * kSec, "AAA", &st),
            0);
  EXPECT_EQ(lrvx_backtest_runner_run_tape_bars(btr, nullptr, 60 * kSec, "AAA", &st), 0);
  lrvx_backtest_runner_destroy(btr);
}

TEST_F(TapeBars, DefaultFillSettingsEqualThePlainRunner)
{
  auto plain = oneCall(1, "AAA", 60.0);
  auto zero = oneCall(1, "AAA", 60.0,
                      lrvx_backtest_runner_create_with_fill_settings(
                          _reg, 0.0004, 10'000.0, 0, 0, 0.0, 0.0, 0.0, 0, 0, 0, 0, 42, 0, 0,
                          nullptr));
  expectSame(zero, plain);
}

TEST_F(TapeBars, FixedBpsSlippageMovesEveryFillAgainstTheTaker)
{
  constexpr double kTick = 0.01;
  auto plain = oneCall(1, "AAA", 60.0);
  auto slipped = oneCall(1, "AAA", 60.0,
                         lrvx_backtest_runner_create_with_fill_settings(
                             _reg, 0.0004, 10'000.0, 2, 0, 0.0, 10.0, 0.0, 0, 0, 0, 0, 42, 0, 0,
                             nullptr));
  ASSERT_EQ(slipped.rc, 1);
  ASSERT_EQ(slipped.trades.size(), plain.trades.size());
  ASSERT_FALSE(plain.trades.empty());
  for (size_t i = 0; i < plain.trades.size(); ++i)
  {
    // long round trips: bought higher, sold lower, 10 bps each way. The
    // symbol declares a 0.01 tick, so a slipped fill is then put on the tick
    // against the taker: a buy up, a sell down, by less than one tick.
    const double slippedBuy = plain.trades[i].entry_price * 1.001;
    const double slippedSell = plain.trades[i].exit_price * 0.999;
    EXPECT_GE(slipped.trades[i].entry_price, slippedBuy - 1e-9);
    EXPECT_LT(slipped.trades[i].entry_price, slippedBuy + kTick);
    EXPECT_LE(slipped.trades[i].exit_price, slippedSell + 1e-9);
    EXPECT_GT(slipped.trades[i].exit_price, slippedSell - kTick);
    EXPECT_NEAR(std::remainder(slipped.trades[i].entry_price, kTick), 0.0, 1e-9);
    EXPECT_NEAR(std::remainder(slipped.trades[i].exit_price, kTick), 0.0, 1e-9);
  }
  EXPECT_LT(slipped.stats.netPnl, plain.stats.netPnl);
}

TEST_F(TapeBars, SubmitLatencyAndLatencyModelDeferFillsByABar)
{
  auto plain = oneCall(1, "AAA", 60.0);
  auto acked = oneCall(1, "AAA", 60.0,
                       lrvx_backtest_runner_create_with_fill_settings(
                           _reg, 0.0004, 10'000.0, 0, 0, 0.0, 0.0, 0.0, 5'000'000, 0, 0, 0, 42, 0,
                           0, nullptr));
  auto* model = lrvx_latency_constant_create(0, 5'000'000, 0);
  auto modelled = oneCall(1, "AAA", 60.0,
                          lrvx_backtest_runner_create_with_fill_settings(
                              _reg, 0.0004, 10'000.0, 0, 0, 0.0, 0.0, 0.0, 0, 0, 0, 0, 42, 0, 0,
                              model));
  lrvx_latency_destroy(model);
  expectSame(modelled, acked);
  ASSERT_FALSE(plain.trades.empty());
  ASSERT_EQ(acked.bars.size(), plain.bars.size());
  // The first entry is sent from bar 1's callback: instant fills at bar 2's open,
  // a 5 ms deferral at bar 3's open (the clock reads a bar's end inside it).
  EXPECT_EQ(plain.trades[0].entry_price, static_cast<double>(plain.bars[1].open_raw) / 1e8);
  EXPECT_EQ(acked.trades[0].entry_price, static_cast<double>(acked.bars[2].open_raw) / 1e8);
}

TEST(CapiRunnerFillSettings, InvalidValuesReturnNullAndNameTheArgument)
{
  auto* reg = lrvx_registry_create();
  struct Case
  {
    int32_t model;
    int32_t ticks;
    double tickSize;
    double bps;
    double impact;
    int64_t submit;
    const char* expect;
  };
  const Case cases[] = {
      {7, 0, 0.0, 0.0, 0.0, 0, "slippage_model"},
      {1, 0, 0.01, 0.0, 0.0, 0, "slippage_ticks"},
      {1, 2, 0.0, 0.0, 0.0, 0, "slippage_tick_size"},
      {1, 2, 0.01, 3.0, 0.0, 0, "slippage_bps"},
      {2, 0, 0.0, 0.0, 0.0, 0, "slippage_bps"},
      {2, 1, 0.0, 2.0, 0.0, 0, "slippage_ticks"},
      {3, 0, 0.0, 0.0, 0.0, 0, "slippage_impact_coeff"},
      {0, 0, 0.0, 2.0, 0.0, 0, "slippage_bps"},
      {0, 0, 0.0, 0.0, 0.0, -1, "submit_ack_latency_ns"},
  };
  for (const auto& c : cases)
  {
    auto* h = lrvx_backtest_runner_create_with_fill_settings(
        reg, 0.0, 1000.0, c.model, c.ticks, c.tickSize, c.bps, c.impact, c.submit, 0, 0, 0, 42, 0,
        0, nullptr);
    EXPECT_EQ(h, nullptr) << c.expect;
    EXPECT_NE(std::string(lrvx_last_error_message()).find(c.expect), std::string::npos)
        << lrvx_last_error_message();
    lrvx_backtest_runner_destroy(h);
  }
  lrvx_registry_destroy(reg);
}

TEST(CapiRunnerFillSettings, VolumeImpactIsRefusedOnBars)
{
  auto* reg = lrvx_registry_create();
  lrvx_registry_add_symbol(reg, "test", "AAA", 0.01);
  auto* btr = lrvx_backtest_runner_create_with_fill_settings(reg, 0.0, 1000.0, 3, 0, 0.0, 0.0, 0.5,
                                                             0, 0, 0, 0, 42, 0, 0, nullptr);
  ASSERT_NE(btr, nullptr);
  int64_t s[1] = {kT0};
  int64_t e[1] = {kT0 + 60 * kSec};
  double p[1] = {100.0};
  LrvxBacktestStats st{};
  EXPECT_EQ(lrvx_backtest_runner_run_bars(btr, s, e, p, p, p, p, p, 1, "AAA", 0, 0, &st), 0);
  EXPECT_NE(std::string(lrvx_last_error_message()).find("volume_impact"), std::string::npos);
  lrvx_backtest_runner_destroy(btr);
  lrvx_registry_destroy(reg);
}

// lrvx_backtest_runner_run_tape_bars_on_ticks: the bars on_bar sees are the
// run_tape_bars bars, and an entry sent from bar n's callback fills at the
// first print of its symbol stamped at or after the end of bar n.
TEST_F(TapeBars, OnTicksSeesTheSameBarsAndFillsAtTheFirstPrintAfterTheBar)
{
  const std::string dir = _dir.string();
  auto bars = oneCall(1, "AAA", 60.0);
  auto ticks = run(1, lrvx_backtest_runner_create(_reg, 0.0004, 10'000.0),
                   [&](LrvxBacktestRunnerHandle h, LrvxBacktestStats* st)
                   {
                     return lrvx_backtest_runner_run_tape_bars_on_ticks(h, dir.c_str(),
                                                                        60 * kSec, "AAA", st);
                   });
  ASSERT_EQ(ticks.rc, 1) << lrvx_last_error_message();
  ASSERT_EQ(ticks.bars.size(), bars.bars.size());
  for (size_t i = 0; i < bars.bars.size(); ++i)
  {
    EXPECT_EQ(std::memcmp(&ticks.bars[i], &bars.bars[i], sizeof(LrvxBarData)), 0) << i;
  }
  ASSERT_FALSE(ticks.trades.empty());
  // The first entry is sent from the first bar's callback.
  const int64_t end1 = ticks.bars[0].end_time_ns;
  const Trade* first = nullptr;
  for (const auto& t : _all)
  {
    if (t.symbol == 1 && t.ts >= end1)
    {
      first = &t;
      break;
    }
  }
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(ticks.trades[0].entry_price, static_cast<double>(first->priceRaw) / 1e8);
  EXPECT_EQ(ticks.trades[0].entry_time_ns, first->ts);
}

TEST_F(TapeBars, OnTicksBadArgumentsFail)
{
  const std::string dir = _dir.string();
  auto* btr = lrvx_backtest_runner_create(_reg, 0.0, 1000.0);
  LrvxBacktestStats st{};
  EXPECT_EQ(lrvx_backtest_runner_run_tape_bars_on_ticks(btr, dir.c_str(), 0, "AAA", &st), 0);
  EXPECT_NE(std::string(lrvx_last_error_message()).find("interval_ns"), std::string::npos);
  EXPECT_EQ(lrvx_backtest_runner_run_tape_bars_on_ticks(btr, dir.c_str(), 60 * kSec, nullptr, &st),
            0);
  EXPECT_NE(std::string(lrvx_last_error_message()).find("more than one symbol"), std::string::npos)
      << lrvx_last_error_message();
  EXPECT_EQ(lrvx_backtest_runner_run_tape_bars_on_ticks(btr, dir.c_str(), 60 * kSec, "ZZZ", &st),
            0);
  EXPECT_EQ(lrvx_backtest_runner_run_tape_bars_on_ticks(btr, nullptr, 60 * kSec, "AAA", &st), 0);
  lrvx_backtest_runner_destroy(btr);
}

// The stats struct says how many option-quote and pool-state records a tape
// run read and delivered to nobody, as Python's skipped_* keys do.
TEST(CapiRunStats, TapeRunsReportSkippedOptionQuotesAndPoolStates)
{
  const auto dir = std::filesystem::temp_directory_path() /
                   ("lrvx_capi_skipped_" + std::to_string(std::random_device{}()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  {
    lrvx::replay::WriterConfig cfg;
    cfg.output_dir = dir;
    lrvx::replay::BinaryLogWriter writer(cfg);
    for (int i = 0; i < 10; ++i)
    {
      lrvx::replay::TradeRecord r{};
      r.exchange_ts_ns = kT0 + i * 20 * kSec;
      r.recv_ts_ns = r.exchange_ts_ns;
      r.price_raw = 100'00000000;
      r.qty_raw = 1'00000000;
      r.symbol_id = 1;
      r.trade_id = static_cast<uint64_t>(i + 1);
      writer.writeTrade(r);
      if (i < 3)
      {
        lrvx::replay::OptionQuoteRecord q{};
        q.exchange_ts_ns = r.exchange_ts_ns + 1;
        q.recv_ts_ns = q.exchange_ts_ns;
        q.symbol_id = 1;
        writer.writeOptionQuote(q);
      }
    }
    writer.close();
  }
  auto* reg = lrvx_registry_create();
  lrvx_registry_add_symbol(reg, "test", "AAA", 0.01);
  // run_tape and run_tape_bars_on_ticks; run_tapes merges tapes that carry
  // venue metadata, which this bare tape has none of.
  for (int mode : {0, 2})
  {
    auto* btr = lrvx_backtest_runner_create(reg, 0.0, 1000.0);
    uint32_t syms[] = {1};
    LrvxStrategyCallbacks cb{};
    auto* strat = lrvx_strategy_create(1, syms, 1, reg, cb);
    lrvx_backtest_runner_set_strategy(btr, strat);
    LrvxBacktestStats st{};
    st.optionQuoteCount = 99;
    st.poolStateCount = 99;
    const std::string d = dir.string();
    const char* dirs[] = {d.c_str()};
    const int rc = mode == 0   ? lrvx_backtest_runner_run_tape(btr, d.c_str(), &st)
                   : mode == 1 ? lrvx_backtest_runner_run_tapes(btr, dirs, 1, &st)
                               : lrvx_backtest_runner_run_tape_bars_on_ticks(
                                     btr, d.c_str(), 60 * kSec, "AAA", &st);
    ASSERT_EQ(rc, 1) << mode << " " << lrvx_last_error_message();
    EXPECT_EQ(st.optionQuoteCount, 3u) << "mode " << mode;
    EXPECT_EQ(st.poolStateCount, 0u) << "mode " << mode;
    // A run that reads no tape reports none.
    LrvxBacktestStats bars{};
    bars.optionQuoteCount = 99;
    bars.poolStateCount = 99;
    int64_t s[1] = {kT0};
    int64_t e[1] = {kT0 + 60 * kSec};
    double p[1] = {100.0};
    ASSERT_EQ(lrvx_backtest_runner_run_bars(btr, s, e, p, p, p, p, p, 1, "AAA", 0, 0, &bars), 1);
    EXPECT_EQ(bars.optionQuoteCount, 0u);
    EXPECT_EQ(bars.poolStateCount, 0u);
    lrvx_strategy_destroy(strat);
    lrvx_backtest_runner_destroy(btr);
  }
  lrvx_registry_destroy(reg);
  std::filesystem::remove_all(dir);
}
