// node/src/indicators.h -- batch + streaming indicator bindings

#pragma once
#include <napi.h>

#include "bindings_common.h"
#include "lrvx/indicator/adx.h"
#include "lrvx/indicator/atr.h"
#include "lrvx/indicator/bollinger.h"
#include "lrvx/indicator/cci.h"
#include "lrvx/indicator/chop.h"
#include "lrvx/indicator/cvd.h"
#include "lrvx/indicator/dema.h"
#include "lrvx/indicator/ema.h"
#include "lrvx/indicator/kama.h"
#include "lrvx/indicator/macd.h"
#include "lrvx/indicator/obv.h"
#include "lrvx/indicator/params.h"
#include "lrvx/indicator/rma.h"
#include "lrvx/indicator/rsi.h"
#include "lrvx/indicator/slope.h"
#include "lrvx/indicator/sma.h"
#include "lrvx/indicator/stochastic.h"
#include "lrvx/indicator/vwap.h"

#include "lrvx/indicator/adf.h"
#include "lrvx/indicator/autocorrelation.h"
#include "lrvx/indicator/correlation.h"
#include "lrvx/indicator/kurtosis.h"
#include "lrvx/indicator/parkinson_vol.h"
#include "lrvx/indicator/rogers_satchell_vol.h"
#include "lrvx/indicator/rolling_zscore.h"
#include "lrvx/indicator/shannon_entropy.h"
#include "lrvx/indicator/skewness.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>

namespace node_lrvx
{

inline Napi::Float64Array vec2arr(Napi::Env env, const std::vector<double>& v)
{
  auto a = Napi::Float64Array::New(env, v.size());
  std::memcpy(a.Data(), v.data(), v.size() * sizeof(double));
  return a;
}

inline std::span<const double> arr2span(Napi::Float64Array& a)
{
  return {a.Data(), a.ElementLength()};
}

// A registry indicator built from a call's arguments. The C++ classes accept
// any period and answer NaN for a window they cannot fill; the binding
// refuses the arguments instead, with the floors of lrvx/indicator/params.h,
// as a JS Error carrying code E_IND_001 (the shape tryLrvx gives an
// LrvxError), so new SMA(0) and sma(data, 0) both throw.
template <typename T, typename... A>
T checkedIndicator(Napi::Env env, const char* row, A... args)
{
  try
  {
    lrvx::indicator::requireArgs(row, args...);
  }
  catch (const lrvx::LrvxError& e)
  {
    auto err = Napi::Error::New(env, e.message());
    err.Value().Set("code", Napi::String::New(env, e.code()));
    err.Value().Set("helpUrl", Napi::String::New(env, e.helpUrl()));
    err.Value().Set("name", Napi::String::New(env, "LrvxError"));
    throw err;
  }
  return T(args...);
}

// Every multi-array batch indicator below used to take its element count
// from the FIRST array only and blindly index the rest at that length.
// A caller passing mismatched arrays (e.g. `close` shorter than `high`)
// read past the short array's end -- silently on small inputs (denormals
// out of whatever memory followed the allocation), or with a SIGSEGV once
// the read crossed a page boundary. Every call site below must check
// lengths before touching `.Data()` on anything but the first array.
// requireSameLength itself now lives in bindings_common.h, included
// above: aggregators.h and stats.h need the identical check for the
// same reason and are included before this file in lrvx_node.cpp.

// ── Batch: single-input indicators ──────────────────────────────────

template <typename Indicator>
Napi::Value batchSingle(const Napi::CallbackInfo& info, const char* fn, Indicator ind)
{
  auto in = argFloat64Array(info[0], (std::string(fn) + ": input").c_str());
  size_t n = in.ElementLength();
  std::vector<double> out(n);
  ind.compute(std::span<const double>(in.Data(), n), std::span<double>(out.data(), n));
  return vec2arr(info.Env(), out);
}

#define BATCH_SINGLE(name, Ind)                                                                    \
  inline Napi::Value batch_##name(const Napi::CallbackInfo& info)                                  \
  {                                                                                                \
    size_t p = argUint32(info[1], #name ": period");                                               \
    return batchSingle(info, #name, checkedIndicator<lrvx::indicator::Ind>(info.Env(), #name, p)); \
  }

BATCH_SINGLE(sma, SMA)
BATCH_SINGLE(ema, EMA)
BATCH_SINGLE(rsi, RSI)
BATCH_SINGLE(rma, RMA)
BATCH_SINGLE(slope, Slope)

#undef BATCH_SINGLE

inline Napi::Value batch_kama(const Napi::CallbackInfo& info)
{
  size_t p = argUint32(info[1], "kama: period");
  return batchSingle(info, "kama", checkedIndicator<lrvx::indicator::KAMA>(info.Env(), "kama", p, size_t{2}, size_t{30}));
}

inline Napi::Value batch_dema(const Napi::CallbackInfo& info)
{
  auto in = argFloat64Array(info[0], "dema: input");
  size_t p = argUint32(info[1], "dema: period");
  auto r = checkedIndicator<lrvx::indicator::DEMA>(info.Env(), "dema", p).compute(arr2span(in));
  return vec2arr(info.Env(), r);
}

inline Napi::Value batch_tema(const Napi::CallbackInfo& info)
{
  auto in = argFloat64Array(info[0], "tema: input");
  size_t p = argUint32(info[1], "tema: period");
  auto r = checkedIndicator<lrvx::indicator::TEMA>(info.Env(), "tema", p).compute(arr2span(in));
  return vec2arr(info.Env(), r);
}

// ── Batch: HLC indicators ───────────────────────────────────────────

inline Napi::Value batch_atr(const Napi::CallbackInfo& info)
{
  auto h = argFloat64Array(info[0], "atr: high");
  auto l = argFloat64Array(info[1], "atr: low");
  auto c = argFloat64Array(info[2], "atr: close");
  size_t n = h.ElementLength(), p = argUint32(info[3], "atr: period");
  if (!requireSameLength(info.Env(), "atr", {n, l.ElementLength(), c.ElementLength()}))
  {
    return info.Env().Undefined();
  }
  std::vector<double> out(n);
  checkedIndicator<lrvx::indicator::ATR>(info.Env(), "atr", p).compute({h.Data(), n}, {l.Data(), n}, {c.Data(), n}, {out.data(), n});
  return vec2arr(info.Env(), out);
}

inline Napi::Value batch_adx(const Napi::CallbackInfo& info)
{
  auto h = argFloat64Array(info[0], "adx: high");
  auto l = argFloat64Array(info[1], "adx: low");
  auto c = argFloat64Array(info[2], "adx: close");
  size_t n = h.ElementLength(), p = argUint32(info[3], "adx: period");
  if (!requireSameLength(info.Env(), "adx", {n, l.ElementLength(), c.ElementLength()}))
  {
    return info.Env().Undefined();
  }
  auto r = lrvx::indicator::ADX(p).compute({h.Data(), n}, {l.Data(), n}, {c.Data(), n});
  auto o = Napi::Object::New(info.Env());
  o.Set("adx", vec2arr(info.Env(), r.adx));
  o.Set("plusDi", vec2arr(info.Env(), r.plus_di));
  o.Set("minusDi", vec2arr(info.Env(), r.minus_di));
  return o;
}

inline Napi::Value batch_stochastic(const Napi::CallbackInfo& info)
{
  auto h = argFloat64Array(info[0], "stochastic: high");
  auto l = argFloat64Array(info[1], "stochastic: low");
  auto c = argFloat64Array(info[2], "stochastic: close");
  size_t n = h.ElementLength();
  size_t kp = argUint32(info[3], "stochastic: kPeriod");
  size_t dp = argUint32(info[4], "stochastic: dPeriod");
  if (!requireSameLength(info.Env(), "stochastic", {n, l.ElementLength(), c.ElementLength()}))
  {
    return info.Env().Undefined();
  }
  auto r = checkedIndicator<lrvx::indicator::Stochastic>(info.Env(), "stochastic", kp, dp).compute({h.Data(), n}, {l.Data(), n}, {c.Data(), n});
  auto o = Napi::Object::New(info.Env());
  o.Set("k", vec2arr(info.Env(), r.k));
  o.Set("d", vec2arr(info.Env(), r.d));
  return o;
}

inline Napi::Value batch_cci(const Napi::CallbackInfo& info)
{
  auto h = argFloat64Array(info[0], "cci: high");
  auto l = argFloat64Array(info[1], "cci: low");
  auto c = argFloat64Array(info[2], "cci: close");
  size_t n = h.ElementLength(), p = argUint32(info[3], "cci: period");
  if (!requireSameLength(info.Env(), "cci", {n, l.ElementLength(), c.ElementLength()}))
  {
    return info.Env().Undefined();
  }
  auto r = checkedIndicator<lrvx::indicator::CCI>(info.Env(), "cci", p).compute({h.Data(), n}, {l.Data(), n}, {c.Data(), n});
  return vec2arr(info.Env(), r);
}

inline Napi::Value batch_chop(const Napi::CallbackInfo& info)
{
  auto h = argFloat64Array(info[0], "chop: high");
  auto l = argFloat64Array(info[1], "chop: low");
  auto c = argFloat64Array(info[2], "chop: close");
  size_t n = h.ElementLength(), p = argUint32(info[3], "chop: period");
  if (!requireSameLength(info.Env(), "chop", {n, l.ElementLength(), c.ElementLength()}))
  {
    return info.Env().Undefined();
  }
  auto r = checkedIndicator<lrvx::indicator::CHOP>(info.Env(), "chop", p).compute({h.Data(), n}, {l.Data(), n}, {c.Data(), n});
  return vec2arr(info.Env(), r);
}

// ── Batch: multi-output / volume ────────────────────────────────────

inline Napi::Value batch_macd(const Napi::CallbackInfo& info)
{
  auto in = argFloat64Array(info[0], "macd: input");
  size_t n = in.ElementLength();
  size_t f = argUint32(info[1], "macd: fast");
  size_t s = argUint32(info[2], "macd: slow");
  size_t sg = argUint32(info[3], "macd: signal");
  std::vector<double> line(n), sig(n), hist(n);
  checkedIndicator<lrvx::indicator::MACD>(info.Env(), "macd", f, s, sg).compute({in.Data(), n}, {line.data(), n}, {sig.data(), n}, {hist.data(), n});
  auto o = Napi::Object::New(info.Env());
  o.Set("line", vec2arr(info.Env(), line));
  o.Set("signal", vec2arr(info.Env(), sig));
  o.Set("histogram", vec2arr(info.Env(), hist));
  return o;
}

inline Napi::Value batch_bollinger(const Napi::CallbackInfo& info)
{
  auto in = argFloat64Array(info[0], "bollinger: input");
  size_t n = in.ElementLength(), p = argUint32(info[1], "bollinger: period");
  double sd = argNumber(info[2], "bollinger: stdDev");
  auto r = checkedIndicator<lrvx::indicator::Bollinger>(info.Env(), "bollinger", p, sd).compute({in.Data(), n});
  auto o = Napi::Object::New(info.Env());
  o.Set("upper", vec2arr(info.Env(), r.upper));
  o.Set("middle", vec2arr(info.Env(), r.middle));
  o.Set("lower", vec2arr(info.Env(), r.lower));
  return o;
}

inline Napi::Value batch_obv(const Napi::CallbackInfo& info)
{
  auto c = argFloat64Array(info[0], "obv: close");
  auto v = argFloat64Array(info[1], "obv: volume");
  size_t n = c.ElementLength();
  if (!requireSameLength(info.Env(), "obv", {n, v.ElementLength()}))
  {
    return info.Env().Undefined();
  }
  std::vector<double> out(n);
  lrvx::indicator::OBV().compute({c.Data(), n}, {v.Data(), n}, {out.data(), n});
  return vec2arr(info.Env(), out);
}

inline Napi::Value batch_vwap(const Napi::CallbackInfo& info)
{
  auto c = argFloat64Array(info[0], "vwap: close");
  auto v = argFloat64Array(info[1], "vwap: volume");
  size_t n = c.ElementLength(), w = argUint32(info[2], "vwap: window");
  if (!requireSameLength(info.Env(), "vwap", {n, v.ElementLength()}))
  {
    return info.Env().Undefined();
  }
  auto r = lrvx::indicator::VWAP(w).compute({c.Data(), n}, {v.Data(), n});
  return vec2arr(info.Env(), r);
}

inline Napi::Value batch_cvd(const Napi::CallbackInfo& info)
{
  auto o = argFloat64Array(info[0], "cvd: open");
  auto h = argFloat64Array(info[1], "cvd: high");
  auto l = argFloat64Array(info[2], "cvd: low");
  auto c = argFloat64Array(info[3], "cvd: close");
  auto v = argFloat64Array(info[4], "cvd: volume");
  size_t n = c.ElementLength();
  if (!requireSameLength(info.Env(), "cvd",
                         {n, o.ElementLength(), h.ElementLength(), l.ElementLength(), v.ElementLength()}))
  {
    return info.Env().Undefined();
  }
  auto r = lrvx::indicator::CVD().compute({o.Data(), n}, {h.Data(), n}, {l.Data(), n},
                                          {c.Data(), n}, {v.Data(), n});
  return vec2arr(info.Env(), r);
}

// ── Batch: statistical / volatility ─────────────────────────────────

inline Napi::Value batch_skewness(const Napi::CallbackInfo& info)
{
  auto in = argFloat64Array(info[0], "skewness: input");
  size_t p = argUint32(info[1], "skewness: period");
  auto r = checkedIndicator<lrvx::indicator::Skewness>(info.Env(), "skewness", p).compute(arr2span(in));
  return vec2arr(info.Env(), r);
}

inline Napi::Value batch_kurtosis(const Napi::CallbackInfo& info)
{
  auto in = argFloat64Array(info[0], "kurtosis: input");
  size_t p = argUint32(info[1], "kurtosis: period");
  auto r = checkedIndicator<lrvx::indicator::Kurtosis>(info.Env(), "kurtosis", p).compute(arr2span(in));
  return vec2arr(info.Env(), r);
}

inline Napi::Value batch_rolling_zscore(const Napi::CallbackInfo& info)
{
  auto in = argFloat64Array(info[0], "rolling_zscore: input");
  size_t p = argUint32(info[1], "rolling_zscore: period");
  auto r = checkedIndicator<lrvx::indicator::RollingZScore>(info.Env(), "rolling_zscore", p).compute(arr2span(in));
  return vec2arr(info.Env(), r);
}

inline Napi::Value batch_shannon_entropy(const Napi::CallbackInfo& info)
{
  auto in = argFloat64Array(info[0], "shannon_entropy: input");
  size_t p = argUint32(info[1], "shannon_entropy: period");
  size_t bins = argUint32(info[2], "shannon_entropy: bins");
  auto r = checkedIndicator<lrvx::indicator::ShannonEntropy>(info.Env(), "shannon_entropy", p, bins).compute(arr2span(in));
  return vec2arr(info.Env(), r);
}

inline Napi::Value batch_parkinson_vol(const Napi::CallbackInfo& info)
{
  auto h = argFloat64Array(info[0], "parkinson_vol: high");
  auto l = argFloat64Array(info[1], "parkinson_vol: low");
  size_t n = h.ElementLength(), p = argUint32(info[2], "parkinson_vol: period");
  if (!requireSameLength(info.Env(), "parkinson_vol", {n, l.ElementLength()}))
  {
    return info.Env().Undefined();
  }
  std::vector<double> out(n);
  checkedIndicator<lrvx::indicator::ParkinsonVol>(info.Env(), "parkinson_vol", p).compute({h.Data(), n}, {l.Data(), n}, {out.data(), n});
  return vec2arr(info.Env(), out);
}

inline Napi::Value batch_rogers_satchell_vol(const Napi::CallbackInfo& info)
{
  auto o = argFloat64Array(info[0], "rogers_satchell_vol: open");
  auto h = argFloat64Array(info[1], "rogers_satchell_vol: high");
  auto l = argFloat64Array(info[2], "rogers_satchell_vol: low");
  auto c = argFloat64Array(info[3], "rogers_satchell_vol: close");
  size_t n = o.ElementLength(), p = argUint32(info[4], "rogers_satchell_vol: period");
  if (!requireSameLength(info.Env(), "rogers_satchell_vol",
                         {n, h.ElementLength(), l.ElementLength(), c.ElementLength()}))
  {
    return info.Env().Undefined();
  }
  std::vector<double> out(n);
  checkedIndicator<lrvx::indicator::RogersSatchellVol>(info.Env(), "rogers_satchell_vol", p).compute({o.Data(), n}, {h.Data(), n}, {l.Data(), n}, {c.Data(), n}, {out.data(), n});
  return vec2arr(info.Env(), out);
}

inline Napi::Value batch_correlation(const Napi::CallbackInfo& info)
{
  auto x = argFloat64Array(info[0], "rollingCorrelation: x");
  auto y = argFloat64Array(info[1], "rollingCorrelation: y");
  size_t n = x.ElementLength(), p = argUint32(info[2], "rollingCorrelation: period");
  if (!requireSameLength(info.Env(), "correlation", {n, y.ElementLength()}))
  {
    return info.Env().Undefined();
  }
  std::vector<double> out(n);
  checkedIndicator<lrvx::indicator::Correlation>(info.Env(), "correlation", p).compute({x.Data(), n}, {y.Data(), n}, {out.data(), n});
  return vec2arr(info.Env(), out);
}

inline Napi::Value batch_adf(const Napi::CallbackInfo& info)
{
  // index.d.ts declared this `(input, lag) => number`; the real signature
  // takes an optional regression term (default "c", matching Python and
  // Codon) and returns { testStat, pValue, usedLag }.
  auto in = argFloat64Array(info[0], "adf: input");
  size_t n = in.ElementLength();
  size_t maxLag = info.Length() > 1 ? argUint32(info[1], "adf: maxLag") : 4u;
  std::string regression =
      given(info[2]) ? argString(info[2], "adf: regression") : "c";
  lrvx::indicator::AdfResult r =
      lrvx::indicator::adf(std::span<const double>(in.Data(), n), maxLag, regression);
  auto o = Napi::Object::New(info.Env());
  o.Set("testStat", Napi::Number::New(info.Env(), r.test_stat));
  o.Set("pValue", Napi::Number::New(info.Env(), r.p_value));
  o.Set("usedLag", Napi::Number::New(info.Env(), static_cast<double>(r.used_lag)));
  return o;
}

inline Napi::Value batch_autocorrelation(const Napi::CallbackInfo& info)
{
  auto in = argFloat64Array(info[0], "autocorrelation: input");
  size_t n = in.ElementLength();
  size_t window = argUint32(info[1], "autocorrelation: window");
  size_t lag = argUint32(info[2], "autocorrelation: lag");
  std::vector<double> out(n);
  checkedIndicator<lrvx::indicator::AutoCorrelation>(info.Env(), "autocorrelation", window, lag)
      .compute(std::span<const double>(in.Data(), n), std::span<double>(out.data(), n));
  return vec2arr(info.Env(), out);
}

// ── Streaming indicator classes ─────────────────────────────────────
//
// Each class is a thin Napi::ObjectWrap delegating into the unified C++
// indicator class (which has compute() + update()/value()/ready()/reset()
// on the same object via CRTP streaming mixins). No ring buffer, no alpha,
// no count maintained here — the C++ class owns all state.

namespace
{

inline Napi::Value optNum(Napi::Env env, double v)
{
  return std::isnan(v) ? Napi::Value(env.Null()) : Napi::Value(Napi::Number::New(env, v));
}

}  // namespace

// Macro for indicators that take ONE size_t in their constructor.
#define WRAP_SINGLE_1(Name, row, period)                                                           \
  class Name##Wrap : public Napi::ObjectWrap<Name##Wrap>                                           \
  {                                                                                                \
   public:                                                                                         \
    static Napi::Function Init(Napi::Env env)                                                      \
    {                                                                                              \
      return DefineClass(                                                                          \
          env, #Name,                                                                              \
          {InstanceMethod("compute", &Name##Wrap::Compute),                                        \
           InstanceMethod("update", &Name##Wrap::Update),                                          \
           InstanceMethod("reset", &Name##Wrap::Reset),                                            \
           InstanceAccessor("value", &Name##Wrap::Value, nullptr),                                 \
           InstanceAccessor("ready", &Name##Wrap::Ready, nullptr),                                 \
           InstanceAccessor("count", &Name##Wrap::Count, nullptr)});                               \
    }                                                                                              \
    Name##Wrap(const Napi::CallbackInfo& info)                                                     \
        : Napi::ObjectWrap<Name##Wrap>(info),                                                      \
          _ind(checkedIndicator<lrvx::indicator::Name>(info.Env(), #row,                           \
                                                       argUint32(info[0], #Name ": " #period)))    \
    {                                                                                              \
    }                                                                                              \
                                                                                                   \
   private:                                                                                        \
    lrvx::indicator::Name _ind;                                                                    \
    Napi::Value Compute(const Napi::CallbackInfo& info)                                            \
    {                                                                                              \
      auto in = argFloat64Array(info[0], "compute: input");                                        \
      auto r = _ind.compute(arr2span(in));                                                         \
      return vec2arr(info.Env(), r);                                                               \
    }                                                                                              \
    Napi::Value Update(const Napi::CallbackInfo& info)                                             \
    {                                                                                              \
      _ind.update(argNumber(info[0], "update: value"));                                            \
      return optNum(info.Env(), _ind.value());                                                     \
    }                                                                                              \
    Napi::Value Value(const Napi::CallbackInfo& info) { return optNum(info.Env(), _ind.value()); } \
    Napi::Value Ready(const Napi::CallbackInfo& info)                                              \
    {                                                                                              \
      return Napi::Boolean::New(info.Env(), _ind.ready());                                         \
    }                                                                                              \
    Napi::Value Count(const Napi::CallbackInfo& info)                                              \
    {                                                                                              \
      return Napi::Number::New(info.Env(), static_cast<double>(_ind.count()));                     \
    }                                                                                              \
    void Reset(const Napi::CallbackInfo&) { _ind.reset(); }                                        \
  };

WRAP_SINGLE_1(SMA, sma, period)
WRAP_SINGLE_1(EMA, ema, period)
WRAP_SINGLE_1(RMA, rma, period)
WRAP_SINGLE_1(RSI, rsi, period)
WRAP_SINGLE_1(DEMA, dema, period)
WRAP_SINGLE_1(TEMA, tema, period)
WRAP_SINGLE_1(Slope, slope, length)
WRAP_SINGLE_1(Skewness, skewness, period)
WRAP_SINGLE_1(Kurtosis, kurtosis, period)
WRAP_SINGLE_1(RollingZScore, rolling_zscore, period)

#undef WRAP_SINGLE_1

// KAMA(period, fast=2, slow=30)
class KAMAWrap : public Napi::ObjectWrap<KAMAWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "KAMA",
                       {InstanceMethod("compute", &KAMAWrap::Compute),
                        InstanceMethod("update", &KAMAWrap::Update),
                        InstanceMethod("reset", &KAMAWrap::Reset),
                        InstanceAccessor("value", &KAMAWrap::Value, nullptr),
                        InstanceAccessor("ready", &KAMAWrap::Ready, nullptr),
                        InstanceAccessor("count", &KAMAWrap::Count, nullptr)});
  }
  KAMAWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<KAMAWrap>(info),
        _ind(checkedIndicator<lrvx::indicator::KAMA>(info.Env(), "kama", argUint32(info[0], "KAMA: period"),
                                                     info.Length() > 1 ? argUint32(info[1], "KAMA: fast") : 2u,
                                                     info.Length() > 2 ? argUint32(info[2], "KAMA: slow") : 30u))
  {
  }

 private:
  lrvx::indicator::KAMA _ind;
  Napi::Value Compute(const Napi::CallbackInfo& info)
  {
    auto in = argFloat64Array(info[0], "compute: input");
    auto r = _ind.compute(arr2span(in));
    return vec2arr(info.Env(), r);
  }
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    _ind.update(argNumber(info[0], "update: value"));
    return optNum(info.Env(), _ind.value());
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return optNum(info.Env(), _ind.value()); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _ind.ready()); }
  Napi::Value Count(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), static_cast<double>(_ind.count()));
  }
  void Reset(const Napi::CallbackInfo&) { _ind.reset(); }
};

// ShannonEntropy(period, bins)
class ShannonEntropyWrap : public Napi::ObjectWrap<ShannonEntropyWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "ShannonEntropy",
                       {InstanceMethod("compute", &ShannonEntropyWrap::Compute),
                        InstanceMethod("update", &ShannonEntropyWrap::Update),
                        InstanceMethod("reset", &ShannonEntropyWrap::Reset),
                        InstanceAccessor("value", &ShannonEntropyWrap::Value, nullptr),
                        InstanceAccessor("ready", &ShannonEntropyWrap::Ready, nullptr),
                        InstanceAccessor("count", &ShannonEntropyWrap::Count, nullptr)});
  }
  ShannonEntropyWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<ShannonEntropyWrap>(info),
        _ind(checkedIndicator<lrvx::indicator::ShannonEntropy>(info.Env(), "shannon_entropy", argUint32(info[0], "ShannonEntropy: period"),
                                                               info.Length() > 1 ? argUint32(info[1], "ShannonEntropy: bins") : 10u))
  {
  }

 private:
  lrvx::indicator::ShannonEntropy _ind;
  Napi::Value Compute(const Napi::CallbackInfo& info)
  {
    auto in = argFloat64Array(info[0], "compute: input");
    auto r = _ind.compute(arr2span(in));
    return vec2arr(info.Env(), r);
  }
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    _ind.update(argNumber(info[0], "update: value"));
    return optNum(info.Env(), _ind.value());
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return optNum(info.Env(), _ind.value()); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _ind.ready()); }
  Napi::Value Count(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), static_cast<double>(_ind.count()));
  }
  void Reset(const Napi::CallbackInfo&) { _ind.reset(); }
};

// AutoCorrelation(window, lag)
class AutoCorrelationWrap : public Napi::ObjectWrap<AutoCorrelationWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "AutoCorrelation",
                       {InstanceMethod("compute", &AutoCorrelationWrap::Compute),
                        InstanceMethod("update", &AutoCorrelationWrap::Update),
                        InstanceMethod("reset", &AutoCorrelationWrap::Reset),
                        InstanceAccessor("value", &AutoCorrelationWrap::Value, nullptr),
                        InstanceAccessor("ready", &AutoCorrelationWrap::Ready, nullptr),
                        InstanceAccessor("count", &AutoCorrelationWrap::Count, nullptr)});
  }
  AutoCorrelationWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<AutoCorrelationWrap>(info),
        _ind(checkedIndicator<lrvx::indicator::AutoCorrelation>(info.Env(), "autocorrelation", argUint32(info[0], "AutoCorrelation: window"),
                                                                argUint32(info[1], "AutoCorrelation: lag")))
  {
  }

 private:
  lrvx::indicator::AutoCorrelation _ind;
  Napi::Value Compute(const Napi::CallbackInfo& info)
  {
    auto in = argFloat64Array(info[0], "compute: input");
    auto r = _ind.compute(arr2span(in));
    return vec2arr(info.Env(), r);
  }
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    _ind.update(argNumber(info[0], "update: value"));
    return optNum(info.Env(), _ind.value());
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return optNum(info.Env(), _ind.value()); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _ind.ready()); }
  Napi::Value Count(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), static_cast<double>(_ind.count()));
  }
  void Reset(const Napi::CallbackInfo&) { _ind.reset(); }
};

// Macro for bar-input indicators (high, low, close), 1-arg ctor.
#define WRAP_BAR_1(Name, row)                                                                      \
  class Name##Wrap : public Napi::ObjectWrap<Name##Wrap>                                           \
  {                                                                                                \
   public:                                                                                         \
    static Napi::Function Init(Napi::Env env)                                                      \
    {                                                                                              \
      return DefineClass(env, #Name,                                                               \
                         {InstanceMethod("compute", &Name##Wrap::Compute),                         \
                          InstanceMethod("update", &Name##Wrap::Update),                           \
                          InstanceMethod("reset", &Name##Wrap::Reset),                             \
                          InstanceAccessor("value", &Name##Wrap::Value, nullptr),                  \
                          InstanceAccessor("ready", &Name##Wrap::Ready, nullptr),                  \
                          InstanceAccessor("count", &Name##Wrap::Count, nullptr)});                \
    }                                                                                              \
    Name##Wrap(const Napi::CallbackInfo& info)                                                     \
        : Napi::ObjectWrap<Name##Wrap>(info),                                                      \
          _ind(checkedIndicator<lrvx::indicator::Name>(info.Env(), #row,                           \
                                                       argUint32(info[0], #Name ": period")))      \
    {                                                                                              \
    }                                                                                              \
                                                                                                   \
   private:                                                                                        \
    lrvx::indicator::Name _ind;                                                                    \
    Napi::Value Compute(const Napi::CallbackInfo& info)                                            \
    {                                                                                              \
      auto h = argFloat64Array(info[0], "compute: high");                                          \
      auto l = argFloat64Array(info[1], "compute: low");                                           \
      auto c = argFloat64Array(info[2], "compute: close");                                         \
      auto r = _ind.compute(arr2span(h), arr2span(l), arr2span(c));                                \
      return vec2arr(info.Env(), r);                                                               \
    }                                                                                              \
    Napi::Value Update(const Napi::CallbackInfo& info)                                             \
    {                                                                                              \
      _ind.update(argNumber(info[0], "update: high"),                                              \
                  argNumber(info[1], "update: low"),                                               \
                  argNumber(info[2], "update: close"));                                            \
      return optNum(info.Env(), _ind.value());                                                     \
    }                                                                                              \
    Napi::Value Value(const Napi::CallbackInfo& info) { return optNum(info.Env(), _ind.value()); } \
    Napi::Value Ready(const Napi::CallbackInfo& info)                                              \
    {                                                                                              \
      return Napi::Boolean::New(info.Env(), _ind.ready());                                         \
    }                                                                                              \
    Napi::Value Count(const Napi::CallbackInfo& info)                                              \
    {                                                                                              \
      return Napi::Number::New(info.Env(), static_cast<double>(_ind.count()));                     \
    }                                                                                              \
    void Reset(const Napi::CallbackInfo&) { _ind.reset(); }                                        \
  };

WRAP_BAR_1(ATR, atr)
WRAP_BAR_1(CCI, cci)
WRAP_BAR_1(CHOP, chop)

#undef WRAP_BAR_1

// ParkinsonVol(period) — high/low input
class ParkinsonVolWrap : public Napi::ObjectWrap<ParkinsonVolWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "ParkinsonVol",
                       {InstanceMethod("compute", &ParkinsonVolWrap::Compute),
                        InstanceMethod("update", &ParkinsonVolWrap::Update),
                        InstanceMethod("reset", &ParkinsonVolWrap::Reset),
                        InstanceAccessor("value", &ParkinsonVolWrap::Value, nullptr),
                        InstanceAccessor("ready", &ParkinsonVolWrap::Ready, nullptr),
                        InstanceAccessor("count", &ParkinsonVolWrap::Count, nullptr)});
  }
  ParkinsonVolWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<ParkinsonVolWrap>(info), _ind(checkedIndicator<lrvx::indicator::ParkinsonVol>(info.Env(), "parkinson_vol", argUint32(info[0], "ParkinsonVol: period")))
  {
  }

 private:
  lrvx::indicator::ParkinsonVol _ind;
  Napi::Value Compute(const Napi::CallbackInfo& info)
  {
    auto h = argFloat64Array(info[0], "compute: high");
    auto l = argFloat64Array(info[1], "compute: low");
    auto r = _ind.compute(arr2span(h), arr2span(l));
    return vec2arr(info.Env(), r);
  }
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    _ind.update(argNumber(info[0], "update: high"),
                argNumber(info[1], "update: low"));
    return optNum(info.Env(), _ind.value());
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return optNum(info.Env(), _ind.value()); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _ind.ready()); }
  Napi::Value Count(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), static_cast<double>(_ind.count()));
  }
  void Reset(const Napi::CallbackInfo&) { _ind.reset(); }
};

// RogersSatchellVol(period) — open/high/low/close input
class RogersSatchellVolWrap : public Napi::ObjectWrap<RogersSatchellVolWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "RogersSatchellVol",
                       {InstanceMethod("compute", &RogersSatchellVolWrap::Compute),
                        InstanceMethod("update", &RogersSatchellVolWrap::Update),
                        InstanceMethod("reset", &RogersSatchellVolWrap::Reset),
                        InstanceAccessor("value", &RogersSatchellVolWrap::Value, nullptr),
                        InstanceAccessor("ready", &RogersSatchellVolWrap::Ready, nullptr),
                        InstanceAccessor("count", &RogersSatchellVolWrap::Count, nullptr)});
  }
  RogersSatchellVolWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<RogersSatchellVolWrap>(info),
        _ind(checkedIndicator<lrvx::indicator::RogersSatchellVol>(info.Env(), "rogers_satchell_vol", argUint32(info[0], "RogersSatchellVol: period")))
  {
  }

 private:
  lrvx::indicator::RogersSatchellVol _ind;
  Napi::Value Compute(const Napi::CallbackInfo& info)
  {
    auto o = argFloat64Array(info[0], "compute: open");
    auto h = argFloat64Array(info[1], "compute: high");
    auto l = argFloat64Array(info[2], "compute: low");
    auto c = argFloat64Array(info[3], "compute: close");
    auto r = _ind.compute(arr2span(o), arr2span(h), arr2span(l), arr2span(c));
    return vec2arr(info.Env(), r);
  }
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    _ind.update(argNumber(info[0], "update: open"),
                argNumber(info[1], "update: high"),
                argNumber(info[2], "update: low"),
                argNumber(info[3], "update: close"));
    return optNum(info.Env(), _ind.value());
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return optNum(info.Env(), _ind.value()); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _ind.ready()); }
  Napi::Value Count(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), static_cast<double>(_ind.count()));
  }
  void Reset(const Napi::CallbackInfo&) { _ind.reset(); }
};

// Correlation(period) — pair input
class CorrelationWrap : public Napi::ObjectWrap<CorrelationWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "Correlation",
                       {InstanceMethod("compute", &CorrelationWrap::Compute),
                        InstanceMethod("update", &CorrelationWrap::Update),
                        InstanceMethod("reset", &CorrelationWrap::Reset),
                        InstanceAccessor("value", &CorrelationWrap::Value, nullptr),
                        InstanceAccessor("ready", &CorrelationWrap::Ready, nullptr),
                        InstanceAccessor("count", &CorrelationWrap::Count, nullptr)});
  }
  CorrelationWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<CorrelationWrap>(info), _ind(checkedIndicator<lrvx::indicator::Correlation>(info.Env(), "correlation", argUint32(info[0], "Correlation: period")))
  {
  }

 private:
  lrvx::indicator::Correlation _ind;
  Napi::Value Compute(const Napi::CallbackInfo& info)
  {
    auto x = argFloat64Array(info[0], "compute: x");
    auto y = argFloat64Array(info[1], "compute: y");
    auto r = _ind.compute(arr2span(x), arr2span(y));
    return vec2arr(info.Env(), r);
  }
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    _ind.update(argNumber(info[0], "update: x"),
                argNumber(info[1], "update: y"));
    return optNum(info.Env(), _ind.value());
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return optNum(info.Env(), _ind.value()); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _ind.ready()); }
  Napi::Value Count(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), static_cast<double>(_ind.count()));
  }
  void Reset(const Napi::CallbackInfo&) { _ind.reset(); }
};

// MACD(fast, slow, signal) — multi-output, named accessors line/signal/histogram
class MACDWrap : public Napi::ObjectWrap<MACDWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "MACD",
                       {InstanceMethod("compute", &MACDWrap::Compute),
                        InstanceMethod("update", &MACDWrap::Update),
                        InstanceMethod("reset", &MACDWrap::Reset),
                        InstanceAccessor("value", &MACDWrap::Value, nullptr),
                        InstanceAccessor("line", &MACDWrap::Line, nullptr),
                        InstanceAccessor("signal", &MACDWrap::Signal, nullptr),
                        InstanceAccessor("histogram", &MACDWrap::Histogram, nullptr),
                        InstanceAccessor("ready", &MACDWrap::Ready, nullptr),
                        InstanceAccessor("count", &MACDWrap::Count, nullptr)});
  }
  MACDWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<MACDWrap>(info),
        _ind(checkedIndicator<lrvx::indicator::MACD>(info.Env(), "macd", info.Length() > 0 ? argUint32(info[0], "MACD: fast") : 12u,
                                                     info.Length() > 1 ? argUint32(info[1], "MACD: slow") : 26u,
                                                     info.Length() > 2 ? argUint32(info[2], "MACD: signal") : 9u))
  {
  }

 private:
  lrvx::indicator::MACD _ind;
  Napi::Value Compute(const Napi::CallbackInfo& info)
  {
    auto in = argFloat64Array(info[0], "compute: input");
    auto r = _ind.compute(arr2span(in));
    auto out = Napi::Object::New(info.Env());
    out.Set("line", vec2arr(info.Env(), r.line));
    out.Set("signal", vec2arr(info.Env(), r.signal));
    out.Set("histogram", vec2arr(info.Env(), r.histogram));
    return out;
  }
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    // Declared to return { line, signal, histogram }, not a bare number:
    // returning optNum() here made `macd.update(x).line` throw on
    // "Cannot read properties of null" whenever update() itself came
    // back null-ish, since the declared object never existed.
    _ind.update(argNumber(info[0], "update: value"));
    auto out = Napi::Object::New(info.Env());
    out.Set("line", optNum(info.Env(), _ind.value()));
    out.Set("signal", optNum(info.Env(), _ind.signalValue()));
    out.Set("histogram", optNum(info.Env(), _ind.histogramValue()));
    return out;
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return optNum(info.Env(), _ind.value()); }
  Napi::Value Line(const Napi::CallbackInfo& info) { return optNum(info.Env(), _ind.value()); }
  Napi::Value Signal(const Napi::CallbackInfo& info) { return optNum(info.Env(), _ind.signalValue()); }
  Napi::Value Histogram(const Napi::CallbackInfo& info) { return optNum(info.Env(), _ind.histogramValue()); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _ind.ready()); }
  Napi::Value Count(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), static_cast<double>(_ind.count()));
  }
  void Reset(const Napi::CallbackInfo&) { _ind.reset(); }
};

// Bollinger(period, multiplier) — multi-output, named accessors upper/middle/lower
class BollingerWrap : public Napi::ObjectWrap<BollingerWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "Bollinger",
                       {InstanceMethod("compute", &BollingerWrap::Compute),
                        InstanceMethod("update", &BollingerWrap::Update),
                        InstanceMethod("reset", &BollingerWrap::Reset),
                        InstanceAccessor("value", &BollingerWrap::Value, nullptr),
                        InstanceAccessor("upper", &BollingerWrap::Upper, nullptr),
                        InstanceAccessor("middle", &BollingerWrap::Middle, nullptr),
                        InstanceAccessor("lower", &BollingerWrap::Lower, nullptr),
                        InstanceAccessor("ready", &BollingerWrap::Ready, nullptr),
                        InstanceAccessor("count", &BollingerWrap::Count, nullptr)});
  }
  BollingerWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<BollingerWrap>(info),
        _ind(checkedIndicator<lrvx::indicator::Bollinger>(info.Env(), "bollinger", info.Length() > 0 ? argUint32(info[0], "Bollinger: period") : 20u,
                                                          info.Length() > 1 ? argNumber(info[1], "Bollinger: stdDev") : 2.0))
  {
  }

 private:
  lrvx::indicator::Bollinger _ind;
  Napi::Value Compute(const Napi::CallbackInfo& info)
  {
    auto in = argFloat64Array(info[0], "compute: input");
    auto r = _ind.compute(arr2span(in));
    auto out = Napi::Object::New(info.Env());
    out.Set("upper", vec2arr(info.Env(), r.upper));
    out.Set("middle", vec2arr(info.Env(), r.middle));
    out.Set("lower", vec2arr(info.Env(), r.lower));
    return out;
  }
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    // See MACDWrap::Update -- same fix, same declared-object contract.
    _ind.update(argNumber(info[0], "update: value"));
    auto out = Napi::Object::New(info.Env());
    out.Set("upper", optNum(info.Env(), _ind.upperValue()));
    out.Set("middle", optNum(info.Env(), _ind.middleValue()));
    out.Set("lower", optNum(info.Env(), _ind.lowerValue()));
    return out;
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return optNum(info.Env(), _ind.value()); }
  Napi::Value Upper(const Napi::CallbackInfo& info) { return optNum(info.Env(), _ind.upperValue()); }
  Napi::Value Middle(const Napi::CallbackInfo& info) { return optNum(info.Env(), _ind.middleValue()); }
  Napi::Value Lower(const Napi::CallbackInfo& info) { return optNum(info.Env(), _ind.lowerValue()); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _ind.ready()); }
  Napi::Value Count(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), static_cast<double>(_ind.count()));
  }
  void Reset(const Napi::CallbackInfo&) { _ind.reset(); }
};

// Stochastic(k_period, d_period) — multi-output bar, named accessors k/d
class StochasticWrap : public Napi::ObjectWrap<StochasticWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "Stochastic",
                       {InstanceMethod("compute", &StochasticWrap::Compute),
                        InstanceMethod("update", &StochasticWrap::Update),
                        InstanceMethod("reset", &StochasticWrap::Reset),
                        InstanceAccessor("value", &StochasticWrap::Value, nullptr),
                        InstanceAccessor("k", &StochasticWrap::K, nullptr),
                        InstanceAccessor("d", &StochasticWrap::D, nullptr),
                        InstanceAccessor("ready", &StochasticWrap::Ready, nullptr),
                        InstanceAccessor("count", &StochasticWrap::Count, nullptr)});
  }
  StochasticWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<StochasticWrap>(info),
        _ind(checkedIndicator<lrvx::indicator::Stochastic>(info.Env(), "stochastic", info.Length() > 0 ? argUint32(info[0], "Stochastic: kPeriod") : 14u,
                                                           info.Length() > 1 ? argUint32(info[1], "Stochastic: dPeriod") : 3u))
  {
  }

 private:
  lrvx::indicator::Stochastic _ind;
  Napi::Value Compute(const Napi::CallbackInfo& info)
  {
    auto h = argFloat64Array(info[0], "compute: high");
    auto l = argFloat64Array(info[1], "compute: low");
    auto c = argFloat64Array(info[2], "compute: close");
    auto r = _ind.compute(arr2span(h), arr2span(l), arr2span(c));
    auto out = Napi::Object::New(info.Env());
    out.Set("k", vec2arr(info.Env(), r.k));
    out.Set("d", vec2arr(info.Env(), r.d));
    return out;
  }
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    // See MACDWrap::Update -- same fix, same declared-object contract.
    _ind.update(argNumber(info[0], "update: high"),
                argNumber(info[1], "update: low"),
                argNumber(info[2], "update: close"));
    auto out = Napi::Object::New(info.Env());
    out.Set("k", optNum(info.Env(), _ind.kValue()));
    out.Set("d", optNum(info.Env(), _ind.dValue()));
    return out;
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return optNum(info.Env(), _ind.value()); }
  Napi::Value K(const Napi::CallbackInfo& info) { return optNum(info.Env(), _ind.kValue()); }
  Napi::Value D(const Napi::CallbackInfo& info) { return optNum(info.Env(), _ind.dValue()); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _ind.ready()); }
  Napi::Value Count(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), static_cast<double>(_ind.count()));
  }
  void Reset(const Napi::CallbackInfo&) { _ind.reset(); }
};

// ── Discovery ──────────────────────────────────────────────────────
inline Napi::Value list_indicators(const Napi::CallbackInfo& info)
{
  auto arr = Napi::Array::New(info.Env());
  uint32_t i = 0;
#define LRVX_INDICATOR(Cls, name, Kind, Args) arr.Set(i++, Napi::String::New(info.Env(), #Cls));
#include "lrvx/indicator/registry.def"
#undef LRVX_INDICATOR
  return arr;
}

// ── Registration ────────────────────────────────────────────────────

inline void registerIndicators(Napi::Env env, Napi::Object exports)
{
  // Batch top-level functions
  exports.Set("sma", Napi::Function::New(env, batch_sma));
  exports.Set("ema", Napi::Function::New(env, batch_ema));
  exports.Set("rsi", Napi::Function::New(env, batch_rsi));
  exports.Set("rma", Napi::Function::New(env, batch_rma));
  exports.Set("dema", Napi::Function::New(env, batch_dema));
  exports.Set("tema", Napi::Function::New(env, batch_tema));
  exports.Set("kama", Napi::Function::New(env, batch_kama));
  exports.Set("slope", Napi::Function::New(env, batch_slope));
  exports.Set("atr", Napi::Function::New(env, batch_atr));
  exports.Set("adx", Napi::Function::New(env, batch_adx));
  exports.Set("macd", Napi::Function::New(env, batch_macd));
  exports.Set("bollinger", Napi::Function::New(env, batch_bollinger));
  exports.Set("stochastic", Napi::Function::New(env, batch_stochastic));
  exports.Set("cci", Napi::Function::New(env, batch_cci));
  exports.Set("chop", Napi::Function::New(env, batch_chop));
  exports.Set("obv", Napi::Function::New(env, batch_obv));
  exports.Set("vwap", Napi::Function::New(env, batch_vwap));
  exports.Set("cvd", Napi::Function::New(env, batch_cvd));
  exports.Set("skewness", Napi::Function::New(env, batch_skewness));
  exports.Set("kurtosis", Napi::Function::New(env, batch_kurtosis));
  exports.Set("rolling_zscore", Napi::Function::New(env, batch_rolling_zscore));
  exports.Set("shannon_entropy", Napi::Function::New(env, batch_shannon_entropy));
  exports.Set("parkinson_vol", Napi::Function::New(env, batch_parkinson_vol));
  exports.Set("rogers_satchell_vol", Napi::Function::New(env, batch_rogers_satchell_vol));
  // Rolling correlation: stat name `correlation` is reserved for the
  // single-number Pearson helper in stats.h (which loaded last and
  // shadowed this entry). Exposed under `rollingCorrelation` so both
  // are reachable.
  exports.Set("rollingCorrelation", Napi::Function::New(env, batch_correlation));
  exports.Set("adf", Napi::Function::New(env, batch_adf));
  exports.Set("autocorrelation", Napi::Function::New(env, batch_autocorrelation));

  // Streaming classes (each has compute() AND update()/value/ready/reset on
  // the same instance — single source of truth in the C++ class).
  exports.Set("SMA", SMAWrap::Init(env));
  exports.Set("EMA", EMAWrap::Init(env));
  exports.Set("RSI", RSIWrap::Init(env));
  exports.Set("ATR", ATRWrap::Init(env));
  exports.Set("MACD", MACDWrap::Init(env));
  exports.Set("Bollinger", BollingerWrap::Init(env));
  exports.Set("RMA", RMAWrap::Init(env));
  exports.Set("DEMA", DEMAWrap::Init(env));
  exports.Set("TEMA", TEMAWrap::Init(env));
  exports.Set("KAMA", KAMAWrap::Init(env));
  exports.Set("Slope", SlopeWrap::Init(env));
  exports.Set("Stochastic", StochasticWrap::Init(env));
  exports.Set("CCI", CCIWrap::Init(env));
  exports.Set("CHOP", CHOPWrap::Init(env));
  exports.Set("Skewness", SkewnessWrap::Init(env));
  exports.Set("Kurtosis", KurtosisWrap::Init(env));
  exports.Set("RollingZScore", RollingZScoreWrap::Init(env));
  exports.Set("ShannonEntropy", ShannonEntropyWrap::Init(env));
  exports.Set("ParkinsonVol", ParkinsonVolWrap::Init(env));
  exports.Set("RogersSatchellVol", RogersSatchellVolWrap::Init(env));
  exports.Set("Correlation", CorrelationWrap::Init(env));
  exports.Set("AutoCorrelation", AutoCorrelationWrap::Init(env));

  // Discovery
  exports.Set("list_indicators", Napi::Function::New(env, list_indicators));
}

}  // namespace node_lrvx
