// node/src/delta_book.h -- DeltaBookEncoder / DeltaBookReplayer wrappers.
//
// Encode L2 snapshots as anchor snapshots plus deltas; replay the
// stream back into reconstructed snapshots. Both classes wrap the
// LrvxDeltaBookEncoderHandle / LrvxDeltaBookReplayerHandle through
// the C ABI and translate level arrays to / from JS objects.

#pragma once

#include <napi.h>

#include "error_translator.h"
#include "lrvx/capi/lrvx_capi.h"

#include <cstdint>
#include <vector>

namespace node_lrvx
{

namespace detail_db
{

inline std::vector<LrvxBookLevel> readLevels(const Napi::Array& arr)
{
  std::vector<LrvxBookLevel> out;
  out.reserve(arr.Length());
  for (uint32_t i = 0; i < arr.Length(); ++i)
  {
    auto entry = argObject(arr.Get(i), "a book level");
    LrvxBookLevel l{};
    l.price_raw = argInt64(entry.Get("priceRaw"), "a book level's priceRaw");
    l.quantity_raw = argInt64(entry.Get("qtyRaw"), "a book level's qtyRaw");
    out.push_back(l);
  }
  return out;
}

inline Napi::Array levelsToJs(Napi::Env env, const std::vector<LrvxBookLevel>& levels)
{
  Napi::Array out = Napi::Array::New(env, levels.size());
  for (size_t i = 0; i < levels.size(); ++i)
  {
    Napi::Object o = Napi::Object::New(env);
    o.Set("priceRaw", Napi::Number::New(env, static_cast<double>(levels[i].price_raw)));
    o.Set("qtyRaw", Napi::Number::New(env, static_cast<double>(levels[i].quantity_raw)));
    out.Set(static_cast<uint32_t>(i), o);
  }
  return out;
}

}  // namespace detail_db

class DeltaBookEncoderWrap : public Napi::ObjectWrap<DeltaBookEncoderWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "DeltaBookEncoder",
                       {InstanceMethod("encode", &DeltaBookEncoderWrap::Encode),
                        InstanceMethod("reset", &DeltaBookEncoderWrap::Reset),
                        InstanceMethod("resetAll", &DeltaBookEncoderWrap::ResetAll)});
  }

  DeltaBookEncoderWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<DeltaBookEncoderWrap>(info)
  {
    uint32_t anchor_every = 100;
    if (given(info[0]))
    {
      auto opts = argObject(info[0], "DeltaBookEncoder: argument 1");
      if (opts.Has("anchorEvery"))
      {
        anchor_every = argUint32(opts.Get("anchorEvery"), "DeltaBookEncoder: anchorEvery");
      }
    }
    _h = lrvx_delta_book_encoder_create(anchor_every);
    if (!_h)
    {
      auto err = Napi::Error::New(info.Env(), "DeltaBookEncoder: construction failed");
      err.Value().Set("code", Napi::String::New(info.Env(), "E_VAL_002"));
      err.Value().Set("name", Napi::String::New(info.Env(), "LrvxError"));
      throw err;
    }
  }
  ~DeltaBookEncoderWrap()
  {
    if (_h)
    {
      lrvx_delta_book_encoder_destroy(_h);
    }
  }

 private:
  Napi::Value Encode(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsArray() || !info[2].IsArray())
    {
      Napi::TypeError::New(env, "encode(symbolId, bids, asks)").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    uint32_t sym = argUint32(info[0], "encode: argument 1");
    auto bids = detail_db::readLevels(argArray(info[1], "encode: argument 2"));
    auto asks = detail_db::readLevels(argArray(info[2], "encode: argument 3"));
    uint8_t is_delta = 0;
    uint64_t bcnt = 0, acnt = 0;
    lrvx_delta_book_encoder_encode(_h, sym,
                                   bids.empty() ? nullptr : bids.data(), bids.size(),
                                   asks.empty() ? nullptr : asks.data(), asks.size(),
                                   &is_delta, &bcnt, &acnt);
    std::vector<LrvxBookLevel> out_bids(bcnt);
    std::vector<LrvxBookLevel> out_asks(acnt);
    if (bcnt > 0)
    {
      lrvx_delta_book_encoder_copy_bids(_h, out_bids.data(), bcnt);
    }
    if (acnt > 0)
    {
      lrvx_delta_book_encoder_copy_asks(_h, out_asks.data(), acnt);
    }
    Napi::Object result = Napi::Object::New(env);
    result.Set("isDelta", Napi::Boolean::New(env, is_delta != 0));
    result.Set("bids", detail_db::levelsToJs(env, out_bids));
    result.Set("asks", detail_db::levelsToJs(env, out_asks));
    return result;
  }

  void Reset(const Napi::CallbackInfo& info)
  {
    lrvx_delta_book_encoder_reset(_h, argUint32(info[0], "reset: argument 1"));
  }

  void ResetAll(const Napi::CallbackInfo&)
  {
    lrvx_delta_book_encoder_reset_all(_h);
  }

  LrvxDeltaBookEncoderHandle _h{nullptr};
};

class DeltaBookReplayerWrap : public Napi::ObjectWrap<DeltaBookReplayerWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "DeltaBookReplayer",
                       {InstanceMethod("apply", &DeltaBookReplayerWrap::Apply),
                        InstanceMethod("reset", &DeltaBookReplayerWrap::Reset),
                        InstanceMethod("resetAll", &DeltaBookReplayerWrap::ResetAll)});
  }

  DeltaBookReplayerWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<DeltaBookReplayerWrap>(info)
  {
    _h = lrvx_delta_book_replayer_create();
  }
  ~DeltaBookReplayerWrap()
  {
    if (_h)
    {
      lrvx_delta_book_replayer_destroy(_h);
    }
  }

 private:
  Napi::Value Apply(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    if (info.Length() < 4)
    {
      Napi::TypeError::New(env, "apply(type, symbolId, bids, asks)").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    uint8_t type = static_cast<uint8_t>(argUint32(info[0], "apply: argument 1"));
    uint32_t sym = argUint32(info[1], "apply: argument 2");
    auto bids = detail_db::readLevels(argArray(info[2], "apply: argument 3"));
    auto asks = detail_db::readLevels(argArray(info[3], "apply: argument 4"));
    uint64_t bcnt = 0, acnt = 0;
    lrvx_delta_book_replayer_apply(_h, type, sym,
                                   bids.empty() ? nullptr : bids.data(), bids.size(),
                                   asks.empty() ? nullptr : asks.data(), asks.size(),
                                   &bcnt, &acnt);
    std::vector<LrvxBookLevel> out_bids(bcnt);
    std::vector<LrvxBookLevel> out_asks(acnt);
    if (bcnt > 0)
    {
      lrvx_delta_book_replayer_copy_bids(_h, out_bids.data(), bcnt);
    }
    if (acnt > 0)
    {
      lrvx_delta_book_replayer_copy_asks(_h, out_asks.data(), acnt);
    }
    Napi::Object result = Napi::Object::New(env);
    result.Set("bids", detail_db::levelsToJs(env, out_bids));
    result.Set("asks", detail_db::levelsToJs(env, out_asks));
    return result;
  }

  void Reset(const Napi::CallbackInfo& info)
  {
    lrvx_delta_book_replayer_reset(_h, argUint32(info[0], "reset: argument 1"));
  }

  void ResetAll(const Napi::CallbackInfo&)
  {
    if (_h)
    {
      lrvx_delta_book_replayer_destroy(_h);
      _h = lrvx_delta_book_replayer_create();
    }
  }

  LrvxDeltaBookReplayerHandle _h{nullptr};
};

inline void registerDeltaBook(Napi::Env env, Napi::Object exports)
{
  exports.Set("DeltaBookEncoder", DeltaBookEncoderWrap::Init(env));
  exports.Set("DeltaBookReplayer", DeltaBookReplayerWrap::Init(env));
}

}  // namespace node_lrvx
