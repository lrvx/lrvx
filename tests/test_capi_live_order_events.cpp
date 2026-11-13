/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Order events a venue reported, on a runner and on a live engine: whose
// position a fill counts in, and which thread hands it to the strategy.

#include "lrvx/capi/bridge_strategy.h"
#include "lrvx/capi/lrvx_capi.h"
#include "lrvx/capi/order_status_names.hpp"
#include "lrvx/common.h"
#include "lrvx/execution/events/order_event.h"
#include "lrvx/strategy/signal.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

constexpr int64_t kPrice = 10100000000LL;
constexpr int64_t kQty = 300000000LL;

// What one strategy heard, in the order it heard it, and on which thread.
struct Heard
{
  struct Entry
  {
    char kind;  // 't' trade, 'f' fill, 'u' order update
    uint64_t order;
    int64_t position;
    std::thread::id thread;
  };

  std::vector<Entry> entries() const
  {
    std::lock_guard lock(mutex);
    return log;
  }
  std::vector<Entry> of(char kind) const
  {
    std::vector<Entry> out;
    for (const Entry& e : entries())
    {
      if (e.kind == kind)
      {
        out.push_back(e);
      }
    }
    return out;
  }
  void note(char kind, uint64_t order, int64_t position)
  {
    std::lock_guard lock(mutex);
    log.push_back({kind, order, position, std::this_thread::get_id()});
  }

  static void onTrade(void* ud, const LrvxSymbolContext* ctx, const LrvxTradeData*)
  {
    auto* self = static_cast<Heard*>(ud);
    if (self->tradeDelay.count() > 0)
    {
      std::this_thread::sleep_for(self->tradeDelay);
    }
    self->note('t', 0, ctx->position_raw);
  }
  static void onFill(void* ud, const LrvxSymbolContext* ctx, const LrvxOrderEventData* ev)
  {
    static_cast<Heard*>(ud)->note('f', ev->order_id, ctx->position_raw);
  }
  static void onUpdate(void* ud, const LrvxSymbolContext* ctx, const LrvxOrderEventData* ev)
  {
    static_cast<Heard*>(ud)->note('u', ev->order_id, ctx->position_raw);
  }

  std::chrono::milliseconds tradeDelay{0};
  mutable std::mutex mutex;
  std::vector<Entry> log;
};

struct OcoExecutor
{
  std::vector<std::pair<LrvxOrder, LrvxOrder>> legs;

  static void submitOco(void* ud, const LrvxOrder* a, const LrvxOrder* b)
  {
    static_cast<OcoExecutor*>(ud)->legs.emplace_back(*a, *b);
  }
};

// Two strategies on a synchronous runner (false) or a live engine (true).
class LiveOrderEvents : public ::testing::TestWithParam<bool>
{
 protected:
  void SetUp() override
  {
    registry = lrvx_registry_create();
    symbol = lrvx_registry_add_symbol(registry, "test", "BTC", 0.01);
    uint32_t syms[] = {symbol};
    for (int i = 0; i < 2; ++i)
    {
      LrvxStrategyCallbacks scb{};
      scb.on_trade = &Heard::onTrade;
      scb.on_fill = &Heard::onFill;
      scb.on_order_update = &Heard::onUpdate;
      scb.user_data = &heard[i];
      strategies[i] = lrvx_strategy_create(static_cast<uint32_t>(i + 1), syms, 1, registry, scb);
    }
    LrvxExecutorCallbacks ecb{};
    ecb.submit_oco = &OcoExecutor::submitOco;
    ecb.user_data = &oco;
    executor = lrvx_executor_create(ecb);
    if (GetParam())
    {
      engine = lrvx_live_engine_create(registry);
      lrvx_live_engine_set_executor(engine, executor);
      lrvx_live_engine_add_strategy(engine, strategies[0], nullptr, nullptr);
      lrvx_live_engine_add_strategy(engine, strategies[1], nullptr, nullptr);
    }
    else
    {
      runner = lrvx_runner_create(registry, nullptr, nullptr);
      lrvx_runner_set_executor(runner, executor);
      lrvx_runner_add_strategy(runner, strategies[0]);
      lrvx_runner_add_strategy(runner, strategies[1]);
    }
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
      lrvx_live_engine_stop(engine);
      lrvx_live_engine_destroy(engine);
    }
    lrvx_executor_destroy(executor);
    lrvx_strategy_destroy(strategies[0]);
    lrvx_strategy_destroy(strategies[1]);
    lrvx_registry_destroy(registry);
    lrvx_clear_last_error();
  }

  void start()
  {
    if (engine != nullptr)
    {
      lrvx_live_engine_start(engine);
    }
  }
  // Everything reported so far has reached its strategy.
  void settle()
  {
    if (engine != nullptr)
    {
      lrvx_live_engine_drain(engine);
    }
  }
  void report(uint64_t order, lrvx::OrderEventStatus status, int64_t qty = 0, uint8_t side = 0)
  {
    LrvxOrderEventData ev{};
    ev.order_id = order;
    ev.symbol_id = symbol;
    ev.side = side;
    ev.order_type = static_cast<uint8_t>(lrvx::OrderType::LIMIT);
    ev.status = static_cast<uint8_t>(status);
    ev.fill_qty_raw = qty;
    ev.fill_price_raw = kPrice;
    if (engine != nullptr)
    {
      lrvx_live_engine_on_order_event(engine, &ev);
    }
    else
    {
      lrvx_runner_on_order_event(runner, &ev);
    }
  }

  Heard heard[2];
  OcoExecutor oco;
  LrvxStrategyHandle strategies[2]{nullptr, nullptr};
  LrvxRegistryHandle registry{nullptr};
  LrvxExecutorHandle executor{nullptr};
  LrvxRunnerHandle runner{nullptr};
  LrvxLiveEngineHandle engine{nullptr};
  uint32_t symbol{0};
};

// One tracker per runner made every strategy read the net of all of them.
TEST_P(LiveOrderEvents, AFillCountsInThePositionOfTheStrategyThatIssuedTheOrder)
{
  using Status = lrvx::OrderEventStatus;
  start();
  const uint64_t ofFirst = lrvx_emit_limit_buy(strategies[0], symbol, kPrice, kQty);
  const uint64_t ofSecond = lrvx_emit_limit_buy(strategies[1], symbol, kPrice, kQty);

  report(ofSecond, Status::FILLED, kQty);
  settle();
  EXPECT_EQ(lrvx_position_raw(strategies[1], symbol), kQty);
  EXPECT_EQ(lrvx_position_raw(strategies[0], symbol), 0) << "it issued no filled order";

  report(ofFirst, Status::PARTIALLY_FILLED, 100000000LL, /*side=*/1);
  settle();
  EXPECT_EQ(lrvx_position_raw(strategies[0], symbol), -100000000LL);
  EXPECT_EQ(lrvx_position_raw(strategies[1], symbol), kQty);

  const auto fills = heard[0].of('f');
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].position, -100000000LL) << "the context counts its own fill only";
}

// An order nobody attached issued is heard by everyone and is nobody's.
TEST_P(LiveOrderEvents, AnOrderNoStrategyIssuedCountsInNoPosition)
{
  start();
  report(77, lrvx::OrderEventStatus::FILLED, kQty);
  settle();
  EXPECT_EQ(heard[0].of('f').size(), 1u);
  EXPECT_EQ(heard[1].of('f').size(), 1u);
  EXPECT_EQ(lrvx_position_raw(strategies[0], symbol), 0);
  EXPECT_EQ(lrvx_position_raw(strategies[1], symbol), 0);
}

// The second leg's id comes from the runner's block, not the strategy's.
TEST_P(LiveOrderEvents, TheSecondLegOfAnOcoBelongsToTheStrategyOfTheFirst)
{
  start();
  static_cast<lrvx::BridgeStrategy*>(strategies[1])
      ->emit(lrvx::Signal::oco(symbol, lrvx::Side::BUY, lrvx::Price::fromRaw(9500000000LL),
                               lrvx::Price::fromRaw(10500000000LL),
                               lrvx::Quantity::fromRaw(kQty), (uint64_t{2} << 32) + 900));
  ASSERT_EQ(oco.legs.size(), 1u);
  const uint64_t second = oco.legs[0].second.id;
  ASSERT_GE(second, lrvx::kRunnerOrderIdBase);

  report(second, lrvx::OrderEventStatus::FILLED, kQty);
  settle();
  EXPECT_TRUE(heard[0].of('f').empty()) << "the other strategy's leg";
  ASSERT_EQ(heard[1].of('f').size(), 1u);
  EXPECT_EQ(heard[1].of('f')[0].position, kQty);
  EXPECT_EQ(lrvx_position_raw(strategies[1], symbol), kQty);
  EXPECT_EQ(lrvx_position_raw(strategies[0], symbol), 0);
}

// A host that restarts with the account not flat had no way to tell the
// runner: the strategy read 0 until its next fill.
TEST_P(LiveOrderEvents, ASeededPositionIsWhatTheStrategyReadsAndFillsAddToIt)
{
  using Status = lrvx::OrderEventStatus;
  const auto seed = [this](LrvxStrategyHandle s, int64_t position, int64_t avg)
  {
    return engine != nullptr ? lrvx_live_engine_seed_position(engine, s, symbol, position, avg)
                             : lrvx_runner_seed_position(runner, s, symbol, position, avg);
  };
  start();
  ASSERT_EQ(seed(strategies[1], -200000000LL, kPrice), 1) << lrvx_last_error_message();
  EXPECT_EQ(lrvx_position_raw(strategies[1], symbol), -200000000LL);
  EXPECT_EQ(lrvx_position_raw(strategies[0], symbol), 0) << "the other strategy's is its own";

  const uint64_t id = lrvx_emit_limit_buy(strategies[1], symbol, kPrice, kQty);
  report(id, Status::FILLED, kQty);
  settle();
  const auto fills = heard[1].of('f');
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].position, 100000000LL) << "the fill adds to the seeded position";

  // Once: a second seed would count the position twice.
  lrvx_clear_last_error();
  EXPECT_EQ(seed(strategies[1], 100000000LL, kPrice), 0);
  EXPECT_NE(std::string(lrvx_last_error_message()).find("already holds"), std::string::npos)
      << lrvx_last_error_message();
  EXPECT_EQ(lrvx_position_raw(strategies[1], symbol), 100000000LL);

  // Zero is accepted and changes nothing.
  lrvx_clear_last_error();
  EXPECT_EQ(seed(strategies[0], 0, 0), 1);
  EXPECT_EQ(lrvx_position_raw(strategies[0], symbol), 0);

  // A strategy that is attached nowhere here.
  uint32_t syms[] = {symbol};
  LrvxStrategyCallbacks scb{};
  LrvxStrategyHandle stranger = lrvx_strategy_create(9, syms, 1, registry, scb);
  lrvx_clear_last_error();
  EXPECT_EQ(seed(stranger, 100000000LL, kPrice), 0);
  EXPECT_NE(std::string(lrvx_last_error_message()).find("not attached"), std::string::npos)
      << lrvx_last_error_message();
  lrvx_strategy_destroy(stranger);
  lrvx_clear_last_error();
}

INSTANTIATE_TEST_SUITE_P(RunnerAndLiveEngine, LiveOrderEvents, ::testing::Bool());

class LiveEngineOrderEvents : public LiveOrderEvents
{
};

// The callbacks ran inside lrvx_live_engine_on_order_event, on whatever
// thread a broker reported from.
TEST_P(LiveEngineOrderEvents, RunOnTheStrategysConsumerThreadNotTheCallers)
{
  using Status = lrvx::OrderEventStatus;
  start();
  const uint64_t id = lrvx_emit_limit_buy(strategies[0], symbol, kPrice, kQty);
  report(id, Status::ACCEPTED);
  report(id, Status::FILLED, kQty);
  settle();

  const auto log = heard[0].entries();
  ASSERT_EQ(log.size(), 2u);
  EXPECT_EQ(log[0].kind, 'u');
  EXPECT_EQ(log[1].kind, 'f') << "in the order reported";
  EXPECT_NE(log[0].thread, std::this_thread::get_id());
  EXPECT_EQ(log[0].thread, log[1].thread) << "one consumer hands a strategy its order events";
}

// A fill overtook the trades published before it: they were still in the
// strategy's queue when the fill's callback ran.
TEST_P(LiveEngineOrderEvents, ArriveAfterTheMarketDataPublishedBeforeThem)
{
  constexpr int kTrades = 20;
  heard[0].tradeDelay = std::chrono::milliseconds(2);
  start();
  const uint64_t id = lrvx_emit_limit_buy(strategies[0], symbol, kPrice, kQty);
  for (int i = 0; i < kTrades; ++i)
  {
    lrvx_live_engine_publish_trade(engine, symbol, 101.0, 1.0, 1, 1'000 + i);
  }
  report(id, lrvx::OrderEventStatus::FILLED, kQty);
  settle();

  const auto log = heard[0].entries();
  ASSERT_EQ(log.size(), static_cast<size_t>(kTrades) + 1);
  EXPECT_EQ(log.back().kind, 'f') << "every earlier trade was handed over first";
}

// Before start there is no consumer thread to hand the event to.
TEST_P(LiveEngineOrderEvents, AreDeliveredOnTheCallingThreadWhileTheEngineIsNotRunning)
{
  const uint64_t id = lrvx_emit_limit_buy(strategies[0], symbol, kPrice, kQty);
  report(id, lrvx::OrderEventStatus::FILLED, kQty);
  const auto fills = heard[0].of('f');
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].thread, std::this_thread::get_id());
}

TEST_P(LiveEngineOrderEvents, QueuedBeforeStopAreDeliveredBeforeStopReturns)
{
  start();
  const uint64_t id = lrvx_emit_limit_buy(strategies[0], symbol, kPrice, kQty);
  for (int i = 0; i < 50; ++i)
  {
    report(id, lrvx::OrderEventStatus::PARTIALLY_FILLED, 1000000LL);
  }
  lrvx_live_engine_stop(engine);
  EXPECT_EQ(heard[0].of('f').size(), 50u);
  EXPECT_EQ(lrvx_position_raw(strategies[0], symbol), 50000000LL);
}

TEST_P(LiveEngineOrderEvents, DrainReturnsAtOnceOnAnEngineThatIsNotRunning)
{
  lrvx_live_engine_drain(engine);
  start();
  lrvx_live_engine_drain(engine);
  lrvx_live_engine_stop(engine);
  lrvx_live_engine_drain(engine);
  EXPECT_EQ(lrvx_last_error_code(), 0) << lrvx_last_error_message();
}

INSTANTIATE_TEST_SUITE_P(LiveEngine, LiveEngineOrderEvents, ::testing::Values(true));

// Node and QuickJS kept a table of their own that named code 1 "ACCEPTED" and
// code 2 "PENDING_NEW"; no table named REJECTED_RATE_LIMIT.
TEST(OrderEventStatusNames, FollowTheEnum)
{
  using Status = lrvx::OrderEventStatus;
  const auto name = [](Status s)
  { return std::string(lrvx::capi::orderEventStatusName(static_cast<uint8_t>(s))); };
  EXPECT_EQ(name(Status::NEW), "NEW");
  EXPECT_EQ(name(Status::SUBMITTED), "SUBMITTED");
  EXPECT_EQ(name(Status::ACCEPTED), "ACCEPTED");
  EXPECT_EQ(name(Status::FILLED), "FILLED");
  EXPECT_EQ(name(Status::REPLACE_REJECTED), "REPLACE_REJECTED");
  EXPECT_EQ(name(Status::REJECTED_RATE_LIMIT), "REJECTED_RATE_LIMIT");
  EXPECT_EQ(name(Status::REPLACED_GAS), "REPLACED_GAS");
  EXPECT_EQ(lrvx::capi::kOrderEventStatusNameCount,
            static_cast<size_t>(Status::REPLACED_GAS) + 1);

  uint8_t code = 0;
  EXPECT_TRUE(lrvx::capi::orderEventStatusCodeFromName("ACCEPTED", &code));
  EXPECT_EQ(code, static_cast<uint8_t>(Status::ACCEPTED));
  EXPECT_FALSE(lrvx::capi::orderEventStatusCodeFromName("PENDING_NEW", &code));
  EXPECT_FALSE(lrvx::capi::orderEventStatusCodeFromName("UNKNOWN", &code));
}

}  // namespace
