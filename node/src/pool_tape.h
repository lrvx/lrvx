// node/src/pool_tape.h -- replay a recorded DEX pool history for Node.js.
//
// A pool-state tape is a delta log; the pool state is derived by replaying the deltas
// through the exact curve. Build a tape (an opaque handle), then replay it: the result
// exposes the drift count, the trade count, and the final curve (an AmmCurve handle).
// Amounts are native BigInt. Everything runs through the C ABI; this is marshalling.

#pragma once

#include <napi.h>

#include "bindings_common.h"

#include "amm_curve.h"
#include "dex_amount.h"
#include "lrvx/capi/lrvx_capi.h"

#include <cstdint>
#include <string>
#include <vector>

namespace node_lrvx
{

namespace detail_tape
{
inline Napi::Value wrapTape(Napi::Env env, LrvxPoolTapeHandle h)
{
  return Napi::External<void>::New(env, h, [](Napi::Env, void* d)
                                   { lrvx_pool_tape_destroy(d); });
}
inline LrvxPoolTapeHandle tapeOf(const Napi::Value& v) { return v.As<Napi::External<void>>().Data(); }
inline Napi::Value wrapReplay(Napi::Env env, LrvxPoolReplayHandle h)
{
  return Napi::External<void>::New(env, h, [](Napi::Env, void* d)
                                   { lrvx_pool_replay_destroy(d); });
}
inline LrvxPoolReplayHandle replayOf(const Napi::Value& v)
{
  return v.As<Napi::External<void>>().Data();
}
}  // namespace detail_tape

inline Napi::Value poolTapeCreate(const Napi::CallbackInfo& info)
{
  return detail_tape::wrapTape(info.Env(), lrvx_pool_tape_create());
}

inline Napi::Value poolTapeDescriptorConstantProduct(const Napi::CallbackInfo& info)
{
  lrvx_pool_tape_descriptor_constant_product(
      detail_tape::tapeOf(info[0]), argInteger(info[1], "poolTapeDescriptorConstantProduct: feeNum"),
      argInteger(info[2], "poolTapeDescriptorConstantProduct: feeDen"), argUint32(info[3], "poolTapeDescriptorConstantProduct: baseDec"),
      argUint32(info[4], "poolTapeDescriptorConstantProduct: quoteDec"));
  return info.Env().Undefined();
}

inline Napi::Value poolTapeDescriptorRaydiumCp(const Napi::CallbackInfo& info)
{
  lrvx_pool_tape_descriptor_raydium_cp(
      detail_tape::tapeOf(info[0]), argInteger(info[1], "poolTapeDescriptorRaydiumCp: tradeFeeRate"),
      argInteger(info[2], "poolTapeDescriptorRaydiumCp: creatorFeeRate"), argBool(info[3], "poolTapeDescriptorRaydiumCp: creatorFeeOnInput") ? 1 : 0,
      argUint32(info[4], "poolTapeDescriptorRaydiumCp: baseDec"), argUint32(info[5], "poolTapeDescriptorRaydiumCp: quoteDec"));
  return info.Env().Undefined();
}

inline Napi::Value poolTapeDescriptorClmm(const Napi::CallbackInfo& info)
{
  lrvx_pool_tape_descriptor_clmm(
      detail_tape::tapeOf(info[0]), argUint32(info[1], "poolTapeDescriptorClmm: venue"),
      argUint32(info[2], "poolTapeDescriptorClmm: feePips"), argUint32(info[3], "poolTapeDescriptorClmm: baseDec"),
      argUint32(info[4], "poolTapeDescriptorClmm: quoteDec"));
  return info.Env().Undefined();
}

inline Napi::Value poolTapeCheckpoint(const Napi::CallbackInfo& info)
{
  Napi::Env env = info.Env();
  const std::string r0 = detail_curve::bigintToDecimal(env, argBigInt(info[2], "poolTapeCheckpoint: reserve0"));
  const std::string r1 = detail_curve::bigintToDecimal(env, argBigInt(info[3], "poolTapeCheckpoint: reserve1"));
  if (!lrvx_pool_tape_checkpoint(detail_tape::tapeOf(info[0]),
                                 toInt64Ns(info[1], "poolTapeCheckpoint: tsNs"), r0.c_str(), r1.c_str()))
  {
    throw Napi::Error::New(env, "poolTapeCheckpoint: bad reserve");
  }
  return env.Undefined();
}

inline Napi::Value poolTapeSwap(const Napi::CallbackInfo& info)
{
  Napi::Env env = info.Env();
  const std::string amt = detail_curve::bigintToDecimal(env, argBigInt(info[3], "poolTapeSwap: amountIn"));
  if (!lrvx_pool_tape_swap(detail_tape::tapeOf(info[0]), toInt64Ns(info[1], "poolTapeSwap: tsNs"),
                           argBool(info[2], "poolTapeSwap: baseForQuote") ? 1 : 0, amt.c_str()))
  {
    throw Napi::Error::New(env, "poolTapeSwap: bad amount");
  }
  return env.Undefined();
}

inline Napi::Value poolTapeReplay(const Napi::CallbackInfo& info)
{
  Napi::Env env = info.Env();
  LrvxPoolReplayHandle r = lrvx_pool_tape_replay(
      detail_tape::tapeOf(info[0]), argUint32(info[1], "poolTapeReplay: baseIdx"),
      argUint32(info[2], "poolTapeReplay: quoteIdx"), argUint32(info[3], "poolTapeReplay: baseDec"),
      argUint32(info[4], "poolTapeReplay: quoteDec"));
  return detail_tape::wrapReplay(env, r);
}

inline Napi::Value poolReplayDriftCount(const Napi::CallbackInfo& info)
{
  return Napi::Number::New(
      info.Env(), static_cast<double>(lrvx_pool_replay_drift_count(detail_tape::replayOf(info[0]))));
}

inline Napi::Value poolReplayTradeCount(const Napi::CallbackInfo& info)
{
  return Napi::Number::New(
      info.Env(), static_cast<double>(lrvx_pool_replay_trade_count(detail_tape::replayOf(info[0]))));
}

// The final curve, cloned into an owned AmmCurve handle (its own destroy frees it),
// so it outlives the replay result.
inline Napi::Value poolReplayCurve(const Napi::CallbackInfo& info)
{
  Napi::Env env = info.Env();
  LrvxCurveHandle c = lrvx_pool_replay_curve(detail_tape::replayOf(info[0]));
  if (c == nullptr)
  {
    throw Napi::Error::New(env, "poolReplayCurve: the tape had no checkpoint");
  }
  return detail_curve::wrapHandle(env, lrvx_curve_clone(c));
}

inline void registerPoolTape(Napi::Env env, Napi::Object exports)
{
  exports.Set("poolTapeCreate", Napi::Function::New(env, &poolTapeCreate, "poolTapeCreate"));
  exports.Set("poolTapeDescriptorConstantProduct",
              Napi::Function::New(env, &poolTapeDescriptorConstantProduct,
                                  "poolTapeDescriptorConstantProduct"));
  exports.Set("poolTapeDescriptorRaydiumCp",
              Napi::Function::New(env, &poolTapeDescriptorRaydiumCp, "poolTapeDescriptorRaydiumCp"));
  exports.Set("poolTapeDescriptorClmm",
              Napi::Function::New(env, &poolTapeDescriptorClmm, "poolTapeDescriptorClmm"));
  exports.Set("poolTapeCheckpoint",
              Napi::Function::New(env, &poolTapeCheckpoint, "poolTapeCheckpoint"));
  exports.Set("poolTapeSwap", Napi::Function::New(env, &poolTapeSwap, "poolTapeSwap"));
  exports.Set("poolTapeReplay", Napi::Function::New(env, &poolTapeReplay, "poolTapeReplay"));
  exports.Set("poolReplayDriftCount",
              Napi::Function::New(env, &poolReplayDriftCount, "poolReplayDriftCount"));
  exports.Set("poolReplayTradeCount",
              Napi::Function::New(env, &poolReplayTradeCount, "poolReplayTradeCount"));
  exports.Set("poolReplayCurve", Napi::Function::New(env, &poolReplayCurve, "poolReplayCurve"));
}

}  // namespace node_lrvx
