// node/src/venue_availability.h — VenueAvailability wrap.
//
// Thin NAPI wrap over lrvx_venue_availability_* C ABI. Build a
// downtime schedule (manual outages + Poisson random outages) and
// attach to a SimulatedExecutor via executor.setVenueAvailability(va).

#pragma once

#include <napi.h>

#include "bindings_common.h"

#include <string>

#include "lrvx/capi/lrvx_capi.h"

namespace node_lrvx
{

inline uint8_t parseOnOutage(const std::string& s)
{
  if (s == "hold")
  {
    return 1;
  }
  if (s == "expire_gtc_after")
  {
    return 2;
  }
  return 0;  // cancel_all
}

inline uint8_t parseOutageType(const std::string& s)
{
  if (s == "submit_only_down" || s == "submit_only")
  {
    return 1;
  }
  if (s == "cancel_only_down" || s == "cancel_only")
  {
    return 2;
  }
  if (s == "slow_degradation")
  {
    return 3;
  }
  if (s == "stale_book")
  {
    return 4;
  }
  if (s == "wrong_side_recovery")
  {
    return 5;
  }
  return 0;  // total
}

class VenueAvailabilityWrap : public Napi::ObjectWrap<VenueAvailabilityWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(
        env, "VenueAvailability",
        {InstanceMethod("scheduleOutage", &VenueAvailabilityWrap::ScheduleOutage),
         InstanceMethod("scheduleOutageEx", &VenueAvailabilityWrap::ScheduleOutageEx),
         InstanceMethod("autoRandomOutages", &VenueAvailabilityWrap::AutoRandomOutages),
         InstanceMethod("isUp", &VenueAvailabilityWrap::IsUp),
         InstanceMethod("submitsAllowed", &VenueAvailabilityWrap::SubmitsAllowed),
         InstanceMethod("cancelsAllowed", &VenueAvailabilityWrap::CancelsAllowed),
         InstanceMethod("bookUpdatesAllowed",
                        &VenueAvailabilityWrap::BookUpdatesAllowed),
         InstanceMethod("tradesAllowed", &VenueAvailabilityWrap::TradesAllowed),
         InstanceMethod("latencyMultiplier",
                        &VenueAvailabilityWrap::LatencyMultiplier),
         InstanceMethod("consumeWrongSideRecoveryBps",
                        &VenueAvailabilityWrap::ConsumeWrongSideRecoveryBps)});
  }

  VenueAvailabilityWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<VenueAvailabilityWrap>(info),
        _h(lrvx_venue_availability_create())
  {
  }
  ~VenueAvailabilityWrap()
  {
    if (_h)
    {
      lrvx_venue_availability_destroy(_h);
    }
  }
  LrvxVenueAvailabilityHandle handle() const { return _h; }

 private:
  void ScheduleOutage(const Napi::CallbackInfo& info)
  {
    int64_t start = toInt64Ns(info[0]);
    int64_t duration = info[1].As<Napi::Number>().Int64Value();
    std::string policy = info.Length() > 2 && info[2].IsString()
                             ? info[2].As<Napi::String>().Utf8Value()
                             : "cancel_all";
    int64_t ttl = info.Length() > 3 ? info[3].As<Napi::Number>().Int64Value() : 0;
    lrvx_venue_availability_schedule_outage(_h, start, duration, parseOnOutage(policy),
                                            ttl);
  }
  void AutoRandomOutages(const Napi::CallbackInfo& info)
  {
    double perDay = info[0].As<Napi::Number>().DoubleValue();
    int64_t meanDur = info[1].As<Napi::Number>().Int64Value();
    std::string policy = info.Length() > 2 && info[2].IsString()
                             ? info[2].As<Napi::String>().Utf8Value()
                             : "cancel_all";
    uint64_t seed = info.Length() > 3 ? info[3].As<Napi::Number>().Int64Value() : 0xC0FFEEULL;
    lrvx_venue_availability_auto_random_outages(_h, perDay, meanDur,
                                                parseOnOutage(policy), seed);
  }
  Napi::Value IsUp(const Napi::CallbackInfo& info)
  {
    int64_t nowNs = toInt64Ns(info[0]);
    return Napi::Boolean::New(info.Env(), lrvx_venue_availability_is_up(_h, nowNs) != 0);
  }
  void ScheduleOutageEx(const Napi::CallbackInfo& info)
  {
    int64_t start = toInt64Ns(info[0]);
    int64_t duration = info[1].As<Napi::Number>().Int64Value();
    std::string outageType = info.Length() > 2 && info[2].IsString()
                                 ? info[2].As<Napi::String>().Utf8Value()
                                 : "total";
    std::string policy = info.Length() > 3 && info[3].IsString()
                             ? info[3].As<Napi::String>().Utf8Value()
                             : "cancel_all";
    int64_t ttl = info.Length() > 4 ? info[4].As<Napi::Number>().Int64Value() : 0;
    double degr = info.Length() > 5 ? info[5].As<Napi::Number>().DoubleValue() : 1.0;
    double wsBps = info.Length() > 6 ? info[6].As<Napi::Number>().DoubleValue() : 0.0;
    lrvx_venue_availability_schedule_outage_ex(
        _h, start, duration, parseOutageType(outageType), parseOnOutage(policy),
        ttl, degr, wsBps);
  }
  Napi::Value SubmitsAllowed(const Napi::CallbackInfo& info)
  {
    int64_t nowNs = toInt64Ns(info[0]);
    return Napi::Boolean::New(
        info.Env(), lrvx_venue_availability_submits_allowed(_h, nowNs) != 0);
  }
  Napi::Value CancelsAllowed(const Napi::CallbackInfo& info)
  {
    int64_t nowNs = toInt64Ns(info[0]);
    return Napi::Boolean::New(
        info.Env(), lrvx_venue_availability_cancels_allowed(_h, nowNs) != 0);
  }
  Napi::Value BookUpdatesAllowed(const Napi::CallbackInfo& info)
  {
    int64_t nowNs = toInt64Ns(info[0]);
    return Napi::Boolean::New(
        info.Env(), lrvx_venue_availability_book_updates_allowed(_h, nowNs) != 0);
  }
  Napi::Value TradesAllowed(const Napi::CallbackInfo& info)
  {
    int64_t nowNs = toInt64Ns(info[0]);
    return Napi::Boolean::New(
        info.Env(), lrvx_venue_availability_trades_allowed(_h, nowNs) != 0);
  }
  Napi::Value LatencyMultiplier(const Napi::CallbackInfo& info)
  {
    int64_t nowNs = toInt64Ns(info[0]);
    return Napi::Number::New(info.Env(),
                             lrvx_venue_availability_latency_multiplier(_h, nowNs));
  }
  Napi::Value ConsumeWrongSideRecoveryBps(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(
        info.Env(), lrvx_venue_availability_consume_wrong_side_recovery_bps(_h));
  }

  LrvxVenueAvailabilityHandle _h;
};

inline void registerVenueAvailability(Napi::Env env, Napi::Object exports)
{
  exports.Set("VenueAvailability", VenueAvailabilityWrap::Init(env));
}

}  // namespace node_lrvx
