// node/src/amm_curve.h -- exact AMM pool curves (price a DEX swap) for Node.js.
//
// A curve is an opaque handle (a Napi::External over the C ABI's LrvxCurveHandle,
// with a finalizer that frees it). Amounts are native BigInt -- lossless 256-bit --
// crossing through the dex_amount limb helpers. Construct per venue with the pool's
// parameters, then price a swap exactly:
//
//   const pool = lrvx.ammCurveConstantProduct(r0, r1, 997, 1000);
//   const out  = lrvx.ammCurveAmountOut(pool, 0, 1, 10n ** 18n);  // to the wei
//
// ammCurveAmountOut does not move the pool; ammCurveApplySwap moves it.

#pragma once

#include <napi.h>

#include "dex_amount.h"
#include "lrvx/capi/lrvx_capi.h"

#include <cstdint>
#include <string>
#include <vector>

namespace node_lrvx
{

namespace detail_curve
{

inline std::string bigintToDecimal(Napi::Env env, const Napi::BigInt& b)
{
  uint64_t words[4];
  int sign = 0;
  if (!detail_dex::bigintToWords(b, words, &sign) || sign != 0)
  {
    throw Napi::RangeError::New(env, "amm curve: amount must be in [0, 2^256)");
  }
  char dec[96] = {0};
  if (!lrvx_u256_from_words(words, dec, sizeof(dec)))
  {
    throw Napi::Error::New(env, "amm curve: amount conversion failed");
  }
  return dec;
}

inline Napi::BigInt decimalToBigint(Napi::Env env, const char* dec)
{
  uint64_t words[4];
  if (!lrvx_u256_to_words(dec, words))
  {
    throw Napi::Error::New(env, "amm curve: result conversion failed");
  }
  return detail_dex::wordsToBigInt(env, 0, words);
}

inline Napi::Value wrapHandle(Napi::Env env, LrvxCurveHandle h)
{
  if (h == nullptr)
  {
    throw Napi::Error::New(env, "amm curve: construction failed (bad parameter)");
  }
  return Napi::External<void>::New(env, h, [](Napi::Env, void* data)
                                   { lrvx_curve_destroy(data); });
}

inline LrvxCurveHandle unwrapHandle(const Napi::Value& v)
{
  return v.As<Napi::External<void>>().Data();
}

inline std::string opOut(Napi::Env env,
                         uint8_t (*op)(LrvxCurveHandle, size_t, size_t, const char*, char*, size_t),
                         LrvxCurveHandle c, size_t i, size_t j, const std::string& amountIn)
{
  char out[96] = {0};
  if (!op(c, i, j, amountIn.c_str(), out, sizeof(out)))
  {
    throw Napi::Error::New(env, "amm curve: swap failed (bad index or amount)");
  }
  return out;
}

}  // namespace detail_curve

inline Napi::Value ammCurveConstantProduct(const Napi::CallbackInfo& info)
{
  Napi::Env env = info.Env();
  const std::string r0 = detail_curve::bigintToDecimal(env, argBigInt(info[0], "ammCurveConstantProduct: reserve0"));
  const std::string r1 = detail_curve::bigintToDecimal(env, argBigInt(info[1], "ammCurveConstantProduct: reserve1"));
  const auto feeNum = argInteger(info[2], "ammCurveConstantProduct: feeNum");
  const auto feeDen = argInteger(info[3], "ammCurveConstantProduct: feeDen");
  return detail_curve::wrapHandle(
      env, lrvx_curve_constant_product(r0.c_str(), r1.c_str(), static_cast<uint64_t>(feeNum),
                                       static_cast<uint64_t>(feeDen)));
}

inline Napi::Value ammCurveRaydiumCp(const Napi::CallbackInfo& info)
{
  Napi::Env env = info.Env();
  const std::string r0 = detail_curve::bigintToDecimal(env, argBigInt(info[0], "ammCurveRaydiumCp: reserve0"));
  const std::string r1 = detail_curve::bigintToDecimal(env, argBigInt(info[1], "ammCurveRaydiumCp: reserve1"));
  const auto tradeFee = argInteger(info[2], "ammCurveRaydiumCp: tradeFeeRate");
  const auto creatorFee = info.Length() > 3 ? argInteger(info[3], "ammCurveRaydiumCp: creatorFeeRate") : 0;
  const bool onInput = info.Length() > 4 ? argBool(info[4], "ammCurveRaydiumCp: creatorFeeOnInput") : true;
  return detail_curve::wrapHandle(
      env, lrvx_curve_raydium_cp(r0.c_str(), r1.c_str(), static_cast<uint64_t>(tradeFee),
                                 static_cast<uint64_t>(creatorFee), onInput ? 1 : 0));
}

inline Napi::Value ammCurveUniswapV3(const Napi::CallbackInfo& info)
{
  Napi::Env env = info.Env();
  const std::string sqrtP = detail_curve::bigintToDecimal(env, argBigInt(info[0], "ammCurveUniswapV3: sqrtPriceX96"));
  const std::string liq = detail_curve::bigintToDecimal(env, argBigInt(info[1], "ammCurveUniswapV3: liquidity"));
  const auto feePips = argUint32(info[2], "ammCurveUniswapV3: feePips");

  // ticks: an array of [sqrtRatio: bigint, liquidityNet: bigint]; signed net rides in
  // i256, so its sign is the BigInt's sign.
  std::vector<std::string> sqrtStore;
  std::vector<std::string> netStore;
  if (given(info[3]))
  {
    Napi::Array arr = argArray(info[3], "ammCurveUniswapV3: ticks");
    for (uint32_t k = 0; k < arr.Length(); ++k)
    {
      Napi::Array pair = argArray(arr.Get(k), "a [sqrtPrice, liquidityNet] pair");
      sqrtStore.push_back(detail_curve::bigintToDecimal(
          env, argBigInt(pair.Get(0u), "a [sqrtPrice, liquidityNet] pair's sqrtPrice")));
      // liquidity net can be negative: format with sign from the limb words.
      uint64_t w[4];
      int sign = 0;
      detail_dex::bigintToWords(
          argBigInt(pair.Get(1u), "a [sqrtPrice, liquidityNet] pair's liquidityNet"), w, &sign);
      char mag[96] = {0};
      lrvx_u256_from_words(w, mag, sizeof(mag));
      netStore.push_back((sign != 0 && std::string(mag) != "0") ? std::string("-") + mag : mag);
    }
  }
  std::vector<const char*> sqrtPtrs;
  std::vector<const char*> netPtrs;
  for (const std::string& s : sqrtStore)
  {
    sqrtPtrs.push_back(s.c_str());
  }
  for (const std::string& s : netStore)
  {
    netPtrs.push_back(s.c_str());
  }
  return detail_curve::wrapHandle(
      env, lrvx_curve_uniswap_v3(sqrtP.c_str(), liq.c_str(), feePips, sqrtPtrs.data(),
                                 netPtrs.data(), sqrtPtrs.size()));
}

inline Napi::Value ammCurveTokenCount(const Napi::CallbackInfo& info)
{
  return Napi::Number::New(info.Env(),
                           static_cast<double>(lrvx_curve_token_count(
                               detail_curve::unwrapHandle(info[0]))));
}

inline Napi::Value ammCurveAmountOut(const Napi::CallbackInfo& info)
{
  Napi::Env env = info.Env();
  LrvxCurveHandle c = detail_curve::unwrapHandle(info[0]);
  const size_t i = argUint32(info[1], "ammCurveAmountOut: i");
  const size_t j = argUint32(info[2], "ammCurveAmountOut: j");
  const std::string amt = detail_curve::bigintToDecimal(env, argBigInt(info[3], "ammCurveAmountOut: amountIn"));
  return detail_curve::decimalToBigint(
      env, detail_curve::opOut(env, &lrvx_curve_amount_out, c, i, j, amt).c_str());
}

inline Napi::Value ammCurveApplySwap(const Napi::CallbackInfo& info)
{
  Napi::Env env = info.Env();
  LrvxCurveHandle c = detail_curve::unwrapHandle(info[0]);
  const size_t i = argUint32(info[1], "ammCurveApplySwap: i");
  const size_t j = argUint32(info[2], "ammCurveApplySwap: j");
  const std::string amt = detail_curve::bigintToDecimal(env, argBigInt(info[3], "ammCurveApplySwap: amountIn"));
  return detail_curve::decimalToBigint(
      env, detail_curve::opOut(env, &lrvx_curve_apply_swap, c, i, j, amt).c_str());
}

inline Napi::Value ammCurveBalance(const Napi::CallbackInfo& info)
{
  Napi::Env env = info.Env();
  LrvxCurveHandle c = detail_curve::unwrapHandle(info[0]);
  const size_t i = argUint32(info[1], "ammCurveBalance: i");
  char out[96] = {0};
  if (!lrvx_curve_balance(c, i, out, sizeof(out)))
  {
    throw Napi::RangeError::New(env, "amm curve: balance index out of range");
  }
  return detail_curve::decimalToBigint(env, out);
}

// A concentrated-liquidity pool's sqrt price / active liquidity as a BigInt, or null
// for a non-CLMM pool (constant-product, Raydium CP).
inline Napi::Value ammCurveClmmField(const Napi::CallbackInfo& info,
                                     uint8_t (*op)(LrvxCurveHandle, char*, size_t))
{
  Napi::Env env = info.Env();
  char out[96] = {0};
  if (!op(detail_curve::unwrapHandle(info[0]), out, sizeof(out)))
  {
    return env.Null();
  }
  return detail_curve::decimalToBigint(env, out);
}

inline Napi::Value ammCurveSqrtPrice(const Napi::CallbackInfo& info)
{
  return ammCurveClmmField(info, &lrvx_curve_sqrt_price);
}

inline Napi::Value ammCurveLiquidity(const Napi::CallbackInfo& info)
{
  return ammCurveClmmField(info, &lrvx_curve_liquidity);
}

inline Napi::Value ammCurveClone(const Napi::CallbackInfo& info)
{
  Napi::Env env = info.Env();
  return detail_curve::wrapHandle(env, lrvx_curve_clone(detail_curve::unwrapHandle(info[0])));
}

inline void registerAmmCurve(Napi::Env env, Napi::Object exports)
{
  exports.Set("ammCurveConstantProduct",
              Napi::Function::New(env, &ammCurveConstantProduct, "ammCurveConstantProduct"));
  exports.Set("ammCurveRaydiumCp",
              Napi::Function::New(env, &ammCurveRaydiumCp, "ammCurveRaydiumCp"));
  exports.Set("ammCurveUniswapV3",
              Napi::Function::New(env, &ammCurveUniswapV3, "ammCurveUniswapV3"));
  exports.Set("ammCurveTokenCount",
              Napi::Function::New(env, &ammCurveTokenCount, "ammCurveTokenCount"));
  exports.Set("ammCurveAmountOut",
              Napi::Function::New(env, &ammCurveAmountOut, "ammCurveAmountOut"));
  exports.Set("ammCurveApplySwap",
              Napi::Function::New(env, &ammCurveApplySwap, "ammCurveApplySwap"));
  exports.Set("ammCurveBalance", Napi::Function::New(env, &ammCurveBalance, "ammCurveBalance"));
  exports.Set("ammCurveSqrtPrice",
              Napi::Function::New(env, &ammCurveSqrtPrice, "ammCurveSqrtPrice"));
  exports.Set("ammCurveLiquidity",
              Napi::Function::New(env, &ammCurveLiquidity, "ammCurveLiquidity"));
  exports.Set("ammCurveClone", Napi::Function::New(env, &ammCurveClone, "ammCurveClone"));
}

}  // namespace node_lrvx
