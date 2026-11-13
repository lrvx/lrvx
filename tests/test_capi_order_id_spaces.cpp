/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Order ids are (subscriber id << 32) + n. A runner or a live engine refuses
// a second strategy with a subscriber id it already holds, the ids it assigns
// itself (the second leg of an OCO) come from a block no strategy reaches,
// and the strategies of one live engine share the record of what went to the
// executor. Everything here stays in the process: the executor is a struct
// that remembers what it was handed.

#include "lrvx/capi/bridge_strategy.h"
#include "lrvx/capi/lrvx_capi.h"
#include "lrvx/common.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace
{

struct FakeExecutor
{
  std::vector<LrvxOrder> submitted;
  std::vector<LrvxOrder> replaced;
  std::vector<LrvxOrder> ocoSecondLegs;

  static void submit(void* ud, const LrvxOrder* o)
  {
    static_cast<FakeExecutor*>(ud)->submitted.push_back(*o);
  }
  static void replace(void* ud, uint64_t, const LrvxOrder* o)
  {
    static_cast<FakeExecutor*>(ud)->replaced.push_back(*o);
  }
  static void oco(void* ud, const LrvxOrder*, const LrvxOrder* second)
  {
    static_cast<FakeExecutor*>(ud)->ocoSecondLegs.push_back(*second);
  }

  LrvxExecutorCallbacks callbacks()
  {
    LrvxExecutorCallbacks cb{};
    cb.submit = &FakeExecutor::submit;
    cb.replace = &FakeExecutor::replace;
    cb.submit_oco = &FakeExecutor::oco;
    cb.user_data = this;
    return cb;
  }
};

void noopSignal(void*, const LrvxSignal*) {}

void emitOco(LrvxStrategyHandle strategy, uint32_t symbol, uint64_t id)
{
  static_cast<lrvx::BridgeStrategy*>(strategy)->emit(
      lrvx::Signal::oco(symbol, lrvx::Side::SELL, lrvx::Price::fromRaw(10500000000LL),
                        lrvx::Price::fromRaw(9500000000LL), lrvx::Quantity::fromRaw(100000000LL),
                        id));
}

class OrderIdSpaces : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    registry = lrvx_registry_create();
    symbol = lrvx_registry_add_symbol(registry, "test", "BTC", 0.01);
    lrvx_clear_last_error();
  }
  void TearDown() override
  {
    if (runner != nullptr)
    {
      lrvx_runner_destroy(runner);
    }
    if (engine != nullptr)
    {
      lrvx_live_engine_destroy(engine);
    }
    if (executor != nullptr)
    {
      lrvx_executor_destroy(executor);
    }
    for (auto* s : strategies)
    {
      lrvx_strategy_destroy(s);
    }
    lrvx_registry_destroy(registry);
  }

  LrvxStrategyHandle strategy(uint32_t id)
  {
    LrvxStrategyCallbacks scb{};
    uint32_t syms[] = {symbol};
    strategies.push_back(lrvx_strategy_create(id, syms, 1, registry, scb));
    return strategies.back();
  }

  void makeRunner()
  {
    runner = lrvx_runner_create(registry, noopSignal, nullptr);
    executor = lrvx_executor_create(fake.callbacks());
    lrvx_runner_set_executor(runner, executor);
  }
  void makeEngine()
  {
    engine = lrvx_live_engine_create(registry);
    executor = lrvx_executor_create(fake.callbacks());
    lrvx_live_engine_set_executor(engine, executor);
  }

  static std::string lastError() { return lrvx_last_error_message(); }

  LrvxRegistryHandle registry{nullptr};
  uint32_t symbol{0};
  LrvxRunnerHandle runner{nullptr};
  LrvxLiveEngineHandle engine{nullptr};
  LrvxExecutorHandle executor{nullptr};
  FakeExecutor fake;
  std::vector<LrvxStrategyHandle> strategies;
};

}  // namespace

TEST_F(OrderIdSpaces, RunnerRefusesASecondStrategyWithTheSameSubscriberId)
{
  makeRunner();
  LrvxStrategyHandle a = strategy(1);
  LrvxStrategyHandle b = strategy(1);

  lrvx_runner_add_strategy(runner, a);
  EXPECT_EQ(lrvx_last_error_code(), 0) << lastError();

  lrvx_runner_add_strategy(runner, b);
  EXPECT_NE(lrvx_last_error_code(), 0);
  EXPECT_NE(lastError().find("lrvx_runner_add_strategy"), std::string::npos) << lastError();
  EXPECT_NE(lastError().find("subscriber id 1"), std::string::npos) << lastError();

  // The refused strategy is not attached: its orders reach no executor.
  lrvx_emit_limit_buy(a, symbol, 9900000000LL, 100000000LL);
  lrvx_emit_limit_buy(b, symbol, 9800000000LL, 100000000LL);
  ASSERT_EQ(fake.submitted.size(), 1u);
  EXPECT_EQ(fake.submitted[0].price_raw, 9900000000LL);
}

TEST_F(OrderIdSpaces, RunnerTakesTwoStrategiesWithDifferentIds)
{
  makeRunner();
  lrvx_runner_add_strategy(runner, strategy(1));
  lrvx_runner_add_strategy(runner, strategy(2));
  EXPECT_EQ(lrvx_last_error_code(), 0) << lastError();
}

TEST_F(OrderIdSpaces, LiveEngineRefusesASecondStrategyWithTheSameSubscriberId)
{
  makeEngine();
  LrvxStrategyHandle a = strategy(7);
  LrvxStrategyHandle b = strategy(7);

  lrvx_live_engine_add_strategy(engine, a, noopSignal, nullptr);
  EXPECT_EQ(lrvx_last_error_code(), 0) << lastError();
  lrvx_live_engine_add_strategy(engine, b, noopSignal, nullptr);
  EXPECT_NE(lrvx_last_error_code(), 0);
  EXPECT_NE(lastError().find("lrvx_live_engine_add_strategy"), std::string::npos) << lastError();
  EXPECT_NE(lastError().find("subscriber id 7"), std::string::npos) << lastError();

  lrvx_emit_limit_buy(b, symbol, 9800000000LL, 100000000LL);
  EXPECT_TRUE(fake.submitted.empty());
}

TEST_F(OrderIdSpaces, ANullStrategyIsAReportedError)
{
  makeRunner();
  lrvx_runner_add_strategy(runner, nullptr);
  EXPECT_NE(lrvx_last_error_code(), 0);
  EXPECT_NE(lastError().find("strategy"), std::string::npos) << lastError();
}

TEST_F(OrderIdSpaces, StrategyIdCanBeReadAndChangedBeforeAttach)
{
  LrvxStrategyHandle s = strategy(1);
  EXPECT_EQ(lrvx_strategy_id(s), 1u);
  lrvx_strategy_set_id(s, 5);
  EXPECT_EQ(lrvx_strategy_id(s), 5u);

  makeRunner();
  lrvx_runner_add_strategy(runner, s);
  const uint64_t id = lrvx_emit_limit_buy(s, symbol, 9900000000LL, 100000000LL);
  EXPECT_EQ(id >> 32, 5u);
}

// The second leg used to take 1, 2, 3...: exactly the ids a strategy with
// subscriber id 0 issues.
TEST_F(OrderIdSpaces, RunnerOcoSecondLegCannotEqualAnIdOfSubscriberZero)
{
  makeRunner();
  LrvxStrategyHandle s = strategy(0);
  lrvx_runner_add_strategy(runner, s);
  ASSERT_EQ(lrvx_last_error_code(), 0) << lastError();

  std::vector<uint64_t> own;
  for (int i = 0; i < 4; ++i)
  {
    own.push_back(lrvx_emit_limit_buy(s, symbol, 9900000000LL, 100000000LL));
  }
  ASSERT_EQ(own.front(), 1u);
  emitOco(s, symbol, 1000);
  emitOco(s, symbol, 1001);

  ASSERT_EQ(fake.ocoSecondLegs.size(), 2u);
  for (const auto& leg : fake.ocoSecondLegs)
  {
    EXPECT_EQ(leg.id >> 32, lrvx::kRunnerOrderIdSubscriber) << leg.id;
    for (uint64_t id : own)
    {
      EXPECT_NE(leg.id, id);
    }
  }
  EXPECT_EQ(fake.ocoSecondLegs[0].id, lrvx::kRunnerOrderIdBase + 1);
  EXPECT_EQ(fake.ocoSecondLegs[1].id, lrvx::kRunnerOrderIdBase + 2);
  // Exact as a double, which is how a JavaScript binding hands an id over.
  const auto asDouble = static_cast<double>(fake.ocoSecondLegs[1].id);
  EXPECT_EQ(static_cast<uint64_t>(asDouble), fake.ocoSecondLegs[1].id);
}

TEST_F(OrderIdSpaces, AStrategyHoldingTheRunnersBlockIsRefused)
{
  makeRunner();
  lrvx_runner_add_strategy(runner,
                           strategy(static_cast<uint32_t>(lrvx::kRunnerOrderIdSubscriber)));
  EXPECT_NE(lrvx_last_error_code(), 0);
  EXPECT_NE(lastError().find("2097151"), std::string::npos) << lastError();

  lrvx_clear_last_error();
  makeEngine();
  lrvx_live_engine_add_strategy(
      engine, strategy(static_cast<uint32_t>(lrvx::kRunnerOrderIdSubscriber)), noopSignal,
      nullptr);
  EXPECT_NE(lrvx_last_error_code(), 0);
}

TEST_F(OrderIdSpaces, BacktestRunnerRefusesAStrategyHoldingItsBlock)
{
  LrvxBacktestRunnerHandle bt = lrvx_backtest_runner_create(registry, 0.0, 10000.0);
  ASSERT_NE(bt, nullptr);
  lrvx_backtest_runner_set_strategy(
      bt, strategy(static_cast<uint32_t>(lrvx::kRunnerOrderIdSubscriber)));
  EXPECT_NE(lrvx_last_error_code(), 0);
  EXPECT_NE(lastError().find("2097151"), std::string::npos) << lastError();

  lrvx_clear_last_error();
  lrvx_backtest_runner_set_strategy(bt, strategy(0));
  EXPECT_EQ(lrvx_last_error_code(), 0) << lastError();
  lrvx_backtest_runner_destroy(bt);
}

TEST_F(OrderIdSpaces, LiveEngineOcoSecondLegComesFromTheSameBlock)
{
  makeEngine();
  LrvxStrategyHandle a = strategy(0);
  LrvxStrategyHandle b = strategy(3);
  lrvx_live_engine_add_strategy(engine, a, noopSignal, nullptr);
  lrvx_live_engine_add_strategy(engine, b, noopSignal, nullptr);

  emitOco(a, symbol, 77);
  emitOco(b, symbol, (uint64_t{3} << 32) + 77);
  ASSERT_EQ(fake.ocoSecondLegs.size(), 2u);
  // One count for the engine: the two strategies share an executor, which
  // must not see an id twice.
  EXPECT_EQ(fake.ocoSecondLegs[0].id, lrvx::kRunnerOrderIdBase + 1);
  EXPECT_EQ(fake.ocoSecondLegs[1].id, lrvx::kRunnerOrderIdBase + 2);
}

// Each strategy of a live engine had its own record of forwarded orders, so
// a modify of an order another strategy placed went out as a bare LIMIT with
// no symbol and no side.
TEST_F(OrderIdSpaces, LiveEngineModifyOfAnotherStrategysOrderCarriesTheOrder)
{
  makeEngine();
  LrvxStrategyHandle placer = strategy(1);
  LrvxStrategyHandle manager = strategy(2);
  lrvx_live_engine_add_strategy(engine, placer, noopSignal, nullptr);
  lrvx_live_engine_add_strategy(engine, manager, noopSignal, nullptr);

  const uint64_t id =
      lrvx_emit_stop_market(placer, symbol, /*side=*/1, 9500000000LL, 300000000LL);
  ASSERT_EQ(fake.submitted.size(), 1u);
  const LrvxOrder placed = fake.submitted[0];
  ASSERT_EQ(placed.id, id);

  lrvx_emit_modify(manager, id, 9400000000LL, 200000000LL);
  ASSERT_EQ(fake.replaced.size(), 1u);
  const LrvxOrder& sent = fake.replaced[0];
  EXPECT_EQ(sent.id, id);
  EXPECT_EQ(sent.symbol, symbol);
  EXPECT_EQ(sent.side, 1u);
  EXPECT_EQ(sent.type, placed.type) << "a stop stays a stop";
  EXPECT_EQ(sent.trigger_price_raw, 9400000000LL);
  EXPECT_EQ(sent.quantity_raw, 200000000LL);

  // The placer's own later modify starts from what the manager sent.
  lrvx_emit_modify(placer, id, 9300000000LL, 200000000LL);
  ASSERT_EQ(fake.replaced.size(), 2u);
  EXPECT_EQ(fake.replaced[1].symbol, symbol);
  EXPECT_EQ(fake.replaced[1].trigger_price_raw, 9300000000LL);

  // A cancel by either strategy forgets the order for both.
  lrvx_emit_cancel(manager, id);
  lrvx_emit_modify(placer, id, 9200000000LL, 200000000LL);
  ASSERT_EQ(fake.replaced.size(), 3u);
  EXPECT_EQ(fake.replaced[2].symbol, 0u);
}

// A strategy added after others have placed orders joins the record, it does
// not empty it; swapping the executor still does.
TEST_F(OrderIdSpaces, LiveEngineRecordSurvivesALaterStrategyAndEndsWithTheExecutor)
{
  makeEngine();
  LrvxStrategyHandle placer = strategy(1);
  lrvx_live_engine_add_strategy(engine, placer, noopSignal, nullptr);
  const uint64_t id = lrvx_emit_limit_sell(placer, symbol, 10100000000LL, 300000000LL);

  LrvxStrategyHandle late = strategy(2);
  lrvx_live_engine_add_strategy(engine, late, noopSignal, nullptr);
  lrvx_emit_modify(late, id, 10200000000LL, 300000000LL);
  ASSERT_EQ(fake.replaced.size(), 1u);
  EXPECT_EQ(fake.replaced[0].symbol, symbol);
  EXPECT_EQ(fake.replaced[0].side, 1u);

  FakeExecutor other;
  LrvxExecutorHandle second = lrvx_executor_create(other.callbacks());
  lrvx_live_engine_set_executor(engine, second);
  lrvx_emit_modify(late, id, 10300000000LL, 300000000LL);
  ASSERT_EQ(other.replaced.size(), 1u);
  EXPECT_EQ(other.replaced[0].symbol, 0u) << "the new executor never held that order";
  lrvx_live_engine_set_executor(engine, nullptr);
  lrvx_executor_destroy(second);
}

// The backtest runner counted second legs from 1 as well.
TEST_F(OrderIdSpaces, BacktestOcoSecondLegCannotEqualAnIdOfSubscriberZero)
{
  LrvxBacktestRunnerHandle bt = lrvx_backtest_runner_create(registry, 0.0, 10000.0);
  executor = lrvx_executor_create(fake.callbacks());
  lrvx_backtest_runner_set_executor(bt, executor);

  struct State
  {
    LrvxStrategyHandle handle{nullptr};
    uint32_t symbol{0};
    std::vector<uint64_t> own;
    bool fired{false};
  } state;
  state.symbol = symbol;
  LrvxStrategyCallbacks scb{};
  scb.user_data = &state;
  scb.on_bar = +[](void* ud, const LrvxSymbolContext*, const LrvxBarData*)
  {
    auto* st = static_cast<State*>(ud);
    if (st->fired)
    {
      return;
    }
    st->fired = true;
    st->own.push_back(lrvx_emit_limit_buy(st->handle, st->symbol, 9000000000LL, 100000000LL));
    st->own.push_back(lrvx_emit_limit_buy(st->handle, st->symbol, 9000000000LL, 100000000LL));
    emitOco(st->handle, st->symbol, 1000);
    emitOco(st->handle, st->symbol, 1001);
  };
  uint32_t syms[] = {symbol};
  state.handle = lrvx_strategy_create(0, syms, 1, registry, scb);
  strategies.push_back(state.handle);
  lrvx_backtest_runner_set_strategy(bt, state.handle);

  int64_t starts[1] = {1'000'000'000};
  int64_t ends[1] = {1'999'999'999};
  double opens[1] = {100.0}, highs[1] = {101.0}, lows[1] = {99.0};
  double closes[1] = {100.5}, volumes[1] = {10.0};
  LrvxBacktestStats stats{};
  ASSERT_EQ(lrvx_backtest_runner_run_bars(bt, starts, ends, opens, highs, lows, closes, volumes, 1,
                                          "BTC", 0, 0, &stats),
            1)
      << lastError();

  ASSERT_EQ(state.own, (std::vector<uint64_t>{1, 2}));
  ASSERT_EQ(fake.ocoSecondLegs.size(), 2u);
  // The same block and the same count as the live path.
  EXPECT_EQ(fake.ocoSecondLegs[0].id, lrvx::kRunnerOrderIdBase + 1);
  EXPECT_EQ(fake.ocoSecondLegs[1].id, lrvx::kRunnerOrderIdBase + 2);
  lrvx_backtest_runner_destroy(bt);
}
