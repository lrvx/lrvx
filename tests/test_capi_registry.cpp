/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/capi/lrvx_capi.h"

#include <gtest/gtest.h>
#include <cstring>
#include <string>

TEST(CapiRegistryTest, CreateAndDestroy)
{
  LrvxRegistryHandle reg = lrvx_registry_create();
  ASSERT_NE(reg, nullptr);
  lrvx_registry_destroy(reg);
}

TEST(CapiRegistryTest, AddSymbolAndCount)
{
  LrvxRegistryHandle reg = lrvx_registry_create();

  EXPECT_EQ(lrvx_registry_symbol_count(reg), 0u);

  uint32_t id1 = lrvx_registry_add_symbol(reg, "Binance", "BTCUSDT", 0.01);
  EXPECT_GT(id1, 0u);
  EXPECT_EQ(lrvx_registry_symbol_count(reg), 1u);

  uint32_t id2 = lrvx_registry_add_symbol(reg, "Binance", "ETHUSDT", 0.01);
  EXPECT_NE(id1, id2);
  EXPECT_EQ(lrvx_registry_symbol_count(reg), 2u);

  lrvx_registry_destroy(reg);
}

TEST(CapiRegistryTest, GetSymbolId)
{
  LrvxRegistryHandle reg = lrvx_registry_create();

  uint32_t added_id = lrvx_registry_add_symbol(reg, "Binance", "BTCUSDT", 0.01);

  uint32_t found_id = 0;
  EXPECT_EQ(lrvx_registry_get_symbol_id(reg, "Binance", "BTCUSDT", &found_id), 1);
  EXPECT_EQ(found_id, added_id);

  // Not found
  uint32_t missing_id = 0;
  EXPECT_EQ(lrvx_registry_get_symbol_id(reg, "Binance", "DOGEUSDT", &missing_id), 0);

  // Wrong exchange
  EXPECT_EQ(lrvx_registry_get_symbol_id(reg, "Bybit", "BTCUSDT", &missing_id), 0);

  lrvx_registry_destroy(reg);
}

TEST(CapiRegistryTest, GetSymbolName)
{
  LrvxRegistryHandle reg = lrvx_registry_create();

  uint32_t id = lrvx_registry_add_symbol(reg, "Binance", "BTCUSDT", 0.01);

  char exchange[64] = {};
  char name[64] = {};
  EXPECT_EQ(lrvx_registry_get_symbol_name(reg, id, exchange, sizeof(exchange), name, sizeof(name)),
            1);
  EXPECT_STREQ(exchange, "Binance");
  EXPECT_STREQ(name, "BTCUSDT");

  // Invalid symbol ID
  EXPECT_EQ(lrvx_registry_get_symbol_name(reg, 9999, exchange, sizeof(exchange), name,
                                          sizeof(name)),
            0);

  lrvx_registry_destroy(reg);
}

TEST(CapiRegistryTest, GetSymbolNameTruncation)
{
  LrvxRegistryHandle reg = lrvx_registry_create();

  lrvx_registry_add_symbol(reg, "Binance", "BTCUSDT", 0.01);

  uint32_t id = 0;
  lrvx_registry_get_symbol_id(reg, "Binance", "BTCUSDT", &id);

  // Small buffer — should truncate safely
  char exchange[4] = {};
  char name[4] = {};
  EXPECT_EQ(lrvx_registry_get_symbol_name(reg, id, exchange, sizeof(exchange), name, sizeof(name)),
            1);
  EXPECT_STREQ(exchange, "Bin");
  EXPECT_STREQ(name, "BTC");

  lrvx_registry_destroy(reg);
}

TEST(CapiRegistryTest, MultipleExchangesSameSymbol)
{
  LrvxRegistryHandle reg = lrvx_registry_create();

  uint32_t binance_id = lrvx_registry_add_symbol(reg, "Binance", "BTCUSDT", 0.01);
  uint32_t bybit_id = lrvx_registry_add_symbol(reg, "Bybit", "BTCUSDT", 0.01);

  EXPECT_NE(binance_id, bybit_id);

  uint32_t found = 0;
  EXPECT_EQ(lrvx_registry_get_symbol_id(reg, "Binance", "BTCUSDT", &found), 1);
  EXPECT_EQ(found, binance_id);

  EXPECT_EQ(lrvx_registry_get_symbol_id(reg, "Bybit", "BTCUSDT", &found), 1);
  EXPECT_EQ(found, bybit_id);

  lrvx_registry_destroy(reg);
}

// An unregistered SymbolId reaches the C API as a plain integer the caller
// invented, and Strategy's base constructor throws std::invalid_argument for
// it. Under extern "C" that exception has nowhere to go, so before the
// try/catch in lrvx_strategy_create this call terminated the process instead
// of returning a handle the caller could test.
TEST(CapiRegistryTest, StrategyCreateReturnsNullForUnregisteredSymbol)
{
  LrvxRegistryHandle reg = lrvx_registry_create();
  ASSERT_NE(reg, nullptr);

  const uint32_t bogus[] = {4242u};
  LrvxStrategyCallbacks cb{};
  LrvxStrategyHandle strat = lrvx_strategy_create(1, bogus, 1, reg, cb);

  EXPECT_EQ(strat, nullptr);

  lrvx_registry_destroy(reg);
}

TEST(CapiRegistryTest, StrategyCreateRejectsNullRegistry)
{
  const uint32_t syms[] = {1u};
  LrvxStrategyCallbacks cb{};
  EXPECT_EQ(lrvx_strategy_create(1, syms, 1, nullptr, cb), nullptr);
}

// A tick passed to lrvx_registry_add_symbol is the venue's, 0.01 included: a
// backtest on that registry refuses a limit price off it. A tick of 0 declares
// none and nothing is refused.
namespace
{

struct TickProbe
{
  LrvxStrategyHandle strategy{nullptr};
  uint32_t symbol{0};
  int bars{0};
  std::string rejectReason;
};

void tickProbeOnBar(void* ud, const LrvxSymbolContext*, const LrvxBarData*)
{
  auto* st = static_cast<TickProbe*>(ud);
  if (st->bars++ == 0)
  {
    // 100.005 is half a cent off the grid.
    lrvx_emit_limit_buy(st->strategy, st->symbol, 10'000'500'000LL, 100'000'000LL);
  }
}

void tickProbeOnOrderUpdate(void* ud, const LrvxSymbolContext*, const LrvxOrderEventData* ev)
{
  auto* st = static_cast<TickProbe*>(ud);
  if (ev->reject_reason != nullptr && ev->reject_reason[0] != '\0')
  {
    st->rejectReason = ev->reject_reason;
  }
}

std::string rejectReasonForTick(double tick)
{
  LrvxRegistryHandle reg = lrvx_registry_create();
  TickProbe probe;
  probe.symbol = lrvx_registry_add_symbol(reg, "test", "BTC", tick);
  LrvxStrategyCallbacks cb{};
  cb.on_bar = tickProbeOnBar;
  cb.on_order_update = tickProbeOnOrderUpdate;
  cb.user_data = &probe;
  const uint32_t syms[] = {probe.symbol};
  probe.strategy = lrvx_strategy_create(1, syms, 1, reg, cb);
  auto* runner = lrvx_backtest_runner_create(reg, 0.0, 10000.0);
  lrvx_backtest_runner_set_strategy(runner, probe.strategy);

  constexpr int64_t kMin = 60'000'000'000LL;
  const int64_t starts[2] = {0, kMin};
  const int64_t ends[2] = {kMin, 2 * kMin};
  const double opens[2] = {100.0, 100.0};
  const double highs[2] = {100.5, 100.5};
  const double lows[2] = {99.5, 99.5};
  const double closes[2] = {100.0, 100.0};
  const double volumes[2] = {10.0, 10.0};
  LrvxBacktestStats stats{};
  lrvx_backtest_runner_run_bars(runner, starts, ends, opens, highs, lows, closes, volumes, 2,
                                "BTC", 0, 0, &stats);

  lrvx_strategy_destroy(probe.strategy);
  lrvx_backtest_runner_destroy(runner);
  lrvx_registry_destroy(reg);
  return probe.rejectReason;
}

}  // namespace

TEST(CapiRegistryTest, ExplicitCentTickIsChecked)
{
  EXPECT_EQ(rejectReasonForTick(0.01), "price_off_tick");
}

TEST(CapiRegistryTest, ZeroTickChecksNothing)
{
  EXPECT_EQ(rejectReasonForTick(0.0), "");
}

namespace
{

struct LotProbe
{
  LrvxStrategyHandle strategy{nullptr};
  uint32_t symbol{0};
  int64_t qtyRaw{0};
  int bars{0};
  std::string rejectReason;
};

void lotProbeOnBar(void* ud, const LrvxSymbolContext*, const LrvxBarData*)
{
  auto* st = static_cast<LotProbe*>(ud);
  if (st->bars++ == 0)
  {
    lrvx_emit_limit_buy(st->strategy, st->symbol, 9'900'000'000LL, st->qtyRaw);
  }
}

void lotProbeOnOrderUpdate(void* ud, const LrvxSymbolContext*, const LrvxOrderEventData* ev)
{
  auto* st = static_cast<LotProbe*>(ud);
  if (ev->reject_reason != nullptr && ev->reject_reason[0] != '\0')
  {
    st->rejectReason = ev->reject_reason;
  }
}

// Registers BTC with `lot` (through the plain add when lot < 0) and sends one
// limit buy of qtyRaw.
std::string rejectReasonForLot(double lot, int64_t qtyRaw)
{
  LrvxRegistryHandle reg = lrvx_registry_create();
  LotProbe probe;
  probe.qtyRaw = qtyRaw;
  probe.symbol = lot < 0.0 ? lrvx_registry_add_symbol(reg, "test", "BTC", 0.01)
                           : lrvx_registry_add_symbol_with_lot(reg, "test", "BTC", 0.01, lot);
  LrvxStrategyCallbacks cb{};
  cb.on_bar = lotProbeOnBar;
  cb.on_order_update = lotProbeOnOrderUpdate;
  cb.user_data = &probe;
  const uint32_t syms[] = {probe.symbol};
  probe.strategy = lrvx_strategy_create(1, syms, 1, reg, cb);
  auto* runner = lrvx_backtest_runner_create(reg, 0.0, 10000.0);
  lrvx_backtest_runner_set_strategy(runner, probe.strategy);

  constexpr int64_t kMin = 60'000'000'000LL;
  const int64_t starts[2] = {0, kMin};
  const int64_t ends[2] = {kMin, 2 * kMin};
  const double opens[2] = {100.0, 100.0};
  const double highs[2] = {100.5, 100.5};
  const double lows[2] = {99.5, 99.5};
  const double closes[2] = {100.0, 100.0};
  const double volumes[2] = {10.0, 10.0};
  LrvxBacktestStats stats{};
  lrvx_backtest_runner_run_bars(runner, starts, ends, opens, highs, lows, closes, volumes, 2,
                                "BTC", 0, 0, &stats);

  lrvx_strategy_destroy(probe.strategy);
  lrvx_backtest_runner_destroy(runner);
  lrvx_registry_destroy(reg);
  return probe.rejectReason;
}

}  // namespace

TEST(CapiRegistryTest, ADeclaredLotIsChecked)
{
  // 0.0015 against a lot of 0.001.
  EXPECT_EQ(rejectReasonForLot(0.001, 150'000), "quantity_off_lot");
  EXPECT_EQ(rejectReasonForLot(0.001, 200'000), "");
}

TEST(CapiRegistryTest, NoLotChecksNoQuantity)
{
  EXPECT_EQ(rejectReasonForLot(0.0, 150'000), "");
  EXPECT_EQ(rejectReasonForLot(-1.0, 150'000), "") << "lrvx_registry_add_symbol declares none";
}

TEST(CapiRegistryTest, ALotBelowTheQuantityScaleIsRefused)
{
  LrvxRegistryHandle reg = lrvx_registry_create();
  lrvx_clear_last_error();
  lrvx_registry_add_symbol_with_lot(reg, "test", "BTC", 0.01, 1e-12);
  EXPECT_NE(lrvx_last_error_code(), 0);
  EXPECT_NE(std::string(lrvx_last_error_message()).find("lot_size"), std::string::npos);
  EXPECT_EQ(lrvx_registry_symbol_count(reg), 0u);
  lrvx_registry_destroy(reg);
}
