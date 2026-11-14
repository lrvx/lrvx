// lrvx -- Python bindings for lrvx.

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "account_bindings.h"
#include "aggregator_bindings.h"
#include "amm_curve_bindings.h"
#include "backtest_bindings.h"
#include "bar_dispatch_bindings.h"
#include "bare_executor_bindings.h"
#include "book_bindings.h"
#include "composite_book_bindings.h"
#include "delta_book_bindings.h"
#include "dex_amount_bindings.h"
#include "execution_algos_bindings.h"
#include "fee_schedule_bindings.h"
#include "feed_clock_bindings.h"
#include "funding_schedule_bindings.h"
#include "graph_bindings.h"
#include "grid_search_bindings.h"
#include "heatmap_bindings.h"
#include "indicator_bindings.h"
#include "latency_bindings.h"
#include "liquidation_engine_bindings.h"
#include "live_queue_position_bindings.h"
#include "lrvx/backtest/backtest_result.h"
#include "lrvx/backtest/simulated_clock.h"
#include "lrvx/backtest/simulated_executor.h"
#include "lrvx/capi/abi_check.hpp"
#include "lrvx/common.h"
#include "lrvx/error/lrvx_error.h"
#include "optimizer_bindings.h"
#include "order_group_bindings.h"
#include "order_journey_tracer_bindings.h"
#include "order_type_bindings.h"
#include "pool_tape_bindings.h"
#include "portfolio_risk_bindings.h"
#include "position_bindings.h"
#include "pricing_bindings.h"
#include "profile_bindings.h"
#include "rate_limit_bindings.h"
#include "replay_bindings.h"
#include "run_trace_bindings.h"
#include "segment_ops_bindings.h"
#include "strategy_bindings.h"
#include "tape_diff_bindings.h"
#include "target_bindings.h"
#include "venue_availability_bindings.h"
#include "venue_stack_bindings.h"
#include "walk_forward_bindings.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>
#include <vector>

namespace py = pybind11;
using namespace lrvx;

using contiguous_f64 = py::array_t<double, py::array::c_style | py::array::forcecast>;
using contiguous_i64 = py::array_t<int64_t, py::array::c_style | py::array::forcecast>;

// ── Bar ─────────────────────────────────────────────────────────────

struct OhlcvBar
{
  int64_t timestamp_ns;
  int64_t open_raw;
  int64_t high_raw;
  int64_t low_raw;
  int64_t close_raw;
  int64_t volume_raw;
};

// Best-effort unit guess for genuinely unit-less input: a raw CSV column,
// dict of arrays, or DataFrame column the caller may have populated with
// seconds, milliseconds, microseconds, or nanoseconds -- there is no way to
// know from here. See the matching comment on
// PyBacktestRunner::normalizeTs in strategy_bindings.h: this guess belongs
// only on data whose unit genuinely isn't known, never on a value the API
// contract already declares to be in nanoseconds. Do not retune these
// thresholds -- the unit ranges overlap on real timestamps, so any cutoff
// is a bet on which dates show up.
static int64_t normalizeTimestamp(int64_t t)
{
  if (t < static_cast<int64_t>(1e12))
  {
    return t * 1'000'000'000LL;
  }
  if (t < static_cast<int64_t>(1e15))
  {
    return t * 1'000'000LL;
  }
  if (t < static_cast<int64_t>(1e18))
  {
    return t * 1'000LL;
  }
  return t;
}

static std::vector<OhlcvBar> parseCsv(const std::string& path)
{
  std::ifstream file(path);
  if (!file.is_open())
  {
    throw lrvx::LrvxError(
        "E_IO_001",
        "Cannot open file: '" + path +
            "'. Check the path is correct and the file is readable.");
  }

  std::vector<OhlcvBar> bars;
  std::string line;
  std::getline(file, line);  // skip header

  while (std::getline(file, line))
  {
    if (line.empty())
    {
      continue;
    }
    std::istringstream ss(line);
    std::string tok;

    std::getline(ss, tok, ',');
    int64_t ts = normalizeTimestamp(std::stoll(tok));
    std::getline(ss, tok, ',');
    double o = std::stod(tok);
    std::getline(ss, tok, ',');
    double h = std::stod(tok);
    std::getline(ss, tok, ',');
    double l = std::stod(tok);
    std::getline(ss, tok, ',');
    double c = std::stod(tok);
    std::getline(ss, tok, ',');
    double v = std::stod(tok);

    bars.push_back({.timestamp_ns = ts,
                    .open_raw = Price::fromDouble(o).raw(),
                    .high_raw = Price::fromDouble(h).raw(),
                    .low_raw = Price::fromDouble(l).raw(),
                    .close_raw = Price::fromDouble(c).raw(),
                    .volume_raw = Volume::fromDouble(v).raw()});
  }
  return bars;
}

static std::vector<OhlcvBar> arraysToOhlcv(contiguous_i64 timestamps, contiguous_f64 open,
                                           contiguous_f64 high, contiguous_f64 low,
                                           contiguous_f64 close, contiguous_f64 volume)
{
  auto n = timestamps.size();
  std::vector<OhlcvBar> bars(n);
  auto ts = timestamps.unchecked<1>();
  auto o = open.unchecked<1>();
  auto h = high.unchecked<1>();
  auto l = low.unchecked<1>();
  auto c = close.unchecked<1>();
  auto v = volume.unchecked<1>();

  for (py::ssize_t i = 0; i < n; ++i)
  {
    bars[i] = {.timestamp_ns = normalizeTimestamp(ts(i)),
               .open_raw = Price::fromDouble(o(i)).raw(),
               .high_raw = Price::fromDouble(h(i)).raw(),
               .low_raw = Price::fromDouble(l(i)).raw(),
               .close_raw = Price::fromDouble(c(i)).raw(),
               .volume_raw = Volume::fromDouble(v(i)).raw()};
  }
  return bars;
}

// ── Resample ────────────────────────────────────────────────────────

static std::vector<OhlcvBar> resampleBars(const std::vector<OhlcvBar>& src,
                                          int64_t intervalNs)
{
  if (src.empty() || intervalNs <= 0)
  {
    return {};
  }

  std::vector<OhlcvBar> out;
  int64_t bucketStart = (src[0].timestamp_ns / intervalNs) * intervalNs;
  OhlcvBar cur = src[0];
  cur.timestamp_ns = bucketStart;

  for (size_t i = 1; i < src.size(); ++i)
  {
    int64_t bucket = (src[i].timestamp_ns / intervalNs) * intervalNs;
    if (bucket != bucketStart)
    {
      out.push_back(cur);
      bucketStart = bucket;
      cur = src[i];
      cur.timestamp_ns = bucket;
    }
    else
    {
      // merge
      if (src[i].high_raw > cur.high_raw)
      {
        cur.high_raw = src[i].high_raw;
      }
      if (src[i].low_raw < cur.low_raw)
      {
        cur.low_raw = src[i].low_raw;
      }
      cur.close_raw = src[i].close_raw;
      cur.volume_raw += src[i].volume_raw;
    }
  }
  out.push_back(cur);
  return out;
}

// ── Signal ──────────────────────────────────────────────────────────

struct InternalSignal
{
  int64_t timestamp_ns;
  int64_t quantity_raw;
  int64_t price_raw;
  uint8_t side;
  uint8_t order_type;
  uint32_t symbol_id;
};

// ── SignalBuilder ───────────────────────────────────────────────────

class SignalBuilder
{
 public:
  void buy(int64_t ts, double qty, const std::string& sym = "")
  {
    add(ts, 0, qty, 0, 0, sym);
  }

  void sell(int64_t ts, double qty, const std::string& sym = "")
  {
    add(ts, 1, qty, 0, 0, sym);
  }

  void limitBuy(int64_t ts, double price, double qty, const std::string& sym = "")
  {
    add(ts, 0, qty, price, 1, sym);
  }

  void limitSell(int64_t ts, double price, double qty, const std::string& sym = "")
  {
    add(ts, 1, qty, price, 1, sym);
  }

  size_t size() const { return _signals.size(); }
  void clear() { _signals.clear(); }

  const std::vector<InternalSignal>& signals() const { return _signals; }
  const std::vector<std::string>& symbolNames() const { return _symbolNames; }

 private:
  void add(int64_t ts, uint8_t side, double qty, double price, uint8_t type,
           const std::string& sym)
  {
    _signals.push_back({.timestamp_ns = normalizeTimestamp(ts),
                        .quantity_raw = Quantity::fromDouble(qty).raw(),
                        .price_raw = price > 0 ? Price::fromDouble(price).raw() : 0,
                        .side = side,
                        .order_type = type,
                        .symbol_id = 0});  // resolved at run time
    _symbolNames.push_back(sym);
  }

  std::vector<InternalSignal> _signals;
  std::vector<std::string> _symbolNames;
};

// ── Stats ───────────────────────────────────────────────────────────

struct PyStats
{
  size_t total_trades, winning_trades, losing_trades;
  double initial_capital, final_capital, total_pnl, total_fees, net_pnl;
  double gross_profit, gross_loss, max_drawdown, max_drawdown_pct;
  double win_rate, profit_factor, avg_win, avg_loss;
  double sharpe_ratio, sortino_ratio, calmar_ratio, return_pct;
  // The position still open at the end, marked at the last close the run
  // replayed; net_pnl, final_capital and return_pct leave it out.
  double unrealized_pnl, final_equity;

  static PyStats fromBacktest(const BacktestStats& s)
  {
    return {s.totalTrades, s.winningTrades, s.losingTrades, s.initialCapital,
            s.finalCapital, s.totalPnl, s.totalFees, s.netPnl,
            s.grossProfit, s.grossLoss, s.maxDrawdown, s.maxDrawdownPct,
            s.winRate, s.profitFactor, s.avgWin, s.avgLoss,
            s.sharpeRatio, s.sortinoRatio, s.calmarRatio, s.returnPct,
            s.unrealizedPnl, s.finalEquity};
  }

  py::dict toDict() const
  {
    py::dict d;
    d["total_trades"] = total_trades;
    d["winning_trades"] = winning_trades;
    d["losing_trades"] = losing_trades;
    d["initial_capital"] = initial_capital;
    d["final_capital"] = final_capital;
    d["total_pnl"] = total_pnl;
    d["total_fees"] = total_fees;
    d["net_pnl"] = net_pnl;
    d["gross_profit"] = gross_profit;
    d["gross_loss"] = gross_loss;
    d["max_drawdown"] = max_drawdown;
    d["max_drawdown_pct"] = max_drawdown_pct;
    d["win_rate"] = win_rate;
    d["profit_factor"] = profit_factor;
    d["avg_win"] = avg_win;
    d["avg_loss"] = avg_loss;
    d["sharpe_ratio"] = sharpe_ratio;
    d["sortino_ratio"] = sortino_ratio;
    d["calmar_ratio"] = calmar_ratio;
    d["return_pct"] = return_pct;
    d["unrealized_pnl"] = unrealized_pnl;
    d["final_equity"] = final_equity;
    return d;
  }

  std::string repr() const
  {
    std::ostringstream os;
    os << "Stats(trades=" << total_trades << " pnl=" << net_pnl << " ret=" << return_pct
       << "% sharpe_ratio=" << sharpe_ratio << " dd=" << max_drawdown_pct << "%)";
    return os.str();
  }
};

// ── SymbolData ──────────────────────────────────────────────────────

struct SymbolData
{
  uint32_t id;
  std::vector<OhlcvBar> bars;
};

// ── Merged event for multi-symbol backtest ──────────────────────────

struct MergedBar
{
  int64_t timestamp_ns;
  int64_t close_raw;
  uint32_t symbol_id;
};

// ── Engine ──────────────────────────────────────────────────────────

class Engine
{
 public:
  Engine(double initialCapital = 100000.0, double feeRate = 0.0001)
  {
    _config.initialCapital = initialCapital;
    _config.feeRate = feeRate;
    _config.usePercentageFee = true;
  }

  // -- Loading --

  void loadCsv(const std::string& path, const std::string& symbol = "")
  {
    std::string sym = symbol.empty() ? inferSymbol(path) : symbol;
    auto& sd = getOrCreate(sym);
    sd.bars = parseCsv(path);
  }

  void loadOhlcv(py::dict d, const std::string& symbol = "")
  {
    auto get = [&](const char* key) -> contiguous_f64
    {
      if (!d.contains(key))
      {
        throw lrvx::LrvxError(
            "E_KEY_001",
            std::string("Required key '") + key +
                "' is missing from the OHLCV dict. Expected keys: "
                "'ts' (or 'timestamp'), 'open', 'high', 'low', 'close', 'volume'.");
      }
      return d[key].cast<contiguous_f64>();
    };

    contiguous_i64 timestamps;
    for (const char* key : {"ts", "timestamp"})
    {
      if (d.contains(key))
      {
        timestamps = d[key].cast<contiguous_i64>();
        break;
      }
    }
    if (timestamps.size() == 0)
    {
      throw lrvx::LrvxError(
          "E_KEY_001",
          "OHLCV dict is missing both 'ts' and 'timestamp' keys. "
          "Provide one of them as an int64 array of nanosecond timestamps.");
    }

    std::string sym = symbol.empty() ? "default" : symbol;
    auto& sd = getOrCreate(sym);
    sd.bars = arraysToOhlcv(timestamps, get("open"), get("high"), get("low"), get("close"),
                            get("volume"));
  }

  void loadDf(py::object df, const std::string& symbol = "")
  {
    auto cols = df.attr("columns").attr("tolist")().cast<py::list>();
    auto colSet = py::set(cols);

    contiguous_i64 timestamps;
    for (const char* key : {"ts", "timestamp", "open_time", "time"})
    {
      if (colSet.contains(key))
      {
        timestamps = df.attr("__getitem__")(key).attr("values").cast<contiguous_i64>();
        break;
      }
    }
    if (timestamps.size() == 0)
    {
      timestamps = df.attr("index").attr("values").cast<contiguous_i64>();
    }

    auto col = [&](const char* key) -> contiguous_f64
    { return df.attr("__getitem__")(key).attr("values").cast<contiguous_f64>(); };

    std::string sym = symbol.empty() ? "default" : symbol;
    auto& sd = getOrCreate(sym);
    sd.bars = arraysToOhlcv(timestamps, col("open"), col("high"), col("low"), col("close"),
                            col("volume"));
  }

  // -- Accessors --

  py::list symbolList() const
  {
    py::list out;
    for (auto& [name, _] : _symbols)
    {
      out.append(name);
    }
    return out;
  }

  size_t barCount(const std::string& symbol = "") const
  {
    return resolve(symbol).bars.size();
  }

  // int64, not float64: a nanosecond timestamp today is ~1.78e18, far past
  // the 2^53 where a double still holds every integer, so a float64 array
  // rounded every value it handed back (256 ns spacing at that magnitude).
  // The OHLCV columns stay float64 -- those are converted out of fixed
  // point for the caller.
  py::array_t<int64_t> timestamps(const std::string& symbol = "") const
  {
    auto& bars = resolve(symbol).bars;
    py::array_t<int64_t> out(bars.size());
    auto* p = out.mutable_data();
    for (size_t i = 0; i < bars.size(); ++i)
    {
      p[i] = bars[i].timestamp_ns;
    }
    return out;
  }

  using OhlcvField = int64_t OhlcvBar::*;
  py::array_t<double> field(const std::string& symbol, OhlcvField f) const
  {
    auto& bars = resolve(symbol).bars;
    py::array_t<double> out(bars.size());
    auto* p = out.mutable_data();
    for (size_t i = 0; i < bars.size(); ++i)
    {
      p[i] = Price::fromRaw(bars[i].*f).toDouble();
    }
    return out;
  }

  py::array_t<double> opens(const std::string& s = "") const { return field(s, &OhlcvBar::open_raw); }
  py::array_t<double> highs(const std::string& s = "") const { return field(s, &OhlcvBar::high_raw); }
  py::array_t<double> lows(const std::string& s = "") const { return field(s, &OhlcvBar::low_raw); }
  py::array_t<double> closes(const std::string& s = "") const { return field(s, &OhlcvBar::close_raw); }
  py::array_t<double> volumes(const std::string& s = "") const { return field(s, &OhlcvBar::volume_raw); }

  // -- Resample --

  void addResample(const std::string& symbol, const std::string& targetName,
                   const std::string& interval)
  {
    int64_t intervalNs = parseInterval(interval);
    auto& src = resolve(symbol);
    auto& dst = getOrCreate(targetName);
    dst.bars = resampleBars(src.bars, intervalNs);
  }

  // -- Run --

  PyStats run(const SignalBuilder& builder, uint32_t defaultSymbol = 0)
  {
    // Resolve signal symbols
    auto sigs = builder.signals();
    const auto& names = builder.symbolNames();
    uint32_t defSym = defaultSymbol > 0 ? defaultSymbol : firstSymbolId();

    for (size_t i = 0; i < sigs.size(); ++i)
    {
      if (names[i].empty())
      {
        sigs[i].symbol_id = defSym;
      }
      else
      {
        auto it = _symbols.find(names[i]);
        if (it == _symbols.end())
        {
          throw lrvx::LrvxError("E_SYM_001",
                                "Symbol '" + names[i] +
                                    "' is not registered. Add it via "
                                    "Engine.add_symbol() before referencing it.");
        }
        sigs[i].symbol_id = it->second.id;
      }
    }

    // Sort signals by time
    std::vector<size_t> sigOrder(sigs.size());
    std::iota(sigOrder.begin(), sigOrder.end(), 0);
    std::sort(sigOrder.begin(), sigOrder.end(),
              [&](size_t a, size_t b)
              { return sigs[a].timestamp_ns < sigs[b].timestamp_ns; });

    // Merge all bars across symbols by timestamp
    std::vector<MergedBar> merged;
    for (auto& [_, sd] : _symbols)
    {
      for (auto& bar : sd.bars)
      {
        merged.push_back({bar.timestamp_ns, bar.close_raw, sd.id});
      }
    }
    std::sort(merged.begin(), merged.end(),
              [](const MergedBar& a, const MergedBar& b)
              { return a.timestamp_ns < b.timestamp_ns; });

    // Execute
    SimulatedClock clock;
    SimulatedExecutor executor(clock);
    OrderId nextOrderId = 1;
    size_t sigIdx = 0;
    std::map<uint32_t, int64_t> lastClose;

    for (const auto& mb : merged)
    {
      clock.advanceTo(UnixNanos::fromRaw(mb.timestamp_ns));
      executor.onBar(mb.symbol_id, Price::fromRaw(mb.close_raw));
      lastClose[mb.symbol_id] = mb.close_raw;

      while (sigIdx < sigs.size() &&
             sigs[sigOrder[sigIdx]].timestamp_ns <= mb.timestamp_ns)
      {
        const auto& sig = sigs[sigOrder[sigIdx]];
        Order order{.id = nextOrderId++,
                    .side = sig.side == 0 ? Side::BUY : Side::SELL,
                    .price = Price::fromRaw(sig.price_raw),
                    .quantity = Quantity::fromRaw(sig.quantity_raw),
                    .type = sig.order_type == 0 ? OrderType::MARKET : OrderType::LIMIT,
                    .symbol = sig.symbol_id};
        executor.submitOrder(order);
        ++sigIdx;
      }
    }

    BacktestResult result(_config, executor.fills().size());
    for (const auto& fill : executor.fills())
    {
      result.recordFill(fill);
    }
    // An open position is valued at the last close the run replayed.
    for (const auto& [sym, close] : lastClose)
    {
      result.markPrice(sym, Price::fromRaw(close));
    }
    return PyStats::fromBacktest(result.computeStats());
  }

 private:
  SymbolData& getOrCreate(const std::string& name)
  {
    auto it = _symbols.find(name);
    if (it != _symbols.end())
    {
      return it->second;
    }
    uint32_t id = static_cast<uint32_t>(_symbols.size()) + 1;
    auto [jt, _] = _symbols.emplace(name, SymbolData{id, {}});
    if (_insertOrder.empty() || _insertOrder.back() != name)
    {
      _insertOrder.push_back(name);
    }
    return jt->second;
  }

  const SymbolData& resolve(const std::string& symbol) const
  {
    if (symbol.empty())
    {
      if (_insertOrder.empty())
      {
        throw lrvx::LrvxError(
            "E_RUN_002",
            "No market data has been loaded into the engine. Call "
            "Engine.load_csv(...) or load_ohlcv(...) before run().");
      }
      return _symbols.at(_insertOrder.front());
    }
    auto it = _symbols.find(symbol);
    if (it == _symbols.end())
    {
      throw lrvx::LrvxError("E_SYM_001",
                            "Symbol '" + symbol +
                                "' is not registered. Add it via "
                                "Engine.add_symbol() before referencing it.");
    }
    return it->second;
  }

  uint32_t firstSymbolId() const
  {
    if (_insertOrder.empty())
    {
      return 1;
    }
    return _symbols.at(_insertOrder.front()).id;
  }

  static std::string inferSymbol(const std::string& path)
  {
    auto pos = path.find_last_of('/');
    std::string filename = (pos != std::string::npos) ? path.substr(pos + 1) : path;
    auto dot = filename.find('.');
    if (dot != std::string::npos)
    {
      filename = filename.substr(0, dot);
    }
    // Remove common suffixes like _1m, _5m, _1h
    for (const char* suf : {"_1m", "_5m", "_15m", "_1h", "_4h", "_1d"})
    {
      size_t sl = std::strlen(suf);
      if (filename.size() > sl && filename.substr(filename.size() - sl) == suf)
      {
        filename = filename.substr(0, filename.size() - sl);
        break;
      }
    }
    // uppercase
    for (auto& c : filename)
    {
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return filename;
  }

  static int64_t parseInterval(const std::string& s)
  {
    if (s.empty())
    {
      return 0;
    }
    size_t numEnd = 0;
    int64_t val = std::stoll(s, &numEnd);
    std::string unit = s.substr(numEnd);
    if (unit == "s")
    {
      return val * 1'000'000'000LL;
    }
    if (unit == "m")
    {
      return val * 60'000'000'000LL;
    }
    if (unit == "h")
    {
      return val * 3'600'000'000'000LL;
    }
    if (unit == "d")
    {
      return val * 86'400'000'000'000LL;
    }
    throw lrvx::LrvxError(
        "E_TIME_001",
        "Unknown interval unit '" + unit +
            "'. Expected one of: 's', 'm', 'h', 'd' "
            "(seconds, minutes, hours, days).");
  }

  BacktestConfig _config;
  std::map<std::string, SymbolData> _symbols;
  std::vector<std::string> _insertOrder;
};

// ── Module ──────────────────────────────────────────────────────────

#pragma pack(push, 1)
struct PyBar
{
  int64_t timestamp_ns;
  int64_t open_raw;
  int64_t high_raw;
  int64_t low_raw;
  int64_t close_raw;
  int64_t volume_raw;
};
#pragma pack(pop)

PYBIND11_MODULE(_lrvx, m)
{
  m.doc() = "lrvx -- Python bindings";

  // The C ABI handshake lrvx_capi.h asks every caller to run once at
  // startup. The extension compiles src/capi/lrvx_capi.cpp into itself
  // today, so the two halves match by construction; an install that ever
  // links a separately-built liblrvx would not, and the structs on that
  // boundary have no reserved tail, so the skew would read as wrong
  // numbers rather than a failed load. Refusing the import is the only
  // point at which that is still cheap to notice.
  {
    std::string abiMessage;
    if (!lrvx::capi::checkAbiVersion(LRVX_CAPI_ABI_VERSION, &abiMessage))
    {
      throw py::import_error(abiMessage);
    }
  }
  m.attr("CAPI_ABI_VERSION") = static_cast<uint32_t>(LRVX_CAPI_ABI_VERSION);
  m.def("capi_abi_version", []()
        { return lrvx_capi_abi_version(); },
        "C ABI version the loaded lrvx library reports. Equal to\n"
        "CAPI_ABI_VERSION, which the extension was compiled against --\n"
        "the import fails outright when they disagree.");
  m.def("version", []()
        { return std::string(lrvx_version()); },
        "The release the compiled library was built from (lrvx_version()),\n"
        "the same string as lrvx.__version__.");
  m.def(
      "last_error", []()
      { return std::string(lrvx_last_error_message()); },
      "Why the last call that reports failure through its return value\n"
      "failed: FundingSchedule.load_tape returning False, recompress\n"
      "returning False, extract_symbols / extract_time_range returning 0.\n"
      "Each clears it on entry, so '' after one means it did not fail (an\n"
      "extract that matched nothing returns 0 with ''). The text opens with\n"
      "the name of the C function. Strategy.modify_order clears it too, and\n"
      "a live Runner that refuses the modify (an id its executor record does\n"
      "not hold) leaves the reason here, ending in [unknown_order]. Per\n"
      "thread.");

  lrvx::bindOrderTypes(m);

  PYBIND11_NUMPY_DTYPE(PyBar, timestamp_ns, open_raw, high_raw, low_raw, close_raw, volume_raw);

  // LrvxError — structured exception. New throw sites in C++ raise
  // lrvx::LrvxError; this translator surfaces them in Python with the
  // `code`, `message`, `help_url` attributes intact. Pre-existing
  // std::invalid_argument / std::runtime_error throws keep their default
  // pybind11 → ValueError / RuntimeError mapping (incremental migration).
  static py::exception<lrvx::LrvxError> lrvxError(m, "LrvxError");
  py::register_exception_translator(
      [](std::exception_ptr p)
      {
        try
        {
          if (p)
          {
            std::rethrow_exception(p);
          }
        }
        catch (const lrvx::LrvxError& e)
        {
          PyObject* excTypePtr = lrvxError.ptr();
          py::object excType = py::reinterpret_borrow<py::object>(excTypePtr);
          py::object inst = excType(e.message());
          inst.attr("code") = e.code();
          inst.attr("message") = e.message();
          inst.attr("help_url") = e.helpUrl();
          PyErr_SetObject(excTypePtr, inst.ptr());
        }
      });

  // Stats
  py::class_<PyStats>(m, "Stats")
      .def_readonly("total_trades", &PyStats::total_trades)
      .def_readonly("winning_trades", &PyStats::winning_trades)
      .def_readonly("losing_trades", &PyStats::losing_trades)
      .def_readonly("initial_capital", &PyStats::initial_capital)
      .def_readonly("final_capital", &PyStats::final_capital)
      .def_readonly("total_pnl", &PyStats::total_pnl)
      .def_readonly("total_fees", &PyStats::total_fees)
      .def_readonly("net_pnl", &PyStats::net_pnl)
      .def_readonly("gross_profit", &PyStats::gross_profit)
      .def_readonly("gross_loss", &PyStats::gross_loss)
      .def_readonly("max_drawdown", &PyStats::max_drawdown)
      .def_readonly("max_drawdown_pct", &PyStats::max_drawdown_pct)
      .def_readonly("win_rate", &PyStats::win_rate)
      .def_readonly("profit_factor", &PyStats::profit_factor)
      .def_readonly("avg_win", &PyStats::avg_win)
      .def_readonly("avg_loss", &PyStats::avg_loss)
      .def_readonly("sharpe_ratio", &PyStats::sharpe_ratio)
      .def_readonly("sortino_ratio", &PyStats::sortino_ratio)
      .def_readonly("calmar_ratio", &PyStats::calmar_ratio)
      .def_readonly("return_pct", &PyStats::return_pct)
      .def_readonly("unrealized_pnl", &PyStats::unrealized_pnl)
      .def_readonly("final_equity", &PyStats::final_equity)
      .def("to_dict", &PyStats::toDict)
      .def("__repr__", &PyStats::repr)
      .def("__getitem__",
           [](const PyStats& s, const std::string& key) -> py::object
           { return s.toDict()[py::str(key)]; });

  // SignalBuilder
  py::class_<SignalBuilder>(m, "SignalBuilder")
      .def(py::init<>())
      .def("buy", &SignalBuilder::buy, py::arg("ts"), py::arg("qty"),
           py::arg("symbol") = "")
      .def("sell", &SignalBuilder::sell, py::arg("ts"), py::arg("qty"),
           py::arg("symbol") = "")
      .def("limit_buy", &SignalBuilder::limitBuy, py::arg("ts"), py::arg("price"),
           py::arg("qty"), py::arg("symbol") = "")
      .def("limit_sell", &SignalBuilder::limitSell, py::arg("ts"), py::arg("price"),
           py::arg("qty"), py::arg("symbol") = "")
      .def("__len__", &SignalBuilder::size)
      .def("clear", &SignalBuilder::clear);

  // Engine
  py::class_<Engine>(m, "Engine")
      .def(py::init<double, double>(), py::arg("initial_capital") = 100000.0,
           py::arg("fee_rate") = 0.0001)
      .def("load_csv", &Engine::loadCsv, py::arg("path"), py::arg("symbol") = "")
      .def("load_ohlcv", &Engine::loadOhlcv, py::arg("data"), py::arg("symbol") = "")
      .def("load_df", &Engine::loadDf, py::arg("df"), py::arg("symbol") = "")
      .def("resample", &Engine::addResample, py::arg("symbol"), py::arg("target"),
           py::arg("interval"))
      .def("run", &Engine::run, py::arg("signals"), py::arg("default_symbol") = 0)
      .def_property_readonly("symbols", &Engine::symbolList)
      .def("bar_count", &Engine::barCount, py::arg("symbol") = "")
      .def("ts", &Engine::timestamps, py::arg("symbol") = "")
      .def("open", &Engine::opens, py::arg("symbol") = "")
      .def("high", &Engine::highs, py::arg("symbol") = "")
      .def("low", &Engine::lows, py::arg("symbol") = "")
      .def("close", &Engine::closes, py::arg("symbol") = "")
      .def("volume", &Engine::volumes, py::arg("symbol") = "");

  bindIndicators(m);
  bindPricing(m);
  bindAggregators(m);
  bindBooks(m);
  bindProfiles(m);
  bindPositions(m);
  bindReplay(m);
  lrvx::bindTapeAggregators(m);
  bindSegmentOps(m);
  bindBacktest(m);
  lrvx::bindWalkForward(m);
  lrvx::bindGridSearch(m);
  lrvx::bindHeatmap(m);
  lrvx::bindLatencyModels(m);
  lrvx::bindTapeDiff(m);
  lrvx::bindPortfolioRisk(m);
  lrvx::bindOrderGroup(m);
  lrvx::bindRateLimitPolicy(m);
  lrvx::bindVenueAvailability(m);
  lrvx::bindFundingSchedule(m);
  lrvx::bindAccount(m);
  lrvx::bindFeeSchedule(m);
  lrvx::bindLiquidationEngine(m);
  lrvx::bindBareExecutor(m);
  lrvx::bindVenueStack(m);
  lrvx::bindLiveQueuePositionEstimator(m);
  lrvx::bindBarDispatchRecorder(m);
  lrvx::bindFeedClock(m);
  lrvx::bindExecutionAlgos(m);
  lrvx::bindDeltaBook(m);
  lrvx::bindDexAmount(m);
  lrvx::bindAmmCurve(m);
  lrvx::bindPoolTape(m);
  lrvx::bindRunTrace(m);
  bindOptimizer(m);
  bindCompositeBook(m);
  bindStrategy(m);
  bindTargets(m);
  bindIndicatorGraph(m);
  registerOrderJourneyTracer(m);

  // Slippage model constants
  m.attr("SLIPPAGE_NONE") = 0;
  m.attr("SLIPPAGE_FIXED_TICKS") = 1;
  m.attr("SLIPPAGE_FIXED_BPS") = 2;
  m.attr("SLIPPAGE_VOLUME_IMPACT") = 3;

  // Queue model constants
  m.attr("QUEUE_NONE") = 0;
  m.attr("QUEUE_TOB") = 1;
  m.attr("QUEUE_FULL") = 2;
}
