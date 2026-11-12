// node/src/bindings_common.h -- small helpers shared across the NAPI
// binding headers.
//
// `requireSameLength` used to live only in indicators.h, which most
// multi-array batch indicators there already used to close exactly this
// gap: an element count read from the FIRST array only, with every other
// array blindly indexed at that same length. aggregators.h and stats.h
// have the identical pattern (extractTrades reads `n` from `timestamps`
// alone; stat_bar_returns / stat_trade_pnl read `n` from `signal_long`
// alone) and need the same check, so this lives in its own header
// included ahead of all three in lrvx_node.cpp rather than duplicated or
// left order-dependent on which binding header happens to declare it.

#pragma once
#include <napi.h>

#include "lrvx/capi/lrvx_capi.h"

#include <cstdint>
#include <initializer_list>
#include <string>

namespace node_lrvx
{

// JS Numbers are float64 and lose precision past 2^53, which truncates
// nanosecond timestamps in unpredictable ways (e.g. 1765615835519000000
// round-trips as 1765615835519000064). Accept BigInt for any int64_t arg
// that may hold a real ns timestamp; fall back to Number for callers that
// pass smaller values. This lived in data_ops.h, where the three other
// headers that police nanoseconds -- strategy.h, live_queue_position.h,
// feed_clock.h -- could not reach it without pulling in the whole reader
// and writer surface, so each of them truncated instead.
inline int64_t toInt64Ns(const Napi::Value& v)
{
  if (v.IsBigInt())
  {
    bool lossless = false;
    return v.As<Napi::BigInt>().Int64Value(&lossless);
  }
  return v.As<Napi::Number>().Int64Value();
}

// The trade durations, time-weighted return and time bounds every stats
// object carries, whichever struct they were computed into. Durations are
// differences and fit a double; the bounds are clock readings and do not.
inline void setTradeTimeStats(Napi::Env env, Napi::Object& obj, double avgTradeDurationNs,
                              double medianTradeDurationNs, double maxTradeDurationNs,
                              double timeWeightedReturn, int64_t startTimeNs, int64_t endTimeNs)
{
  obj.Set("avgTradeDurationNs", Napi::Number::New(env, avgTradeDurationNs));
  obj.Set("medianTradeDurationNs", Napi::Number::New(env, medianTradeDurationNs));
  obj.Set("maxTradeDurationNs", Napi::Number::New(env, maxTradeDurationNs));
  obj.Set("timeWeightedReturn", Napi::Number::New(env, timeWeightedReturn));
  obj.Set("startTimeNs", Napi::BigInt::New(env, startTimeNs));
  obj.Set("endTimeNs", Napi::BigInt::New(env, endTimeNs));
}

inline void setTradeTimeStats(Napi::Env env, Napi::Object& obj, const LrvxBacktestStats& s)
{
  setTradeTimeStats(env, obj, s.avgTradeDurationNs, s.medianTradeDurationNs,
                    s.maxTradeDurationNs, s.timeWeightedReturn, s.startTimeNs, s.endTimeNs);
}

inline bool requireSameLength(Napi::Env env, const char* fnName,
                              std::initializer_list<size_t> lens)
{
  auto it = lens.begin();
  size_t n = *it;
  for (++it; it != lens.end(); ++it)
  {
    if (*it != n)
    {
      Napi::RangeError::New(env, std::string(fnName) + ": input arrays must have the same length")
          .ThrowAsJavaScriptException();
      return false;
    }
  }
  return true;
}

}  // namespace node_lrvx
