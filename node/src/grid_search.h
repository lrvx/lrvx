// node/src/grid_search.h — GridSearch type-erased grid for Node.js

#pragma once

#include <napi.h>

#include "bindings_common.h"
#include "error_translator.h"
#include "lrvx/capi/lrvx_capi.h"
#include "walk_forward.h"  // for statsToJsObj

#include <string>
#include <vector>

namespace node_lrvx
{

struct GridSearchCallContext
{
  Napi::Env env;
  Napi::FunctionReference factory_fn;
  std::vector<std::vector<double>> per_call_params;
  // What the factory threw, or what was wrong with the stats it returned.
  // The run unwinds through the C API, which keeps only a message, so run
  // rethrows this one to keep its class.
  Napi::ObjectReference error;
};

// The stats the factory returned, read into the C struct. A key left out
// reads 0; a key of the wrong type used to read 0 as well.
inline void gridSearchReadStats(const Napi::Value& result, LrvxBacktestStats* out_stats)
{
  auto obj = argObject(result, "setFactory: the stats the factory returns");
  auto getNum = [&](const char* key) -> double
  {
    auto v = obj.Get(key);
    return given(v) ? argNumber(v, (std::string("setFactory: the stats' ") + key).c_str())
                    : 0.0;
  };
  out_stats->totalTrades = static_cast<uint64_t>(getNum("totalTrades"));
  out_stats->winningTrades = static_cast<uint64_t>(getNum("winningTrades"));
  out_stats->losingTrades = static_cast<uint64_t>(getNum("losingTrades"));
  out_stats->initialCapital = getNum("initialCapital");
  out_stats->finalCapital = getNum("finalCapital");
  out_stats->totalPnl = getNum("netPnl");
  out_stats->netPnl = out_stats->totalPnl;
  out_stats->totalFees = getNum("totalFees");
  out_stats->returnPct = getNum("returnPct");
  out_stats->maxDrawdown = getNum("maxDrawdown");
  out_stats->maxDrawdownPct = getNum("maxDrawdownPct");
  out_stats->sharpeRatio = getNum("sharpeRatio");
  out_stats->sortinoRatio = getNum("sortinoRatio");
  out_stats->calmarRatio = getNum("calmarRatio");
  out_stats->winRate = getNum("winRate");
  out_stats->profitFactor = getNum("profitFactor");
  out_stats->avgTradeDurationNs = getNum("avgTradeDurationNs");
  out_stats->medianTradeDurationNs = getNum("medianTradeDurationNs");
  out_stats->maxTradeDurationNs = getNum("maxTradeDurationNs");
  out_stats->timeWeightedReturn = getNum("timeWeightedReturn");
  // The run's bounds are clock readings: a runner hands them over as BigInt.
  auto getNs = [&](const char* key) -> int64_t
  {
    auto v = obj.Get(key);
    return given(v) ? toInt64Ns(v, (std::string("setFactory: the stats' ") + key).c_str()) : 0;
  };
  out_stats->startTimeNs = getNs("startTimeNs");
  out_stats->endTimeNs = getNs("endTimeNs");
}

inline int gridSearchFactoryThunk(void* user_data, uint64_t param_index,
                                  const double* params, uint32_t num_params,
                                  LrvxBacktestStats* out_stats)
{
  auto* ctx = static_cast<GridSearchCallContext*>(user_data);
  Napi::HandleScope scope(ctx->env);
  try
  {
    auto arr = Napi::Array::New(ctx->env, num_params);
    for (uint32_t i = 0; i < num_params; ++i)
    {
      arr[i] = Napi::Number::New(ctx->env, params[i]);
    }
    gridSearchReadStats(ctx->factory_fn.Call({arr}), out_stats);
  }
  catch (const Napi::Error& e)
  {
    if (ctx->error.IsEmpty())
    {
      ctx->error = Napi::Persistent(e.Value());
    }
    throw;
  }
  (void)param_index;
  return 1;
}

class GridSearchNode : public Napi::ObjectWrap<GridSearchNode>
{
 public:
  static Napi::Object Init(Napi::Env env)
  {
    return DefineClass(env, "GridSearch",
                       {
                           InstanceMethod("addAxis", &GridSearchNode::addAxis),
                           InstanceMethod("setFactory", &GridSearchNode::setFactory),
                           InstanceMethod("total", &GridSearchNode::total),
                           InstanceMethod("paramsForIndex", &GridSearchNode::paramsForIndex),
                           InstanceMethod("run", &GridSearchNode::run),
                       });
  }

  GridSearchNode(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<GridSearchNode>(info)
  {
    _handle = lrvx_grid_search_create();
  }

  ~GridSearchNode() override
  {
    if (_handle)
    {
      lrvx_grid_search_destroy(_handle);
    }
  }

 private:
  LrvxGridSearchHandle _handle{nullptr};
  Napi::FunctionReference _factory;

  Napi::Value addAxis(const Napi::CallbackInfo& info)
  {
    auto arr = argArray(info[0], "addAxis: values");
    std::vector<double> vals(arr.Length());
    for (uint32_t i = 0; i < arr.Length(); ++i)
    {
      vals[i] = argNumber(arr.Get(i), "addAxis: a value");
    }
    lrvx_grid_search_add_axis(_handle, vals.data(),
                              static_cast<uint32_t>(vals.size()));
    return info.Env().Undefined();
  }

  Napi::Value setFactory(const Napi::CallbackInfo& info)
  {
    _factory = Napi::Persistent(argFunction(info[0], "setFactory: factory"));
    return info.Env().Undefined();
  }

  Napi::Value total(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(),
                             static_cast<double>(lrvx_grid_search_total(_handle)));
  }

  Napi::Value paramsForIndex(const Napi::CallbackInfo& info)
  {
    uint64_t idx = static_cast<uint64_t>(argInteger(info[0], "paramsForIndex: index"));
    uint32_t total = lrvx_grid_search_params_for_index(_handle, idx, nullptr, 0);
    std::vector<double> out(total);
    if (total > 0)
    {
      lrvx_grid_search_params_for_index(_handle, idx, out.data(), total);
    }
    auto arr = Napi::Array::New(info.Env(), total);
    for (uint32_t i = 0; i < total; ++i)
    {
      arr[i] = Napi::Number::New(info.Env(), out[i]);
    }
    return arr;
  }

  Napi::Value run(const Napi::CallbackInfo& info)
  {
    return tryLrvx(info.Env(), [&]() -> Napi::Value
                   {
      auto env = info.Env();
      if (_factory.IsEmpty())
      {
        throw lrvx::LrvxError(
            "E_RUN_001",
            "GridSearch.run called before setFactory.");
      }
      const uint64_t total = lrvx_grid_search_total(_handle);
      if (total == 0)
      {
        throw lrvx::LrvxError(
            "E_RUN_002",
            "GridSearch.run called with no axes / empty axis.");
      }
      GridSearchCallContext ctx{env, Napi::FunctionReference(), {}, {}};
      ctx.factory_fn = Napi::Persistent(_factory.Value());
      std::vector<LrvxBacktestStats> stats(total);
      lrvx_clear_last_error();
      const uint64_t ran = lrvx_grid_search_run(_handle, &gridSearchFactoryThunk, &ctx,
                                                stats.data(), static_cast<uint32_t>(total));
      // A run that failed part way used to hand back zeroed stats for every
      // combination from the failure on.
      if (!ctx.error.IsEmpty())
      {
        throw Napi::Error(env, ctx.error.Value());
      }
      if (ran == 0 && lrvx_last_error_code() != 0)
      {
        throw lrvx::LrvxError("E_VAL_002", "GridSearch.run: " + capiReason());
      }
      auto results = Napi::Array::New(env, total);
      for (uint64_t i = 0; i < total; ++i)
      {
        // Decode params back from index for the result tuple.
        uint32_t np = lrvx_grid_search_params_for_index(_handle, i, nullptr, 0);
        std::vector<double> p(np);
        if (np > 0)
        {
          lrvx_grid_search_params_for_index(_handle, i, p.data(), np);
        }
        auto pArr = Napi::Array::New(env, np);
        for (uint32_t j = 0; j < np; ++j)
        {
          pArr[j] = Napi::Number::New(env, p[j]);
        }
        auto o = Napi::Object::New(env);
        o.Set("index", Napi::Number::New(env, static_cast<double>(i)));
        o.Set("params", pArr);
        o.Set("stats", statsToJsObj(env, stats[i]));
        results[i] = o;
      }
      return results; });
  }
};

}  // namespace node_lrvx
