/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// The C API side of the strategy helpers:
//  - the entry price and bars-in-position through lrvx_entry_price_raw_opt
//    and lrvx_bars_in_position (LrvxSymbolContext keeps its ABI 6 layout),
//    in a backtest and on the synchronous runner fed with venue fills;
//  - exit rule handles (lrvx_exit_*), pinned to the levels of
//    tests/test_exits.cpp and tests/test_exits_percent_time.cpp;
//  - History handles and lrvx_crossed_above / lrvx_crossed_below;
//  - the batch lrvx_indicator_highest / lrvx_indicator_lowest.

#include "lrvx/capi/lrvx_capi.h"
#include "lrvx/execution/events/order_event.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace
{

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr int64_t kOne = 100'000'000;
constexpr int64_t kMinuteNs = 60'000'000'000LL;

// The position facts are read through accessors; the callback struct keeps
// its layout, so a binding built against ABI 6 reads it unchanged.
static_assert(sizeof(LrvxSymbolContext) == 104);
static_assert(offsetof(LrvxSymbolContext, last_touch_ns) == 96);

// ---- Position facts ------------------------------------------------------

struct Seen
{
  std::optional<double> entry;
  uint32_t bars;
  double position;
  // The same facts asked a second time in the same callback.
  std::optional<double> askedEntry;
  uint32_t askedBars;
};

// The entry price and bar count of `symbol` as the strategy reads them now.
std::optional<double> entryOf(LrvxStrategyHandle strategy, uint32_t symbol)
{
  int64_t raw = 0;
  if (!lrvx_entry_price_raw_opt(strategy, symbol, &raw))
  {
    return std::nullopt;
  }
  return lrvx_price_to_double(raw);
}

struct Scenario
{
  LrvxStrategyHandle strategy{nullptr};
  uint32_t symbol{0};
  size_t bar{0};
  std::vector<Seen> seen;
  std::vector<uint64_t> orders;
};

void onBar(void* ud, const LrvxSymbolContext* ctx, const LrvxBarData*)
{
  auto* s = static_cast<Scenario*>(ud);
  // Asked twice: the second answer must not differ from the first (an
  // accessor that counts the bar it is asked on would).
  Seen seen{entryOf(s->strategy, s->symbol), lrvx_bars_in_position(s->strategy, s->symbol),
            lrvx_quantity_to_double(ctx->position_raw), entryOf(s->strategy, s->symbol),
            lrvx_bars_in_position(s->strategy, s->symbol)};
  s->seen.push_back(seen);
  switch (s->bar++)
  {
    case 1:
    case 3:
      s->orders.push_back(lrvx_emit_market_buy(s->strategy, s->symbol, kOne));
      break;
    case 5:
      s->orders.push_back(lrvx_emit_market_sell(s->strategy, s->symbol, kOne));
      break;
    case 7:
      s->orders.push_back(lrvx_emit_market_sell(s->strategy, s->symbol, 3 * kOne));
      break;
    case 9:
      s->orders.push_back(lrvx_emit_market_buy(s->strategy, s->symbol, 2 * kOne));
      break;
    default:
      break;
  }
}

struct Expect
{
  std::optional<double> entry;
  uint32_t bars;
};

// Bars open at 100 + 10 i; an order from bar i fills at bar i + 1's open.
const std::vector<Expect> kExpected = {
    {std::nullopt, 0},
    {std::nullopt, 0},
    {120.0, 1},
    {120.0, 2},
    {130.0, 3},
    {130.0, 4},
    {130.0, 5},
    {130.0, 6},
    {180.0, 1},
    {180.0, 2},
    {std::nullopt, 0},
    {std::nullopt, 0},
};

void expectScenario(const std::vector<Seen>& seen, const char* label)
{
  ASSERT_EQ(seen.size(), kExpected.size()) << label;
  const double positions[] = {0, 0, 1, 1, 2, 2, 1, 1, -2, -2, 0, 0};
  for (size_t i = 0; i < kExpected.size(); ++i)
  {
    ASSERT_DOUBLE_EQ(seen[i].position, positions[i]) << label << " bar " << i << " (premise)";
    const auto& want = kExpected[i];
    EXPECT_EQ(seen[i].entry.has_value(), want.entry.has_value()) << label << " bar " << i;
    EXPECT_EQ(seen[i].askedEntry.has_value(), want.entry.has_value()) << label << " bar " << i;
    if (want.entry && seen[i].entry && seen[i].askedEntry)
    {
      EXPECT_NEAR(*seen[i].entry, *want.entry, 1e-9) << label << " bar " << i;
      EXPECT_NEAR(*seen[i].askedEntry, *want.entry, 1e-9) << label << " bar " << i;
    }
    EXPECT_EQ(seen[i].bars, want.bars) << label << " bar " << i;
    EXPECT_EQ(seen[i].askedBars, want.bars) << label << " bar " << i;
  }
}

struct Bars
{
  std::vector<int64_t> start, end;
  std::vector<double> open, high, low, close, volume;
};

Bars scenarioBars()
{
  Bars b;
  for (int i = 0; i < 12; ++i)
  {
    const double o = 100.0 + 10.0 * i;
    b.start.push_back(i * kMinuteNs);
    b.end.push_back((i + 1) * kMinuteNs);
    b.open.push_back(o);
    b.high.push_back(o + 3.0);
    b.low.push_back(o - 2.0);
    b.close.push_back(o + 1.0);
    b.volume.push_back(100.0);
  }
  return b;
}

void runScenario(double feeRate, const char* label)
{
  LrvxRegistryHandle registry = lrvx_registry_create();
  Scenario s;
  s.symbol = lrvx_registry_add_symbol(registry, "sim", "BTCUSDT", 0.01);
  LrvxStrategyCallbacks cb{};
  cb.on_bar = onBar;
  cb.user_data = &s;
  uint32_t syms[] = {s.symbol};
  s.strategy = lrvx_strategy_create(1, syms, 1, registry, cb);
  LrvxBacktestRunnerHandle runner = lrvx_backtest_runner_create(registry, feeRate, 1'000'000.0);
  lrvx_backtest_runner_set_strategy(runner, s.strategy);

  const Bars b = scenarioBars();
  LrvxBacktestStats stats{};
  ASSERT_EQ(lrvx_backtest_runner_run_bars(runner, b.start.data(), b.end.data(), b.open.data(),
                                          b.high.data(), b.low.data(), b.close.data(),
                                          b.volume.data(), 12, "BTCUSDT", 0, 0, &stats),
            1);
  expectScenario(s.seen, label);

  // A second run on the same runner starts clean.
  s.seen.clear();
  s.bar = 0;
  ASSERT_EQ(lrvx_backtest_runner_run_bars(runner, b.start.data(), b.end.data(), b.open.data(),
                                          b.high.data(), b.low.data(), b.close.data(),
                                          b.volume.data(), 12, "BTCUSDT", 0, 0, &stats),
            1);
  expectScenario(s.seen, "second run");

  lrvx_backtest_runner_destroy(runner);
  lrvx_strategy_destroy(s.strategy);
  lrvx_registry_destroy(registry);
}

}  // namespace

TEST(CapiPositionFacts, RunBarsCarriesEntryAndBarsOnTheContext) { runScenario(0.0, "no fee"); }

TEST(CapiPositionFacts, FeesDoNotMoveTheEntryPrice) { runScenario(0.01, "fee 1%"); }

TEST(CapiPositionFacts, AccessorsRefuseANullStrategy)
{
  int64_t raw = 123;
  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_entry_price_raw_opt(nullptr, 1, &raw), 0);
  EXPECT_EQ(lrvx_last_error_code(), 1);
  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_bars_in_position(nullptr, 1), 0u);
  EXPECT_EQ(lrvx_last_error_code(), 1);
  lrvx_clear_last_error();
}

namespace
{

// The live path: a synchronous runner, bars pushed by hand, fills reported
// by the venue through lrvx_runner_on_order_event. Two symbols, one
// strategy; each symbol keeps its own facts.
struct Live
{
  LrvxStrategyHandle strategy{nullptr};
  uint32_t a{0}, b{0};
  std::vector<std::pair<uint32_t, Seen>> seen;
};

void onLiveBar(void* ud, const LrvxSymbolContext* ctx, const LrvxBarData* bar)
{
  auto* s = static_cast<Live*>(ud);
  s->seen.emplace_back(bar->symbol, Seen{entryOf(s->strategy, bar->symbol),
                                         lrvx_bars_in_position(s->strategy, bar->symbol),
                                         lrvx_quantity_to_double(ctx->position_raw), std::nullopt,
                                         0});
}

void fill(LrvxRunnerHandle runner, uint64_t order, uint32_t symbol, uint8_t side, int64_t qty,
          double price)
{
  LrvxOrderEventData ev{};
  ev.order_id = order;
  ev.symbol_id = symbol;
  ev.side = side;
  ev.order_type = 1;  // market
  ev.status = static_cast<uint8_t>(lrvx::OrderEventStatus::FILLED);
  ev.fill_qty_raw = qty;
  ev.fill_price_raw = static_cast<int64_t>(price * 1e8);
  lrvx_runner_on_order_event(runner, &ev);
}

void bar(LrvxRunnerHandle runner, uint32_t symbol, int i, double open)
{
  lrvx_runner_on_bar(runner, symbol, 0, static_cast<uint64_t>(kMinuteNs), open, open + 3.0,
                     open - 2.0, open + 1.0, 100.0, 50.0, i * kMinuteNs, (i + 1) * kMinuteNs, 0);
}

}  // namespace

TEST(CapiPositionFacts, LiveRunnerFillsTheSameContext)
{
  LrvxRegistryHandle registry = lrvx_registry_create();
  Live s;
  s.a = lrvx_registry_add_symbol(registry, "test", "AAA", 0.01);
  s.b = lrvx_registry_add_symbol(registry, "test", "BBB", 0.01);
  LrvxStrategyCallbacks cb{};
  cb.on_bar = onLiveBar;
  cb.user_data = &s;
  uint32_t syms[] = {s.a, s.b};
  s.strategy = lrvx_strategy_create(7, syms, 2, registry, cb);
  LrvxRunnerHandle runner = lrvx_runner_create(registry, nullptr, nullptr);
  lrvx_runner_add_strategy(runner, s.strategy);
  lrvx_runner_start(runner);

  // Orders are sent through the strategy, so the runner knows whose they
  // are, and the venue's fills are reported against their ids.
  auto buy = [&](uint32_t sym, int64_t qty)
  { return lrvx_emit_market_buy(s.strategy, sym, qty); };
  auto sell = [&](uint32_t sym, int64_t qty)
  { return lrvx_emit_market_sell(s.strategy, sym, qty); };
  bar(runner, s.a, 0, 40.0);  // A flat
  fill(runner, buy(s.a, kOne), s.a, 0, kOne, 50.0);
  bar(runner, s.a, 1, 50.0);   // A long 1 @ 50, bar 1
  bar(runner, s.b, 1, 500.0);  // B flat
  fill(runner, buy(s.a, 3 * kOne), s.a, 0, 3 * kOne, 70.0);
  bar(runner, s.a, 2, 70.0);  // A long 4 @ (50 + 210) / 4 = 65, bar 2
  fill(runner, sell(s.b, kOne), s.b, 1, kOne, 500.0);
  bar(runner, s.b, 2, 510.0);  // B short 1 @ 500, bar 1
  fill(runner, sell(s.a, 2 * kOne), s.a, 1, 2 * kOne, 75.0);
  bar(runner, s.a, 3, 75.0);  // A long 2, partial close: 65, bar 3
  fill(runner, sell(s.a, 4 * kOne), s.a, 1, 4 * kOne, 80.0);
  bar(runner, s.a, 4, 80.0);  // A flipped short 2 @ 80, bar 1
  fill(runner, buy(s.a, 2 * kOne), s.a, 0, 2 * kOne, 85.0);
  bar(runner, s.a, 5, 85.0);  // A flat
  lrvx_runner_stop(runner);

  struct Want
  {
    uint32_t sym;
    double position;
    std::optional<double> entry;
    uint32_t bars;
  };
  const std::vector<Want> want = {
      {s.a, 0, std::nullopt, 0},
      {s.a, 1, 50.0, 1},
      {s.b, 0, std::nullopt, 0},
      {s.a, 4, 65.0, 2},
      {s.b, -1, 500.0, 1},
      {s.a, 2, 65.0, 3},
      {s.a, -2, 80.0, 1},
      {s.a, 0, std::nullopt, 0},
  };
  ASSERT_EQ(s.seen.size(), want.size());
  for (size_t i = 0; i < want.size(); ++i)
  {
    const auto& [sym, got] = s.seen[i];
    EXPECT_EQ(sym, want[i].sym) << "callback " << i;
    ASSERT_DOUBLE_EQ(got.position, want[i].position) << "callback " << i << " (premise)";
    EXPECT_EQ(got.entry.has_value(), want[i].entry.has_value()) << "callback " << i;
    if (got.entry && want[i].entry)
    {
      EXPECT_NEAR(*got.entry, *want[i].entry, 1e-9) << "callback " << i;
    }
    EXPECT_EQ(got.bars, want[i].bars) << "callback " << i;
  }

  lrvx_runner_destroy(runner);
  lrvx_strategy_destroy(s.strategy);
  lrvx_registry_destroy(registry);
}

TEST(CapiPositionFacts, EntryAccessorLeavesTheOutputAloneWhenFlat)
{
  LrvxRegistryHandle registry = lrvx_registry_create();
  const uint32_t sym = lrvx_registry_add_symbol(registry, "sim", "BTCUSDT", 0.01);
  LrvxStrategyCallbacks cb{};
  uint32_t syms[] = {sym};
  LrvxStrategyHandle strategy = lrvx_strategy_create(1, syms, 1, registry, cb);

  int64_t raw = -777;
  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_entry_price_raw_opt(strategy, sym, &raw), 0) << "flat: no entry";
  EXPECT_EQ(raw, -777) << "flat: price_out is not written";
  EXPECT_EQ(lrvx_last_error_code(), 0) << "flat is an answer, not an error";
  EXPECT_EQ(lrvx_entry_price_raw_opt(strategy, sym, nullptr), 0) << "a NULL price_out is allowed";

  lrvx_strategy_destroy(strategy);
  lrvx_registry_destroy(registry);
}

namespace
{

// Scenario B: an add after a partial close, then a short added to, partly
// closed and added to again. Bars open at 100 + 10 i; an order from bar i
// fills at bar i + 1's open.
//
//   bar 1  buy 1   -> 120   long 1, entry 120
//   bar 3  buy 1   -> 140   long 2, entry 130
//   bar 5  sell 1  -> 160   long 1, entry 130 (the manager's FIFO basis: 140)
//   bar 7  buy 1   -> 180   long 2, entry 155 (FIFO: 160)
//   bar 9  sell 4  -> 200   short 2, entry 200
//   bar 11 sell 2  -> 220   short 4, entry 210
//   bar 13 buy 1   -> 240   short 3, entry 210
//   bar 15 sell 1  -> 260   short 4, entry 222.5 (FIFO: 225)
//   bar 17 buy 4   -> 280   flat
struct ScenarioB
{
  LrvxStrategyHandle strategy{nullptr};
  uint32_t symbol{0};
  size_t bar{0};
  std::vector<Seen> seen;
  std::vector<double> managerAvg;
  // (fill price, what the fill callback reads)
  std::vector<std::pair<double, Seen>> fills;
};

void onBarB(void* ud, const LrvxSymbolContext* ctx, const LrvxBarData*)
{
  auto* s = static_cast<ScenarioB*>(ud);
  s->seen.push_back(Seen{entryOf(s->strategy, s->symbol),
                         lrvx_bars_in_position(s->strategy, s->symbol),
                         lrvx_quantity_to_double(ctx->position_raw), std::nullopt, 0});
  s->managerAvg.push_back(lrvx_price_to_double(ctx->avg_entry_price_raw));
  int64_t qty = 0;
  switch (s->bar++)
  {
    case 1:
    case 3:
    case 7:
    case 13:
      qty = 1;
      break;
    case 5:
    case 15:
      qty = -1;
      break;
    case 9:
      qty = -4;
      break;
    case 11:
      qty = -2;
      break;
    case 17:
      qty = 4;
      break;
    default:
      break;
  }
  if (qty > 0)
  {
    lrvx_emit_market_buy(s->strategy, s->symbol, qty * kOne);
  }
  else if (qty < 0)
  {
    lrvx_emit_market_sell(s->strategy, s->symbol, -qty * kOne);
  }
}

void onFillB(void* ud, const LrvxSymbolContext* ctx, const LrvxOrderEventData* ev)
{
  auto* s = static_cast<ScenarioB*>(ud);
  s->fills.emplace_back(lrvx_price_to_double(ev->fill_price_raw),
                        Seen{entryOf(s->strategy, s->symbol),
                             lrvx_bars_in_position(s->strategy, s->symbol),
                             lrvx_quantity_to_double(ctx->position_raw), std::nullopt, 0});
}

}  // namespace

TEST(CapiPositionFacts, AddsAfterAPartialCloseAndOnAShortAreWeightedByTheirFills)
{
  LrvxRegistryHandle registry = lrvx_registry_create();
  ScenarioB s;
  s.symbol = lrvx_registry_add_symbol(registry, "sim", "BTCUSDT", 0.01);
  LrvxStrategyCallbacks cb{};
  cb.on_bar = onBarB;
  cb.on_fill = onFillB;
  cb.user_data = &s;
  uint32_t syms[] = {s.symbol};
  s.strategy = lrvx_strategy_create(1, syms, 1, registry, cb);
  LrvxBacktestRunnerHandle runner = lrvx_backtest_runner_create(registry, 0.0, 1'000'000.0);
  lrvx_backtest_runner_set_strategy(runner, s.strategy);

  Bars b;
  for (int i = 0; i < 20; ++i)
  {
    const double o = 100.0 + 10.0 * i;
    b.start.push_back(i * kMinuteNs);
    b.end.push_back((i + 1) * kMinuteNs);
    b.open.push_back(o);
    b.high.push_back(o + 3.0);
    b.low.push_back(o - 2.0);
    b.close.push_back(o + 1.0);
    b.volume.push_back(100.0);
  }
  LrvxBacktestStats stats{};
  ASSERT_EQ(lrvx_backtest_runner_run_bars(runner, b.start.data(), b.end.data(), b.open.data(),
                                          b.high.data(), b.low.data(), b.close.data(),
                                          b.volume.data(), 20, "BTCUSDT", 0, 0, &stats),
            1);

  const double positions[] = {0, 0, 1, 1, 2, 2, 1, 1, 2, 2, -2, -2, -4, -4, -3, -3, -4, -4, 0, 0};
  const std::vector<Expect> want = {
      {std::nullopt, 0},
      {std::nullopt, 0},
      {120.0, 1},
      {120.0, 2},
      {130.0, 3},
      {130.0, 4},
      {130.0, 5},
      {130.0, 6},
      {155.0, 7},
      {155.0, 8},
      {200.0, 1},
      {200.0, 2},
      {210.0, 3},
      {210.0, 4},
      {210.0, 5},
      {210.0, 6},
      {222.5, 7},
      {222.5, 8},
      {std::nullopt, 0},
      {std::nullopt, 0},
  };
  ASSERT_EQ(s.seen.size(), want.size());
  // The premise: after the partial closes the manager's cost basis on the
  // context (avg_entry_price_raw) is a FIFO figure, not the weighted entry.
  EXPECT_NEAR(s.managerAvg[8], 160.0, 1e-9) << "premise";
  EXPECT_NEAR(s.managerAvg[16], 225.0, 1e-9) << "premise";
  for (size_t i = 0; i < want.size(); ++i)
  {
    ASSERT_DOUBLE_EQ(s.seen[i].position, positions[i]) << "bar " << i << " (premise)";
    EXPECT_EQ(s.seen[i].entry.has_value(), want[i].entry.has_value()) << "bar " << i;
    if (s.seen[i].entry && want[i].entry)
    {
      EXPECT_NEAR(*s.seen[i].entry, *want[i].entry, 1e-9) << "bar " << i;
    }
    EXPECT_EQ(s.seen[i].bars, want[i].bars) << "bar " << i;
  }

  // The fill callbacks: an opening or flipping fill reads 0 bars, the
  // flattening fill reads no entry and 0 bars.
  struct WantFill
  {
    double price, position;
    std::optional<double> entry;
    uint32_t bars;
  };
  const std::vector<WantFill> wantFills = {
      {120, 1, 120.0, 0},
      {140, 2, 130.0, 2},
      {160, 1, 130.0, 4},
      {180, 2, 155.0, 6},
      {200, -2, 200.0, 0},
      {220, -4, 210.0, 2},
      {240, -3, 210.0, 4},
      {260, -4, 222.5, 6},
      {280, 0, std::nullopt, 0},
  };
  ASSERT_EQ(s.fills.size(), wantFills.size());
  for (size_t i = 0; i < wantFills.size(); ++i)
  {
    const auto& [price, got] = s.fills[i];
    ASSERT_DOUBLE_EQ(price, wantFills[i].price) << "fill " << i << " (premise)";
    ASSERT_DOUBLE_EQ(got.position, wantFills[i].position) << "fill " << i << " (premise)";
    EXPECT_EQ(got.entry.has_value(), wantFills[i].entry.has_value()) << "fill " << i;
    if (got.entry && wantFills[i].entry)
    {
      EXPECT_NEAR(*got.entry, *wantFills[i].entry, 1e-9) << "fill " << i;
    }
    EXPECT_EQ(got.bars, wantFills[i].bars) << "fill " << i;
  }

  lrvx_backtest_runner_destroy(runner);
  lrvx_strategy_destroy(s.strategy);
  lrvx_registry_destroy(registry);
}

// ---- Exit rules ------------------------------------------------------------

namespace
{

struct ExitBar
{
  double high, low, atr;
};

constexpr ExitBar kLongBars[] = {
    {103.0, 99.0, 2.0},
    {106.0, 102.0, 2.0},
    {110.0, 105.0, 2.5},
    {108.0, 104.0, 2.5},
    {105.0, 101.5, 2.5},
    {103.0, 97.0, 3.0},
};

class Rule
{
 public:
  explicit Rule(LrvxExitRuleHandle h) : _h(h) {}
  ~Rule() { lrvx_exit_rule_destroy(_h); }
  Rule(const Rule&) = delete;
  Rule& operator=(const Rule&) = delete;
  LrvxExitRuleHandle get() const { return _h; }

 private:
  LrvxExitRuleHandle _h;
};

}  // namespace

TEST(CapiExitRules, AtrStopIsFrozenAtEntryMinusMultipleOfAtr)
{
  Rule r(lrvx_exit_atr_stop_create(2.0));
  ASSERT_NE(r.get(), nullptr);
  EXPECT_EQ(lrvx_exit_rule_armed(r.get()), 0);
  EXPECT_EQ(lrvx_exit_rule_update(r.get(), 101.0, 99.0, 2.0), 0) << "nothing before arm";

  lrvx_exit_rule_arm(r.get(), 0, 100.0, 2.0);
  EXPECT_EQ(lrvx_exit_rule_armed(r.get()), 1);
  EXPECT_DOUBLE_EQ(lrvx_exit_rule_level(r.get()), 96.0);
  for (const ExitBar& b : kLongBars)
  {
    EXPECT_EQ(lrvx_exit_rule_update(r.get(), b.high, b.low, b.atr), 0);
    EXPECT_DOUBLE_EQ(lrvx_exit_rule_level(r.get()), 96.0);
  }

  lrvx_exit_rule_arm(r.get(), 1, 100.0, 2.0);
  EXPECT_DOUBLE_EQ(lrvx_exit_rule_level(r.get()), 104.0);
  EXPECT_EQ(lrvx_exit_rule_update(r.get(), 103.0, 99.0, 2.0), 0);
  EXPECT_EQ(lrvx_exit_rule_update(r.get(), 104.0, 101.0, 2.0), 1);
  EXPECT_EQ(lrvx_exit_rule_update(r.get(), 104.0, 101.0, 2.0), 1) << "reported until disarm";
  lrvx_exit_rule_disarm(r.get());
  EXPECT_EQ(lrvx_exit_rule_armed(r.get()), 0);
  EXPECT_TRUE(std::isnan(lrvx_exit_rule_level(r.get())));
}

TEST(CapiExitRules, ChandelierHangsBelowTheHighestHighAndOnlyRises)
{
  Rule r(lrvx_exit_chandelier_create(0, 3.0));
  ASSERT_NE(r.get(), nullptr);
  lrvx_exit_rule_arm(r.get(), 0, 100.0, 2.0);
  EXPECT_DOUBLE_EQ(lrvx_exit_rule_level(r.get()), 94.0);
  const double after[] = {97.0, 100.0, 102.5, 102.5};
  for (int i = 0; i < 4; ++i)
  {
    EXPECT_EQ(lrvx_exit_rule_update(r.get(), kLongBars[i].high, kLongBars[i].low, kLongBars[i].atr),
              0);
    EXPECT_DOUBLE_EQ(lrvx_exit_rule_level(r.get()), after[i]) << "bar " << i;
  }
  EXPECT_EQ(lrvx_exit_rule_update(r.get(), 105.0, 101.5, 2.5), 1);
  EXPECT_DOUBLE_EQ(lrvx_exit_rule_level(r.get()), 102.5);

  Rule lb(lrvx_exit_chandelier_create(2, 3.0));
  lrvx_exit_rule_arm(lb.get(), 0, 100.0, 2.0);
  for (int i = 0; i < 4; ++i)
  {
    EXPECT_EQ(
        lrvx_exit_rule_update(lb.get(), kLongBars[i].high, kLongBars[i].low, kLongBars[i].atr), 0);
  }
  EXPECT_DOUBLE_EQ(lrvx_exit_rule_level(lb.get()), 102.5);
  EXPECT_EQ(lrvx_exit_rule_update(lb.get(), 105.0, 101.5, 2.5), 1);
  EXPECT_DOUBLE_EQ(lrvx_exit_rule_level(lb.get()), 102.5);
}

TEST(CapiExitRules, BreakEvenTrailMovesToEntryThenTrails)
{
  Rule r(lrvx_exit_break_even_trail_create(2.0, 3.0, 1.5));
  ASSERT_NE(r.get(), nullptr);
  lrvx_exit_rule_arm(r.get(), 0, 100.0, 2.0);
  EXPECT_DOUBLE_EQ(lrvx_exit_rule_level(r.get()), 96.0);
  EXPECT_EQ(lrvx_exit_rule_at_break_even(r.get()), 0);
  EXPECT_EQ(lrvx_exit_rule_update(r.get(), 103.0, 99.0, 2.0), 0);
  EXPECT_DOUBLE_EQ(lrvx_exit_rule_level(r.get()), 96.0);
  EXPECT_EQ(lrvx_exit_rule_update(r.get(), 106.0, 102.0, 2.0), 0);
  EXPECT_EQ(lrvx_exit_rule_at_break_even(r.get()), 1);
  EXPECT_DOUBLE_EQ(lrvx_exit_rule_level(r.get()), 103.0);
  EXPECT_EQ(lrvx_exit_rule_update(r.get(), 110.0, 105.0, 2.5), 0);
  EXPECT_DOUBLE_EQ(lrvx_exit_rule_level(r.get()), 106.25);
  EXPECT_EQ(lrvx_exit_rule_update(r.get(), 108.0, 104.0, 2.5), 1);
  EXPECT_DOUBLE_EQ(lrvx_exit_rule_level(r.get()), 106.25);
  lrvx_exit_rule_disarm(r.get());
  EXPECT_EQ(lrvx_exit_rule_at_break_even(r.get()), 0);
}

TEST(CapiExitRules, PercentStopLevelsAndHits)
{
  Rule r(lrvx_exit_percent_stop_create(5.0));
  ASSERT_NE(r.get(), nullptr);
  EXPECT_EQ(lrvx_exit_rule_update(r.get(), 1000.0, 1.0, 0.0), 0) << "nothing before arm";
  lrvx_exit_rule_arm(r.get(), 0, 200.0, 0.0);
  EXPECT_DOUBLE_EQ(lrvx_exit_rule_level(r.get()), 190.0);
  EXPECT_EQ(lrvx_exit_rule_update(r.get(), 215.0, 195.0, 0.0), 0);
  EXPECT_EQ(lrvx_exit_rule_update(r.get(), 212.0, 190.0, 0.0), 1);
  EXPECT_EQ(lrvx_exit_rule_update(r.get(), 212.0, 190.0, 0.0), 1) << "same bar again: still a hit";
  EXPECT_EQ(lrvx_exit_rule_update(r.get(), 212.0, 191.0, 0.0), 0)
      << "per bar: a later bar that does not reach the stop is no hit";
  EXPECT_EQ(lrvx_exit_rule_armed(r.get()), 1);
  lrvx_exit_rule_arm(r.get(), 1, 200.0, 0.0);
  EXPECT_DOUBLE_EQ(lrvx_exit_rule_level(r.get()), 210.0);
  EXPECT_EQ(lrvx_exit_rule_update(r.get(), 209.0, 195.0, 0.0), 0);
  EXPECT_EQ(lrvx_exit_rule_update(r.get(), 210.0, 201.0, 0.0), 1);

  Rule pin(lrvx_exit_percent_stop_create(2.5));
  lrvx_exit_rule_arm(pin.get(), 1, 100.0, 0.0);
  EXPECT_EQ(lrvx_exit_rule_level(pin.get()), 100.0 * (1.0 + 2.5 / 100.0));
  lrvx_exit_rule_arm(pin.get(), 0, 100.0, 0.0);
  EXPECT_EQ(lrvx_exit_rule_level(pin.get()), 97.5);
  lrvx_exit_rule_disarm(pin.get());
  EXPECT_TRUE(std::isnan(lrvx_exit_rule_level(pin.get())));
}

TEST(CapiExitRules, TakeProfitLevelsAndHits)
{
  Rule r(lrvx_exit_take_profit_create(25.0));
  ASSERT_NE(r.get(), nullptr);
  lrvx_exit_rule_arm(r.get(), 0, 200.0, 0.0);
  EXPECT_DOUBLE_EQ(lrvx_exit_rule_level(r.get()), 250.0);
  EXPECT_EQ(lrvx_exit_rule_update(r.get(), 249.0, 190.0, 0.0), 0);
  EXPECT_EQ(lrvx_exit_rule_update(r.get(), 250.0, 205.0, 0.0), 1);
  EXPECT_EQ(lrvx_exit_rule_update(r.get(), 250.0, 205.0, 0.0), 1) << "same bar again: still a hit";
  EXPECT_EQ(lrvx_exit_rule_update(r.get(), 240.0, 205.0, 0.0), 0) << "per bar: below the target";
  EXPECT_EQ(lrvx_exit_rule_armed(r.get()), 1);
  lrvx_exit_rule_arm(r.get(), 1, 200.0, 0.0);
  EXPECT_DOUBLE_EQ(lrvx_exit_rule_level(r.get()), 150.0);
  EXPECT_EQ(lrvx_exit_rule_update(r.get(), 195.0, 151.0, 0.0), 0);
  EXPECT_EQ(lrvx_exit_rule_update(r.get(), 190.0, 150.0, 0.0), 1);
}

TEST(CapiExitRules, TimeStopCountsUpdatesSinceArm)
{
  Rule r(lrvx_exit_time_stop_create(3));
  ASSERT_NE(r.get(), nullptr);
  EXPECT_EQ(lrvx_exit_rule_update(r.get(), 0.0, 0.0, 0.0), 0) << "nothing before arm";
  lrvx_exit_rule_arm(r.get(), 0, 0.0, 0.0);
  EXPECT_EQ(lrvx_exit_rule_armed(r.get()), 1);
  const uint8_t hits[] = {0, 0, 1, 1};
  for (uint32_t i = 0; i < 4; ++i)
  {
    EXPECT_EQ(lrvx_exit_rule_update(r.get(), 0.0, 0.0, 0.0), hits[i]) << "update " << i + 1;
    EXPECT_EQ(lrvx_exit_rule_bars(r.get()), i + 1);
  }
  EXPECT_TRUE(std::isnan(lrvx_exit_rule_level(r.get()))) << "a time stop has no price level";
  lrvx_exit_rule_disarm(r.get());
  EXPECT_EQ(lrvx_exit_rule_armed(r.get()), 0);
  EXPECT_EQ(lrvx_exit_rule_bars(r.get()), 0u);
  lrvx_exit_rule_arm(r.get(), 1, 0.0, 0.0);
  EXPECT_EQ(lrvx_exit_rule_update(r.get(), 0.0, 0.0, 0.0), 0) << "re-arming restarts the count";
}

TEST(CapiExitRules, CreateRejectsBadArguments)
{
  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_exit_percent_stop_create(0.0), nullptr);
  EXPECT_EQ(lrvx_last_error_code(), 2);
  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_exit_percent_stop_create(-5.0), nullptr);
  EXPECT_EQ(lrvx_exit_percent_stop_create(kNaN), nullptr);
  EXPECT_EQ(lrvx_exit_take_profit_create(0.0), nullptr);
  EXPECT_EQ(lrvx_last_error_code(), 2);
  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_exit_take_profit_create(kNaN), nullptr);
  EXPECT_EQ(lrvx_exit_time_stop_create(0), nullptr);
  EXPECT_EQ(lrvx_last_error_code(), 2);
  lrvx_clear_last_error();
}

TEST(CapiExitRules, ArmRejectsAnUnknownSide)
{
  Rule r(lrvx_exit_percent_stop_create(5.0));
  lrvx_clear_last_error();
  lrvx_exit_rule_arm(r.get(), 2, 200.0, 0.0);
  EXPECT_EQ(lrvx_last_error_code(), 2);
  EXPECT_EQ(lrvx_exit_rule_armed(r.get()), 0) << "a refused arm leaves the rule as it was";
  lrvx_clear_last_error();
}

TEST(CapiExitRules, NullHandleIsRefused)
{
  lrvx_clear_last_error();
  lrvx_exit_rule_arm(nullptr, 0, 1.0, 1.0);
  EXPECT_EQ(lrvx_last_error_code(), 1);
  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_exit_rule_update(nullptr, 1.0, 1.0, 1.0), 0);
  EXPECT_EQ(lrvx_last_error_code(), 1);
  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_exit_rule_level(nullptr), 0.0);
  EXPECT_EQ(lrvx_exit_rule_armed(nullptr), 0);
  EXPECT_EQ(lrvx_exit_rule_at_break_even(nullptr), 0);
  EXPECT_EQ(lrvx_exit_rule_bars(nullptr), 0u);
  lrvx_exit_rule_disarm(nullptr);
  EXPECT_EQ(lrvx_last_error_code(), 1);
  lrvx_clear_last_error();
  lrvx_exit_rule_destroy(nullptr);
}

// ---- History and crossings -----------------------------------------------

TEST(CapiHistory, NewestFirstRing)
{
  LrvxHistoryHandle h = lrvx_history_create(3);
  ASSERT_NE(h, nullptr);
  EXPECT_EQ(lrvx_history_capacity(h), 3u);
  EXPECT_EQ(lrvx_history_size(h), 0u);
  EXPECT_EQ(lrvx_history_full(h), 0);
  for (double v : {10.0, 20.0, 30.0, 40.0})
  {
    lrvx_history_push(h, v);
  }
  EXPECT_EQ(lrvx_history_size(h), 3u);
  EXPECT_EQ(lrvx_history_full(h), 1);
  EXPECT_EQ(lrvx_history_get(h, 0), 40.0);
  EXPECT_EQ(lrvx_history_get(h, 1), 30.0);
  EXPECT_EQ(lrvx_history_get(h, 2), 20.0);

  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_history_get(h, 3), 0.0);
  EXPECT_EQ(lrvx_last_error_code(), 2) << "index == size is out of range";
  lrvx_clear_last_error();

  lrvx_history_push(h, kNaN);
  EXPECT_TRUE(std::isnan(lrvx_history_get(h, 0))) << "a NaN is stored as given";
  EXPECT_EQ(lrvx_last_error_code(), 0);
  lrvx_history_destroy(h);
}

TEST(CapiHistory, CreateRejectsZeroAndNullIsRefused)
{
  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_history_create(0), nullptr);
  EXPECT_EQ(lrvx_last_error_code(), 2);
  lrvx_clear_last_error();
  lrvx_history_push(nullptr, 1.0);
  EXPECT_EQ(lrvx_last_error_code(), 1);
  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_history_get(nullptr, 0), 0.0);
  EXPECT_EQ(lrvx_history_size(nullptr), 0u);
  EXPECT_EQ(lrvx_history_capacity(nullptr), 0u);
  EXPECT_EQ(lrvx_history_full(nullptr), 0);
  EXPECT_EQ(lrvx_last_error_code(), 1);
  lrvx_clear_last_error();
  lrvx_history_destroy(nullptr);
}

TEST(CapiCrossing, EqualityEdgesAndNaN)
{
  EXPECT_EQ(lrvx_crossed_above(1.0, 3.0, 2.0, 2.0), 1);
  EXPECT_EQ(lrvx_crossed_above(1.0, 2.0, 2.0, 2.0), 0) << "touching is not a cross";
  EXPECT_EQ(lrvx_crossed_above(2.0, 2.5, 2.0, 2.0), 1) << "leaving from equal is a cross";
  EXPECT_EQ(lrvx_crossed_above(3.0, 4.0, 2.0, 2.0), 0);
  EXPECT_EQ(lrvx_crossed_above(10.0, 12.0, 11.0, 13.0), 0);
  EXPECT_EQ(lrvx_crossed_below(3.0, 1.0, 2.0, 2.0), 1);
  EXPECT_EQ(lrvx_crossed_below(3.0, 2.0, 2.0, 2.0), 0) << "touching is not a cross";
  EXPECT_EQ(lrvx_crossed_below(2.0, 1.5, 2.0, 2.0), 1) << "leaving from equal is a cross";
  EXPECT_EQ(lrvx_crossed_below(1.0, 0.5, 2.0, 2.0), 0);
  for (int i = 0; i < 4; ++i)
  {
    double v[4] = {1.0, 3.0, 2.0, 2.0};
    v[i] = kNaN;
    EXPECT_EQ(lrvx_crossed_above(v[0], v[1], v[2], v[3]), 0) << "NaN at " << i;
    double w[4] = {3.0, 1.0, 2.0, 2.0};
    w[i] = kNaN;
    EXPECT_EQ(lrvx_crossed_below(w[0], w[1], w[2], w[3]), 0) << "NaN at " << i;
  }
}

// ---- Highest / Lowest batch --------------------------------------------

TEST(CapiHighestLowest, BatchMatchesTheStreamingHandle)
{
  const std::vector<double> in = {5, 9, 4, 3, 7, 7, 2, 8, kNaN, 1, 6, 0.5, 6};
  const std::vector<double> hi = {kNaN, kNaN, 9, 9, 7, 7, 7, 8, kNaN, kNaN, kNaN, 6, 6};
  const std::vector<double> lo = {kNaN, kNaN, 4, 3, 3, 3, 2, 2, kNaN, kNaN, kNaN, 0.5, 0.5};
  std::vector<double> outHi(in.size(), -1.0);
  std::vector<double> outLo(in.size(), -1.0);
  lrvx_indicator_highest(in.data(), in.size(), 3, outHi.data());
  lrvx_indicator_lowest(in.data(), in.size(), 3, outLo.data());

  const double args[] = {3};
  LrvxStreamingIndicatorHandle sh = lrvx_streaming_indicator_create("highest", args, 1);
  LrvxStreamingIndicatorHandle sl = lrvx_streaming_indicator_create("lowest", args, 1);
  ASSERT_NE(sh, nullptr) << lrvx_last_error_message();
  ASSERT_NE(sl, nullptr) << lrvx_last_error_message();
  auto same = [](double a, double b)
  { return (std::isnan(a) && std::isnan(b)) || a == b; };
  for (size_t i = 0; i < in.size(); ++i)
  {
    EXPECT_TRUE(same(outHi[i], hi[i])) << "highest[" << i << "] = " << outHi[i];
    EXPECT_TRUE(same(outLo[i], lo[i])) << "lowest[" << i << "] = " << outLo[i];
    EXPECT_TRUE(same(lrvx_streaming_indicator_update(sh, in[i]), hi[i])) << "stream " << i;
    EXPECT_TRUE(same(lrvx_streaming_indicator_update(sl, in[i]), lo[i])) << "stream " << i;
  }
  lrvx_streaming_indicator_destroy(sh);
  lrvx_streaming_indicator_destroy(sl);

  const double zero[] = {0};
  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_streaming_indicator_create("highest", zero, 1), nullptr) << "period floor is 1";
  EXPECT_EQ(lrvx_last_error_code(), 2);
  lrvx_clear_last_error();
}
