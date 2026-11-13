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

#include <cmath>
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
//
// `what` names the argument in what is thrown: a TypeError for a value that
// is neither a Number nor a BigInt (it used to be a bare "A number was
// expected") and for NaN or an infinity, and a RangeError for a BigInt that
// does not fit 64 signed bits, which used to wrap without a word. A Number
// with a fraction is truncated, as it always was: a clock reading computed in
// floating point is not an error.
inline int64_t toInt64Ns(const Napi::Value& v, const char* what = "timestamp")
{
  if (v.IsBigInt())
  {
    bool lossless = false;
    const int64_t ns = v.As<Napi::BigInt>().Int64Value(&lossless);
    if (!lossless)
    {
      throw Napi::RangeError::New(
          v.Env(), std::string(what) + " is a BigInt that does not fit 64 signed bits");
    }
    return ns;
  }
  if (v.IsNumber())
  {
    const double d = v.As<Napi::Number>().DoubleValue();
    if (std::isfinite(d))
    {
      return v.As<Napi::Number>().Int64Value();
    }
  }
  throw Napi::TypeError::New(v.Env(),
                             std::string(what) + " must be a finite Number or a BigInt");
}

// Arguments read with a check. `v.As<Napi::Number>()` does not look at what
// `v` is: a string or undefined surfaced as a bare "A number was expected"
// naming nothing, and a 64-bit id read through Int64Value() took NaN, a
// fraction or a negative for whatever the conversion made of it and lost the
// low bits of anything above 2^53. Each of these throws a TypeError that
// names the argument.
inline double argNumber(const Napi::Value& v, const char* what)
{
  if (!v.IsNumber())
  {
    throw Napi::TypeError::New(v.Env(), std::string(what) + " must be a number");
  }
  return v.As<Napi::Number>().DoubleValue();
}

inline uint32_t argUint32(const Napi::Value& v, const char* what)
{
  if (v.IsNumber())
  {
    const double d = v.As<Napi::Number>().DoubleValue();
    if (d >= 0.0 && d <= 4294967295.0 && d == std::floor(d))
    {
      return static_cast<uint32_t>(d);
    }
  }
  throw Napi::TypeError::New(v.Env(),
                             std::string(what) + " must be an integer from 0 to 4294967295");
}

// A 64-bit id (an order id, a signal id, a fill id): a non-negative integer
// Number, which is exact up to 2^53, or a BigInt that fits 64 unsigned bits.
inline uint64_t argUint64(const Napi::Value& v, const char* what)
{
  if (v.IsBigInt())
  {
    bool lossless = false;
    const uint64_t id = v.As<Napi::BigInt>().Uint64Value(&lossless);
    if (lossless)
    {
      return id;
    }
  }
  else if (v.IsNumber())
  {
    const double d = v.As<Napi::Number>().DoubleValue();
    if (d >= 0.0 && d <= 9007199254740992.0 && d == std::floor(d))
    {
      return static_cast<uint64_t>(d);
    }
  }
  throw Napi::TypeError::New(
      v.Env(), std::string(what) +
                   " must be a non-negative integer Number (at most 2^53) or a BigInt below 2^64");
}

// A signed 64-bit value (a raw fixed-point price or quantity): an integer
// Number within +-2^53, or a BigInt that fits 64 signed bits.
inline int64_t argInt64(const Napi::Value& v, const char* what)
{
  if (v.IsBigInt())
  {
    bool lossless = false;
    const int64_t x = v.As<Napi::BigInt>().Int64Value(&lossless);
    if (lossless)
    {
      return x;
    }
  }
  else if (v.IsNumber())
  {
    const double d = v.As<Napi::Number>().DoubleValue();
    if (d >= -9007199254740992.0 && d <= 9007199254740992.0 && d == std::floor(d))
    {
      return static_cast<int64_t>(d);
    }
  }
  throw Napi::TypeError::New(
      v.Env(), std::string(what) +
                   " must be an integer Number (within 2^53) or a BigInt that fits 64 bits");
}

// A whole number that need not be exact: a count, a size, a sequence number.
// A finite Number, truncated toward zero as Int64Value() always truncated
// it, or a BigInt that fits 64 signed bits. This is the rule toInt64Ns reads
// a clock by.
inline int64_t argInteger(const Napi::Value& v, const char* what)
{
  return toInt64Ns(v, what);
}

inline int32_t argInt32(const Napi::Value& v, const char* what)
{
  if (v.IsNumber())
  {
    const double d = v.As<Napi::Number>().DoubleValue();
    if (d >= -2147483648.0 && d <= 2147483647.0 && d == std::floor(d))
    {
      return static_cast<int32_t>(d);
    }
  }
  throw Napi::TypeError::New(
      v.Env(), std::string(what) + " must be an integer from -2147483648 to 2147483647");
}

inline std::string argString(const Napi::Value& v, const char* what)
{
  if (!v.IsString())
  {
    throw Napi::TypeError::New(v.Env(), std::string(what) + " must be a string");
  }
  return v.As<Napi::String>().Utf8Value();
}

inline bool argBool(const Napi::Value& v, const char* what)
{
  if (!v.IsBoolean())
  {
    throw Napi::TypeError::New(v.Env(), std::string(what) + " must be a boolean");
  }
  return v.As<Napi::Boolean>().Value();
}

inline Napi::Object argObject(const Napi::Value& v, const char* what)
{
  if (!v.IsObject())
  {
    throw Napi::TypeError::New(v.Env(), std::string(what) + " must be an object");
  }
  return v.As<Napi::Object>();
}

inline Napi::Array argArray(const Napi::Value& v, const char* what)
{
  if (!v.IsArray())
  {
    throw Napi::TypeError::New(v.Env(), std::string(what) + " must be an array");
  }
  return v.As<Napi::Array>();
}

inline Napi::Function argFunction(const Napi::Value& v, const char* what)
{
  if (!v.IsFunction())
  {
    throw Napi::TypeError::New(v.Env(), std::string(what) + " must be a function");
  }
  return v.As<Napi::Function>();
}

inline Napi::BigInt argBigInt(const Napi::Value& v, const char* what)
{
  if (!v.IsBigInt())
  {
    throw Napi::TypeError::New(v.Env(), std::string(what) + " must be a BigInt");
  }
  return v.As<Napi::BigInt>();
}

// A typed array of exactly the element type the native side reads. As<>()
// does not look: a Float32Array taken for a Float64Array hands back a
// pointer to 4-byte elements with a count of them, and the native loop reads
// 8 bytes per element off the end of the buffer.
template <typename Array>
inline Array argTypedArray(const Napi::Value& v, napi_typedarray_type type, const char* what,
                           const char* name)
{
  if (!v.IsTypedArray() || v.As<Napi::TypedArray>().TypedArrayType() != type)
  {
    throw Napi::TypeError::New(v.Env(), std::string(what) + " must be " + name);
  }
  return v.As<Array>();
}

inline Napi::Float64Array argFloat64Array(const Napi::Value& v, const char* what)
{
  return argTypedArray<Napi::Float64Array>(v, napi_float64_array, what, "a Float64Array");
}

inline Napi::BigInt64Array argBigInt64Array(const Napi::Value& v, const char* what)
{
  return argTypedArray<Napi::BigInt64Array>(v, napi_bigint64_array, what, "a BigInt64Array");
}

inline Napi::Int8Array argInt8Array(const Napi::Value& v, const char* what)
{
  return argTypedArray<Napi::Int8Array>(v, napi_int8_array, what, "an Int8Array");
}

inline Napi::Uint8Array argUint8Array(const Napi::Value& v, const char* what)
{
  return argTypedArray<Napi::Uint8Array>(v, napi_uint8_array, what, "a Uint8Array");
}

inline Napi::Uint32Array argUint32Array(const Napi::Value& v, const char* what)
{
  return argTypedArray<Napi::Uint32Array>(v, napi_uint32_array, what, "a Uint32Array");
}

// The options object a method takes as its only argument.
inline Napi::Object argOptions(const Napi::CallbackInfo& info, const char* method)
{
  if (info.Length() < 1 || !info[0].IsObject())
  {
    throw Napi::TypeError::New(info.Env(), std::string(method) + ": expected an options object");
  }
  return info[0].As<Napi::Object>();
}

// A 64-bit id handed to JavaScript whole. A Number holds one up to 2^53, and
// an order id is (subscriber id << 32) + n: with a subscriber id of 2^21 or
// more it is past that.
inline Napi::Value idToBigInt(Napi::Env env, uint64_t id) { return Napi::BigInt::New(env, id); }

// An order id as JavaScript sees it everywhere: the Number that order events
// carry in orderId and that emit.cancel / emit.modify take back.
inline Napi::Value orderIdToJs(Napi::Env env, uint64_t id)
{
  return Napi::Number::New(env, static_cast<double>(id));
}

// lrvx_last_error_message() without the C function name it opens with: a
// JavaScript caller called a method, not lrvx_run_reader_open.
inline std::string capiReason()
{
  std::string msg = lrvx_last_error_message();
  const auto colon = msg.find(": ");
  if (msg.rfind("lrvx_", 0) == 0 && colon != std::string::npos)
  {
    msg.erase(0, colon + 2);
  }
  return msg;
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
