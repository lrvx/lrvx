// node/src/walk_forward.h — WalkForwardRunner for Node.js

#pragma once

#include <napi.h>

#include "bindings_common.h"
#include "error_translator.h"
#include "hooks.h"
#include "lrvx/capi/bridge_strategy.h"
#include "lrvx/capi/lrvx_capi.h"
#include "lrvx/engine/symbol_registry.h"
#include "strategy.h"

#include <memory>
#include <string>
#include <vector>

namespace node_lrvx
{

// One walk-forward run keeps a vector of strategy hosts alive — each
// fold uses a fresh strategy, so the engine calls the JS factory
// twice per fold (train + test), and we need the hosts not to be
// destroyed until run() returns.
struct WalkForwardCallContext
{
  Napi::Env env;
  Napi::FunctionReference factory_fn;
  lrvx::SymbolRegistry* reg;
  std::vector<std::unique_ptr<NodeStrategyHost>> hosts;
  uint32_t next_id{1};
  // What the factory threw, or what was wrong with the strategy it returned.
  // The run unwinds through the C API, which keeps only a message, so runCsv
  // rethrows this one to keep its class.
  Napi::ObjectReference error;
};

inline LrvxStrategyHandle walkForwardFactoryThunk(void* user_data,
                                                  uint64_t fold_index)
{
  auto* ctx = static_cast<WalkForwardCallContext*>(user_data);
  Napi::HandleScope scope(ctx->env);
  try
  {
    auto val = ctx->factory_fn.Call(
        {Napi::Number::New(ctx->env, static_cast<double>(fold_index))});
    auto obj = argObject(val, "setStrategyFactory: the strategy the factory returns");
    auto host = std::make_unique<NodeStrategyHost>(
        ctx->env, obj, ctx->reg, ctx->next_id++,
        strategySymbols(obj, "setStrategyFactory: symbols"));
    LrvxStrategyHandle handle =
        static_cast<LrvxStrategyHandle>(host->bridge.get());
    ctx->hosts.push_back(std::move(host));
    return handle;
  }
  catch (const Napi::Error& e)
  {
    if (ctx->error.IsEmpty())
    {
      ctx->error = Napi::Persistent(e.Value());
    }
    throw;
  }
}

inline Napi::Object statsToJsObj(Napi::Env env, const LrvxBacktestStats& s)
{
  auto obj = Napi::Object::New(env);
  obj.Set("totalTrades", Napi::Number::New(env, static_cast<double>(s.totalTrades)));
  obj.Set("winningTrades", Napi::Number::New(env, static_cast<double>(s.winningTrades)));
  obj.Set("losingTrades", Napi::Number::New(env, static_cast<double>(s.losingTrades)));
  obj.Set("initialCapital", Napi::Number::New(env, s.initialCapital));
  obj.Set("finalCapital", Napi::Number::New(env, s.finalCapital));
  obj.Set("netPnl", Napi::Number::New(env, s.netPnl));
  obj.Set("totalFees", Napi::Number::New(env, s.totalFees));
  obj.Set("returnPct", Napi::Number::New(env, s.returnPct));
  obj.Set("maxDrawdown", Napi::Number::New(env, s.maxDrawdown));
  obj.Set("maxDrawdownPct", Napi::Number::New(env, s.maxDrawdownPct));
  obj.Set("sharpeRatio", Napi::Number::New(env, s.sharpeRatio));
  obj.Set("sortinoRatio", Napi::Number::New(env, s.sortinoRatio));
  obj.Set("calmarRatio", Napi::Number::New(env, s.calmarRatio));
  obj.Set("winRate", Napi::Number::New(env, s.winRate));
  obj.Set("profitFactor", Napi::Number::New(env, s.profitFactor));
  setTradeTimeStats(env, obj, s);
  return obj;
}

class WalkForwardRunnerNode
    : public Napi::ObjectWrap<WalkForwardRunnerNode>
{
 public:
  static Napi::Object Init(Napi::Env env)
  {
    return DefineClass(
        env, "WalkForwardRunner",
        {
            InstanceMethod("setStrategyFactory",
                           &WalkForwardRunnerNode::setStrategyFactory),
            InstanceMethod("runCsv", &WalkForwardRunnerNode::runCsv),
        });
  }

  // new WalkForwardRunner(registry, feeRate, initialCapital, config)
  WalkForwardRunnerNode(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<WalkForwardRunnerNode>(info)
  {
    _reg = SymbolRegistryNode::of(info[0], "WalkForwardRunner: registry");
    _fee = argNumber(info[1], "WalkForwardRunner: argument 2");
    _cap = argNumber(info[2], "WalkForwardRunner: argument 3");
    auto cfg = argObject(info[3], "WalkForwardRunner: argument 4");
    std::string mode = given(cfg.Get("mode"))
                           ? argString(cfg.Get("mode"), "WalkForwardRunner: mode")
                           : "anchored";
    if (mode != "anchored" && mode != "sliding")
    {
      throw Napi::TypeError::New(info.Env(),
                                 "WalkForwardRunner: mode must be 'anchored' or 'sliding'; got '" +
                                     mode + "'");
    }
    _cfg.mode = (mode == "sliding") ? 1 : 0;
    const auto count = [&](const char* key, const char* what)
    { return given(cfg.Get(key)) ? argInteger(cfg.Get(key), what) : 0; };
    _cfg.train_size = count("trainSize", "WalkForwardRunner: trainSize");
    _cfg.test_size = argInteger(cfg.Get("testSize"), "WalkForwardRunner: testSize");
    _cfg.step = count("step", "WalkForwardRunner: step");
    _cfg.min_train_size = count("minTrainSize", "WalkForwardRunner: minTrainSize");
  }

 private:
  lrvx::SymbolRegistry* _reg{nullptr};
  double _fee{0.0004};
  double _cap{10000.0};
  LrvxWalkForwardConfig _cfg{};
  Napi::FunctionReference _factory;

  Napi::Value setStrategyFactory(const Napi::CallbackInfo& info)
  {
    _factory = Napi::Persistent(argFunction(info[0], "setStrategyFactory: argument 1"));
    return info.Env().Undefined();
  }

  Napi::Value runCsv(const Napi::CallbackInfo& info)
  {
    return tryLrvx(info.Env(), [&]() -> Napi::Value
                   {
      auto env = info.Env();
      if (_factory.IsEmpty())
      {
        throw lrvx::LrvxError(
            "E_RUN_001",
            "WalkForwardRunner.runCsv called before setStrategyFactory.");
      }
      std::string path = argString(info[0], "runCsv: argument 1");
      std::string symbol = argString(info[1], "runCsv: argument 2");

      WalkForwardCallContext ctx{env,
                                 Napi::FunctionReference(),
                                 _reg,
                                 {},
                                 1};
      ctx.factory_fn = Napi::Persistent(_factory.Value());

      // First call with NULL out_buf to get the count. Zero is also what a
      // file too short for one fold gives, so the error is what tells an
      // unreadable file or an unknown symbol from that.
      lrvx_clear_last_error();
      uint32_t total = lrvx_walk_forward_run_csv(
          static_cast<LrvxRegistryHandle>(_reg), path.c_str(),
          symbol.c_str(), _fee, _cap, &_cfg,
          &walkForwardFactoryThunk, &ctx, nullptr, 0);
      if (total == 0 && lrvx_last_error_code() != 0)
      {
        throw lrvx::LrvxError("E_VAL_002", "WalkForwardRunner.runCsv: " + capiReason());
      }
      std::vector<LrvxWalkForwardFold> folds(total);
      if (total > 0)
      {
        lrvx_clear_last_error();
        const uint32_t ran = lrvx_walk_forward_run_csv(
            static_cast<LrvxRegistryHandle>(_reg), path.c_str(),
            symbol.c_str(), _fee, _cap, &_cfg,
            &walkForwardFactoryThunk, &ctx, folds.data(), total);
        // A run that failed part way used to hand back `total` zeroed folds.
        if (!ctx.error.IsEmpty())
        {
          throw Napi::Error(env, ctx.error.Value());
        }
        if (ran == 0 && lrvx_last_error_code() != 0)
        {
          throw lrvx::LrvxError("E_VAL_002", "WalkForwardRunner.runCsv: " + capiReason());
        }
      }
      auto arr = Napi::Array::New(env, total);
      for (uint32_t i = 0; i < total; ++i)
      {
        const auto& f = folds[i];
        auto o = Napi::Object::New(env);
        o.Set("foldIndex", Napi::Number::New(env, static_cast<double>(f.fold_index)));
        o.Set("trainStartBar", Napi::Number::New(env, static_cast<double>(f.train_start_bar)));
        o.Set("trainEndBar", Napi::Number::New(env, static_cast<double>(f.train_end_bar)));
        o.Set("testStartBar", Napi::Number::New(env, static_cast<double>(f.test_start_bar)));
        o.Set("testEndBar", Napi::Number::New(env, static_cast<double>(f.test_end_bar)));
        o.Set("trainStartNs", Napi::BigInt::New(env, f.train_start_ns));
        o.Set("trainEndNs", Napi::BigInt::New(env, f.train_end_ns));
        o.Set("testStartNs", Napi::BigInt::New(env, f.test_start_ns));
        o.Set("testEndNs", Napi::BigInt::New(env, f.test_end_ns));
        o.Set("trainStats", statsToJsObj(env, f.train_stats));
        o.Set("testStats", statsToJsObj(env, f.test_stats));
        arr[i] = o;
      }
      return arr; });
  }
};

}  // namespace node_lrvx
