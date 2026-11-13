// node/src/dex_amount.h -- DEX 256-bit amounts (u256 / i256) for Node.js.
//
// The DEX curves compute in exact 256-bit integers. At the Node boundary they are
// native BigInt -- arbitrary precision, so a 256-bit wei amount is lossless. A BigInt
// is limb-addressable (little-endian uint64 words), which is exactly the u256 layout,
// so the C ABI's lrvx_u256_*_words / lrvx_*_roundtrip cross the value through the exact
// C++ type and back. Everything runs through the C ABI; this is purely marshalling.

#pragma once

#include <napi.h>

#include "lrvx/capi/lrvx_capi.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace node_lrvx
{

namespace detail_dex
{

// A BigInt's little-endian uint64 words into out[4]. Returns false if it is wider than
// 256 bits. sign_out receives the sign bit (1 = negative). Taken by value because
// Napi::BigInt::ToWords is non-const.
inline bool bigintToWords(Napi::BigInt b, uint64_t out[4], int* sign_out)
{
  out[0] = out[1] = out[2] = out[3] = 0;
  if (b.WordCount() > 4)
  {
    return false;
  }
  int sign = 0;
  size_t wc = 4;
  b.ToWords(&sign, &wc, out);
  if (sign_out != nullptr)
  {
    *sign_out = sign;
  }
  return true;
}

inline Napi::BigInt wordsToBigInt(Napi::Env env, int sign, const uint64_t words[4])
{
  return Napi::BigInt::New(env, sign, static_cast<size_t>(4), words);
}

}  // namespace detail_dex

// u256Roundtrip(value: bigint): bigint -- a value up to 2^256 - 1 survives the exact
// u256 (through its decimal form) to the wei.
inline Napi::Value u256Roundtrip(const Napi::CallbackInfo& info)
{
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsBigInt())
  {
    throw Napi::TypeError::New(env, "u256Roundtrip(value): value must be a BigInt");
  }
  uint64_t words[4];
  int sign = 0;
  if (!detail_dex::bigintToWords(argBigInt(info[0], "u256Roundtrip: value"), words, &sign) || sign != 0)
  {
    throw Napi::RangeError::New(env, "u256Roundtrip: value must be in [0, 2^256)");
  }
  char dec[96] = {0};
  char dec2[96] = {0};
  uint64_t out[4];
  if (!lrvx_u256_from_words(words, dec, sizeof(dec)) ||
      !lrvx_u256_roundtrip(dec, dec2, sizeof(dec2)) || !lrvx_u256_to_words(dec2, out))
  {
    throw Napi::Error::New(env, "u256Roundtrip: conversion failed");
  }
  return detail_dex::wordsToBigInt(env, 0, out);
}

// i256Roundtrip(value: bigint): bigint -- the signed variant, carrying the sign.
inline Napi::Value i256Roundtrip(const Napi::CallbackInfo& info)
{
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsBigInt())
  {
    throw Napi::TypeError::New(env, "i256Roundtrip(value): value must be a BigInt");
  }
  uint64_t words[4];
  int sign = 0;
  if (!detail_dex::bigintToWords(argBigInt(info[0], "i256Roundtrip: value"), words, &sign))
  {
    throw Napi::RangeError::New(env, "i256Roundtrip: magnitude must be < 2^256");
  }
  char mag[96] = {0};
  if (!lrvx_u256_from_words(words, mag, sizeof(mag)))
  {
    throw Napi::Error::New(env, "i256Roundtrip: conversion failed");
  }
  std::string dec = (sign != 0 && std::strcmp(mag, "0") != 0) ? std::string("-") + mag : mag;
  char dec2[96] = {0};
  if (!lrvx_i256_roundtrip(dec.c_str(), dec2, sizeof(dec2)))
  {
    throw Napi::Error::New(env, "i256Roundtrip: conversion failed");
  }
  const bool neg = dec2[0] == '-';
  const char* magStr = neg ? dec2 + 1 : dec2;
  uint64_t out[4];
  if (!lrvx_u256_to_words(magStr, out))
  {
    throw Napi::Error::New(env, "i256Roundtrip: conversion failed");
  }
  return detail_dex::wordsToBigInt(env, neg ? 1 : 0, out);
}

// u256FromHex(hex: string): bigint -- hex (with or without 0x), for chain data.
inline Napi::Value u256FromHex(const Napi::CallbackInfo& info)
{
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsString())
  {
    throw Napi::TypeError::New(env, "u256FromHex(hex): hex must be a string");
  }
  std::string hex = info[0].As<Napi::String>();
  char dec[96] = {0};
  uint64_t out[4];
  if (!lrvx_u256_from_hex(hex.c_str(), dec, sizeof(dec)) || !lrvx_u256_to_words(dec, out))
  {
    throw Napi::RangeError::New(env, "u256FromHex: not a valid 256-bit hex value");
  }
  return detail_dex::wordsToBigInt(env, 0, out);
}

inline void registerDexAmount(Napi::Env env, Napi::Object exports)
{
  exports.Set("u256Roundtrip", Napi::Function::New(env, &u256Roundtrip, "u256Roundtrip"));
  exports.Set("i256Roundtrip", Napi::Function::New(env, &i256Roundtrip, "i256Roundtrip"));
  exports.Set("u256FromHex", Napi::Function::New(env, &u256FromHex, "u256FromHex"));
}

}  // namespace node_lrvx
