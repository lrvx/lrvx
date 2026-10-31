// docs/examples/cpp_sma_backtest.cpp
//
// SMA(10/30) crossover backtest in C++. Demonstrates the same pattern as
// docs/examples/python_backtest_vs_live.py — single strategy class, run
// it through BacktestRunner against a CSV.
//
// Build:
//   cmake -B build -DLRVX_ENABLE_BACKTEST=ON -DCMAKE_BUILD_TYPE=Release
//   cmake --build build --target cpp_sma_backtest
//
// Run:
//   ./build/docs/examples/cpp_sma_backtest docs/examples/data/btcusdt_1m.csv

#include "lrvx/backtest/backtest_config.h"
#include "lrvx/backtest/backtest_runner.h"
#include "lrvx/engine/symbol_registry.h"
#include "lrvx/replay/abstract_event_reader.h"
#include "lrvx/replay/readers/csv_ohlcv_reader.h"
#include "lrvx/strategy/strategy.h"

#include <deque>
#include <iostream>

using namespace lrvx;

class SmaCrossover : public Strategy
{
 public:
  SmaCrossover(SymbolId symbol, const SymbolRegistry& registry,
               size_t fast = 10, size_t slow = 30, double size = 0.01)
      : Strategy(/*id=*/1, symbol, registry),
        _fast(fast),
        _slow(slow),
        _size(Quantity::fromDouble(size))
  {
  }

  void start() override
  {
    _running = true;
    std::cout << "  SmaCrossover started\n";
  }
  void stop() override
  {
    _running = false;
    std::cout << "  SmaCrossover stopped (" << _signals << " signals)\n";
  }

 protected:
  void onSymbolTrade(SymbolContext& /*ctx*/, const TradeEvent& ev) override
  {
    if (!_running)
    {
      return;
    }
    _prices.push_back(ev.trade.price.toDouble());
    if (_prices.size() > _slow)
    {
      _prices.pop_front();
    }
    if (_prices.size() < _slow)
    {
      return;
    }

    double f = sma(_fast), s = sma(_slow);
    bool above = f > s;
    if (above && !_prevAbove && !_long)
    {
      emitMarketBuy(symbol(), _size);
      _long = true;
      _short = false;
      ++_signals;
    }
    else if (!above && _prevAbove && !_short)
    {
      emitMarketSell(symbol(), _size);
      _short = true;
      _long = false;
      ++_signals;
    }
    _prevAbove = above;
  }

 private:
  double sma(size_t n) const
  {
    double sum = 0;
    auto it = _prices.end();
    for (size_t i = 0; i < n; ++i)
    {
      sum += *--it;
    }
    return sum / static_cast<double>(n);
  }

  size_t _fast, _slow;
  Quantity _size;
  std::deque<double> _prices;
  bool _running{false}, _prevAbove{false}, _long{false}, _short{false};
  size_t _signals{0};
};

int main(int argc, char** argv)
{
  if (argc < 2)
  {
    std::cerr << "usage: " << argv[0] << " <csv_path>\n";
    return 1;
  }

  SymbolRegistry registry;
  SymbolInfo info{.exchange = "binance", .symbol = "BTCUSDT", .tickSize = Price::fromDouble(0.01)};
  auto symId = registry.registerSymbol(info);

  auto reader = replay::createCsvOhlcvReader(argv[1], symId);

  BacktestConfig config{
      .initialCapital = 10'000.0,
      .feeRate = 0.0004,  // 4 bps
  };
  BacktestRunner runner(config);

  SmaCrossover strat(symId, registry);
  runner.setStrategy(&strat);

  auto result = runner.run(*reader);
  auto stats = result.computeStats();

  std::cout << "  Return   : " << stats.returnPct << "%\n"
            << "  Trades   : " << stats.totalTrades
            << "  win=" << stats.winRate * 100.0 << "%\n"
            << "  Sharpe   : " << stats.sharpeRatio << "\n"
            << "  Max DD   : " << stats.maxDrawdownPct << "%\n"
            << "  Net PnL  : " << stats.netPnl << "\n";
  return 0;
}
