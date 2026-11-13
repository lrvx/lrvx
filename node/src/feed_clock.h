// node/src/feed_clock.h — Multi-feed clock.
//
// Thin NAPI wrap over the C ABI lrvx_feed_clock_* surface. Policy is
// taken as a string ('WaitForAll' / 'FireOnAny' / 'LeaderFollower');
// snapshot returned as `{ fired, triggeredBy, lastTsNs: Map, stalenessNs: Map }`.

#pragma once

#include <napi.h>

#include "bindings_common.h"
#include "lrvx/capi/lrvx_capi.h"

#include <map>
#include <string>
#include <vector>

namespace node_lrvx
{

class MultiFeedClockWrap : public Napi::ObjectWrap<MultiFeedClockWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "MultiFeedClock",
                       {InstanceMethod("tick", &MultiFeedClockWrap::Tick),
                        InstanceMethod("reset", &MultiFeedClockWrap::Reset),
                        InstanceMethod("symbolCount", &MultiFeedClockWrap::SymbolCount)});
  }

  MultiFeedClockWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<MultiFeedClockWrap>(info)
  {
    if (info.Length() == 0 || !info[0].IsObject())
    {
      Napi::TypeError::New(info.Env(), "MultiFeedClock requires an options object")
          .ThrowAsJavaScriptException();
      return;
    }
    auto opts = argObject(info[0], "MultiFeedClock: opts");
    if (!given(opts.Get("symbols")) || !opts.Get("symbols").IsArray())
    {
      Napi::TypeError::New(info.Env(), "options.symbols must be an array")
          .ThrowAsJavaScriptException();
      return;
    }
    auto syms = argArray(opts.Get("symbols"), "MultiFeedClock: symbols");
    std::vector<uint32_t> symVec;
    symVec.reserve(syms.Length());
    for (uint32_t i = 0; i < syms.Length(); ++i)
    {
      symVec.push_back(argUint32(syms.Get(i), "MultiFeedClock: a symbol id"));
    }

    uint8_t policy = 0;  // WaitForAll
    if (given(opts.Get("policy")))
    {
      auto p = opts.Get("policy");
      if (p.IsString())
      {
        std::string s = p.As<Napi::String>().Utf8Value();
        if (s == "WaitForAll")
        {
          policy = 0;
        }
        else if (s == "FireOnAny")
        {
          policy = 1;
        }
        else if (s == "LeaderFollower")
        {
          policy = 2;
        }
        else
        {
          Napi::TypeError::New(
              info.Env(),
              "Unknown FeedClock policy: '" + s +
                  "'. Use 'WaitForAll' / 'FireOnAny' / 'LeaderFollower'.")
              .ThrowAsJavaScriptException();
          return;
        }
      }
      else
      {
        policy = static_cast<uint8_t>(argUint32(p, "MultiFeedClock: policy"));
      }
    }

    int64_t timeout = given(opts.Get("timeoutMs"))
                          ? argInteger(opts.Get("timeoutMs"), "MultiFeedClock: timeoutMs")
                          : 200;
    uint32_t leader = given(opts.Get("leaderSymbol"))
                          ? argUint32(opts.Get("leaderSymbol"), "MultiFeedClock: leaderSymbol")
                          : 0;
    int64_t budget = given(opts.Get("stalenessBudgetMs"))
                         ? argInteger(opts.Get("stalenessBudgetMs"), "MultiFeedClock: stalenessBudgetMs")
                         : 200;
    _h = lrvx_feed_clock_create(symVec.data(), static_cast<uint32_t>(symVec.size()), policy,
                                timeout, leader, budget);
  }

  ~MultiFeedClockWrap()
  {
    if (_h)
    {
      lrvx_feed_clock_destroy(_h);
      _h = nullptr;
    }
  }

 private:
  LrvxFeedClockHandle _h{nullptr};

  Napi::Value Tick(const Napi::CallbackInfo& info)
  {
    int64_t ts = toInt64Ns(info[0], "tick: tsNs");
    uint32_t sym = argUint32(info[1], "tick: symbol");
    uint8_t fired = lrvx_feed_clock_tick(_h, ts, sym);
    Napi::Object out = Napi::Object::New(info.Env());
    out.Set("fired", Napi::Boolean::New(info.Env(), fired != 0));
    out.Set("triggeredBy",
            Napi::Number::New(info.Env(), lrvx_feed_clock_last_triggered_by(_h)));
    uint32_t n = lrvx_feed_clock_symbol_count(_h);
    Napi::Object lastTs = Napi::Object::New(info.Env());
    Napi::Object stale = Napi::Object::New(info.Env());
    for (uint32_t i = 0; i < n; ++i)
    {
      uint32_t s = lrvx_feed_clock_symbol_at(_h, i);
      std::string key = std::to_string(s);
      // lastTsNs is a clock reading and has to survive at full width; a
      // staleness is a difference of two of them, exact in a double below
      // 104 days, and stays a Number.
      lastTs.Set(key, Napi::BigInt::New(info.Env(), lrvx_feed_clock_last_seen_at(_h, i)));
      stale.Set(key, Napi::Number::New(info.Env(), static_cast<double>(
                                                       lrvx_feed_clock_staleness_at(_h, i))));
    }
    out.Set("lastTsNs", lastTs);
    out.Set("stalenessNs", stale);
    return out;
  }

  void Reset(const Napi::CallbackInfo&) { lrvx_feed_clock_reset(_h); }

  Napi::Value SymbolCount(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), lrvx_feed_clock_symbol_count(_h));
  }
};

inline void registerFeedClock(Napi::Env env, Napi::Object exports)
{
  exports.Set("MultiFeedClock", MultiFeedClockWrap::Init(env));

  Napi::Object policy = Napi::Object::New(env);
  policy.Set("WaitForAll", Napi::String::New(env, "WaitForAll"));
  policy.Set("FireOnAny", Napi::String::New(env, "FireOnAny"));
  policy.Set("LeaderFollower", Napi::String::New(env, "LeaderFollower"));
  policy.Freeze();
  exports.Set("FeedClockPolicy", policy);
}

}  // namespace node_lrvx
