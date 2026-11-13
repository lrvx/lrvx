// node/src/graph.h -- IndicatorGraph (batch) wrapper.

#pragma once
#include <napi.h>

#include <cstring>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "lrvx/aggregator/bar.h"
#include "lrvx/common.h"
#include "lrvx/indicator/indicator_pipeline.h"
#include "lrvx/indicator/streaming_graph.h"

namespace node_lrvx
{

class IndicatorGraphWrap : public Napi::ObjectWrap<IndicatorGraphWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "IndicatorGraph",
                       {InstanceMethod("setBars", &IndicatorGraphWrap::SetBars),
                        InstanceMethod("addNode", &IndicatorGraphWrap::AddNode),
                        InstanceMethod("require", &IndicatorGraphWrap::Require),
                        InstanceMethod("get", &IndicatorGraphWrap::Get),
                        InstanceMethod("close", &IndicatorGraphWrap::Close),
                        InstanceMethod("high", &IndicatorGraphWrap::High),
                        InstanceMethod("low", &IndicatorGraphWrap::Low),
                        InstanceMethod("volume", &IndicatorGraphWrap::Volume),
                        InstanceMethod("invalidate", &IndicatorGraphWrap::Invalidate),
                        InstanceMethod("invalidateAll", &IndicatorGraphWrap::InvalidateAll)});
  }

  IndicatorGraphWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<IndicatorGraphWrap>(info), _self(info.This().As<Napi::Object>().Get("__handle__"))
  {
    // _self is unused; we store info.This() via a persistent reference inside addNode.
  }

 private:
  Napi::Value vec2arr(Napi::Env env, const std::vector<double>& v)
  {
    auto a = Napi::Float64Array::New(env, v.size());
    std::memcpy(a.Data(), v.data(), v.size() * sizeof(double));
    return a;
  }

  Napi::Value SetBars(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    uint32_t symbol = argUint32(info[0], "setBars: symbol");
    auto close = argFloat64Array(info[1], "setBars: close");
    size_t n = close.ElementLength();

    auto fetch = [&](size_t idx) -> const double*
    {
      if (info.Length() <= idx || info[idx].IsUndefined() || info[idx].IsNull())
      {
        return nullptr;
      }
      auto a = argFloat64Array(info[idx], "setBars: a price or volume column");
      if (a.ElementLength() != n)
      {
        Napi::TypeError::New(env, "setBars: arrays must match close length")
            .ThrowAsJavaScriptException();
        return nullptr;
      }
      return a.Data();
    };

    const double* h = fetch(2);
    const double* l = fetch(3);
    const double* v = fetch(4);
    const double* c = close.Data();

    std::vector<lrvx::Bar> bars(n);
    for (size_t i = 0; i < n; ++i)
    {
      bars[i].open = lrvx::Price::fromDouble(c[i]);
      bars[i].high = lrvx::Price::fromDouble(h ? h[i] : c[i]);
      bars[i].low = lrvx::Price::fromDouble(l ? l[i] : c[i]);
      bars[i].close = lrvx::Price::fromDouble(c[i]);
      bars[i].volume = v ? lrvx::Volume::fromDouble(v[i]) : lrvx::Volume{};
    }
    _bars[symbol] = std::move(bars);
    _graph.setBars(static_cast<lrvx::SymbolId>(symbol),
                   std::span<const lrvx::Bar>(_bars[symbol]));
    return env.Undefined();
  }

  Napi::Value AddNode(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    std::string name = argString(info[0], "addNode: name");

    auto depsArr = argArray(info[1], "addNode: dependencies");
    std::vector<std::string> deps;
    deps.reserve(depsArr.Length());
    for (uint32_t i = 0; i < depsArr.Length(); ++i)
    {
      deps.push_back(argString(depsArr.Get(i), "addNode: a dependency name"));
    }

    auto fnRef = std::make_shared<Napi::FunctionReference>(
        Napi::Persistent(argFunction(info[2], "addNode: compute")));
    auto thisRef = std::make_shared<Napi::ObjectReference>(Napi::Persistent(info.This().As<Napi::Object>()));
    Napi::Env fnEnv = env;

    _graph.addNode(name, std::move(deps),
                   [fnRef, thisRef, fnEnv](lrvx::indicator::IndicatorGraph&,
                                           lrvx::SymbolId sym) -> std::vector<double>
                   {
                     Napi::HandleScope scope(fnEnv);
                     Napi::Value result = fnRef->Call(
                         {thisRef->Value(),
                          Napi::Number::New(fnEnv, static_cast<uint32_t>(sym))});
                     auto arr = result.As<Napi::Float64Array>();
                     return std::vector<double>(arr.Data(), arr.Data() + arr.ElementLength());
                   });
    return env.Undefined();
  }

  Napi::Value Require(const Napi::CallbackInfo& info)
  {
    uint32_t symbol = argUint32(info[0], "require: symbol");
    std::string name = argString(info[1], "require: name");
    try
    {
      const auto& v = _graph.require(static_cast<lrvx::SymbolId>(symbol), name);
      return vec2arr(info.Env(), v);
    }
    catch (const std::exception& e)
    {
      Napi::Error::New(info.Env(), e.what()).ThrowAsJavaScriptException();
      return info.Env().Undefined();
    }
  }

  Napi::Value Get(const Napi::CallbackInfo& info)
  {
    uint32_t symbol = argUint32(info[0], "get: symbol");
    std::string name = argString(info[1], "get: name");
    const auto* v = _graph.get(static_cast<lrvx::SymbolId>(symbol), name);
    if (!v)
    {
      return info.Env().Null();
    }
    return vec2arr(info.Env(), *v);
  }

  Napi::Value Close(const Napi::CallbackInfo& info)
  {
    uint32_t s = argUint32(info[0], "close: symbol");
    return vec2arr(info.Env(), _graph.close(static_cast<lrvx::SymbolId>(s)));
  }
  Napi::Value High(const Napi::CallbackInfo& info)
  {
    uint32_t s = argUint32(info[0], "high: symbol");
    return vec2arr(info.Env(), _graph.high(static_cast<lrvx::SymbolId>(s)));
  }
  Napi::Value Low(const Napi::CallbackInfo& info)
  {
    uint32_t s = argUint32(info[0], "low: symbol");
    return vec2arr(info.Env(), _graph.low(static_cast<lrvx::SymbolId>(s)));
  }
  Napi::Value Volume(const Napi::CallbackInfo& info)
  {
    uint32_t s = argUint32(info[0], "volume: symbol");
    return vec2arr(info.Env(), _graph.volume(static_cast<lrvx::SymbolId>(s)));
  }
  Napi::Value Invalidate(const Napi::CallbackInfo& info)
  {
    _graph.invalidate(static_cast<lrvx::SymbolId>(argUint32(info[0], "invalidate: symbol")));
    return info.Env().Undefined();
  }
  Napi::Value InvalidateAll(const Napi::CallbackInfo& info)
  {
    _graph.invalidateAll();
    return info.Env().Undefined();
  }

  Napi::Value _self;  // unused, kept to silence -Wunused-private-field issues
  lrvx::indicator::IndicatorGraph _graph;
  std::unordered_map<uint32_t, std::vector<lrvx::Bar>> _bars;
};

class StreamingIndicatorGraphWrap : public Napi::ObjectWrap<StreamingIndicatorGraphWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(
        env, "StreamingIndicatorGraph",
        {InstanceMethod("addNode", &StreamingIndicatorGraphWrap::AddNode),
         InstanceMethod("step", &StreamingIndicatorGraphWrap::Step),
         InstanceMethod("current", &StreamingIndicatorGraphWrap::Current),
         InstanceMethod("barCount", &StreamingIndicatorGraphWrap::BarCount),
         InstanceMethod("reset", &StreamingIndicatorGraphWrap::Reset),
         InstanceMethod("resetAll", &StreamingIndicatorGraphWrap::ResetAll),
         InstanceMethod("close", &StreamingIndicatorGraphWrap::Close),
         InstanceMethod("high", &StreamingIndicatorGraphWrap::High),
         InstanceMethod("low", &StreamingIndicatorGraphWrap::Low),
         InstanceMethod("volume", &StreamingIndicatorGraphWrap::Volume)});
  }

  StreamingIndicatorGraphWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<StreamingIndicatorGraphWrap>(info)
  {
  }

 private:
  Napi::Value vec2arr(Napi::Env env, const std::vector<double>& v)
  {
    auto a = Napi::Float64Array::New(env, v.size());
    std::memcpy(a.Data(), v.data(), v.size() * sizeof(double));
    return a;
  }

  Napi::Value AddNode(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    std::string name = argString(info[0], "addNode: name");

    auto depsArr = argArray(info[1], "addNode: dependencies");
    std::vector<std::string> deps;
    deps.reserve(depsArr.Length());
    for (uint32_t i = 0; i < depsArr.Length(); ++i)
    {
      deps.push_back(argString(depsArr.Get(i), "addNode: a dependency name"));
    }

    auto fnRef = std::make_shared<Napi::FunctionReference>(
        Napi::Persistent(argFunction(info[2], "addNode: compute")));
    auto thisRef = std::make_shared<Napi::ObjectReference>(
        Napi::Persistent(info.This().As<Napi::Object>()));
    Napi::Env fnEnv = env;

    _sg.addNode(name, std::move(deps),
                [fnRef, thisRef, fnEnv](lrvx::indicator::IndicatorGraph&,
                                        lrvx::SymbolId sym) -> std::vector<double>
                {
                  Napi::HandleScope scope(fnEnv);
                  Napi::Value result = fnRef->Call(
                      {thisRef->Value(),
                       Napi::Number::New(fnEnv, static_cast<uint32_t>(sym))});
                  auto arr = result.As<Napi::Float64Array>();
                  return std::vector<double>(arr.Data(), arr.Data() + arr.ElementLength());
                });
    return env.Undefined();
  }

  Napi::Value Step(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    uint32_t symbol = argUint32(info[0], "step: symbol");
    double c = argNumber(info[1], "step: close");
    double h = info.Length() > 2 && !info[2].IsUndefined() && !info[2].IsNull()
                   ? argNumber(info[2], "step: high")
                   : c;
    double l = info.Length() > 3 && !info[3].IsUndefined() && !info[3].IsNull()
                   ? argNumber(info[3], "step: low")
                   : c;
    double v = info.Length() > 4 && !info[4].IsUndefined() && !info[4].IsNull()
                   ? argNumber(info[4], "step: volume")
                   : 0.0;
    lrvx::Bar bar;
    bar.open = lrvx::Price::fromDouble(c);
    bar.high = lrvx::Price::fromDouble(h);
    bar.low = lrvx::Price::fromDouble(l);
    bar.close = lrvx::Price::fromDouble(c);
    bar.volume = lrvx::Volume::fromDouble(v);
    _sg.step(static_cast<lrvx::SymbolId>(symbol), bar);
    return env.Undefined();
  }

  Napi::Value Current(const Napi::CallbackInfo& info)
  {
    uint32_t symbol = argUint32(info[0], "current: symbol");
    std::string name = argString(info[1], "current: name");
    return Napi::Number::New(info.Env(),
                             _sg.current(static_cast<lrvx::SymbolId>(symbol), name));
  }

  Napi::Value BarCount(const Napi::CallbackInfo& info)
  {
    uint32_t symbol = argUint32(info[0], "barCount: symbol");
    return Napi::Number::New(info.Env(),
                             static_cast<double>(_sg.barCount(static_cast<lrvx::SymbolId>(symbol))));
  }

  Napi::Value Reset(const Napi::CallbackInfo& info)
  {
    _sg.reset(static_cast<lrvx::SymbolId>(argUint32(info[0], "reset: symbol")));
    return info.Env().Undefined();
  }

  Napi::Value ResetAll(const Napi::CallbackInfo& info)
  {
    _sg.resetAll();
    return info.Env().Undefined();
  }

  Napi::Value Close(const Napi::CallbackInfo& info)
  {
    uint32_t s = argUint32(info[0], "close: symbol");
    return vec2arr(info.Env(), _sg.batchGraph().close(static_cast<lrvx::SymbolId>(s)));
  }
  Napi::Value High(const Napi::CallbackInfo& info)
  {
    uint32_t s = argUint32(info[0], "high: symbol");
    return vec2arr(info.Env(), _sg.batchGraph().high(static_cast<lrvx::SymbolId>(s)));
  }
  Napi::Value Low(const Napi::CallbackInfo& info)
  {
    uint32_t s = argUint32(info[0], "low: symbol");
    return vec2arr(info.Env(), _sg.batchGraph().low(static_cast<lrvx::SymbolId>(s)));
  }
  Napi::Value Volume(const Napi::CallbackInfo& info)
  {
    uint32_t s = argUint32(info[0], "volume: symbol");
    return vec2arr(info.Env(), _sg.batchGraph().volume(static_cast<lrvx::SymbolId>(s)));
  }

  lrvx::indicator::StreamingIndicatorGraph _sg;
};

inline void registerGraph(Napi::Env env, Napi::Object exports)
{
  exports.Set("IndicatorGraph", IndicatorGraphWrap::Init(env));
  exports.Set("StreamingIndicatorGraph", StreamingIndicatorGraphWrap::Init(env));
}

}  // namespace node_lrvx
