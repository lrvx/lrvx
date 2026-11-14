/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// The open position at the end of a backtest, through the C API. The
// LrvxBacktestStats struct keeps its shape (changing it would bump the ABI);
// the new values come from additive accessors on a result handle:
//
//   void     lrvx_backtest_result_mark_price(result, symbol, price)
//   double   lrvx_backtest_result_unrealized_pnl(result)
//   double   lrvx_backtest_result_final_equity(result)
//   uint32_t lrvx_backtest_result_open_positions(result, LrvxBacktestOpenPosition* out, max)
//
// LrvxBacktestOpenPosition is { uint32_t symbol; uint8_t side (0 long,
// 1 short, as LrvxBacktestTrade); double quantity; double entry_price;
// double mark_price; double unrealized_pnl; }. open_positions follows
// lrvx_backtest_result_trades: it returns the total, and with a buffer
// writes up to max entries, in ascending symbol order. A runner's result
// (lrvx_backtest_runner_take_result) is already marked at the run's last
// price per symbol.

#include "lrvx/capi/lrvx_capi.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace
{

constexpr double kEps = 1e-9;
constexpr int64_t kSec = 1'000'000'000;
constexpr int64_t kQtyScale = 100'000'000;

LrvxBacktestResultHandle makeResult(double capital = 10'000.0, double fee = 0.0004)
{
  return lrvx_backtest_result_create(capital, fee, 1, 0.0, 0.0, 252.0);
}

std::vector<LrvxBacktestOpenPosition> openPositions(LrvxBacktestResultHandle r)
{
  const uint32_t n = lrvx_backtest_result_open_positions(r, nullptr, 0);
  std::vector<LrvxBacktestOpenPosition> out(n);
  if (n > 0)
  {
    EXPECT_EQ(lrvx_backtest_result_open_positions(r, out.data(), n), n);
  }
  return out;
}

}  // namespace

TEST(CapiOpenPosition, FlatResultHasNoUnrealized)
{
  auto r = makeResult();
  lrvx_backtest_result_record_fill(r, 1, 1, /*buy=*/0, 100.0, 1.0, 1 * kSec);
  lrvx_backtest_result_record_fill(r, 2, 1, /*sell=*/1, 110.0, 1.0, 2 * kSec);
  lrvx_backtest_result_mark_price(r, 1, 150.0);

  LrvxBacktestStats s{};
  lrvx_backtest_result_stats(r, &s);
  EXPECT_EQ(lrvx_backtest_result_unrealized_pnl(r), 0.0);
  EXPECT_NEAR(lrvx_backtest_result_final_equity(r), s.finalCapital, kEps);
  EXPECT_EQ(lrvx_backtest_result_open_positions(r, nullptr, 0), 0u);
  lrvx_backtest_result_destroy(r);
}

TEST(CapiOpenPosition, LongAndShortAreMarkedAndSummed)
{
  auto r = makeResult();
  lrvx_backtest_result_record_fill(r, 1, 1, 0, 100.0, 1.0, 1 * kSec);
  lrvx_backtest_result_record_fill(r, 2, 1, 1, 110.0, 1.0, 2 * kSec);
  lrvx_backtest_result_record_fill(r, 3, 1, 0, 105.0, 2.0, 3 * kSec);  // long 2 @ 105
  lrvx_backtest_result_record_fill(r, 4, 2, 1, 50.0, 3.0, 4 * kSec);   // short 3 @ 50
  lrvx_backtest_result_mark_price(r, 1, 108.5);                        // +7
  lrvx_backtest_result_mark_price(r, 2, 52.0);                         // -6

  LrvxBacktestStats s{};
  lrvx_backtest_result_stats(r, &s);
  EXPECT_NEAR(lrvx_backtest_result_unrealized_pnl(r), 1.0, kEps);
  EXPECT_NEAR(lrvx_backtest_result_final_equity(r), s.initialCapital + s.netPnl + 1.0, kEps);
  // net_pnl keeps every fill's fee and only the closed trade's pnl.
  EXPECT_NEAR(s.totalPnl, 10.0, kEps);
  EXPECT_NEAR(s.totalFees, (100.0 + 110.0 + 210.0 + 150.0) * 0.0004, kEps);

  const auto open = openPositions(r);
  ASSERT_EQ(open.size(), 2u);
  EXPECT_EQ(open[0].symbol, 1u);
  EXPECT_EQ(open[0].side, 0);
  EXPECT_NEAR(open[0].quantity, 2.0, kEps);
  EXPECT_NEAR(open[0].entry_price, 105.0, kEps);
  EXPECT_NEAR(open[0].mark_price, 108.5, kEps);
  EXPECT_NEAR(open[0].unrealized_pnl, 7.0, kEps);
  EXPECT_EQ(open[1].symbol, 2u);
  EXPECT_EQ(open[1].side, 1);
  EXPECT_NEAR(open[1].quantity, 3.0, kEps);
  EXPECT_NEAR(open[1].entry_price, 50.0, kEps);
  EXPECT_NEAR(open[1].mark_price, 52.0, kEps);
  EXPECT_NEAR(open[1].unrealized_pnl, -6.0, kEps);

  // A short buffer gets the first entries only; the total is still returned
  // by the sizing call.
  LrvxBacktestOpenPosition one{};
  EXPECT_EQ(lrvx_backtest_result_open_positions(r, &one, 1), 1u);
  EXPECT_EQ(one.symbol, 1u);
  lrvx_backtest_result_destroy(r);
}

TEST(CapiOpenPosition, UnmarkedSymbolIsValuedAtItsLastFill)
{
  auto r = makeResult(10'000.0, 0.0);
  lrvx_backtest_result_record_fill(r, 1, 1, 0, 100.0, 2.0, 1 * kSec);
  lrvx_backtest_result_record_fill(r, 2, 1, 1, 104.0, 1.0, 2 * kSec);
  EXPECT_NEAR(lrvx_backtest_result_unrealized_pnl(r), 4.0, kEps);
  lrvx_backtest_result_destroy(r);
}

TEST(CapiOpenPosition, NullHandleIsRefusedNotDereferenced)
{
  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_backtest_result_unrealized_pnl(nullptr), 0.0);
  EXPECT_NE(lrvx_last_error_code(), 0);
  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_backtest_result_open_positions(nullptr, nullptr, 0), 0u);
  lrvx_backtest_result_mark_price(nullptr, 1, 1.0);
  lrvx_clear_last_error();
}

// Each accessor records the documented code for a NULL handle (1) and names
// itself in the message, so a caller can tell which call was refused.
TEST(CapiOpenPosition, EachNullHandleRecordsCodeOneNamingTheFunction)
{
  struct Case
  {
    const char* fn;
    void (*call)();
  };
  const Case cases[] = {
      {"lrvx_backtest_result_mark_price",
       []
       { lrvx_backtest_result_mark_price(nullptr, 1, 1.0); }},
      {"lrvx_backtest_result_open_positions",
       []
       {
         LrvxBacktestOpenPosition out[2]{};
         EXPECT_EQ(lrvx_backtest_result_open_positions(nullptr, out, 2), 0u);
       }},
      {"lrvx_backtest_result_unrealized_pnl",
       []
       { EXPECT_EQ(lrvx_backtest_result_unrealized_pnl(nullptr), 0.0); }},
      {"lrvx_backtest_result_final_equity",
       []
       { EXPECT_EQ(lrvx_backtest_result_final_equity(nullptr), 0.0); }},
  };
  for (const auto& c : cases)
  {
    lrvx_clear_last_error();
    ASSERT_EQ(lrvx_last_error_code(), 0);
    c.call();
    EXPECT_EQ(lrvx_last_error_code(), 1) << c.fn;
    const char* msg = lrvx_last_error_message();
    ASSERT_NE(msg, nullptr) << c.fn;
    EXPECT_NE(std::string(msg).find(c.fn), std::string::npos) << c.fn << ": " << msg;
  }
  lrvx_clear_last_error();
}

// ── Through a runner ───────────────────────────────────────────────────

namespace
{

struct BuyOnFirstBar
{
  LrvxStrategyHandle strategy{nullptr};
  uint32_t symbol{0};
  int bars{0};
  bool sell{false};
};

void onBar(void* ud, const LrvxSymbolContext*, const LrvxBarData*)
{
  auto* st = static_cast<BuyOnFirstBar*>(ud);
  if (st->bars++ == 0)
  {
    if (st->sell)
    {
      lrvx_emit_market_sell(st->strategy, st->symbol, 2 * kQtyScale);
    }
    else
    {
      lrvx_emit_market_buy(st->strategy, st->symbol, 2 * kQtyScale);
    }
  }
}

}  // namespace

TEST(CapiOpenPosition, RunBarsResultIsMarkedAtTheLastBarClose)
{
  for (const bool sell : {false, true})
  {
    auto* registry = lrvx_registry_create();
    const uint32_t sym = lrvx_registry_add_symbol(registry, "test", "BTC", 0.01);
    BuyOnFirstBar state;
    state.symbol = sym;
    state.sell = sell;
    LrvxStrategyCallbacks cb{};
    cb.on_bar = onBar;
    cb.user_data = &state;
    uint32_t syms[] = {sym};
    auto* strat = lrvx_strategy_create(1, syms, 1, registry, cb);
    state.strategy = strat;
    auto* btr = lrvx_backtest_runner_create(registry, 0.0004, 10'000.0);
    lrvx_backtest_runner_set_strategy(btr, strat);

    const int64_t m = 60 * kSec;
    int64_t starts[3] = {0, m, 2 * m};
    int64_t ends[3] = {m, 2 * m, 3 * m};
    double opens[3] = {100.0, 101.0, 103.0};
    double highs[3] = {100.5, 103.0, 108.0};
    double lows[3] = {99.5, 100.0, 96.0};
    double closes[3] = {100.0, 102.0, 97.0};
    double volumes[3] = {10.0, 10.0, 10.0};
    LrvxBacktestStats stats{};
    ASSERT_EQ(lrvx_backtest_runner_run_bars(btr, starts, ends, opens, highs, lows, closes,
                                            volumes, 3, "BTC", 0, 0, &stats),
              1);

    auto* result = lrvx_backtest_runner_take_result(btr);
    ASSERT_NE(result, nullptr);
    // Entry at bar 1's open, 101; mark at the last close, 97.
    const double expected = sell ? 8.0 : -8.0;
    EXPECT_NEAR(lrvx_backtest_result_unrealized_pnl(result), expected, kEps);
    EXPECT_NEAR(lrvx_backtest_result_final_equity(result),
                stats.initialCapital + stats.netPnl + expected, kEps);
    const auto open = openPositions(result);
    ASSERT_EQ(open.size(), 1u);
    EXPECT_EQ(open[0].side, sell ? 1 : 0);
    EXPECT_NEAR(open[0].entry_price, 101.0, kEps);
    EXPECT_NEAR(open[0].mark_price, 97.0, kEps);

    lrvx_backtest_result_destroy(result);
    lrvx_strategy_destroy(strat);
    lrvx_backtest_runner_destroy(btr);
    lrvx_registry_destroy(registry);
  }
}
