/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Tests for the Executor hook. A binding-supplied executor
// receives signals (submit / cancel / replace / cancel_all / submit_oco)
// instead of the built-in SimulatedExecutor. Lifecycle (on_start /
// on_stop) is balanced against runner / live engine / backtest start.

#include "lrvx/capi/bridge_strategy.h"
#include "lrvx/capi/lrvx_capi.h"

#include <gtest/gtest.h>
#include <atomic>
#include <vector>

namespace
{

struct ExecState
{
  std::atomic<int> submit{0};
  std::atomic<int> cancel{0};
  std::atomic<int> cancel_all{0};
  std::atomic<int> replace{0};
  std::atomic<int> oco{0};
  std::atomic<int> caps_calls{0};
  std::atomic<int> on_start{0};
  std::atomic<int> on_stop{0};

  // Last seen payloads.
  uint64_t last_submit_id{0};
  uint32_t last_submit_symbol{0};
  uint8_t last_submit_side{0};
  uint8_t last_submit_type{0};
  int64_t last_submit_price_raw{0};
  int64_t last_submit_qty_raw{0};

  uint64_t last_cancel_id{0};
  uint32_t last_cancel_all_symbol{0};
  uint64_t last_replace_old_id{0};
  LrvxOrder last_replace_order{};
  LrvxOrder last_oco_first{};
  LrvxOrder last_oco_second{};
};

void ex_submit(void* ud, const LrvxOrder* o)
{
  auto* s = static_cast<ExecState*>(ud);
  s->submit++;
  s->last_submit_id = o->id;
  s->last_submit_symbol = o->symbol;
  s->last_submit_side = o->side;
  s->last_submit_type = o->type;
  s->last_submit_price_raw = o->price_raw;
  s->last_submit_qty_raw = o->quantity_raw;
}
void ex_cancel(void* ud, uint64_t id)
{
  auto* s = static_cast<ExecState*>(ud);
  s->cancel++;
  s->last_cancel_id = id;
}
void ex_cancel_all(void* ud, uint32_t sym)
{
  auto* s = static_cast<ExecState*>(ud);
  s->cancel_all++;
  s->last_cancel_all_symbol = sym;
}
void ex_replace(void* ud, uint64_t old_id, const LrvxOrder* o)
{
  auto* s = static_cast<ExecState*>(ud);
  s->replace++;
  s->last_replace_old_id = old_id;
  s->last_replace_order = *o;
}
void ex_oco(void* ud, const LrvxOrder* first, const LrvxOrder* second)
{
  auto* s = static_cast<ExecState*>(ud);
  s->oco++;
  s->last_oco_first = *first;
  s->last_oco_second = *second;
}
void ex_caps(void* ud, LrvxExchangeCapabilities* out)
{
  static_cast<ExecState*>(ud)->caps_calls++;
  out->supports_stop_market = 1;
  out->supports_stop_limit = 1;
  out->supports_oco = 1;
  out->supports_trailing_stop = 1;
  out->supports_iceberg = 0;
  out->supports_post_only = 1;
  out->supports_reduce_only = 1;
  out->supports_close_position = 0;
  out->supports_gtc = 1;
  out->supports_ioc = 1;
  out->supports_fok = 0;
  out->supports_gtd = 0;
  out->supports_take_profit_market = 1;
  out->supports_take_profit_limit = 1;
  out->supports_batch_submit = 1;
}
void ex_on_start(void* ud) { static_cast<ExecState*>(ud)->on_start++; }
void ex_on_stop(void* ud) { static_cast<ExecState*>(ud)->on_stop++; }

LrvxExecutorCallbacks make_cb(ExecState& s)
{
  LrvxExecutorCallbacks cb{};
  cb.submit = ex_submit;
  cb.cancel = ex_cancel;
  cb.cancel_all = ex_cancel_all;
  cb.replace = ex_replace;
  cb.submit_oco = ex_oco;
  cb.capabilities = ex_caps;
  cb.on_start = ex_on_start;
  cb.on_stop = ex_on_stop;
  cb.user_data = &s;
  return cb;
}

}  // namespace

TEST(CapiExecutor, CreateDestroyCapabilities)
{
  ExecState s;
  auto* exec = lrvx_executor_create(make_cb(s));
  ASSERT_NE(exec, nullptr);

  LrvxExchangeCapabilities caps{};
  lrvx_executor_get_capabilities(exec, &caps);
  EXPECT_EQ(s.caps_calls.load(), 1);
  EXPECT_EQ(caps.supports_stop_market, 1);
  EXPECT_EQ(caps.supports_oco, 1);
  EXPECT_EQ(caps.supports_iceberg, 0);
  EXPECT_EQ(caps.supports_batch_submit, 1);

  lrvx_executor_destroy(exec);
}

TEST(CapiExecutor, GetCapabilitiesWithNullCallbackZeroes)
{
  // Executor with null capabilities callback — get should zero-init out.
  LrvxExecutorCallbacks cb{};
  cb.user_data = nullptr;
  auto* exec = lrvx_executor_create(cb);
  LrvxExchangeCapabilities caps{};
  // Pre-fill with garbage to make sure the API zeroes it.
  caps.supports_oco = 1;
  lrvx_executor_get_capabilities(exec, &caps);
  EXPECT_EQ(caps.supports_oco, 0);
  lrvx_executor_destroy(exec);
}

namespace
{

// Helper: build a runner + strategy that fires a market buy on the first
// trade event. Used by the runner-level executor tests.
struct RunnerCtx
{
  LrvxRegistryHandle registry{nullptr};
  LrvxStrategyHandle strategy{nullptr};
  LrvxRunnerHandle runner{nullptr};
  uint32_t symbol{0};
  ~RunnerCtx()
  {
    if (runner)
    {
      lrvx_runner_destroy(runner);
    }
    if (strategy)
    {
      lrvx_strategy_destroy(strategy);
    }
    if (registry)
    {
      lrvx_registry_destroy(registry);
    }
  }
};

void noop_signal(void*, const LrvxSignal*) {}

void make_market_buy_runner(RunnerCtx& s)
{
  s.registry = lrvx_registry_create();
  s.symbol = lrvx_registry_add_symbol(s.registry, "test", "BTC", 0.01);

  static std::atomic<bool> fired{false};
  fired.store(false);

  static LrvxStrategyHandle strat_capture;
  static uint32_t sym_capture;
  auto on_trade = +[](void* /*ud*/, const LrvxSymbolContext*, const LrvxTradeData*)
  {
    if (fired.exchange(true))
    {
      return;
    }
    lrvx_emit_market_buy(strat_capture, sym_capture, 100000000LL);
  };

  LrvxStrategyCallbacks scb{};
  scb.on_trade = on_trade;
  uint32_t syms[] = {s.symbol};
  s.strategy = lrvx_strategy_create(/*id=*/1, syms, 1, s.registry, scb);
  strat_capture = s.strategy;
  sym_capture = s.symbol;
  s.runner = lrvx_runner_create(s.registry, noop_signal, nullptr);
  lrvx_runner_add_strategy(s.runner, s.strategy);
}

}  // namespace

TEST(CapiExecutor, RunnerForwardsMarketBuyToExecutor)
{
  ExecState s;
  RunnerCtx ctx;
  make_market_buy_runner(ctx);

  auto* exec = lrvx_executor_create(make_cb(s));
  lrvx_runner_set_executor(ctx.runner, exec);
  lrvx_runner_start(ctx.runner);
  EXPECT_EQ(s.on_start.load(), 1);

  lrvx_runner_on_trade(ctx.runner, ctx.symbol, /*price=*/100.0, /*qty=*/1.0,
                       /*is_buy=*/1, /*ts=*/1'000);

  EXPECT_EQ(s.submit.load(), 1) << "executor should see the market buy";
  EXPECT_EQ(s.last_submit_symbol, ctx.symbol);
  EXPECT_EQ(s.last_submit_side, 0u) << "BUY → side=0";
  EXPECT_EQ(s.last_submit_type, 1u) << "MARKET → type=1";
  EXPECT_EQ(s.last_submit_qty_raw, 100000000LL);

  lrvx_runner_stop(ctx.runner);
  EXPECT_EQ(s.on_stop.load(), 1);

  lrvx_executor_destroy(exec);
}

TEST(CapiExecutor, RunnerForwardsCancelAndCancelAll)
{
  ExecState s;
  RunnerCtx ctx;
  ctx.registry = lrvx_registry_create();
  ctx.symbol = lrvx_registry_add_symbol(ctx.registry, "test", "BTC", 0.01);

  LrvxStrategyCallbacks scb{};
  uint32_t syms[] = {ctx.symbol};
  ctx.strategy = lrvx_strategy_create(1, syms, 1, ctx.registry, scb);
  ctx.runner = lrvx_runner_create(ctx.registry, noop_signal, nullptr);
  lrvx_runner_add_strategy(ctx.runner, ctx.strategy);

  auto* exec = lrvx_executor_create(make_cb(s));
  lrvx_runner_set_executor(ctx.runner, exec);
  lrvx_runner_start(ctx.runner);

  lrvx_emit_cancel(ctx.strategy, /*order_id=*/42);
  EXPECT_EQ(s.cancel.load(), 1);
  EXPECT_EQ(s.last_cancel_id, 42u);

  lrvx_emit_cancel_all(ctx.strategy, ctx.symbol);
  EXPECT_EQ(s.cancel_all.load(), 1);
  EXPECT_EQ(s.last_cancel_all_symbol, ctx.symbol);

  lrvx_emit_modify(ctx.strategy, /*order_id=*/7,
                   /*new_price_raw=*/10000000000LL, /*new_qty_raw=*/200000000LL);
  EXPECT_EQ(s.replace.load(), 1);
  EXPECT_EQ(s.last_replace_old_id, 7u);

  lrvx_runner_stop(ctx.runner);
  lrvx_executor_destroy(exec);
}

namespace
{
// A runner with one strategy over two symbols and the fake executor attached.
// Orders are emitted straight from the test; nothing leaves the process.
struct ModifyCtx : RunnerCtx
{
  ExecState state;
  LrvxExecutorHandle exec{nullptr};
  uint32_t other{0};

  ModifyCtx()
  {
    registry = lrvx_registry_create();
    other = lrvx_registry_add_symbol(registry, "test", "ETH", 0.01);
    symbol = lrvx_registry_add_symbol(registry, "test", "BTC", 0.01);
    LrvxStrategyCallbacks scb{};
    uint32_t syms[] = {other, symbol};
    strategy = lrvx_strategy_create(1, syms, 2, registry, scb);
    runner = lrvx_runner_create(registry, noop_signal, nullptr);
    lrvx_runner_add_strategy(runner, strategy);
    exec = lrvx_executor_create(make_cb(state));
    lrvx_runner_set_executor(runner, exec);
    lrvx_runner_start(runner);
  }
  ~ModifyCtx()
  {
    lrvx_runner_stop(runner);
    lrvx_executor_destroy(exec);
  }
};
}  // namespace

TEST(CapiExecutor, RunnerModifyCarriesTheOrderItNames)
{
  ModifyCtx ctx;
  ASSERT_NE(ctx.symbol, 0u) << "the modified order must not sit on symbol 0";

  const uint64_t id = lrvx_emit_limit_sell(ctx.strategy, ctx.symbol,
                                           /*price_raw=*/10100000000LL, /*qty_raw=*/300000000LL);
  ASSERT_NE(id, 0u);
  lrvx_emit_modify(ctx.strategy, id, /*new_price_raw=*/10200000000LL,
                   /*new_qty_raw=*/200000000LL);

  ASSERT_EQ(ctx.state.replace.load(), 1);
  const LrvxOrder& o = ctx.state.last_replace_order;
  EXPECT_EQ(ctx.state.last_replace_old_id, id);
  EXPECT_EQ(o.id, id);
  EXPECT_EQ(o.symbol, ctx.symbol);
  EXPECT_EQ(o.side, 1u) << "the order was a sell";
  EXPECT_EQ(o.type, 0u) << "LIMIT";
  EXPECT_EQ(o.price_raw, 10200000000LL);
  EXPECT_EQ(o.quantity_raw, 200000000LL);
  EXPECT_EQ(o.filled_quantity_raw, 0);
}

TEST(CapiExecutor, RunnerModifyKeepsAConditionalOrdersTypeAndTrigger)
{
  ModifyCtx ctx;
  const uint64_t id = lrvx_emit_stop_limit(ctx.strategy, ctx.symbol, /*side=*/1,
                                           /*trigger_raw=*/9900000000LL,
                                           /*limit_raw=*/9890000000LL, /*qty_raw=*/100000000LL);
  ASSERT_NE(id, 0u);
  lrvx_emit_modify(ctx.strategy, id, 9880000000LL, 100000000LL);

  ASSERT_EQ(ctx.state.replace.load(), 1);
  const LrvxOrder& o = ctx.state.last_replace_order;
  EXPECT_EQ(o.id, id);
  EXPECT_EQ(o.symbol, ctx.symbol);
  EXPECT_EQ(o.side, 1u);
  EXPECT_EQ(o.type, 3u) << "STOP_LIMIT";
  EXPECT_EQ(o.trigger_price_raw, 9900000000LL);
  EXPECT_EQ(o.price_raw, 9880000000LL);
}

TEST(CapiExecutor, RunnerModifyMovesTheTriggerOfAStopThatFillsAtMarket)
{
  ModifyCtx ctx;
  const uint64_t id = lrvx_emit_stop_market(ctx.strategy, ctx.symbol, /*side=*/1,
                                            /*trigger_raw=*/9900000000LL, /*qty_raw=*/100000000LL);
  ASSERT_NE(id, 0u);
  lrvx_emit_modify(ctx.strategy, id, 9950000000LL, 200000000LL);

  ASSERT_EQ(ctx.state.replace.load(), 1);
  const LrvxOrder& o = ctx.state.last_replace_order;
  EXPECT_EQ(o.id, id);
  EXPECT_EQ(o.symbol, ctx.symbol);
  EXPECT_EQ(o.side, 1u);
  EXPECT_EQ(o.type, 2u) << "STOP_MARKET";
  EXPECT_EQ(o.trigger_price_raw, 9950000000LL);
  EXPECT_EQ(o.price_raw, 0);
  EXPECT_EQ(o.quantity_raw, 200000000LL);
}

TEST(CapiExecutor, RunnerSecondModifySeesTheFirst)
{
  ModifyCtx ctx;
  const uint64_t id = lrvx_emit_limit_sell(ctx.strategy, ctx.symbol, 10100000000LL, 300000000LL);
  lrvx_emit_modify(ctx.strategy, id, 10200000000LL, 200000000LL);
  lrvx_emit_modify(ctx.strategy, id, 10300000000LL, 100000000LL);

  ASSERT_EQ(ctx.state.replace.load(), 2);
  const LrvxOrder& o = ctx.state.last_replace_order;
  EXPECT_EQ(o.id, id);
  EXPECT_EQ(o.symbol, ctx.symbol);
  EXPECT_EQ(o.side, 1u);
  EXPECT_EQ(o.price_raw, 10300000000LL);
  EXPECT_EQ(o.quantity_raw, 100000000LL);
}

TEST(CapiExecutor, RunnerModifyAfterCancelNamesNoOrder)
{
  ModifyCtx ctx;
  const uint64_t a = lrvx_emit_limit_sell(ctx.strategy, ctx.symbol, 10100000000LL, 300000000LL);
  const uint64_t b = lrvx_emit_limit_sell(ctx.strategy, ctx.other, 10100000000LL, 300000000LL);
  lrvx_emit_cancel(ctx.strategy, a);
  lrvx_emit_cancel_all(ctx.strategy, ctx.other);

  // The request still reaches the executor under the id the strategy named;
  // there is no order behind it any more, so no symbol or side to report.
  lrvx_emit_modify(ctx.strategy, a, 10200000000LL, 200000000LL);
  EXPECT_EQ(ctx.state.last_replace_order.id, a);
  EXPECT_EQ(ctx.state.last_replace_order.symbol, 0u);
  lrvx_emit_modify(ctx.strategy, b, 10200000000LL, 200000000LL);
  EXPECT_EQ(ctx.state.last_replace_order.id, b);
  EXPECT_EQ(ctx.state.last_replace_order.symbol, 0u);
  EXPECT_EQ(ctx.state.replace.load(), 2);
}

namespace
{
// The C API has no emitter for an OCO, so the signal goes in through the
// bridge, the way a binding built on it would send one.
void emitOco(LrvxStrategyHandle strategy, uint32_t symbol, uint64_t id)
{
  static_cast<lrvx::BridgeStrategy*>(strategy)->emit(
      lrvx::Signal::oco(symbol, lrvx::Side::SELL, lrvx::Price::fromRaw(10500000000LL),
                        lrvx::Price::fromRaw(9500000000LL), lrvx::Quantity::fromRaw(100000000LL),
                        id));
}
}  // namespace

TEST(CapiExecutor, RunnerOcoLegsCarryDifferentIds)
{
  ModifyCtx ctx;
  const uint64_t id = (uint64_t{1} << 32) + 77;
  emitOco(ctx.strategy, ctx.symbol, id);

  ASSERT_EQ(ctx.state.oco.load(), 1);
  const LrvxOrder& first = ctx.state.last_oco_first;
  const LrvxOrder& second = ctx.state.last_oco_second;
  EXPECT_EQ(first.id, id) << "the first leg keeps the id the strategy chose";
  EXPECT_NE(second.id, 0u);
  EXPECT_NE(second.id, first.id) << "a cancel or modify has to be able to name one leg";
  EXPECT_EQ(first.price_raw, 10500000000LL);
  EXPECT_EQ(second.price_raw, 9500000000LL);
  EXPECT_EQ(second.symbol, ctx.symbol);
  EXPECT_EQ(second.side, 1u);

  // A second pair does not reuse the first pair's second-leg id.
  const uint64_t firstPairSecondId = second.id;
  emitOco(ctx.strategy, ctx.symbol, id + 1);
  EXPECT_NE(ctx.state.last_oco_second.id, firstPairSecondId);
  EXPECT_NE(ctx.state.last_oco_second.id, ctx.state.last_oco_first.id);
}

TEST(CapiExecutor, RunnerModifyNamesOneOcoLeg)
{
  ModifyCtx ctx;
  const uint64_t id = (uint64_t{1} << 32) + 77;
  emitOco(ctx.strategy, ctx.symbol, id);
  ASSERT_EQ(ctx.state.oco.load(), 1);
  const uint64_t secondId = ctx.state.last_oco_second.id;
  ASSERT_NE(secondId, id);

  lrvx_emit_modify(ctx.strategy, secondId, 9400000000LL, 50000000LL);
  ASSERT_EQ(ctx.state.replace.load(), 1);
  EXPECT_EQ(ctx.state.last_replace_old_id, secondId);
  EXPECT_EQ(ctx.state.last_replace_order.id, secondId);
  EXPECT_EQ(ctx.state.last_replace_order.symbol, ctx.symbol);
  EXPECT_EQ(ctx.state.last_replace_order.side, 1u);
  EXPECT_EQ(ctx.state.last_replace_order.type, 0u) << "LIMIT";
  EXPECT_EQ(ctx.state.last_replace_order.price_raw, 9400000000LL);
  EXPECT_EQ(ctx.state.last_replace_order.quantity_raw, 50000000LL);

  lrvx_emit_modify(ctx.strategy, id, 10600000000LL, 100000000LL);
  ASSERT_EQ(ctx.state.replace.load(), 2);
  EXPECT_EQ(ctx.state.last_replace_order.id, id);
  EXPECT_EQ(ctx.state.last_replace_order.symbol, ctx.symbol);
  EXPECT_EQ(ctx.state.last_replace_order.price_raw, 10600000000LL);

  // Cancelling one leg forgets that leg only.
  lrvx_emit_cancel(ctx.strategy, secondId);
  lrvx_emit_modify(ctx.strategy, secondId, 9300000000LL, 50000000LL);
  EXPECT_EQ(ctx.state.last_replace_order.symbol, 0u);
  lrvx_emit_modify(ctx.strategy, id, 10700000000LL, 100000000LL);
  EXPECT_EQ(ctx.state.last_replace_order.symbol, ctx.symbol);
}

// Order ids carry the strategy's subscriber id in their upper half, so two
// strategies on one runner number their orders apart and the handler's record
// of forwarded orders, keyed by id alone, cannot mix them up.
TEST(CapiExecutor, TwoStrategiesOnOneRunnerNeverShareAnOrderId)
{
  // Declared first so it outlives the runner that holds it.
  struct Owned
  {
    LrvxStrategyHandle handle{nullptr};
    ~Owned() { lrvx_strategy_destroy(handle); }
  } owned;
  ModifyCtx ctx;
  LrvxStrategyCallbacks scb{};
  uint32_t syms[] = {ctx.other, ctx.symbol};
  owned.handle = lrvx_strategy_create(2, syms, 2, ctx.registry, scb);
  LrvxStrategyHandle second = owned.handle;
  ASSERT_NE(second, nullptr);
  lrvx_runner_add_strategy(ctx.runner, second);

  // Each strategy's first order: the same position in its own numbering.
  const uint64_t a = lrvx_emit_limit_sell(ctx.strategy, ctx.symbol, 10100000000LL, 300000000LL);
  const uint64_t b = lrvx_emit_limit_buy(second, ctx.other, 9900000000LL, 500000000LL);
  ASSERT_NE(a, b);
  EXPECT_EQ(a >> 32, 1u);
  EXPECT_EQ(b >> 32, 2u);
  EXPECT_EQ(a & 0xFFFFFFFFu, b & 0xFFFFFFFFu);

  lrvx_emit_modify(second, b, 9800000000LL, 400000000LL);
  EXPECT_EQ(ctx.state.last_replace_order.id, b);
  EXPECT_EQ(ctx.state.last_replace_order.symbol, ctx.other);
  EXPECT_EQ(ctx.state.last_replace_order.side, 0u);
  EXPECT_EQ(ctx.state.last_replace_order.price_raw, 9800000000LL);

  lrvx_emit_modify(ctx.strategy, a, 10200000000LL, 200000000LL);
  EXPECT_EQ(ctx.state.last_replace_order.id, a);
  EXPECT_EQ(ctx.state.last_replace_order.symbol, ctx.symbol);
  EXPECT_EQ(ctx.state.last_replace_order.side, 1u);

  // One strategy cancelling its order leaves the other's on record.
  lrvx_emit_cancel(ctx.strategy, a);
  lrvx_emit_modify(second, b, 9700000000LL, 400000000LL);
  EXPECT_EQ(ctx.state.last_replace_order.symbol, ctx.other);
}

TEST(CapiExecutor, NullDetachesExecutor)
{
  ExecState s;
  RunnerCtx ctx;
  make_market_buy_runner(ctx);

  auto* exec = lrvx_executor_create(make_cb(s));
  lrvx_runner_set_executor(ctx.runner, exec);
  lrvx_runner_start(ctx.runner);
  EXPECT_EQ(s.on_start.load(), 1);

  // Detach mid-run — outgoing executor sees on_stop.
  lrvx_runner_set_executor(ctx.runner, nullptr);
  EXPECT_EQ(s.on_stop.load(), 1);

  lrvx_runner_on_trade(ctx.runner, ctx.symbol, 100.0, 1.0, 1, 1'000);
  EXPECT_EQ(s.submit.load(), 0) << "no submit after detach";

  lrvx_runner_stop(ctx.runner);
  // No additional on_stop; the runner stopped after detachment.
  EXPECT_EQ(s.on_stop.load(), 1);
  lrvx_executor_destroy(exec);
}

TEST(CapiExecutor, BacktestRunnerSetExecutorRoutesSignals)
{
  ExecState s;
  auto* exec = lrvx_executor_create(make_cb(s));

  auto* registry = lrvx_registry_create();
  uint32_t sym = lrvx_registry_add_symbol(registry, "test", "BTC", 0.01);
  auto* btr = lrvx_backtest_runner_create(registry, 0.0, 10000.0);
  lrvx_backtest_runner_set_executor(btr, exec);

  static std::atomic<bool> fired{false};
  fired.store(false);
  struct St
  {
    LrvxStrategyHandle h;
    uint32_t sym;
  } st{nullptr, sym};
  auto on_bar = +[](void* ud, const LrvxSymbolContext*, const LrvxBarData*)
  {
    if (fired.exchange(true))
    {
      return;
    }
    auto* p = static_cast<St*>(ud);
    lrvx_emit_market_buy(p->h, p->sym, 100000000LL);
  };
  LrvxStrategyCallbacks scb{};
  scb.on_bar = on_bar;
  scb.user_data = &st;
  uint32_t syms[] = {sym};
  auto* strat = lrvx_strategy_create(1, syms, 1, registry, scb);
  st.h = strat;
  lrvx_backtest_runner_set_strategy(btr, strat);

  int64_t starts[1] = {1'000'000'000};
  int64_t ends[1] = {1'999'999'999};
  double opens[1] = {100.0}, highs[1] = {101.0}, lows[1] = {99.0};
  double closes[1] = {100.5}, volumes[1] = {10.0};
  LrvxBacktestStats stats{};
  int rc = lrvx_backtest_runner_run_bars(btr, starts, ends, opens, highs, lows,
                                         closes, volumes, 1, "BTC", 0, 0, &stats);
  EXPECT_EQ(rc, 1);
  EXPECT_GE(s.submit.load(), 1)
      << "BacktestRunner must route through the binding executor when set";

  lrvx_strategy_destroy(strat);
  lrvx_backtest_runner_destroy(btr);
  lrvx_registry_destroy(registry);
  lrvx_executor_destroy(exec);
}

TEST(CapiExecutor, NullCallbacksAreNoOp)
{
  // Executor with all callbacks null shouldn't crash even when the
  // runner forwards events.
  LrvxExecutorCallbacks cb{};
  auto* exec = lrvx_executor_create(cb);
  ASSERT_NE(exec, nullptr);

  RunnerCtx ctx;
  make_market_buy_runner(ctx);
  lrvx_runner_set_executor(ctx.runner, exec);
  lrvx_runner_start(ctx.runner);
  lrvx_runner_on_trade(ctx.runner, ctx.symbol, 100.0, 1.0, 1, 1'000);
  lrvx_emit_cancel(ctx.strategy, 1);
  lrvx_runner_stop(ctx.runner);

  lrvx_executor_destroy(exec);
}
