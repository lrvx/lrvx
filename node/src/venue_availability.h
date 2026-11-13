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
  static Napi::FunctionReference& Ctor()
  {
    static Napi::FunctionReference ref;
    return ref;
  }

  static Napi::Function Init(Napi::Env env)
  {
    return keepCtor(Ctor(), DefineClass(
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
                                                &VenueAvailabilityWrap::ConsumeWrongSideRecoveryBps)}));
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
    int64_t start = toInt64Ns(info[0], "scheduleOutage: argument 1");
    int64_t duration = argInteger(info[1], "scheduleOutage: argument 2");
    std::string policy = given(info[2])
                             ? argString(info[2], "scheduleOutage: argument 3")
                             : "cancel_all";
    int64_t ttl = info.Length() > 3 ? argInteger(info[3], "scheduleOutage: argument 4") : 0;
    lrvx_venue_availability_schedule_outage(_h, start, duration, parseOnOutage(policy),
                                            ttl);
  }
  void AutoRandomOutages(const Napi::CallbackInfo& info)
  {
    double perDay = argNumber(info[0], "autoRandomOutages: argument 1");
    int64_t meanDur = argInteger(info[1], "autoRandomOutages: argument 2");
    std::string policy = given(info[2])
                             ? argString(info[2], "autoRandomOutages: argument 3")
                             : "cancel_all";
    uint64_t seed = info.Length() > 3 ? argInteger(info[3], "autoRandomOutages: argument 4") : 0xC0FFEEULL;
    lrvx_venue_availability_auto_random_outages(_h, perDay, meanDur,
                                                parseOnOutage(policy), seed);
  }
  Napi::Value IsUp(const Napi::CallbackInfo& info)
  {
    int64_t nowNs = toInt64Ns(info[0], "isUp: argument 1");
    return Napi::Boolean::New(info.Env(), lrvx_venue_availability_is_up(_h, nowNs) != 0);
  }
  void ScheduleOutageEx(const Napi::CallbackInfo& info)
  {
    int64_t start = toInt64Ns(info[0], "scheduleOutageEx: argument 1");
    int64_t duration = argInteger(info[1], "scheduleOutageEx: argument 2");
    std::string outageType = given(info[2])
                                 ? argString(info[2], "scheduleOutageEx: argument 3")
                                 : "total";
    std::string policy = given(info[3])
                             ? argString(info[3], "scheduleOutageEx: argument 4")
                             : "cancel_all";
    int64_t ttl = info.Length() > 4 ? argInteger(info[4], "scheduleOutageEx: argument 5") : 0;
    double degr = info.Length() > 5 ? argNumber(info[5], "scheduleOutageEx: argument 6") : 1.0;
    double wsBps = info.Length() > 6 ? argNumber(info[6], "scheduleOutageEx: argument 7") : 0.0;
    lrvx_venue_availability_schedule_outage_ex(
        _h, start, duration, parseOutageType(outageType), parseOnOutage(policy),
        ttl, degr, wsBps);
  }
  Napi::Value SubmitsAllowed(const Napi::CallbackInfo& info)
  {
    int64_t nowNs = toInt64Ns(info[0], "submitsAllowed: argument 1");
    return Napi::Boolean::New(
        info.Env(), lrvx_venue_availability_submits_allowed(_h, nowNs) != 0);
  }
  Napi::Value CancelsAllowed(const Napi::CallbackInfo& info)
  {
    int64_t nowNs = toInt64Ns(info[0], "cancelsAllowed: argument 1");
    return Napi::Boolean::New(
        info.Env(), lrvx_venue_availability_cancels_allowed(_h, nowNs) != 0);
  }
  Napi::Value BookUpdatesAllowed(const Napi::CallbackInfo& info)
  {
    int64_t nowNs = toInt64Ns(info[0], "bookUpdatesAllowed: argument 1");
    return Napi::Boolean::New(
        info.Env(), lrvx_venue_availability_book_updates_allowed(_h, nowNs) != 0);
  }
  Napi::Value TradesAllowed(const Napi::CallbackInfo& info)
  {
    int64_t nowNs = toInt64Ns(info[0], "tradesAllowed: argument 1");
    return Napi::Boolean::New(
        info.Env(), lrvx_venue_availability_trades_allowed(_h, nowNs) != 0);
  }
  Napi::Value LatencyMultiplier(const Napi::CallbackInfo& info)
  {
    int64_t nowNs = toInt64Ns(info[0], "latencyMultiplier: argument 1");
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
