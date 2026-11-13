// node/src/rate_limit.h — Client-side RateLimitPolicy wrap.
//
// Thin NAPI wrap over the C ABI lrvx_rate_limit_policy_* surface.
// Build a policy with buckets + ban rule, then attach to a
// SimulatedExecutor via executor.setRateLimitPolicy(policy).

#pragma once

#include <napi.h>

#include <vector>

#include "bindings_common.h"
#include "lrvx/capi/lrvx_capi.h"

namespace node_lrvx
{

class RateLimitPolicyWrap : public Napi::ObjectWrap<RateLimitPolicyWrap>
{
 public:
  static Napi::FunctionReference& Ctor(Napi::Env env) { return ctorOf<RateLimitPolicyWrap>(env); }

  static Napi::Function Init(Napi::Env env)
  {
    return keepCtor(Ctor(env), DefineClass(
                                   env, "RateLimitPolicy",
                                   {InstanceMethod("addBucket", &RateLimitPolicyWrap::AddBucket),
                                    InstanceMethod("addFamilyBucket", &RateLimitPolicyWrap::AddFamilyBucket),
                                    InstanceMethod("setBan", &RateLimitPolicyWrap::SetBan),
                                    InstanceMethod("loadProfile", &RateLimitPolicyWrap::LoadProfile),
                                    InstanceMethod("banUntilNs", &RateLimitPolicyWrap::BanUntilNs),
                                    InstanceMethod("consecutiveRejects",
                                                   &RateLimitPolicyWrap::ConsecutiveRejects),
                                    InstanceMethod("bucketStates", &RateLimitPolicyWrap::BucketStates)}));
  }

  RateLimitPolicyWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<RateLimitPolicyWrap>(info),
        _h(lrvx_rate_limit_policy_create())
  {
  }
  ~RateLimitPolicyWrap()
  {
    if (_h)
    {
      lrvx_rate_limit_policy_destroy(_h);
    }
  }
  LrvxRateLimitPolicyHandle handle() const { return _h; }

 private:
  void AddBucket(const Napi::CallbackInfo& info)
  {
    std::string name = argString(info[0], "addBucket: name");
    int64_t window_ns = argInteger(info[1], "addBucket: windowNs");
    uint32_t capacity = argUint32(info[2], "addBucket: capacity");
    uint32_t sw = info.Length() > 3 ? argUint32(info[3], "addBucket: submitWeight") : 1;
    uint32_t cw = info.Length() > 4 ? argUint32(info[4], "addBucket: cancelWeight") : 1;
    uint32_t rw = info.Length() > 5 ? argUint32(info[5], "addBucket: replaceWeight") : 2;
    lrvx_rate_limit_policy_add_bucket(_h, name.c_str(), window_ns, capacity, sw, cw, rw);
  }
  void AddFamilyBucket(const Napi::CallbackInfo& info)
  {
    // (family, name, windowNs, capacity, queryWeight=1)
    const uint8_t family = static_cast<uint8_t>(argNamedCode(
        info[0], "addFamilyBucket: family", {"trading", "market_data", "account"}));
    std::string name = argString(info[1], "addFamilyBucket: name");
    int64_t window_ns = argInteger(info[2], "addFamilyBucket: windowNs");
    uint32_t capacity = argUint32(info[3], "addFamilyBucket: capacity");
    uint32_t qw = info.Length() > 4 ? argUint32(info[4], "addFamilyBucket: queryWeight") : 1;
    lrvx_rate_limit_policy_add_bucket_family(_h, name.c_str(), window_ns, capacity,
                                             1, 1, 2, family, qw);
  }
  void SetBan(const Napi::CallbackInfo& info)
  {
    lrvx_rate_limit_policy_set_ban(_h, argUint32(info[0], "setBan: afterConsecutiveRejects"),
                                   argInteger(info[1], "setBan: banDurationNs"));
  }
  void LoadProfile(const Napi::CallbackInfo& info)
  {
    std::string name = argString(info[0], "loadProfile: name");
    // A typo used to be a silent no-op, leaving default rate limit policy
    // values in place -- a zero-fee or unthrottled backtest that
    // looks perfectly healthy.
    if (lrvx_rate_limit_policy_load_profile(_h, name.c_str()) == 0)
    {
      Napi::Error::New(info.Env(), "unknown rate limit policy profile: " + name +
                                       ". known: binance_um_futures, bybit_linear, okx_swap, deribit")
          .ThrowAsJavaScriptException();
    }
  }
  Napi::Value BanUntilNs(const Napi::CallbackInfo& info)
  {
    return Napi::BigInt::New(info.Env(), lrvx_rate_limit_policy_ban_until_ns(_h));
  }
  Napi::Value ConsecutiveRejects(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(),
                             lrvx_rate_limit_policy_consecutive_rejects(_h));
  }
  Napi::Value BucketStates(const Napi::CallbackInfo& info)
  {
    int64_t now_ns = toInt64Ns(info[0], "bucketStates: nowNs");
    uint32_t count = lrvx_rate_limit_policy_bucket_state(_h, now_ns, nullptr, 0);
    std::vector<int64_t> buf(count * 4, 0);
    lrvx_rate_limit_policy_bucket_state(_h, now_ns, buf.data(), count);
    Napi::Array out = Napi::Array::New(info.Env(), count);
    for (uint32_t i = 0; i < count; ++i)
    {
      Napi::Object o = Napi::Object::New(info.Env());
      o.Set("windowNs", Napi::Number::New(info.Env(), static_cast<double>(buf[i * 4 + 0])));
      o.Set("used", Napi::Number::New(info.Env(), static_cast<double>(buf[i * 4 + 1])));
      o.Set("capacity",
            Napi::Number::New(info.Env(), static_cast<double>(buf[i * 4 + 2])));
      out.Set(i, o);
    }
    return out;
  }

  LrvxRateLimitPolicyHandle _h;
};

inline void registerRateLimitPolicy(Napi::Env env, Napi::Object exports)
{
  exports.Set("RateLimitPolicy", RateLimitPolicyWrap::Init(env));
}

}  // namespace node_lrvx
