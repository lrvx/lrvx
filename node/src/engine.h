// node/src/engine.h -- Engine, SignalBuilder, CSV, resample

#pragma once
#include <napi.h>

#include "lrvx/backtest/backtest_result.h"
#include "lrvx/backtest/simulated_clock.h"
#include "lrvx/backtest/simulated_executor.h"
#include "lrvx/common.h"
#include "lrvx/error/lrvx_error.h"

#include "bindings_common.h"
#include "error_translator.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <numeric>
#include <sstream>
#include <vector>

namespace node_lrvx
{

using namespace lrvx;

// ── Types ───────────────────────────────────────────────────────────

struct OhlcvBar
{
  int64_t timestamp_ns, open_raw, high_raw, low_raw, close_raw, volume_raw;
};

struct InternalSignal
{
  int64_t timestamp_ns, quantity_raw, price_raw;
  uint8_t side, order_type;
  uint32_t symbol_id;
};

struct SymbolData
{
  uint32_t id;
  std::vector<OhlcvBar> bars;
};

// ── Utilities ───────────────────────────────────────────────────────

// Best-effort unit guess for genuinely unit-less input: a raw CSV column or
// a bare number array the caller may have populated with seconds,
// milliseconds, microseconds, or nanoseconds -- there is no way to know
// from here. Valid only for that case. Never apply this to a value whose
// unit the API already declares -- BacktestRunnerNode.runBars/runOhlcv
// take already-nanosecond BigInt64Array timestamps and pass them straight
// through without going near this function, which is deliberate: guessing
// a unit for data that already has one silently corrupts legitimate small
// values (see python/strategy_bindings.h's normalizeTs for the bug this
// caused when a copy of this same heuristic was applied to that contract).
// Do not retune the thresholds either -- the unit ranges overlap on real
// timestamps, so any cutoff is a bet on which dates show up.
inline int64_t normalizeTs(int64_t t)
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

inline std::vector<OhlcvBar> parseCsv(const std::string& path)
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
  std::getline(file, line);

  while (std::getline(file, line))
  {
    if (line.empty())
    {
      continue;
    }
    std::istringstream ss(line);
    std::string tok;
    std::getline(ss, tok, ',');
    int64_t ts = normalizeTs(std::stoll(tok));
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
    bars.push_back({ts, Price::fromDouble(o).raw(), Price::fromDouble(h).raw(),
                    Price::fromDouble(l).raw(), Price::fromDouble(c).raw(),
                    Volume::fromDouble(v).raw()});
  }
  return bars;
}

inline std::vector<OhlcvBar> resampleBars(const std::vector<OhlcvBar>& src, int64_t intervalNs)
{
  if (src.empty() || intervalNs <= 0)
  {
    return {};
  }
  std::vector<OhlcvBar> out;
  int64_t bucket = (src[0].timestamp_ns / intervalNs) * intervalNs;
  OhlcvBar cur = src[0];
  cur.timestamp_ns = bucket;
  for (size_t i = 1; i < src.size(); ++i)
  {
    int64_t b = (src[i].timestamp_ns / intervalNs) * intervalNs;
    if (b != bucket)
    {
      out.push_back(cur);
      bucket = b;
      cur = src[i];
      cur.timestamp_ns = b;
    }
    else
    {
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

inline std::string inferSymbol(const std::string& path)
{
  auto pos = path.find_last_of('/');
  std::string f = (pos != std::string::npos) ? path.substr(pos + 1) : path;
  auto dot = f.find('.');
  if (dot != std::string::npos)
  {
    f = f.substr(0, dot);
  }
  for (const char* suf : {"_1m", "_5m", "_15m", "_1h", "_4h", "_1d"})
  {
    size_t sl = std::strlen(suf);
    if (f.size() > sl && f.substr(f.size() - sl) == suf)
    {
      f = f.substr(0, f.size() - sl);
      break;
    }
  }
  for (auto& c : f)
  {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return f;
}

inline int64_t parseInterval(const std::string& s)
{
  size_t numEnd = 0;
  int64_t val = std::stoll(s, &numEnd);
  std::string u = s.substr(numEnd);
  if (u == "s")
  {
    return val * 1'000'000'000LL;
  }
  if (u == "m")
  {
    return val * 60'000'000'000LL;
  }
  if (u == "h")
  {
    return val * 3'600'000'000'000LL;
  }
  if (u == "d")
  {
    return val * 86'400'000'000'000LL;
  }
  throw std::invalid_argument("unknown interval: " + u);
}

inline Napi::Object statsObj(Napi::Env env, const BacktestStats& s)
{
  auto o = Napi::Object::New(env);
  o.Set("totalTrades", (double)s.totalTrades);
  o.Set("winningTrades", (double)s.winningTrades);
  o.Set("losingTrades", (double)s.losingTrades);
  o.Set("initialCapital", s.initialCapital);
  o.Set("finalCapital", s.finalCapital);
  o.Set("netPnl", s.netPnl);
  o.Set("totalPnl", s.totalPnl);
  o.Set("totalFees", s.totalFees);
  o.Set("grossProfit", s.grossProfit);
  o.Set("grossLoss", s.grossLoss);
  o.Set("maxDrawdown", s.maxDrawdown);
  o.Set("maxDrawdownPct", s.maxDrawdownPct);
  o.Set("winRate", s.winRate);
  o.Set("profitFactor", s.profitFactor);
  o.Set("avgWin", s.avgWin);
  o.Set("avgLoss", s.avgLoss);
  o.Set("sharpeRatio", s.sharpeRatio);
  o.Set("sortinoRatio", s.sortinoRatio);
  o.Set("calmarRatio", s.calmarRatio);
  o.Set("returnPct", s.returnPct);
  setTradeTimeStats(env, o, s.avgTradeDurationNs, s.medianTradeDurationNs, s.maxTradeDurationNs,
                    s.timeWeightedReturn, s.startTimeNs.raw(), s.endTimeNs.raw());
  return o;
}

// ── SignalBuilder ───────────────────────────────────────────────────

class SignalBuilderWrap : public Napi::ObjectWrap<SignalBuilderWrap>
{
 public:
  static Napi::FunctionReference& Ctor()
  {
    static Napi::FunctionReference ref;
    return ref;
  }

  static Napi::Function Init(Napi::Env env)
  {
    return keepCtor(Ctor(), DefineClass(env, "SignalBuilder",
                                        {InstanceMethod("buy", &SignalBuilderWrap::Buy),
                                         InstanceMethod("sell", &SignalBuilderWrap::Sell),
                                         InstanceMethod("limitBuy", &SignalBuilderWrap::LimitBuy),
                                         InstanceMethod("limitSell", &SignalBuilderWrap::LimitSell),
                                         InstanceAccessor("length", &SignalBuilderWrap::Length, nullptr),
                                         InstanceMethod("clear", &SignalBuilderWrap::Clear)}));
  }

  SignalBuilderWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<SignalBuilderWrap>(info) {}

  const std::vector<InternalSignal>& signals() const { return _signals; }
  const std::vector<std::string>& symbolNames() const { return _names; }

 private:
  void add(int64_t ts, uint8_t side, double qty, double price, uint8_t type,
           const std::string& sym)
  {
    _signals.push_back({normalizeTs(ts), Quantity::fromDouble(qty).raw(),
                        price > 0 ? Price::fromDouble(price).raw() : 0LL, side, type, 0});
    _names.push_back(sym);
  }

  std::string getSym(const Napi::CallbackInfo& info, int idx, const char* what)
  {
    return given(info[idx]) ? argString(info[idx], what) : "";
  }

  Napi::Value Buy(const Napi::CallbackInfo& info)
  {
    add(toInt64Ns(info[0], "buy: ts"), 0, argNumber(info[1], "buy: qty"), 0, 0, getSym(info, 2, "buy: symbol"));
    return info.This();
  }
  Napi::Value Sell(const Napi::CallbackInfo& info)
  {
    add(toInt64Ns(info[0], "sell: ts"), 1, argNumber(info[1], "sell: qty"), 0, 0, getSym(info, 2, "sell: symbol"));
    return info.This();
  }
  Napi::Value LimitBuy(const Napi::CallbackInfo& info)
  {
    add(toInt64Ns(info[0], "limitBuy: ts"), 0, argNumber(info[2], "limitBuy: qty"), argNumber(info[1], "limitBuy: price"), 1, getSym(info, 3, "limitBuy: symbol"));
    return info.This();
  }
  Napi::Value LimitSell(const Napi::CallbackInfo& info)
  {
    add(toInt64Ns(info[0], "limitSell: ts"), 1, argNumber(info[2], "limitSell: qty"), argNumber(info[1], "limitSell: price"), 1, getSym(info, 3, "limitSell: symbol"));
    return info.This();
  }
  Napi::Value Length(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _signals.size()); }
  void Clear(const Napi::CallbackInfo&)
  {
    _signals.clear();
    _names.clear();
  }

  std::vector<InternalSignal> _signals;
  std::vector<std::string> _names;
};

// ── Engine ──────────────────────────────────────────────────────────

class EngineWrap : public Napi::ObjectWrap<EngineWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "Engine",
                       {InstanceMethod("loadCsv", &EngineWrap::LoadCsv),
                        InstanceMethod("loadOhlcv", &EngineWrap::LoadOhlcv),
                        InstanceMethod("resample", &EngineWrap::Resample),
                        InstanceMethod("run", &EngineWrap::Run),
                        InstanceMethod("barCount", &EngineWrap::BarCount),
                        InstanceMethod("ts", &EngineWrap::Timestamps),
                        InstanceMethod("open", &EngineWrap::Opens),
                        InstanceMethod("high", &EngineWrap::Highs),
                        InstanceMethod("low", &EngineWrap::Lows),
                        InstanceMethod("close", &EngineWrap::Closes),
                        InstanceMethod("volume", &EngineWrap::Volumes),
                        InstanceAccessor("symbols", &EngineWrap::Symbols, nullptr)});
  }

  EngineWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<EngineWrap>(info)
  {
    _config.initialCapital = info.Length() > 0 ? argNumber(info[0], "Engine: initialCapital") : 100000.0;
    _config.feeRate = info.Length() > 1 ? argNumber(info[1], "Engine: feeRate") : 0.0001;
    _config.usePercentageFee = true;
  }

 private:
  SymbolData& getOrCreate(const std::string& name)
  {
    auto it = _syms.find(name);
    if (it != _syms.end())
    {
      return it->second;
    }
    uint32_t id = static_cast<uint32_t>(_syms.size()) + 1;
    _order.push_back(name);
    return _syms.emplace(name, SymbolData{id, {}}).first->second;
  }

  const SymbolData& resolve(const Napi::CallbackInfo& info, int idx = 0) const
  {
    const std::string sym = given(info[idx]) ? argString(info[idx], "symbol") : "";
    if (sym.empty())
    {
      if (_order.empty())
      {
        throw std::runtime_error("no data loaded");
      }
      return _syms.at(_order.front());
    }
    auto it = _syms.find(sym);
    if (it == _syms.end())
    {
      throw std::invalid_argument("unknown symbol: " + sym);
    }
    return it->second;
  }

  using OhlcvField = int64_t OhlcvBar::*;
  Napi::Value extractField(const Napi::CallbackInfo& info, OhlcvField f)
  {
    auto& bars = resolve(info).bars;
    auto buf = Napi::Float64Array::New(info.Env(), bars.size());
    for (size_t i = 0; i < bars.size(); ++i)
    {
      buf[i] = Price::fromRaw(bars[i].*f).toDouble();
    }
    return buf;
  }

  Napi::Value LoadCsv(const Napi::CallbackInfo& info)
  {
    return tryLrvx(info.Env(), [&]() -> Napi::Value
                   {
      std::string path = argString(info[0], "loadCsv: path");
      std::string sym = given(info[1]) ? argString(info[1], "loadCsv: symbol") : inferSymbol(path);
      getOrCreate(sym).bars = parseCsv(path);
      return info.Env().Undefined(); });
  }

  Napi::Value LoadOhlcv(const Napi::CallbackInfo& info)
  {
    return tryLrvx(info.Env(), [&]() -> Napi::Value
                   {
      auto obj = argObject(info[0], "loadOhlcv: bars");
      std::string sym = given(info[1]) ? argString(info[1], "loadOhlcv: symbol") : "default";
      auto ts = argFloat64Array(obj.Get("ts"), "loadOhlcv: ts");
      auto op = argFloat64Array(obj.Get("open"), "loadOhlcv: open");
      auto hi = argFloat64Array(obj.Get("high"), "loadOhlcv: high");
      auto lo = argFloat64Array(obj.Get("low"), "loadOhlcv: low");
      auto cl = argFloat64Array(obj.Get("close"), "loadOhlcv: close");
      auto vo = argFloat64Array(obj.Get("volume"), "loadOhlcv: volume");
      size_t n = ts.ElementLength();
      auto& sd = getOrCreate(sym);
      sd.bars.resize(n);
      for (size_t i = 0; i < n; ++i)
      {
        sd.bars[i] = {normalizeTs(static_cast<int64_t>(ts[i])),
                      Price::fromDouble(op[i]).raw(), Price::fromDouble(hi[i]).raw(),
                      Price::fromDouble(lo[i]).raw(), Price::fromDouble(cl[i]).raw(),
                      Volume::fromDouble(vo[i]).raw()};
      }
      return info.Env().Undefined(); });
  }

  Napi::Value Resample(const Napi::CallbackInfo& info)
  {
    return tryLrvx(info.Env(), [&]() -> Napi::Value
                   {
      std::string src = argString(info[0], "resample: src");
      std::string dst = argString(info[1], "resample: dst");
      std::string interval = argString(info[2], "resample: interval");
      auto it = _syms.find(src);
      if (it == _syms.end())
      {
        throw lrvx::LrvxError(
            "E_SYM_001",
            "Symbol '" + src + "' is not loaded. Call loadCsv / loadOhlcv first.");
      }
      getOrCreate(dst).bars = resampleBars(it->second.bars, parseInterval(interval));
      return info.Env().Undefined(); });
  }

  Napi::Value Run(const Napi::CallbackInfo& info)
  {
    auto* builder = argInstance<SignalBuilderWrap>(info[0], "run: signals");
    auto sigs = builder->signals();
    const auto& names = builder->symbolNames();

    uint32_t defSym = _order.empty() ? 1 : _syms.at(_order.front()).id;
    for (size_t i = 0; i < sigs.size(); ++i)
    {
      if (names[i].empty())
      {
        sigs[i].symbol_id = defSym;
      }
      else
      {
        auto it = _syms.find(names[i]);
        if (it == _syms.end())
        {
          throw std::invalid_argument("unknown symbol: " + names[i]);
        }
        sigs[i].symbol_id = it->second.id;
      }
    }

    std::vector<size_t> sigOrder(sigs.size());
    std::iota(sigOrder.begin(), sigOrder.end(), 0);
    std::sort(sigOrder.begin(), sigOrder.end(),
              [&](size_t a, size_t b)
              { return sigs[a].timestamp_ns < sigs[b].timestamp_ns; });

    struct MB
    {
      int64_t ts;
      int64_t close;
      uint32_t sym;
    };
    std::vector<MB> merged;
    for (auto& [_, sd] : _syms)
    {
      for (auto& b : sd.bars)
      {
        merged.push_back({b.timestamp_ns, b.close_raw, sd.id});
      }
    }
    std::sort(merged.begin(), merged.end(), [](const MB& a, const MB& b)
              { return a.ts < b.ts; });

    SimulatedClock clock;
    SimulatedExecutor executor(clock);
    OrderId nextId = 1;
    size_t si = 0;

    for (const auto& mb : merged)
    {
      clock.advanceTo(UnixNanos::fromRaw(mb.ts));
      executor.onBar(mb.sym, Price::fromRaw(mb.close));
      while (si < sigs.size() && sigs[sigOrder[si]].timestamp_ns <= mb.ts)
      {
        const auto& sg = sigs[sigOrder[si]];
        Order order{.id = nextId++, .side = sg.side == 0 ? Side::BUY : Side::SELL, .price = Price::fromRaw(sg.price_raw), .quantity = Quantity::fromRaw(sg.quantity_raw), .type = sg.order_type == 0 ? OrderType::MARKET : OrderType::LIMIT, .symbol = sg.symbol_id};
        executor.submitOrder(order);
        ++si;
      }
    }

    BacktestResult result(_config, executor.fills().size());
    for (const auto& fill : executor.fills())
    {
      result.recordFill(fill);
    }
    return statsObj(info.Env(), result.computeStats());
  }

  Napi::Value BarCount(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), resolve(info).bars.size()); }
  // The bars hold int64 nanoseconds and parseCsv reads them from text, so a
  // present-day reading arrives here exact; a Float64Array gave it back
  // quantised to 256 ns. The sibling accessors stay Float64Array -- they
  // carry prices and volumes, which are doubles either way.
  Napi::Value Timestamps(const Napi::CallbackInfo& info)
  {
    auto& bars = resolve(info).bars;
    auto buf = Napi::BigInt64Array::New(info.Env(), bars.size());
    for (size_t i = 0; i < bars.size(); ++i)
    {
      buf[i] = bars[i].timestamp_ns;
    }
    return buf;
  }
  Napi::Value Opens(const Napi::CallbackInfo& info) { return extractField(info, &OhlcvBar::open_raw); }
  Napi::Value Highs(const Napi::CallbackInfo& info) { return extractField(info, &OhlcvBar::high_raw); }
  Napi::Value Lows(const Napi::CallbackInfo& info) { return extractField(info, &OhlcvBar::low_raw); }
  Napi::Value Closes(const Napi::CallbackInfo& info) { return extractField(info, &OhlcvBar::close_raw); }
  Napi::Value Volumes(const Napi::CallbackInfo& info) { return extractField(info, &OhlcvBar::volume_raw); }
  Napi::Value Symbols(const Napi::CallbackInfo& info)
  {
    auto arr = Napi::Array::New(info.Env(), _order.size());
    for (size_t i = 0; i < _order.size(); ++i)
    {
      arr.Set(i, _order[i]);
    }
    return arr;
  }

  BacktestConfig _config;
  std::map<std::string, SymbolData> _syms;
  std::vector<std::string> _order;
};

inline void registerEngine(Napi::Env env, Napi::Object exports)
{
  exports.Set("Engine", EngineWrap::Init(env));
  exports.Set("SignalBuilder", SignalBuilderWrap::Init(env));
}

}  // namespace node_lrvx
