/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// The record a runner and a live engine keep of the orders they forwarded to
// an attached executor, which is what a Modify is built from. Two things
// were wrong with it: nothing ever took a finished order off it, and a Modify
// for an id it did not hold went out as a made-up LIMIT / BUY order with no
// symbol. Both paths are driven here with an in-process executor.

#include "lrvx/capi/bridge_strategy.h"
#include "lrvx/capi/lrvx_capi.h"
#include "lrvx/common.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace
{

struct FakeExecutor
{
  std::vector<LrvxOrder> submitted;
  std::vector<LrvxOrder> replaced;
  std::vector<uint64_t> cancelled;

  static void submit(void* ud, const LrvxOrder* o)
  {
    static_cast<FakeExecutor*>(ud)->submitted.push_back(*o);
  }
  static void replace(void* ud, uint64_t, const LrvxOrder* o)
  {
    static_cast<FakeExecutor*>(ud)->replaced.push_back(*o);
  }
  static void cancel(void* ud, uint64_t id)
  {
    static_cast<FakeExecutor*>(ud)->cancelled.push_back(id);
  }
  static void oco(void* ud, const LrvxOrder* first, const LrvxOrder* second)
  {
    static_cast<FakeExecutor*>(ud)->ocoLegs.emplace_back(*first, *second);
  }

  std::vector<std::pair<LrvxOrder, LrvxOrder>> ocoLegs;

  LrvxExecutorCallbacks callbacks()
  {
    LrvxExecutorCallbacks cb{};
    cb.submit = &FakeExecutor::submit;
    cb.replace = &FakeExecutor::replace;
    cb.cancel = &FakeExecutor::cancel;
    cb.submit_oco = &FakeExecutor::oco;
    cb.user_data = this;
    return cb;
  }
};

void noopSignal(void*, const LrvxSignal*) {}

// The three pre-trade gates, each answering from a flag the test flips.
struct Gates
{
  bool killSwitchAllows{true};
  bool validatorAllows{true};
  bool riskAllows{true};

  static uint8_t kill(void* ud, const LrvxSignal*)
  {
    return static_cast<Gates*>(ud)->killSwitchAllows ? 1 : 0;
  }
  static uint8_t validate(void* ud, const LrvxSignal*)
  {
    return static_cast<Gates*>(ud)->validatorAllows ? 1 : 0;
  }
  static uint8_t risk(void* ud, const LrvxSignal*)
  {
    return static_cast<Gates*>(ud)->riskAllows ? 1 : 0;
  }
};

constexpr int64_t kPrice = 10100000000LL;
constexpr int64_t kQty = 300000000LL;
const uint8_t kIoc = static_cast<uint8_t>(lrvx::TimeInForce::IOC);
const uint8_t kFok = static_cast<uint8_t>(lrvx::TimeInForce::FOK);
const uint8_t kGtc = static_cast<uint8_t>(lrvx::TimeInForce::GTC);

// Runs every case over both executor paths: the synchronous runner and the
// live engine.
class ForwardedOrders : public ::testing::TestWithParam<bool>
{
 protected:
  void SetUp() override
  {
    registry = lrvx_registry_create();
    symbol = lrvx_registry_add_symbol(registry, "test", "BTC", 0.01);
    LrvxStrategyCallbacks scb{};
    uint32_t syms[] = {symbol};
    strategy = lrvx_strategy_create(1, syms, 1, registry, scb);
    executor = lrvx_executor_create(fake.callbacks());
    if (GetParam())
    {
      engine = lrvx_live_engine_create(registry);
      lrvx_live_engine_set_executor(engine, executor);
      lrvx_live_engine_add_strategy(engine, strategy, noopSignal, nullptr);
    }
    else
    {
      runner = lrvx_runner_create(registry, noopSignal, nullptr);
      lrvx_runner_set_executor(runner, executor);
      lrvx_runner_add_strategy(runner, strategy);
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
      lrvx_live_engine_destroy(engine);
    }
    lrvx_executor_destroy(executor);
    lrvx_strategy_destroy(strategy);
    lrvx_registry_destroy(registry);
    lrvx_clear_last_error();
  }

  void closed(uint64_t id)
  {
    if (engine != nullptr)
    {
      lrvx_live_engine_order_closed(engine, id);
    }
    else
    {
      lrvx_runner_order_closed(runner, id);
    }
  }

  // A modify of `id`; true when the executor received a replace for it.
  bool modifyReaches(uint64_t id)
  {
    const size_t before = fake.replaced.size();
    lrvx_clear_last_error();
    lrvx_emit_modify(strategy, id, kPrice + 100000000LL, kQty);
    return fake.replaced.size() == before + 1;
  }

  void expectRefused(uint64_t id)
  {
    EXPECT_FALSE(modifyReaches(id)) << "order " << id << " went to the executor";
    EXPECT_NE(lrvx_last_error_code(), 0);
    const std::string why = lrvx_last_error_message();
    EXPECT_NE(why.find(std::to_string(id)), std::string::npos) << why;
    EXPECT_NE(why.find("unknown_order"), std::string::npos) << why;
  }

  LrvxRegistryHandle registry{nullptr};
  uint32_t symbol{0};
  LrvxStrategyHandle strategy{nullptr};
  LrvxRunnerHandle runner{nullptr};
  LrvxLiveEngineHandle engine{nullptr};
  LrvxExecutorHandle executor{nullptr};
  FakeExecutor fake;
};

}  // namespace

// The record had no way out for an order that filled: the host now says so.
TEST_P(ForwardedOrders, AClosedOrderLeavesTheRecord)
{
  const uint64_t a = lrvx_emit_limit_sell(strategy, symbol, kPrice, kQty);
  const uint64_t b = lrvx_emit_limit_sell(strategy, symbol, kPrice, kQty);
  ASSERT_TRUE(modifyReaches(a));
  EXPECT_EQ(lrvx_last_error_code(), 0) << lrvx_last_error_message();
  EXPECT_EQ(fake.replaced.back().symbol, symbol);

  closed(a);
  EXPECT_EQ(lrvx_last_error_code(), 0) << lrvx_last_error_message();
  expectRefused(a);

  // The other order is untouched, and closing it twice or closing an id the
  // record never held is not an error.
  ASSERT_TRUE(modifyReaches(b));
  EXPECT_EQ(fake.replaced.back().id, b);
  closed(b);
  closed(b);
  closed(123456789);
  EXPECT_EQ(lrvx_last_error_code(), 0) << lrvx_last_error_message();
  expectRefused(b);
}

// A Modify for an id the record does not hold used to be forwarded as a
// LIMIT / BUY order with symbol 0. It is not sent, and the reason is left
// in the last error.
TEST_P(ForwardedOrders, AModifyOfAnUnknownIdIsNotSent)
{
  expectRefused(7);
  EXPECT_TRUE(fake.replaced.empty());

  const uint64_t a = lrvx_emit_limit_sell(strategy, symbol, kPrice, kQty);
  lrvx_emit_cancel(strategy, a);
  ASSERT_EQ(fake.cancelled.size(), 1u);
  expectRefused(a);
  EXPECT_TRUE(fake.replaced.empty());
}

// A market, IOC or FOK order is answered at once. It was kept for good
// (market orders aside); none of the three is kept.
TEST_P(ForwardedOrders, OrdersThatCannotRestAreNotRecorded)
{
  const uint64_t market = lrvx_emit_market_buy(strategy, symbol, kQty);
  const uint64_t ioc = lrvx_emit_limit_buy_tif(strategy, symbol, kPrice, kQty, kIoc);
  const uint64_t fok = lrvx_emit_limit_sell_tif(strategy, symbol, kPrice, kQty, kFok);
  const uint64_t gtc = lrvx_emit_limit_buy_tif(strategy, symbol, kPrice, kQty, kGtc);
  ASSERT_EQ(fake.submitted.size(), 4u) << "all four still reach the executor";

  expectRefused(market);
  expectRefused(ioc);
  expectRefused(fok);
  EXPECT_TRUE(modifyReaches(gtc));
}

TEST_P(ForwardedOrders, OrderClosedOnANullHandleIsReported)
{
  lrvx_clear_last_error();
  lrvx_runner_order_closed(nullptr, 1);
  EXPECT_NE(lrvx_last_error_code(), 0);
  lrvx_clear_last_error();
  lrvx_live_engine_order_closed(nullptr, 1);
  EXPECT_NE(lrvx_last_error_code(), 0);
}

// A venue cancels the other leg of an OCO when one leg fills or is cancelled.
// The record kept that leg until it was cancelled or reported closed by its
// own id, which a host that only hears about the leg that finished never
// does.
TEST_P(ForwardedOrders, ClosingOneOcoLegTakesBothOffTheRecord)
{
  const auto emitOco = [this](uint64_t id)
  {
    static_cast<lrvx::BridgeStrategy*>(strategy)->emit(
        lrvx::Signal::oco(symbol, lrvx::Side::SELL, lrvx::Price::fromRaw(10500000000LL),
                          lrvx::Price::fromRaw(9500000000LL),
                          lrvx::Quantity::fromRaw(100000000LL), id));
    return fake.ocoLegs.back();
  };

  // Closed by the first leg's id.
  {
    const auto [first, second] = emitOco((uint64_t{1} << 32) + 500);
    ASSERT_NE(first.id, second.id);
    ASSERT_TRUE(modifyReaches(first.id));
    ASSERT_TRUE(modifyReaches(second.id));
    closed(first.id);
    expectRefused(first.id);
    expectRefused(second.id);
  }
  // Closed by the second leg's id.
  {
    const auto [first, second] = emitOco((uint64_t{1} << 32) + 501);
    closed(second.id);
    expectRefused(second.id);
    expectRefused(first.id);
  }
  // Cancelled by the strategy: the executor is asked to cancel the leg that
  // was named, and both leave the record.
  {
    const auto [first, second] = emitOco((uint64_t{1} << 32) + 502);
    const size_t before = fake.cancelled.size();
    lrvx_emit_cancel(strategy, first.id);
    ASSERT_EQ(fake.cancelled.size(), before + 1);
    EXPECT_EQ(fake.cancelled.back(), first.id);
    expectRefused(first.id);
    expectRefused(second.id);
  }
  // A plain order that shares nothing with an OCO is not touched by one.
  {
    const uint64_t plain = lrvx_emit_limit_sell(strategy, symbol, kPrice, kQty);
    const auto [first, second] = emitOco((uint64_t{1} << 32) + 503);
    closed(first.id);
    EXPECT_TRUE(modifyReaches(plain));
    // A modified leg is still its sibling's sibling.
    const auto [third, fourth] = emitOco((uint64_t{1} << 32) + 504);
    ASSERT_TRUE(modifyReaches(third.id));
    closed(fourth.id);
    expectRefused(third.id);
    (void)second;
  }
}

// A signal one of the three gates drops used to vanish: the emit returned an
// order id and nothing said the order went nowhere.
TEST_P(ForwardedOrders, ADroppedSignalLeavesItsReasonInTheLastError)
{
  Gates gates;
  LrvxKillSwitchHandle ks = lrvx_kill_switch_create({&Gates::kill, &gates});
  LrvxOrderValidatorHandle ov = lrvx_order_validator_create({&Gates::validate, &gates});
  LrvxRiskManagerHandle rm = lrvx_risk_manager_create({&Gates::risk, &gates});
  if (engine != nullptr)
  {
    lrvx_live_engine_set_kill_switch(engine, ks);
    lrvx_live_engine_set_order_validator(engine, ov);
    lrvx_live_engine_set_risk_manager(engine, rm);
  }
  else
  {
    lrvx_runner_set_kill_switch(runner, ks);
    lrvx_runner_set_order_validator(runner, ov);
    lrvx_runner_set_risk_manager(runner, rm);
  }

  const auto expectDropped = [this](uint64_t id, const std::string& gate,
                                    const std::string& token)
  {
    EXPECT_NE(lrvx_last_error_code(), 0) << "dropped by the " << gate << " and nothing said";
    const std::string why = lrvx_last_error_message();
    EXPECT_NE(why.find(std::to_string(id)), std::string::npos) << why;
    EXPECT_NE(why.find(gate), std::string::npos) << why;
    EXPECT_NE(why.find("limit"), std::string::npos) << why;
    EXPECT_EQ(why.rfind("[" + token + "]"), why.size() - token.size() - 2) << why;
  };

  // Nothing drops it: no error, and the order reaches the executor.
  lrvx_clear_last_error();
  const uint64_t passed = lrvx_emit_limit_sell(strategy, symbol, kPrice, kQty);
  EXPECT_EQ(lrvx_last_error_code(), 0) << lrvx_last_error_message();
  ASSERT_EQ(fake.submitted.size(), 1u);
  EXPECT_EQ(fake.submitted.back().id, passed);

  gates.riskAllows = false;
  lrvx_clear_last_error();
  const uint64_t byRisk = lrvx_emit_limit_sell(strategy, symbol, kPrice, kQty);
  expectDropped(byRisk, "risk manager", "risk_manager");

  gates.validatorAllows = false;
  lrvx_clear_last_error();
  const uint64_t byValidator = lrvx_emit_limit_sell(strategy, symbol, kPrice, kQty);
  expectDropped(byValidator, "order validator", "order_validator");

  // The kill switch is asked first, so it is the one named.
  gates.killSwitchAllows = false;
  lrvx_clear_last_error();
  const uint64_t byKill = lrvx_emit_limit_sell(strategy, symbol, kPrice, kQty);
  expectDropped(byKill, "kill switch", "kill_switch");

  EXPECT_EQ(fake.submitted.size(), 1u) << "none of the three reached the executor";

  // A dropped order was never put on the record: with the gates open again a
  // modify of it is refused as unknown.
  gates = Gates{};
  expectRefused(byKill);
  EXPECT_TRUE(modifyReaches(passed));

  if (engine != nullptr)
  {
    lrvx_live_engine_set_kill_switch(engine, nullptr);
    lrvx_live_engine_set_order_validator(engine, nullptr);
    lrvx_live_engine_set_risk_manager(engine, nullptr);
  }
  else
  {
    lrvx_runner_set_kill_switch(runner, nullptr);
    lrvx_runner_set_order_validator(runner, nullptr);
    lrvx_runner_set_risk_manager(runner, nullptr);
  }
  lrvx_kill_switch_destroy(ks);
  lrvx_order_validator_destroy(ov);
  lrvx_risk_manager_destroy(rm);
}

INSTANTIATE_TEST_SUITE_P(RunnerAndLiveEngine, ForwardedOrders, ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& info)
                         { return info.param ? "LiveEngine" : "Runner"; });
