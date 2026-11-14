/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "bindings_common.h"
#include "lrvx/aggregator/events/bar_event.h"
#include "lrvx/backtest/walk_forward.h"
#include "lrvx/common.h"
#include "lrvx/error/lrvx_error.h"
#include "lrvx/replay/ohlcv_replay_source.h"
#include "strategy_bindings.h"

#include <pybind11/numpy.h>
#include <chrono>

#include <fstream>
#include <sstream>

namespace py = pybind11;

namespace lrvx
{

inline std::vector<lrvx::OhlcvReplaySource::Bar> loadOhlcvCsvForWf(
    const std::string& path, lrvx::SymbolId symbolId)
{
  std::vector<lrvx::OhlcvReplaySource::Bar> bars;
  std::ifstream f(path);
  if (!f.is_open())
  {
    throw lrvx::LrvxError(
        "E_IO_001",
        "Cannot open file: '" + path +
            "'. Check the path is correct and the file is readable.");
  }
  std::string line;
  std::getline(f, line);  // header
  while (std::getline(f, line))
  {
    if (line.empty())
    {
      continue;
    }
    std::istringstream ss(line);
    std::string tok;
    std::getline(ss, tok, ',');
    int64_t ts = std::stoll(tok);
    // Best-effort unit guess for a raw CSV column -- see the matching
    // comment on PyBacktestRunner::normalizeTs in strategy_bindings.h. Valid
    // only for genuinely unit-less input; never apply this to a value whose
    // unit the API already declares (e.g. an already-nanosecond timestamp).
    if (ts < static_cast<int64_t>(1e12))
    {
      ts *= 1'000'000'000LL;
    }
    else if (ts < static_cast<int64_t>(1e15))
    {
      ts *= 1'000'000LL;
    }
    else if (ts < static_cast<int64_t>(1e18))
    {
      ts *= 1'000LL;
    }
    std::getline(ss, tok, ',');  // open
    std::getline(ss, tok, ',');  // high
    std::getline(ss, tok, ',');  // low
    std::getline(ss, tok, ',');
    double c = std::stod(tok);
    lrvx::OhlcvReplaySource::Bar bar;
    bar.ts_ns = ts;
    bar.price_raw = lrvx::Price::fromDouble(c).raw();
    bar.symbol_id = symbolId;
    bars.push_back(bar);
  }
  return bars;
}

inline py::dict statsToDict(const lrvx::BacktestStats& s)
{
  py::dict d;
  d["total_trades"] = s.totalTrades;
  d["winning_trades"] = s.winningTrades;
  d["losing_trades"] = s.losingTrades;
  d["initial_capital"] = s.initialCapital;
  d["final_capital"] = s.finalCapital;
  d["total_pnl"] = s.totalPnl;
  d["total_fees"] = s.totalFees;
  d["net_pnl"] = s.netPnl;
  d["gross_profit"] = s.grossProfit;
  d["gross_loss"] = s.grossLoss;
  d["max_drawdown"] = s.maxDrawdown;
  d["max_drawdown_pct"] = s.maxDrawdownPct;
  d["win_rate"] = s.winRate;
  d["profit_factor"] = s.profitFactor;
  d["sharpe_ratio"] = s.sharpeRatio;
  d["sortino_ratio"] = s.sortinoRatio;
  d["calmar_ratio"] = s.calmarRatio;
  d["return_pct"] = s.returnPct;
  d["unrealized_pnl"] = s.unrealizedPnl;
  d["final_equity"] = s.finalEquity;
  return d;
}

class PyWalkForwardRunner
{
 public:
  PyWalkForwardRunner(lrvx::SymbolRegistry* reg,
                      double feeRate, double initialCapital,
                      const std::string& mode,
                      std::size_t trainSize, std::size_t testSize,
                      std::size_t step, std::size_t minTrainSize)
      : _reg(reg)
  {
    _bcfg.feeRate = feeRate;
    _bcfg.initialCapital = initialCapital;
    _bcfg.usePercentageFee = true;
    _wcfg.mode = parseChoice<lrvx::WalkForwardMode>(
        "walk-forward mode", mode,
        {{"anchored", lrvx::WalkForwardMode::Anchored},
         {"sliding", lrvx::WalkForwardMode::Sliding}});
    _wcfg.trainSize = trainSize;
    _wcfg.testSize = testSize;
    _wcfg.step = step;
    _wcfg.minTrainSize = minTrainSize;
  }

  void set_strategy_factory(py::object factory)
  {
    _factory = std::move(factory);
  }

  py::list run_csv(const std::string& path, const std::string& symbol)
  {
    if (!_factory || _factory.is_none())
    {
      throw lrvx::LrvxError(
          "E_RUN_001",
          "WalkForwardRunner.run_csv() called before set_strategy_factory().");
    }
    lrvx::SymbolId sid = resolveSymbolId(symbol);

    auto bars = loadOhlcvCsvForWf(path, sid);

    lrvx::WalkForwardRunner wfr(_bcfg, _wcfg);

    // Keep strategies and their hosts alive across the whole walk-forward
    // run. Each call holds a Python ref so on_trade etc. on the wrapped
    // object stay valid until run() returns. The hosts own the
    // BridgeStrategy that the engine uses.
    std::vector<py::object> aliveStrategies;
    std::vector<std::unique_ptr<PyStrategyHost>> aliveHosts;
    uint32_t hostId = 1;
    wfr.setStrategyFactory(
        [this, &aliveStrategies, &aliveHosts, &hostId](std::size_t foldIdx)
            -> lrvx::IStrategy*
        {
          py::gil_scoped_acquire gil;
          py::object pyStrat = _factory(foldIdx);
          auto* strat = pyStrat.cast<PyStrategyBase*>();
          auto host = std::make_unique<PyStrategyHost>(strat, _reg, hostId++, false);
          lrvx::IStrategy* iface = host->bridge.get();
          aliveStrategies.push_back(std::move(pyStrat));
          aliveHosts.push_back(std::move(host));
          return iface;
        });

    auto folds = wfr.run(bars);
    return foldsToList(folds);
  }

  py::list run_bars(
      py::array_t<int64_t, py::array::c_style | py::array::forcecast> start_ns,
      py::array_t<int64_t, py::array::c_style | py::array::forcecast> end_ns,
      py::array_t<double, py::array::c_style | py::array::forcecast> open,
      py::array_t<double, py::array::c_style | py::array::forcecast> high,
      py::array_t<double, py::array::c_style | py::array::forcecast> low,
      py::array_t<double, py::array::c_style | py::array::forcecast> close,
      py::array_t<double, py::array::c_style | py::array::forcecast> volume,
      const std::string& symbol,
      uint8_t bar_type = 0,
      uint64_t bar_type_param = 0)
  {
    if (!_factory || _factory.is_none())
    {
      throw lrvx::LrvxError(
          "E_RUN_001",
          "WalkForwardRunner.run_bars() called before set_strategy_factory().");
    }
    lrvx::SymbolId sid = resolveSymbolId(symbol);

    const auto n = start_ns.size();
    if (end_ns.size() != n || open.size() != n || high.size() != n ||
        low.size() != n || close.size() != n || volume.size() != n)
    {
      throw lrvx::LrvxError(
          "E_LEN_001",
          "run_bars: all arrays (start_time_ns, end_time_ns, open, high, "
          "low, close, volume) must have the same length.");
    }
    auto ps = start_ns.unchecked<1>();
    auto pe = end_ns.unchecked<1>();
    auto po = open.unchecked<1>();
    auto ph = high.unchecked<1>();
    auto pl = low.unchecked<1>();
    auto pc = close.unchecked<1>();
    auto pv = volume.unchecked<1>();

    std::vector<lrvx::BarEvent> events;
    events.reserve(static_cast<std::size_t>(n));
    for (py::ssize_t i = 0; i < n; ++i)
    {
      lrvx::BarEvent ev{};
      ev.symbol = sid;
      ev.barType = static_cast<lrvx::BarType>(bar_type);
      ev.barTypeParam = bar_type_param;
      ev.bar.open = lrvx::Price::fromDouble(po(i));
      ev.bar.high = lrvx::Price::fromDouble(ph(i));
      ev.bar.low = lrvx::Price::fromDouble(pl(i));
      ev.bar.close = lrvx::Price::fromDouble(pc(i));
      ev.bar.volume = lrvx::Volume::fromDouble(pv(i));
      ev.bar.startTime = lrvx::TimePoint{std::chrono::nanoseconds{ps(i)}};
      ev.bar.endTime = lrvx::TimePoint{std::chrono::nanoseconds{pe(i)}};
      ev.bar.reason = lrvx::BarCloseReason::Threshold;
      events.push_back(ev);
    }

    lrvx::WalkForwardRunner wfr(_bcfg, _wcfg);

    std::vector<py::object> aliveStrategies;
    std::vector<std::unique_ptr<PyStrategyHost>> aliveHosts;
    uint32_t hostId = 1;
    wfr.setStrategyFactory(
        [this, &aliveStrategies, &aliveHosts, &hostId](std::size_t foldIdx)
            -> lrvx::IStrategy*
        {
          py::gil_scoped_acquire gil;
          py::object pyStrat = _factory(foldIdx);
          auto* strat = pyStrat.cast<PyStrategyBase*>();
          auto host = std::make_unique<PyStrategyHost>(strat, _reg, hostId++, false);
          lrvx::IStrategy* iface = host->bridge.get();
          aliveStrategies.push_back(std::move(pyStrat));
          aliveHosts.push_back(std::move(host));
          return iface;
        });

    auto folds = wfr.run(events);
    return foldsToList(folds);
  }

 private:
  lrvx::SymbolId resolveSymbolId(const std::string& symbol)
  {
    auto opt = _reg->getSymbolId("", symbol);
    lrvx::SymbolId sid = 0;
    if (opt)
    {
      sid = *opt;
    }
    else
    {
      for (const auto& info : _reg->getAllSymbols())
      {
        if (info.symbol == symbol)
        {
          sid = info.id;
          break;
        }
      }
    }
    if (sid == 0)
    {
      throw lrvx::LrvxError(
          "E_SYM_001",
          "Symbol '" + symbol + "' is not registered.");
    }
    return sid;
  }

  py::list foldsToList(const std::vector<lrvx::WalkForwardFold>& folds) const
  {
    py::list out;
    for (const auto& f : folds)
    {
      py::dict d;
      d["fold_index"] = f.foldIndex;
      d["train_start_bar"] = f.trainStartBar;
      d["train_end_bar"] = f.trainEndBar;
      d["test_start_bar"] = f.testStartBar;
      d["test_end_bar"] = f.testEndBar;
      d["train_start_ns"] = f.trainStartNs;
      d["train_end_ns"] = f.trainEndNs;
      d["test_start_ns"] = f.testStartNs;
      d["test_end_ns"] = f.testEndNs;
      d["train_stats"] = statsToDict(f.trainStats);
      d["test_stats"] = statsToDict(f.testStats);
      out.append(d);
    }
    return out;
  }

 private:
  lrvx::SymbolRegistry* _reg;
  lrvx::BacktestConfig _bcfg;
  lrvx::WalkForwardConfig _wcfg;
  py::object _factory;
};

inline void bindWalkForward(py::module_& m)
{
  py::class_<PyWalkForwardRunner>(m, "WalkForwardRunner")
      .def(py::init([](lrvx::SymbolRegistry* reg, double feeRate,
                       double initialCapital, const std::string& mode,
                       std::size_t trainSize, std::size_t testSize,
                       std::size_t step, std::size_t minTrainSize)
                    { return std::make_unique<PyWalkForwardRunner>(
                          reg, feeRate, initialCapital, mode,
                          trainSize, testSize, step, minTrainSize); }),
           py::arg("registry"),
           py::arg("fee_rate") = 0.0004,
           py::arg("initial_capital") = 10000.0,
           py::arg("mode") = "anchored",
           py::arg("train_size") = 0,
           py::arg("test_size") = 0,
           py::arg("step") = 0,
           py::arg("min_train_size") = 0,
           py::keep_alive<1, 2>())
      .def("set_strategy_factory",
           &PyWalkForwardRunner::set_strategy_factory,
           py::arg("factory"),
           "Callable[[int], Strategy] — receives fold_index, returns "
           "a fresh Strategy instance per fold (called twice per fold: "
           "train + test).")
      .def("run_csv", &PyWalkForwardRunner::run_csv,
           py::arg("path"), py::arg("symbol"),
           "Walk-forward over an OHLCV CSV. Bars are dispatched to the "
           "strategy as synthetic trade events at close price — useful "
           "for on_trade strategies. For on_bar strategies that need "
           "open/high/low/volume, use run_bars().")
      .def("run_bars", &PyWalkForwardRunner::run_bars,
           py::arg("start_time_ns"),
           py::arg("end_time_ns"),
           py::arg("open"),
           py::arg("high"),
           py::arg("low"),
           py::arg("close"),
           py::arg("volume"),
           py::arg("symbol"),
           py::arg("bar_type") = static_cast<uint8_t>(0),
           py::arg("bar_type_param") = static_cast<uint64_t>(0),
           "Walk-forward over a sequence of full OHLCV bars. Each fold "
           "dispatches BarEvents through Strategy.on_bar with open / "
           "high / low / close / volume preserved. Bars must be sorted "
           "by end_time_ns. bar_type: 0=Time, 1=Tick, 2=Volume, 3=Renko, "
           "4=Range, 5=HeikinAshi.");
}

}  // namespace lrvx
