// node/src/funding_schedule.h — Perpetual funding rate schedule.

#pragma once

#include <napi.h>

#include "bindings_common.h"

#include <vector>

#include "lrvx/capi/lrvx_capi.h"

namespace node_lrvx
{

class FundingScheduleWrap : public Napi::ObjectWrap<FundingScheduleWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(
        env, "FundingSchedule",
        {InstanceMethod("setConstant", &FundingScheduleWrap::SetConstant),
         InstanceMethod("setTape", &FundingScheduleWrap::SetTape),
         InstanceMethod("setTapeBySymbol", &FundingScheduleWrap::SetTapeBySymbol),
         InstanceMethod("loadTape", &FundingScheduleWrap::LoadTape),
         InstanceMethod("loadProfile", &FundingScheduleWrap::LoadProfile),
         InstanceMethod("setConstantRate", &FundingScheduleWrap::SetConstantRate),
         InstanceMethod("reset", &FundingScheduleWrap::Reset),
         InstanceMethod("tick", &FundingScheduleWrap::Tick)});
  }

  FundingScheduleWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<FundingScheduleWrap>(info),
        _h(lrvx_funding_schedule_create())
  {
  }
  ~FundingScheduleWrap()
  {
    if (_h)
    {
      lrvx_funding_schedule_destroy(_h);
    }
  }
  LrvxFundingScheduleHandle handle() const { return _h; }

 private:
  void SetConstant(const Napi::CallbackInfo& info)
  {
    lrvx_funding_schedule_set_constant(_h, info[0].As<Napi::Number>().Int64Value(),
                                       info[1].As<Napi::Number>().DoubleValue());
  }
  void SetTape(const Napi::CallbackInfo& info)
  {
    auto tsArr = info[0].As<Napi::Array>();
    auto rateArr = info[1].As<Napi::Array>();
    std::vector<int64_t> ts(tsArr.Length());
    std::vector<double> rates(rateArr.Length());
    for (uint32_t i = 0; i < tsArr.Length(); ++i)
    {
      ts[i] = toInt64Ns(tsArr.Get(i));
    }
    for (uint32_t i = 0; i < rateArr.Length(); ++i)
    {
      rates[i] = rateArr.Get(i).As<Napi::Number>().DoubleValue();
    }
    lrvx_funding_schedule_set_tape(_h, ts.data(), rates.data(),
                                   static_cast<uint32_t>(ts.size()));
  }
  void SetTapeBySymbol(const Napi::CallbackInfo& info)
  {
    // (timestampsNs[], symbols[], rates[])
    auto tsArr = info[0].As<Napi::Array>();
    auto symArr = info[1].As<Napi::Array>();
    auto rateArr = info[2].As<Napi::Array>();
    const uint32_t n = tsArr.Length();
    std::vector<int64_t> ts(n);
    std::vector<uint32_t> sy(n);
    std::vector<double> rates(n);
    for (uint32_t i = 0; i < n; ++i)
    {
      ts[i] = toInt64Ns(tsArr.Get(i));
      sy[i] = symArr.Get(i).As<Napi::Number>().Uint32Value();
      rates[i] = rateArr.Get(i).As<Napi::Number>().DoubleValue();
    }
    lrvx_funding_schedule_set_tape_by_symbol(_h, ts.data(), sy.data(), rates.data(), n);
  }
  Napi::Value LoadTape(const Napi::CallbackInfo& info)
  {
    std::string path = info[0].As<Napi::String>().Utf8Value();
    const uint8_t ok = lrvx_funding_schedule_load_tape(_h, path.c_str());
    return Napi::Boolean::New(info.Env(), ok != 0);
  }
  void LoadProfile(const Napi::CallbackInfo& info)
  {
    std::string name = info[0].As<Napi::String>().Utf8Value();
    // A typo used to be a silent no-op, leaving default funding schedule
    // values in place -- a zero-fee or unthrottled backtest that
    // looks perfectly healthy.
    if (lrvx_funding_schedule_load_profile(_h, name.c_str()) == 0)
    {
      Napi::Error::New(info.Env(), "unknown funding schedule profile: " + name +
                                       ". known: binance_um_futures, bybit_linear, okx_swap, bitget_hourly")
          .ThrowAsJavaScriptException();
    }
  }
  void SetConstantRate(const Napi::CallbackInfo& info)
  {
    lrvx_funding_schedule_set_constant_rate(_h, info[0].As<Napi::Number>().DoubleValue());
  }
  void Reset(const Napi::CallbackInfo&) { lrvx_funding_schedule_reset(_h); }

  Napi::Value Tick(const Napi::CallbackInfo& info)
  {
    int64_t nowNs = toInt64Ns(info[0]);
    auto symbols = info[1].As<Napi::Array>();
    auto positions = info[2].As<Napi::Array>();
    auto marks = info[3].As<Napi::Array>();
    uint32_t n = symbols.Length();
    std::vector<uint32_t> sy(n);
    std::vector<double> ps(n);
    std::vector<double> mk(n);
    for (uint32_t i = 0; i < n; ++i)
    {
      sy[i] = symbols.Get(i).As<Napi::Number>().Uint32Value();
      ps[i] = positions.Get(i).As<Napi::Number>().DoubleValue();
      mk[i] = marks.Get(i).As<Napi::Number>().DoubleValue();
    }
    uint32_t count = lrvx_funding_schedule_tick(_h, nowNs, sy.data(), ps.data(),
                                                mk.data(), n, nullptr, 0);
    std::vector<double> buf(count * 6, 0.0);
    lrvx_funding_schedule_tick(_h, nowNs, sy.data(), ps.data(), mk.data(), n, buf.data(),
                               count);
    Napi::Array out = Napi::Array::New(info.Env(), count);
    for (uint32_t i = 0; i < count; ++i)
    {
      Napi::Object o = Napi::Object::New(info.Env());
      o.Set("timestampNs", Napi::Number::New(info.Env(), buf[i * 6 + 0]));
      o.Set("symbol", Napi::Number::New(info.Env(), buf[i * 6 + 1]));
      o.Set("rate", Napi::Number::New(info.Env(), buf[i * 6 + 2]));
      o.Set("markPrice", Napi::Number::New(info.Env(), buf[i * 6 + 3]));
      o.Set("positionSigned", Napi::Number::New(info.Env(), buf[i * 6 + 4]));
      o.Set("amount", Napi::Number::New(info.Env(), buf[i * 6 + 5]));
      out.Set(i, o);
    }
    return out;
  }

  LrvxFundingScheduleHandle _h;
};

inline void registerFundingSchedule(Napi::Env env, Napi::Object exports)
{
  exports.Set("FundingSchedule", FundingScheduleWrap::Init(env));
}

}  // namespace node_lrvx
