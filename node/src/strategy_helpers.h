// node/src/strategy_helpers.h -- exit rules, History and the crossings.
//
// Thin NAPI wraps over the C ABI handles lrvx_exit_* and lrvx_history_*,
// each of which owns the C++ class (lrvx/strategy/exits.h,
// lrvx/strategy/history.h), so the levels are the C++ arithmetic.

#pragma once

#include <napi.h>

#include "bindings_common.h"
#include "lrvx/capi/lrvx_capi.h"

#include <cmath>
#include <limits>
#include <string>

namespace node_lrvx
{

namespace helpers_detail
{

inline uint8_t exitSide(const Napi::Value& v)
{
  if (v.IsString())
  {
    const std::string s = v.As<Napi::String>().Utf8Value();
    if (s == "long")
    {
      return 0;
    }
    if (s == "short")
    {
      return 1;
    }
  }
  throw Napi::RangeError::New(v.Env(), "side must be 'long' or 'short'");
}

// The C API refuses with NULL and code 2; the message says why.
inline LrvxExitRuleHandle requireRule(Napi::Env env, LrvxExitRuleHandle h)
{
  if (h == nullptr)
  {
    throw Napi::RangeError::New(env, lrvx_last_error_message());
  }
  return h;
}

inline double positivePct(const Napi::Value& v, const char* rule)
{
  const double pct = argNumber(v, rule);
  if (!(pct > 0.0))
  {
    throw Napi::RangeError::New(v.Env(), std::string(rule) + " must be > 0");
  }
  return pct;
}

}  // namespace helpers_detail

// The members every rule shares, over one C handle. Kind picks the shape
// of arm / update; each concrete wrap below registers the members that
// apply to it.
enum class ExitKind
{
  Atr,
  Percent,
  Time,
};

template <typename Derived, ExitKind Kind>
class ExitRuleWrap : public Napi::ObjectWrap<Derived>
{
 public:
  explicit ExitRuleWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<Derived>(info) {}
  ~ExitRuleWrap() { lrvx_exit_rule_destroy(_h); }

  Napi::Value Arm(const Napi::CallbackInfo& info)
  {
    if constexpr (Kind == ExitKind::Time)
    {
      lrvx_exit_rule_arm(_h, 0, 0.0, 0.0);
    }
    else
    {
      const uint8_t side = helpers_detail::exitSide(info[0]);
      const double entry = argNumber(info[1], "arm: entryPrice");
      const double atr = Kind == ExitKind::Atr ? argNumber(info[2], "arm: atr") : 0.0;
      lrvx_exit_rule_arm(_h, side, entry, atr);
    }
    return info.Env().Undefined();
  }

  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    double high = 0.0;
    double low = 0.0;
    double atr = 0.0;
    if constexpr (Kind != ExitKind::Time)
    {
      high = argNumber(info[0], "update: high");
      low = argNumber(info[1], "update: low");
    }
    if constexpr (Kind == ExitKind::Atr)
    {
      atr = argNumber(info[2], "update: atr");
    }
    return Napi::Boolean::New(info.Env(), lrvx_exit_rule_update(_h, high, low, atr) != 0);
  }

  Napi::Value Disarm(const Napi::CallbackInfo& info)
  {
    lrvx_exit_rule_disarm(_h);
    return info.Env().Undefined();
  }

  // The stop, or the target; NaN when disarmed.
  Napi::Value Level(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), lrvx_exit_rule_level(_h));
  }

  Napi::Value Armed(const Napi::CallbackInfo& info)
  {
    return Napi::Boolean::New(info.Env(), lrvx_exit_rule_armed(_h) != 0);
  }

  Napi::Value AtBreakEven(const Napi::CallbackInfo& info)
  {
    return Napi::Boolean::New(info.Env(), lrvx_exit_rule_at_break_even(_h) != 0);
  }

  Napi::Value Bars(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), lrvx_exit_rule_bars(_h));
  }

  // A constructor argument kept as a read-only property.
  Napi::Value Param0(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _p0); }
  Napi::Value Param1(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _p1); }

 protected:
  LrvxExitRuleHandle _h{nullptr};
  double _p0{0.0};
  double _p1{0.0};
};

// A stop atrMultiple ATRs from the entry, frozen there.
class AtrStopWrap : public ExitRuleWrap<AtrStopWrap, ExitKind::Atr>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "AtrStop",
                       {InstanceMethod("arm", &AtrStopWrap::Arm),
                        InstanceMethod("update", &AtrStopWrap::Update),
                        InstanceMethod("disarm", &AtrStopWrap::Disarm),
                        InstanceAccessor("stop", &AtrStopWrap::Level, nullptr),
                        InstanceAccessor("armed", &AtrStopWrap::Armed, nullptr),
                        InstanceAccessor("atrMultiple", &AtrStopWrap::Param0, nullptr)});
  }

  explicit AtrStopWrap(const Napi::CallbackInfo& info) : ExitRuleWrap(info)
  {
    _p0 = argNumber(info[0], "AtrStop: atrMultiple");
    _h = helpers_detail::requireRule(info.Env(), lrvx_exit_atr_stop_create(_p0));
  }
};

// The stop hangs atrMultiple ATRs from the best price of the last lookback
// bars (0: since the entry) and only moves in the position's favour.
class ChandelierExitWrap : public ExitRuleWrap<ChandelierExitWrap, ExitKind::Atr>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "ChandelierExit",
                       {InstanceMethod("arm", &ChandelierExitWrap::Arm),
                        InstanceMethod("update", &ChandelierExitWrap::Update),
                        InstanceMethod("disarm", &ChandelierExitWrap::Disarm),
                        InstanceAccessor("stop", &ChandelierExitWrap::Level, nullptr),
                        InstanceAccessor("armed", &ChandelierExitWrap::Armed, nullptr),
                        InstanceAccessor("lookback", &ChandelierExitWrap::Param0, nullptr),
                        InstanceAccessor("atrMultiple", &ChandelierExitWrap::Param1, nullptr)});
  }

  explicit ChandelierExitWrap(const Napi::CallbackInfo& info) : ExitRuleWrap(info)
  {
    const uint32_t lookback = argUint32(info[0], "ChandelierExit: lookback");
    _p0 = lookback;
    _p1 = argNumber(info[1], "ChandelierExit: atrMultiple");
    _h = helpers_detail::requireRule(info.Env(), lrvx_exit_chandelier_create(lookback, _p1));
  }
};

// A hard stop that moves to the entry once the position is breakEvenAtr ATRs
// in profit, then trails the best price at trailAtr ATRs.
class BreakEvenTrailWrap : public ExitRuleWrap<BreakEvenTrailWrap, ExitKind::Atr>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "BreakEvenTrail",
                       {InstanceMethod("arm", &BreakEvenTrailWrap::Arm),
                        InstanceMethod("update", &BreakEvenTrailWrap::Update),
                        InstanceMethod("disarm", &BreakEvenTrailWrap::Disarm),
                        InstanceAccessor("stop", &BreakEvenTrailWrap::Level, nullptr),
                        InstanceAccessor("armed", &BreakEvenTrailWrap::Armed, nullptr),
                        InstanceAccessor("atBreakEven", &BreakEvenTrailWrap::AtBreakEven, nullptr)});
  }

  explicit BreakEvenTrailWrap(const Napi::CallbackInfo& info) : ExitRuleWrap(info)
  {
    _h = helpers_detail::requireRule(
        info.Env(), lrvx_exit_break_even_trail_create(
                        argNumber(info[0], "BreakEvenTrail: hardStopAtr"),
                        argNumber(info[1], "BreakEvenTrail: breakEvenAtr"),
                        argNumber(info[2], "BreakEvenTrail: trailAtr")));
  }
};

// A stop at entry * (1 - pct / 100) for a long, entry * (1 + pct / 100) for
// a short.
class PercentStopWrap : public ExitRuleWrap<PercentStopWrap, ExitKind::Percent>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "PercentStop",
                       {InstanceMethod("arm", &PercentStopWrap::Arm),
                        InstanceMethod("update", &PercentStopWrap::Update),
                        InstanceMethod("disarm", &PercentStopWrap::Disarm),
                        InstanceAccessor("stop", &PercentStopWrap::Level, nullptr),
                        InstanceAccessor("armed", &PercentStopWrap::Armed, nullptr),
                        InstanceAccessor("pct", &PercentStopWrap::Param0, nullptr)});
  }

  explicit PercentStopWrap(const Napi::CallbackInfo& info) : ExitRuleWrap(info)
  {
    _p0 = helpers_detail::positivePct(info[0], "PercentStop: pct");
    _h = helpers_detail::requireRule(info.Env(), lrvx_exit_percent_stop_create(_p0));
  }
};

// A target at entry * (1 + pct / 100) for a long, entry * (1 - pct / 100)
// for a short.
class TakeProfitWrap : public ExitRuleWrap<TakeProfitWrap, ExitKind::Percent>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "TakeProfit",
                       {InstanceMethod("arm", &TakeProfitWrap::Arm),
                        InstanceMethod("update", &TakeProfitWrap::Update),
                        InstanceMethod("disarm", &TakeProfitWrap::Disarm),
                        InstanceAccessor("target", &TakeProfitWrap::Level, nullptr),
                        InstanceAccessor("armed", &TakeProfitWrap::Armed, nullptr),
                        InstanceAccessor("pct", &TakeProfitWrap::Param0, nullptr)});
  }

  explicit TakeProfitWrap(const Napi::CallbackInfo& info) : ExitRuleWrap(info)
  {
    _p0 = helpers_detail::positivePct(info[0], "TakeProfit: pct");
    _h = helpers_detail::requireRule(info.Env(), lrvx_exit_take_profit_create(_p0));
  }
};

// True from the maxBars-th update after arm() on.
class TimeStopWrap : public ExitRuleWrap<TimeStopWrap, ExitKind::Time>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "TimeStop",
                       {InstanceMethod("arm", &TimeStopWrap::Arm),
                        InstanceMethod("update", &TimeStopWrap::Update),
                        InstanceMethod("disarm", &TimeStopWrap::Disarm),
                        InstanceAccessor("bars", &TimeStopWrap::Bars, nullptr),
                        InstanceAccessor("armed", &TimeStopWrap::Armed, nullptr),
                        InstanceAccessor("maxBars", &TimeStopWrap::Param0, nullptr)});
  }

  explicit TimeStopWrap(const Napi::CallbackInfo& info) : ExitRuleWrap(info)
  {
    const double n = argNumber(info[0], "TimeStop: maxBars");
    if (!(n >= 1.0) || n > 4294967295.0 || n != std::floor(n))
    {
      throw Napi::RangeError::New(info.Env(), "TimeStop: maxBars must be a whole number >= 1");
    }
    _p0 = n;
    _h = helpers_detail::requireRule(info.Env(),
                                     lrvx_exit_time_stop_create(static_cast<uint32_t>(n)));
  }
};

// History(capacity): the last `capacity` values, newest first.
class HistoryWrap : public Napi::ObjectWrap<HistoryWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "History",
                       {InstanceMethod("push", &HistoryWrap::Push),
                        InstanceMethod("get", &HistoryWrap::Get),
                        InstanceAccessor("size", &HistoryWrap::Size, nullptr),
                        InstanceAccessor("capacity", &HistoryWrap::Capacity, nullptr),
                        InstanceAccessor("full", &HistoryWrap::Full, nullptr)});
  }

  explicit HistoryWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<HistoryWrap>(info)
  {
    const double n = argNumber(info[0], "History: capacity");
    if (!(n >= 1.0) || n > 4294967295.0 || n != std::floor(n))
    {
      throw Napi::RangeError::New(info.Env(), "History: capacity must be a whole number >= 1");
    }
    _h = lrvx_history_create(static_cast<uint32_t>(n));
    if (_h == nullptr)
    {
      throw Napi::RangeError::New(info.Env(), lrvx_last_error_message());
    }
  }

  ~HistoryWrap() { lrvx_history_destroy(_h); }

 private:
  LrvxHistoryHandle _h{nullptr};

  Napi::Value Push(const Napi::CallbackInfo& info)
  {
    lrvx_history_push(_h, argNumber(info[0], "History.push: value"));
    return info.Env().Undefined();
  }

  // 0 is the newest; RangeError outside 0 .. size - 1.
  Napi::Value Get(const Napi::CallbackInfo& info)
  {
    const double i = argNumber(info[0], "History.get: index");
    const uint32_t n = lrvx_history_size(_h);
    if (!(i >= 0.0) || i != std::floor(i) || i >= static_cast<double>(n))
    {
      throw Napi::RangeError::New(info.Env(), "History index " + std::to_string(i) +
                                                  " out of range (size " + std::to_string(n) +
                                                  ")");
    }
    return Napi::Number::New(info.Env(), lrvx_history_get(_h, static_cast<uint32_t>(i)));
  }

  Napi::Value Size(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), lrvx_history_size(_h));
  }

  Napi::Value Capacity(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), lrvx_history_capacity(_h));
  }

  Napi::Value Full(const Napi::CallbackInfo& info)
  {
    return Napi::Boolean::New(info.Env(), lrvx_history_full(_h) != 0);
  }
};

// A value that is not a number (null, undefined) is no cross, as NaN is.
inline Napi::Value crossed(const Napi::CallbackInfo& info, bool above)
{
  for (size_t i = 0; i < 4; ++i)
  {
    if (!info[i].IsNumber())
    {
      return Napi::Boolean::New(info.Env(), false);
    }
  }
  const double ap = info[0].As<Napi::Number>().DoubleValue();
  const double a = info[1].As<Napi::Number>().DoubleValue();
  const double bp = info[2].As<Napi::Number>().DoubleValue();
  const double b = info[3].As<Napi::Number>().DoubleValue();
  const uint8_t r = above ? lrvx_crossed_above(ap, a, bp, b) : lrvx_crossed_below(ap, a, bp, b);
  return Napi::Boolean::New(info.Env(), r != 0);
}

inline void registerStrategyHelpers(Napi::Env env, Napi::Object exports)
{
  exports.Set("AtrStop", AtrStopWrap::Init(env));
  exports.Set("ChandelierExit", ChandelierExitWrap::Init(env));
  exports.Set("BreakEvenTrail", BreakEvenTrailWrap::Init(env));
  exports.Set("PercentStop", PercentStopWrap::Init(env));
  exports.Set("TakeProfit", TakeProfitWrap::Init(env));
  exports.Set("TimeStop", TimeStopWrap::Init(env));
  exports.Set("History", HistoryWrap::Init(env));
  exports.Set("crossedAbove", Napi::Function::New(env, [](const Napi::CallbackInfo& info)
                                                  { return crossed(info, true); }));
  exports.Set("crossedBelow", Napi::Function::New(env, [](const Napi::CallbackInfo& info)
                                                  { return crossed(info, false); }));
}

}  // namespace node_lrvx
