// node/src/profiles.h -- VolumeProfile, MarketProfile, FootprintBar, CompositeBookMatrix

#pragma once
#include <napi.h>
#include "data_ops.h"
#include "lrvx/capi/lrvx_capi.h"

namespace node_lrvx
{

class VolumeProfileWrap : public Napi::ObjectWrap<VolumeProfileWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "VolumeProfile",
                       {InstanceMethod("addTrade", &VolumeProfileWrap::AddTrade),
                        InstanceMethod("poc", &VolumeProfileWrap::Poc),
                        InstanceMethod("valueAreaHigh", &VolumeProfileWrap::Vah),
                        InstanceMethod("valueAreaLow", &VolumeProfileWrap::Val),
                        InstanceMethod("totalVolume", &VolumeProfileWrap::TotalVol),
                        InstanceMethod("clear", &VolumeProfileWrap::Clear)});
  }
  VolumeProfileWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<VolumeProfileWrap>(info),
                                                      _h(lrvx_volume_profile_create(argNumber(info[0], "VolumeProfile: argument 1"))) {}
  ~VolumeProfileWrap()
  {
    if (_h)
    {
      lrvx_volume_profile_destroy(_h);
    }
  }

 private:
  void AddTrade(const Napi::CallbackInfo& info) { lrvx_volume_profile_add_trade(_h, argNumber(info[0], "addTrade: argument 1"), argNumber(info[1], "addTrade: argument 2"), argBool(info[2], "addTrade: argument 3") ? 1 : 0); }
  Napi::Value Poc(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), lrvx_volume_profile_poc(_h)); }
  Napi::Value Vah(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), lrvx_volume_profile_vah(_h)); }
  Napi::Value Val(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), lrvx_volume_profile_val(_h)); }
  Napi::Value TotalVol(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), lrvx_volume_profile_total_volume(_h)); }
  void Clear(const Napi::CallbackInfo&) { lrvx_volume_profile_clear(_h); }
  LrvxVolumeProfileHandle _h;
};

class MarketProfileWrap : public Napi::ObjectWrap<MarketProfileWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "MarketProfile",
                       {InstanceMethod("addTrade", &MarketProfileWrap::AddTrade),
                        InstanceMethod("poc", &MarketProfileWrap::Poc),
                        InstanceMethod("valueAreaHigh", &MarketProfileWrap::Vah),
                        InstanceMethod("valueAreaLow", &MarketProfileWrap::Val),
                        InstanceMethod("initialBalanceHigh", &MarketProfileWrap::IbHigh),
                        InstanceMethod("initialBalanceLow", &MarketProfileWrap::IbLow),
                        InstanceMethod("isPoorHigh", &MarketProfileWrap::PoorHigh),
                        InstanceMethod("isPoorLow", &MarketProfileWrap::PoorLow),
                        InstanceMethod("clear", &MarketProfileWrap::Clear)});
  }
  MarketProfileWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<MarketProfileWrap>(info),
                                                      _h(lrvx_market_profile_create(argNumber(info[0], "MarketProfile: argument 1"),
                                                                                    argUint32(info[1], "MarketProfile: argument 2"), toInt64Ns(info[2], "MarketProfile: argument 3"))) {}
  ~MarketProfileWrap()
  {
    if (_h)
    {
      lrvx_market_profile_destroy(_h);
    }
  }

 private:
  void AddTrade(const Napi::CallbackInfo& info) { lrvx_market_profile_add_trade(_h, toInt64Ns(info[0], "addTrade: argument 1"), argNumber(info[1], "addTrade: argument 2"), argNumber(info[2], "addTrade: argument 3"), argBool(info[3], "addTrade: argument 4") ? 1 : 0); }
  Napi::Value Poc(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), lrvx_market_profile_poc(_h)); }
  Napi::Value Vah(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), lrvx_market_profile_vah(_h)); }
  Napi::Value Val(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), lrvx_market_profile_val(_h)); }
  Napi::Value IbHigh(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), lrvx_market_profile_ib_high(_h)); }
  Napi::Value IbLow(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), lrvx_market_profile_ib_low(_h)); }
  Napi::Value PoorHigh(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), lrvx_market_profile_is_poor_high(_h)); }
  Napi::Value PoorLow(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), lrvx_market_profile_is_poor_low(_h)); }
  void Clear(const Napi::CallbackInfo&) { lrvx_market_profile_clear(_h); }
  LrvxMarketProfileHandle _h;
};

class FootprintBarWrap : public Napi::ObjectWrap<FootprintBarWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "FootprintBar",
                       {InstanceMethod("addTrade", &FootprintBarWrap::AddTrade),
                        InstanceMethod("totalDelta", &FootprintBarWrap::Delta),
                        InstanceMethod("totalVolume", &FootprintBarWrap::Volume),
                        InstanceAccessor("numLevels", &FootprintBarWrap::Levels, nullptr),
                        InstanceMethod("clear", &FootprintBarWrap::Clear)});
  }
  FootprintBarWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<FootprintBarWrap>(info),
                                                     _h(lrvx_footprint_create(argNumber(info[0], "FootprintBar: argument 1"))) {}
  ~FootprintBarWrap()
  {
    if (_h)
    {
      lrvx_footprint_destroy(_h);
    }
  }

 private:
  void AddTrade(const Napi::CallbackInfo& info) { lrvx_footprint_add_trade(_h, argNumber(info[0], "addTrade: argument 1"), argNumber(info[1], "addTrade: argument 2"), argBool(info[2], "addTrade: argument 3") ? 1 : 0); }
  Napi::Value Delta(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), lrvx_footprint_total_delta(_h)); }
  Napi::Value Volume(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), lrvx_footprint_total_volume(_h)); }
  Napi::Value Levels(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), lrvx_footprint_num_levels(_h)); }
  void Clear(const Napi::CallbackInfo&) { lrvx_footprint_clear(_h); }
  LrvxFootprintHandle _h;
};

inline void registerProfiles(Napi::Env env, Napi::Object exports)
{
  exports.Set("VolumeProfile", VolumeProfileWrap::Init(env));
  exports.Set("MarketProfile", MarketProfileWrap::Init(env));
  exports.Set("FootprintBar", FootprintBarWrap::Init(env));
}

}  // namespace node_lrvx
