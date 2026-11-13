/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// The context handed to on_fill and on_order_update carries the position
// after the fill being delivered: the runner's position tracker takes the
// fill before the strategy hears of it.

#include "lrvx/capi/lrvx_capi.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace
{

constexpr int64_t kSec = 1'000'000'000;
constexpr int64_t kOne = 100'000'000;

struct InAndOut
{
  LrvxStrategyHandle strategy{nullptr};
  uint32_t symbol{0};
  int seen{0};
  uint64_t parked{0};
  // ctx position next to the position the strategy can ask for, per callback.
  std::vector<int64_t> fillCtx;
  std::vector<int64_t> fillAsked;
  std::vector<int64_t> updateCtx;
  std::vector<int64_t> updateAsked;
};

void onTrade(void* ud, const LrvxSymbolContext*, const LrvxTradeData*)
{
  auto* s = static_cast<InAndOut*>(ud);
  ++s->seen;
  if (s->seen == 1)
  {
    lrvx_emit_market_buy(s->strategy, s->symbol, kOne);
    s->parked = lrvx_emit_limit_buy(s->strategy, s->symbol, 50 * kOne, kOne);
  }
  else if (s->seen == 3)
  {
    lrvx_emit_market_sell(s->strategy, s->symbol, 40'000'000);
  }
  else if (s->seen == 5)
  {
    lrvx_emit_close_position(s->strategy, s->symbol);
  }
  else if (s->seen == 7)
  {
    lrvx_emit_cancel(s->strategy, s->parked);
  }
}

void onFill(void* ud, const LrvxSymbolContext* ctx, const LrvxOrderEventData*)
{
  auto* s = static_cast<InAndOut*>(ud);
  s->fillCtx.push_back(ctx->position_raw);
  s->fillAsked.push_back(lrvx_position_raw(s->strategy, s->symbol));
}

void onOrderUpdate(void* ud, const LrvxSymbolContext* ctx, const LrvxOrderEventData*)
{
  auto* s = static_cast<InAndOut*>(ud);
  s->updateCtx.push_back(ctx->position_raw);
  s->updateAsked.push_back(lrvx_position_raw(s->strategy, s->symbol));
}

TEST(CapiFillPosition, ContextInOnFillCountsTheFill)
{
  LrvxRegistryHandle registry = lrvx_registry_create();
  InAndOut state;
  state.symbol = lrvx_registry_add_symbol(registry, "sim", "BTCUSDT", 0.5);
  LrvxStrategyCallbacks cb{};
  cb.on_trade = onTrade;
  cb.on_fill = onFill;
  cb.on_order_update = onOrderUpdate;
  cb.user_data = &state;
  uint32_t syms[] = {state.symbol};
  state.strategy = lrvx_strategy_create(1, syms, 1, registry, cb);
  LrvxBacktestRunnerHandle runner = lrvx_backtest_runner_create(registry, 0.0, 100'000.0);
  lrvx_backtest_runner_set_strategy(runner, state.strategy);

  const double close[] = {100.0, 100.5, 101.0, 100.5, 100.0, 99.5, 100.0, 100.5};
  int64_t ts[8];
  for (int i = 0; i < 8; ++i)
  {
    ts[i] = (i + 1) * kSec;
  }
  LrvxBacktestStats stats{};
  ASSERT_EQ(lrvx_backtest_runner_run_ohlcv(runner, ts, close, 8, "BTCUSDT", &stats), 1);

  const std::vector<int64_t> after = {kOne, 60'000'000, 0};
  EXPECT_EQ(state.fillAsked, after);
  EXPECT_EQ(state.fillCtx, after) << "ctx.position in on_fill is the position after the fill";
  ASSERT_FALSE(state.updateCtx.empty());
  EXPECT_EQ(state.updateCtx, state.updateAsked);

  lrvx_backtest_runner_destroy(runner);
  lrvx_strategy_destroy(state.strategy);
  lrvx_registry_destroy(registry);
}

}  // namespace
