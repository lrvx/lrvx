/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// A Modify signal carries only the order id and the new price and quantity.
// The runner has to turn it into a replacement that is still the same order:
// same id, same symbol, same side and type. These tests pin that on the
// built-in executor and on a binding-supplied one.

#include "lrvx/backtest/backtest_runner.h"
#include "lrvx/engine/symbol_registry.h"
#include "lrvx/execution/abstract_execution_listener.h"
#include "lrvx/execution/abstract_executor.h"
#include "lrvx/strategy/strategy.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <utility>
#include <vector>

using namespace lrvx;

namespace
{

constexpr int64_t kMinuteNs = 60'000'000'000LL;

SymbolId addSymbol(SymbolRegistry& reg, const std::string& name)
{
  SymbolInfo info;
  info.exchange = "test";
  info.symbol = name;
  info.type = InstrumentType::Spot;
  info.tickSize = Price::fromDouble(0.5);
  return reg.registerSymbol(info);
}

BarEvent makeBar(SymbolId sym, size_t index, double high, double low)
{
  const int64_t startNs = 1'700'000'000'000'000'000LL + static_cast<int64_t>(index) * kMinuteNs;
  BarEvent ev{};
  ev.symbol = sym;
  ev.barType = BarType::Time;
  ev.barTypeParam = static_cast<uint64_t>(kMinuteNs);
  ev.bar.open = Price::fromDouble(100.0);
  ev.bar.high = Price::fromDouble(high);
  ev.bar.low = Price::fromDouble(low);
  ev.bar.close = Price::fromDouble(100.0);
  ev.bar.volume = Volume::fromDouble(10.0);
  ev.bar.startTime = TimePoint{std::chrono::nanoseconds{startNs}};
  ev.bar.endTime = TimePoint{std::chrono::nanoseconds{startNs + kMinuteNs}};
  return ev;
}

// Six bars around 100. Only the fifth leaves the 99.5 .. 100.5 band, and it
// reaches `spikeHigh` and `spikeLow`.
std::vector<BarEvent> makeBars(SymbolId sym, double spikeHigh, double spikeLow)
{
  std::vector<BarEvent> bars;
  for (size_t i = 0; i < 6; ++i)
  {
    const bool spike = i == 4;
    bars.push_back(makeBar(sym, i, spike ? spikeHigh : 100.5, spike ? spikeLow : 99.5));
  }
  return bars;
}

// Sends a limit on the first bar (it reaches the book at the second bar's
// open), modifies it on the third and, when asked to, cancels it by the same
// id on the fourth.
class ModifyStrategy : public Strategy
{
 public:
  ModifyStrategy(SymbolId sym, const SymbolRegistry& reg, Side side, double restPx,
                 double newPx, double newQty, bool cancelAfter = false)
      : Strategy(1, std::vector<SymbolId>{sym}, reg),
        _sym(sym),
        _side(side),
        _restPx(restPx),
        _newPx(newPx),
        _newQty(newQty),
        _cancelAfter(cancelAfter)
  {
  }

  OrderId orderId{0};

 protected:
  void onSymbolBar(SymbolContext& /*ctx*/, const BarEvent& /*ev*/) override
  {
    ++_seen;
    if (_seen == 1)
    {
      const Price px = Price::fromDouble(_restPx);
      const Quantity qty = Quantity::fromDouble(1.0);
      orderId = _side == Side::BUY ? emitLimitBuy(_sym, px, qty) : emitLimitSell(_sym, px, qty);
    }
    else if (_seen == 3)
    {
      emitModify(orderId, Price::fromDouble(_newPx), Quantity::fromDouble(_newQty));
    }
    else if (_seen == 4 && _cancelAfter)
    {
      emitCancel(orderId);
    }
  }

 private:
  SymbolId _sym;
  Side _side;
  double _restPx;
  double _newPx;
  double _newQty;
  bool _cancelAfter;
  size_t _seen{0};
};

class ReplaceRecorder : public IOrderExecutionListener
{
 public:
  ReplaceRecorder() : IOrderExecutionListener(77) {}

  std::vector<std::pair<Order, Order>> replaced;
  std::vector<std::string> replaceRejects;
  std::vector<Order> canceled;

  void onOrderCanceled(const Order& o) override { canceled.push_back(o); }
  void onOrderReplaced(const Order& oldOrder, const Order& newOrder) override
  {
    replaced.emplace_back(oldOrder, newOrder);
  }
  void onOrderReplaceRejected(const Order&, const Order&, const std::string& reason) override
  {
    replaceRejects.push_back(reason);
  }
};

class RecordingExecutor : public IOrderExecutor
{
 public:
  std::vector<Order> submitted;
  std::vector<std::pair<OrderId, Order>> replaces;

  void start() override {}
  void stop() override {}
  void submitOrder(const Order& order) override { submitted.push_back(order); }
  void replaceOrder(OrderId oldOrderId, const Order& newOrder) override
  {
    replaces.emplace_back(oldOrderId, newOrder);
  }
};

}  // namespace

TEST(BacktestRunnerModify, ModifiedBuyKeepsItsIdentityAndFillsAtTheNewPrice)
{
  SymbolRegistry reg;
  SymbolId sym = addSymbol(reg, "BTCUSDT");

  // The market never comes near the original 90; the fifth bar trades through
  // the modified 99.
  ModifyStrategy strat(sym, reg, Side::BUY, 90.0, 99.0, 2.0);
  ReplaceRecorder rec;

  BacktestRunner runner;
  runner.setSymbolRegistry(&reg);
  runner.setStrategy(&strat);
  runner.addExecutionListener(&rec);
  BacktestResult result = runner.runBars(makeBars(sym, 100.5, 98.0));

  ASSERT_NE(strat.orderId, 0u);
  ASSERT_EQ(rec.replaced.size(), 1u);
  const Order& oldOrder = rec.replaced[0].first;
  const Order& newOrder = rec.replaced[0].second;
  EXPECT_EQ(oldOrder.id, strat.orderId);
  EXPECT_EQ(newOrder.id, strat.orderId) << "the replacement must stay reachable by the strategy's id";
  EXPECT_EQ(newOrder.symbol, sym);
  EXPECT_EQ(newOrder.side, Side::BUY);
  EXPECT_EQ(newOrder.type, OrderType::LIMIT);
  EXPECT_EQ(newOrder.price.raw(), Price::fromDouble(99.0).raw());
  EXPECT_EQ(newOrder.quantity.raw(), Quantity::fromDouble(2.0).raw());

  ASSERT_EQ(result.fills().size(), 1u);
  const Fill& fill = result.fills()[0];
  EXPECT_EQ(fill.orderId, strat.orderId);
  EXPECT_EQ(fill.symbol, sym);
  EXPECT_EQ(fill.side, Side::BUY);
  EXPECT_EQ(fill.price.raw(), Price::fromDouble(99.0).raw());
  EXPECT_EQ(fill.quantity.raw(), Quantity::fromDouble(2.0).raw());
}

TEST(BacktestRunnerModify, ModifiedSellStaysASell)
{
  SymbolRegistry reg;
  SymbolId sym = addSymbol(reg, "BTCUSDT");

  ModifyStrategy strat(sym, reg, Side::SELL, 110.0, 101.0, 0.5);

  BacktestRunner runner;
  runner.setSymbolRegistry(&reg);
  runner.setStrategy(&strat);
  BacktestResult result = runner.runBars(makeBars(sym, 102.0, 99.5));

  ASSERT_EQ(result.fills().size(), 1u);
  const Fill& fill = result.fills()[0];
  EXPECT_EQ(fill.orderId, strat.orderId);
  EXPECT_EQ(fill.symbol, sym);
  EXPECT_EQ(fill.side, Side::SELL);
  EXPECT_EQ(fill.price.raw(), Price::fromDouble(101.0).raw());
  EXPECT_EQ(fill.quantity.raw(), Quantity::fromDouble(0.5).raw());
}

TEST(BacktestRunnerModify, ModifiedOrderCancelsByTheSameId)
{
  SymbolRegistry reg;
  SymbolId sym = addSymbol(reg, "BTCUSDT");

  ModifyStrategy strat(sym, reg, Side::BUY, 90.0, 99.0, 2.0, /*cancelAfter=*/true);
  ReplaceRecorder rec;

  BacktestRunner runner;
  runner.setSymbolRegistry(&reg);
  runner.setStrategy(&strat);
  runner.addExecutionListener(&rec);
  BacktestResult result = runner.runBars(makeBars(sym, 100.5, 98.0));

  ASSERT_EQ(rec.canceled.size(), 1u);
  EXPECT_EQ(rec.canceled[0].id, strat.orderId);
  EXPECT_EQ(rec.canceled[0].symbol, sym);
  EXPECT_EQ(rec.canceled[0].price.raw(), Price::fromDouble(99.0).raw());
  EXPECT_TRUE(result.fills().empty());
}

TEST(BacktestRunnerModify, OffTickModifyIsRejectedAgainstTheOrdersOwnTick)
{
  SymbolRegistry reg;
  SymbolId sym = addSymbol(reg, "BTCUSDT");

  ModifyStrategy strat(sym, reg, Side::BUY, 90.0, 99.25, 2.0);
  ReplaceRecorder rec;

  BacktestRunner runner;
  runner.setSymbolRegistry(&reg);
  runner.setStrategy(&strat);
  runner.addExecutionListener(&rec);
  BacktestResult result = runner.runBars(makeBars(sym, 100.5, 98.0));

  EXPECT_TRUE(rec.replaced.empty());
  ASSERT_EQ(rec.replaceRejects.size(), 1u);
  EXPECT_EQ(rec.replaceRejects[0], "price_off_tick");
  EXPECT_TRUE(result.fills().empty()) << "the original order at 90 stays as it was";
}

TEST(BacktestRunnerModify, CustomExecutorReceivesTheWholeReplacement)
{
  SymbolRegistry reg;
  SymbolId sym = addSymbol(reg, "BTCUSDT");

  ModifyStrategy strat(sym, reg, Side::SELL, 110.0, 101.0, 0.5);
  RecordingExecutor exec;

  BacktestRunner runner;
  runner.setSymbolRegistry(&reg);
  runner.setStrategy(&strat);
  runner.setExecutor(&exec);
  runner.runBars(makeBars(sym, 100.5, 99.5));

  ASSERT_EQ(exec.submitted.size(), 1u);
  ASSERT_EQ(exec.replaces.size(), 1u);
  EXPECT_EQ(exec.replaces[0].first, strat.orderId);
  const Order& repl = exec.replaces[0].second;
  EXPECT_EQ(repl.id, strat.orderId);
  EXPECT_EQ(repl.symbol, sym);
  EXPECT_EQ(repl.side, Side::SELL);
  EXPECT_EQ(repl.type, OrderType::LIMIT);
  EXPECT_EQ(repl.price.raw(), Price::fromDouble(101.0).raw());
  EXPECT_EQ(repl.quantity.raw(), Quantity::fromDouble(0.5).raw());
}
