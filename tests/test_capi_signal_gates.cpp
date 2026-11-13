/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// What a signal passes on its way out of a strategy, on the runner, the live
// engine and the backtest runner; and two C API inputs that used to be taken
// at less than face value: a segment operation's error list and a segment
// size in megabytes.

#include "lrvx/capi/lrvx_capi.h"
#include "lrvx/common.h"
#include "lrvx/execution/events/order_event.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace
{

struct FakeExecutor
{
  std::vector<LrvxOrder> submitted;
  std::vector<uint64_t> cancelled;
  std::vector<uint32_t> cancelledSymbols;
  std::vector<uint64_t> replaced;

  static void submit(void* ud, const LrvxOrder* o)
  {
    static_cast<FakeExecutor*>(ud)->submitted.push_back(*o);
  }
  static void cancel(void* ud, uint64_t id)
  {
    static_cast<FakeExecutor*>(ud)->cancelled.push_back(id);
  }

  static void cancelAll(void* ud, uint32_t symbol)
  {
    static_cast<FakeExecutor*>(ud)->cancelledSymbols.push_back(symbol);
  }
  static void replace(void* ud, uint64_t id, const LrvxOrder*)
  {
    static_cast<FakeExecutor*>(ud)->replaced.push_back(id);
  }

  LrvxExecutorCallbacks callbacks()
  {
    LrvxExecutorCallbacks cb{};
    cb.submit = &FakeExecutor::submit;
    cb.cancel = &FakeExecutor::cancel;
    cb.cancel_all = &FakeExecutor::cancelAll;
    cb.replace = &FakeExecutor::replace;
    cb.user_data = this;
    return cb;
  }
};

struct Gates
{
  bool killSwitchAllows{true};
  bool validatorAllows{true};
  bool riskAllows{true};
  int asked{0};

  static uint8_t kill(void* ud, const LrvxSignal*)
  {
    ++static_cast<Gates*>(ud)->asked;
    return static_cast<Gates*>(ud)->killSwitchAllows ? 1 : 0;
  }
  static uint8_t validate(void* ud, const LrvxSignal*)
  {
    ++static_cast<Gates*>(ud)->asked;
    return static_cast<Gates*>(ud)->validatorAllows ? 1 : 0;
  }
  static uint8_t risk(void* ud, const LrvxSignal*)
  {
    ++static_cast<Gates*>(ud)->asked;
    return static_cast<Gates*>(ud)->riskAllows ? 1 : 0;
  }
};

constexpr int64_t kPrice = 10100000000LL;
constexpr int64_t kQty = 300000000LL;

bool endsWith(const std::string& s, const std::string& tail)
{
  return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

// The synchronous runner (false) and the live engine (true), each created
// with no on_signal callback.
class NoSignalCallback : public ::testing::TestWithParam<bool>
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
    ks = lrvx_kill_switch_create({&Gates::kill, &gates});
    ov = lrvx_order_validator_create({&Gates::validate, &gates});
    rm = lrvx_risk_manager_create({&Gates::risk, &gates});
    if (GetParam())
    {
      engine = lrvx_live_engine_create(registry);
      lrvx_live_engine_set_executor(engine, executor);
      lrvx_live_engine_set_kill_switch(engine, ks);
      lrvx_live_engine_set_order_validator(engine, ov);
      lrvx_live_engine_set_risk_manager(engine, rm);
      lrvx_live_engine_add_strategy(engine, strategy, nullptr, nullptr);
    }
    else
    {
      runner = lrvx_runner_create(registry, nullptr, nullptr);
      lrvx_runner_set_executor(runner, executor);
      lrvx_runner_set_kill_switch(runner, ks);
      lrvx_runner_set_order_validator(runner, ov);
      lrvx_runner_set_risk_manager(runner, rm);
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
    lrvx_risk_manager_destroy(rm);
    lrvx_order_validator_destroy(ov);
    lrvx_kill_switch_destroy(ks);
    lrvx_executor_destroy(executor);
    lrvx_strategy_destroy(strategy);
    lrvx_registry_destroy(registry);
    lrvx_clear_last_error();
  }

  void reportFill(uint64_t order, int64_t qty)
  {
    LrvxOrderEventData ev{};
    ev.order_id = order;
    ev.symbol_id = symbol;
    ev.side = 0;
    ev.order_type = static_cast<uint8_t>(lrvx::OrderType::MARKET);
    ev.status = static_cast<uint8_t>(lrvx::OrderEventStatus::FILLED);
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

  FakeExecutor fake;
  Gates gates;
  LrvxRegistryHandle registry{nullptr};
  LrvxStrategyHandle strategy{nullptr};
  LrvxExecutorHandle executor{nullptr};
  LrvxKillSwitchHandle ks{nullptr};
  LrvxOrderValidatorHandle ov{nullptr};
  LrvxRiskManagerHandle rm{nullptr};
  LrvxRunnerHandle runner{nullptr};
  LrvxLiveEngineHandle engine{nullptr};
  uint32_t symbol{0};
};

// on_signal observes. Without one the early return skipped the gates and the
// executor both: nothing was gated and nothing was sent.
TEST_P(NoSignalCallback, TheSignalIsStillGatedAndForwarded)
{
  const uint64_t passed = lrvx_emit_limit_sell(strategy, symbol, kPrice, kQty);
  EXPECT_EQ(gates.asked, 3) << "every gate is asked";
  ASSERT_EQ(fake.submitted.size(), 1u) << "the order reaches the executor";
  EXPECT_EQ(fake.submitted.back().id, passed);
  EXPECT_EQ(lrvx_last_error_code(), 0) << lrvx_last_error_message();

  lrvx_emit_cancel(strategy, passed);
  ASSERT_EQ(fake.cancelled.size(), 1u);
  EXPECT_EQ(fake.cancelled.back(), passed);

  gates.riskAllows = false;
  lrvx_clear_last_error();
  const uint64_t dropped = lrvx_emit_limit_sell(strategy, symbol, kPrice, kQty);
  EXPECT_EQ(fake.submitted.size(), 1u) << "a dropped order is not sent";
  const std::string why = lrvx_last_error_message();
  EXPECT_NE(why.find(std::to_string(dropped)), std::string::npos) << why;
  EXPECT_TRUE(endsWith(why, "[risk_manager]")) << why;
}

// The live gates were asked about everything, so a triggered kill switch held
// a cancel back and a tightened limit held a strategy in its position; the
// backtest runner let all four through.
TEST_P(NoSignalCallback, ACancelAModifyAndAReduceOnlyOrderPassEveryGateUnasked)
{
  const uint64_t resting = lrvx_emit_limit_sell(strategy, symbol, kPrice, kQty);
  const uint64_t entry = lrvx_emit_market_buy(strategy, symbol, kQty);
  reportFill(entry, kQty);
  ASSERT_EQ(lrvx_position_raw(strategy, symbol), kQty);
  ASSERT_EQ(fake.submitted.size(), 2u);

  gates.killSwitchAllows = false;
  gates.validatorAllows = false;
  gates.riskAllows = false;
  gates.asked = 0;
  lrvx_clear_last_error();

  lrvx_emit_modify(strategy, resting, kPrice + 100000000LL, kQty);
  EXPECT_EQ(fake.replaced, std::vector<uint64_t>{resting});
  lrvx_emit_cancel(strategy, resting);
  EXPECT_EQ(fake.cancelled, std::vector<uint64_t>{resting});
  lrvx_emit_cancel_all(strategy, symbol);
  EXPECT_EQ(fake.cancelledSymbols, std::vector<uint32_t>{symbol});
  const uint64_t closing = lrvx_emit_close_position(strategy, symbol);
  ASSERT_EQ(fake.submitted.size(), 3u) << "the reduce-only order reaches the executor";
  EXPECT_EQ(fake.submitted.back().id, closing);
  EXPECT_NE(fake.submitted.back().flags & 0x01, 0) << "it is reduce-only";

  EXPECT_EQ(gates.asked, 0) << "no gate was asked about any of the four";
  EXPECT_EQ(lrvx_last_error_code(), 0) << lrvx_last_error_message();

  // An order that adds exposure is still dropped.
  lrvx_emit_market_buy(strategy, symbol, kQty);
  EXPECT_EQ(fake.submitted.size(), 3u);
  EXPECT_TRUE(endsWith(lrvx_last_error_message(), "[kill_switch]")) << lrvx_last_error_message();
}

INSTANTIATE_TEST_SUITE_P(RunnerAndLiveEngine, NoSignalCallback, ::testing::Bool());

// A strategy that emits one scripted signal per trade and keeps what the
// last error read straight after it.
struct ScriptedBacktest
{
  enum class Step
  {
    LimitBuy,
    MarketBuy,
    ClosePosition,
    Cancel,
  };

  ScriptedBacktest()
  {
    registry = lrvx_registry_create();
    symbol = lrvx_registry_add_symbol(registry, "test", "BTC", 0.01);
    LrvxStrategyCallbacks scb{};
    scb.on_trade = &ScriptedBacktest::onTrade;
    scb.user_data = this;
    uint32_t syms[] = {symbol};
    strategy = lrvx_strategy_create(1, syms, 1, registry, scb);
    runner = lrvx_backtest_runner_create(registry, 0.0, 10000.0);
    lrvx_backtest_runner_set_strategy(runner, strategy);
    ks = lrvx_kill_switch_create({&Gates::kill, &gates});
    ov = lrvx_order_validator_create({&Gates::validate, &gates});
    rm = lrvx_risk_manager_create({&Gates::risk, &gates});
    lrvx_backtest_runner_set_kill_switch(runner, ks);
    lrvx_backtest_runner_set_order_validator(runner, ov);
    lrvx_backtest_runner_set_risk_manager(runner, rm);
  }
  ~ScriptedBacktest()
  {
    lrvx_backtest_runner_destroy(runner);
    lrvx_risk_manager_destroy(rm);
    lrvx_order_validator_destroy(ov);
    lrvx_kill_switch_destroy(ks);
    lrvx_strategy_destroy(strategy);
    lrvx_registry_destroy(registry);
    lrvx_clear_last_error();
  }

  static void onTrade(void* ud, const LrvxSymbolContext*, const LrvxTradeData*)
  {
    auto* self = static_cast<ScriptedBacktest*>(ud);
    const size_t i = self->errors.size();
    if (i >= self->steps.size())
    {
      return;
    }
    if (self->before)
    {
      self->before(*self, i);
    }
    lrvx_clear_last_error();
    uint64_t id = 0;
    switch (self->steps[i])
    {
      case Step::LimitBuy:
        id = lrvx_emit_limit_buy(self->strategy, self->symbol, 9000000000LL, kQty);
        break;
      case Step::MarketBuy:
        id = lrvx_emit_market_buy(self->strategy, self->symbol, kQty);
        break;
      case Step::ClosePosition:
        id = lrvx_emit_close_position(self->strategy, self->symbol);
        break;
      case Step::Cancel:
        lrvx_emit_cancel(self->strategy, self->ids.empty() ? 1 : self->ids.front());
        break;
    }
    self->ids.push_back(id);
    self->errors.emplace_back(lrvx_last_error_message());
  }

  void run()
  {
    std::vector<int64_t> ts;
    std::vector<double> close;
    for (size_t i = 0; i < steps.size() + 1; ++i)
    {
      ts.push_back(1'700'000'000'000'000'000LL + static_cast<int64_t>(i) * 1'000'000'000LL);
      close.push_back(100.0);
    }
    LrvxBacktestStats stats{};
    ASSERT_EQ(lrvx_backtest_runner_run_ohlcv(runner, ts.data(), close.data(),
                                             static_cast<uint32_t>(ts.size()), "BTC", &stats),
              1)
        << lrvx_last_error_message();
  }

  Gates gates;
  std::vector<Step> steps;
  void (*before)(ScriptedBacktest&, size_t){nullptr};
  std::vector<uint64_t> ids;
  std::vector<std::string> errors;
  LrvxRegistryHandle registry{nullptr};
  LrvxStrategyHandle strategy{nullptr};
  LrvxBacktestRunnerHandle runner{nullptr};
  LrvxKillSwitchHandle ks{nullptr};
  LrvxOrderValidatorHandle ov{nullptr};
  LrvxRiskManagerHandle rm{nullptr};
  uint32_t symbol{0};
};

// A backtest drop left nothing behind: the emit returned an id and the order
// was never seen again.
TEST(BacktestRunnerGates, ADroppedSignalLeavesItsReasonInTheLastError)
{
  ScriptedBacktest bt;
  using Step = ScriptedBacktest::Step;
  bt.steps = {Step::LimitBuy, Step::LimitBuy, Step::LimitBuy, Step::LimitBuy};
  bt.before = [](ScriptedBacktest& self, size_t i)
  {
    self.gates.riskAllows = i != 1;
    self.gates.validatorAllows = i != 2;
    self.gates.killSwitchAllows = i != 3;
  };
  bt.run();

  ASSERT_EQ(bt.errors.size(), 4u);
  EXPECT_EQ(bt.errors[0], "") << "nothing dropped the first order";
  const char* gate[] = {"", "risk manager", "order validator", "kill switch"};
  const char* token[] = {"", "[risk_manager]", "[order_validator]", "[kill_switch]"};
  for (size_t i = 1; i < 4; ++i)
  {
    const std::string& why = bt.errors[i];
    EXPECT_NE(why.find("limit signal for order " + std::to_string(bt.ids[i])), std::string::npos)
        << why;
    EXPECT_NE(why.find(std::string("dropped by the ") + gate[i]), std::string::npos) << why;
    EXPECT_NE(why.find("it reached no executor"), std::string::npos) << why;
    EXPECT_TRUE(endsWith(why, token[i])) << why;
  }
}

// The two things the backtest runner's gates leave alone are not drops and
// leave no reason: a reduce-only order, and a cancel.
TEST(BacktestRunnerGates, AReduceOnlyOrderAndACancelPassEveryGateUnasked)
{
  ScriptedBacktest bt;
  using Step = ScriptedBacktest::Step;
  bt.steps = {Step::MarketBuy, Step::ClosePosition, Step::Cancel};
  bt.before = [](ScriptedBacktest& self, size_t i)
  {
    if (i == 1)
    {
      self.gates.asked = 0;
      self.gates.riskAllows = false;
      self.gates.validatorAllows = false;
    }
  };
  bt.run();

  ASSERT_EQ(bt.errors.size(), 3u);
  EXPECT_NE(bt.ids[1], 0u) << "there was a position to close";
  EXPECT_EQ(bt.errors[1], "");
  EXPECT_EQ(bt.errors[2], "");
  EXPECT_EQ(bt.gates.asked, 0) << "no gate was asked about either";
}

std::filesystem::path scratchDir(const char* name)
{
  auto dir = std::filesystem::temp_directory_path() / (std::string("lrvx_signal_gates_") + name);
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

// The struct-returning merge kept the first error only; a merge that skips
// two inputs said why for one.
TEST(SegmentOpErrors, AFailedMergeListsEveryError)
{
  const auto dir = scratchDir("merge");
  const std::string first = (dir / "first_missing.lrvx").string();
  const std::string second = (dir / "second_missing.lrvx").string();
  std::string paths = first;
  paths.push_back('\0');
  paths += second;
  paths.push_back('\0');

  lrvx_clear_last_error();
  const LrvxMergeResult r =
      lrvx_segment_merge_full(paths.data(), 2, (dir / "out").string().c_str(), "merged", 1);
  EXPECT_EQ(r.success, 0);
  const std::string why = lrvx_last_error_message();
  EXPECT_NE(why.find("first_missing"), std::string::npos) << why;
  EXPECT_NE(why.find("second_missing"), std::string::npos) << why;
  std::filesystem::remove_all(dir);
}

// megabytes * 2^20 wrapped: 2^44 + 1 MB became a 1 MB segment size.
TEST(SegmentSize, AMegabyteCountThatOverflowsIsRefused)
{
  const auto dir = scratchDir("size");
  const uint64_t tooMany = (std::numeric_limits<uint64_t>::max() >> 20) + 2;
  const std::string out = dir.string();

  lrvx_clear_last_error();
  LrvxDataWriterHandle writer = lrvx_data_writer_create(out.c_str(), tooMany, 0);
  EXPECT_EQ(writer, nullptr);
  std::string why = lrvx_last_error_message();
  EXPECT_NE(why.find("max_segment_mb " + std::to_string(tooMany)), std::string::npos) << why;
  if (writer != nullptr)
  {
    lrvx_data_writer_destroy(writer);
  }

  lrvx_clear_last_error();
  LrvxBinaryLogRecorderHookHandle hook =
      lrvx_binary_log_recorder_hook_create(out.c_str(), tooMany, 0, 0);
  EXPECT_EQ(hook, nullptr);
  why = lrvx_last_error_message();
  EXPECT_NE(why.find("max_segment_mb " + std::to_string(tooMany)), std::string::npos) << why;
  if (hook != nullptr)
  {
    lrvx_binary_log_recorder_hook_destroy(hook);
  }

  // The largest count that fits is taken.
  lrvx_clear_last_error();
  writer = lrvx_data_writer_create(out.c_str(), std::numeric_limits<uint64_t>::max() >> 20, 0);
  EXPECT_NE(writer, nullptr) << lrvx_last_error_message();
  if (writer != nullptr)
  {
    lrvx_data_writer_destroy(writer);
  }
  std::filesystem::remove_all(dir);
}

// What a strategy heard of its orders, per callback.
struct Heard
{
  struct Event
  {
    uint64_t order;
    uint8_t status;
    int64_t qty;
    int64_t position;
  };
  std::vector<Event> fills;
  std::vector<Event> updates;

  static void onFill(void* ud, const LrvxSymbolContext* ctx, const LrvxOrderEventData* ev)
  {
    static_cast<Heard*>(ud)->fills.push_back(
        {ev->order_id, ev->status, ev->fill_qty_raw, ctx->position_raw});
  }
  static void onUpdate(void* ud, const LrvxSymbolContext* ctx, const LrvxOrderEventData* ev)
  {
    static_cast<Heard*>(ud)->updates.push_back(
        {ev->order_id, ev->status, ev->fill_qty_raw, ctx->position_raw});
  }
};

// A runner and a live engine had no way to tell a strategy of a live order's
// fill: on_fill and on_order_update fired in a backtest only.
class ReportedOrderEvent : public ::testing::TestWithParam<bool>
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
      scb.on_fill = &Heard::onFill;
      scb.on_order_update = &Heard::onUpdate;
      scb.user_data = &heard[i];
      strategies[i] = lrvx_strategy_create(static_cast<uint32_t>(i + 1), syms, 1, registry, scb);
    }
    if (GetParam())
    {
      engine = lrvx_live_engine_create(registry);
      lrvx_live_engine_add_strategy(engine, strategies[0], nullptr, nullptr);
      lrvx_live_engine_add_strategy(engine, strategies[1], nullptr, nullptr);
    }
    else
    {
      runner = lrvx_runner_create(registry, nullptr, nullptr);
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
      lrvx_live_engine_destroy(engine);
    }
    // After the runner: a strategy keeps the position tracker it was given.
    EXPECT_EQ(lrvx_position_raw(strategies[1], symbol), positionAtTheEnd);
    lrvx_strategy_destroy(strategies[0]);
    lrvx_strategy_destroy(strategies[1]);
    lrvx_registry_destroy(registry);
    lrvx_clear_last_error();
  }

  void report(uint64_t order, lrvx::OrderEventStatus status, int64_t qty = 0)
  {
    LrvxOrderEventData ev{};
    ev.order_id = order;
    ev.symbol_id = symbol;
    ev.side = 0;
    ev.order_type = static_cast<uint8_t>(lrvx::OrderType::LIMIT);
    ev.status = static_cast<uint8_t>(status);
    ev.fill_qty_raw = qty;
    ev.fill_price_raw = kPrice;
    deliver(ev);
  }
  void deliver(const LrvxOrderEventData& ev)
  {
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
  LrvxStrategyHandle strategies[2]{nullptr, nullptr};
  LrvxRegistryHandle registry{nullptr};
  LrvxRunnerHandle runner{nullptr};
  LrvxLiveEngineHandle engine{nullptr};
  uint32_t symbol{0};
  int64_t positionAtTheEnd{0};
};

TEST_P(ReportedOrderEvent, ReachesTheStrategyThatIssuedTheOrderThroughTheBacktestCallbacks)
{
  using Status = lrvx::OrderEventStatus;
  const uint64_t ofSecond = lrvx_emit_limit_buy(strategies[1], symbol, kPrice, kQty);
  ASSERT_EQ(ofSecond >> 32, 2u);

  report(ofSecond, Status::ACCEPTED);
  report(ofSecond, Status::PARTIALLY_FILLED, 100000000LL);
  report(ofSecond, Status::FILLED, 200000000LL);

  EXPECT_TRUE(heard[0].fills.empty() && heard[0].updates.empty())
      << "the other strategy's order is not this one's";
  ASSERT_EQ(heard[1].updates.size(), 1u);
  EXPECT_EQ(heard[1].updates[0].status, static_cast<uint8_t>(Status::ACCEPTED));
  ASSERT_EQ(heard[1].fills.size(), 2u) << "fills reach on_fill and only on_fill";
  EXPECT_EQ(heard[1].fills[0].qty, 100000000LL);
  EXPECT_EQ(heard[1].fills[0].position, 100000000LL) << "the context counts the fill";
  EXPECT_EQ(heard[1].fills[1].position, 300000000LL);

  // An order no attached strategy issued goes to every subscriber.
  report(77, Status::CANCELED);
  EXPECT_EQ(heard[0].updates.size(), 1u);
  EXPECT_EQ(heard[1].updates.size(), 2u);
  positionAtTheEnd = 300000000LL;
}

TEST_P(ReportedOrderEvent, AStatusSideOrTypeOutOfRangeIsRefused)
{
  LrvxOrderEventData ev{};
  ev.order_id = (uint64_t{1} << 32) + 1;
  ev.symbol_id = symbol;
  ev.status = 200;
  deliver(ev);
  EXPECT_NE(std::string(lrvx_last_error_message()).find("status 200"), std::string::npos)
      << lrvx_last_error_message();

  ev.status = static_cast<uint8_t>(lrvx::OrderEventStatus::FILLED);
  ev.side = 7;
  lrvx_clear_last_error();
  deliver(ev);
  EXPECT_NE(std::string(lrvx_last_error_message()).find("side 7"), std::string::npos)
      << lrvx_last_error_message();

  ev.side = 1;
  ev.order_type = 99;
  lrvx_clear_last_error();
  deliver(ev);
  EXPECT_NE(std::string(lrvx_last_error_message()).find("order_type 99"), std::string::npos)
      << lrvx_last_error_message();
  EXPECT_TRUE(heard[0].fills.empty() && heard[0].updates.empty());
}

INSTANTIATE_TEST_SUITE_P(RunnerAndLiveEngine, ReportedOrderEvent, ::testing::Bool());

}  // namespace
