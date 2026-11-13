// node/src/run_trace.h -- TraceRecorder / TraceReader wrappers for .lrvxrun.

#pragma once

#include <napi.h>

#include "bindings_common.h"
#include "error_translator.h"
#include "lrvx/capi/lrvx_capi.h"

#include <cstdint>
#include <string>
#include <vector>

namespace node_lrvx
{

namespace detail_runtrace
{

inline std::vector<uint32_t> readU32Array(const Napi::Array& arr)
{
  std::vector<uint32_t> out;
  out.reserve(arr.Length());
  for (uint32_t i = 0; i < arr.Length(); ++i)
  {
    out.push_back(argUint32(arr.Get(i), "a symbol id"));
  }
  return out;
}

// A payload: a Buffer or a string. Anything else used to be written as an
// empty payload.
inline std::vector<uint8_t> readBytes(const Napi::Value& v, const char* what)
{
  if (v.IsBuffer())
  {
    auto buf = v.As<Napi::Buffer<uint8_t>>();
    return std::vector<uint8_t>(buf.Data(), buf.Data() + buf.Length());
  }
  if (v.IsString())
  {
    std::string s = v.As<Napi::String>().Utf8Value();
    return std::vector<uint8_t>(s.begin(), s.end());
  }
  throw Napi::TypeError::New(v.Env(), std::string(what) + " must be a Buffer or a string");
}

}  // namespace detail_runtrace

class TraceRecorderWrap : public Napi::ObjectWrap<TraceRecorderWrap>
{
 public:
  static Napi::FunctionReference& Ctor()
  {
    static Napi::FunctionReference ref;
    return ref;
  }

  static Napi::Function Init(Napi::Env env)
  {
    return keepCtor(Ctor(), DefineClass(env, "TraceRecorder",
                                        {InstanceMethod("addTapeRef", &TraceRecorderWrap::AddTapeRef),
                                         InstanceMethod("setRunEndedNs", &TraceRecorderWrap::SetRunEndedNs),
                                         InstanceMethod("writeSignal", &TraceRecorderWrap::WriteSignal),
                                         InstanceMethod("writeOrderEvent", &TraceRecorderWrap::WriteOrderEvent),
                                         InstanceMethod("writeFill", &TraceRecorderWrap::WriteFill),
                                         InstanceMethod("close", &TraceRecorderWrap::Close)}));
  }

  TraceRecorderWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<TraceRecorderWrap>(info)
  {
    auto env = info.Env();
    if (info.Length() < 1 || !info[0].IsObject())
    {
      Napi::TypeError::New(env, "TraceRecorder({path, strategyId?, strategyHash?, runStartedNs?})")
          .ThrowAsJavaScriptException();
      return;
    }
    auto opts = argObject(info[0], "TraceRecorder: opts");
    std::string path = argString(opts.Get("path"), "TraceRecorder: path");
    std::string sid = opts.Has("strategyId") ? argString(opts.Get("strategyId"), "TraceRecorder: strategyId") : "";
    std::string shash = opts.Has("strategyHash") ? argString(opts.Get("strategyHash"), "TraceRecorder: strategyHash") : "";
    int64_t started = opts.Has("runStartedNs") ? toInt64Ns(opts.Get("runStartedNs"), "TraceRecorder: runStartedNs") : 0;
    _h = lrvx_run_recorder_create(path.c_str(), sid.c_str(), shash.c_str(), started);
    if (!_h)
    {
      auto err = Napi::Error::New(env, "TraceRecorder: " + capiReason());
      err.Value().Set("code", Napi::String::New(env, "E_VAL_002"));
      err.Value().Set("name", Napi::String::New(env, "LrvxError"));
      throw err;
    }
  }

  ~TraceRecorderWrap()
  {
    if (_h)
    {
      lrvx_run_recorder_destroy(_h);
    }
  }

 private:
  void AddTapeRef(const Napi::CallbackInfo& info)
  {
    auto opts = argObject(info[0], "addTapeRef: opts");
    std::string path = argString(opts.Get("path"), "addTapeRef: path");
    std::string ch = opts.Has("contentHash") ? argString(opts.Get("contentHash"), "addTapeRef: contentHash") : "";
    int64_t first = opts.Has("firstEventNs") ? toInt64Ns(opts.Get("firstEventNs"), "addTapeRef: firstEventNs") : 0;
    int64_t last = opts.Has("lastEventNs") ? toInt64Ns(opts.Get("lastEventNs"), "addTapeRef: lastEventNs") : 0;
    lrvx_run_recorder_add_tape_ref(_h, path.c_str(), ch.c_str(), first, last);
  }

  void SetRunEndedNs(const Napi::CallbackInfo& info)
  {
    lrvx_run_recorder_set_run_ended_ns(_h, toInt64Ns(info[0], "setRunEndedNs: ns"));
  }

  void WriteSignal(const Napi::CallbackInfo& info)
  {
    auto opts = argOptions(info, "writeSignal");
    int64_t run_ts = toInt64Ns(opts.Get("runTsNs"), "writeSignal: runTsNs");
    int64_t feed_ts = opts.Has("feedTsNs") ? toInt64Ns(opts.Get("feedTsNs"), "writeSignal: feedTsNs") : 0;
    // A Number or a BigInt; the whole 64-bit id is written.
    uint64_t sid =
        opts.Has("signalId") ? argUint64(opts.Get("signalId"), "writeSignal: signalId") : 0;
    uint32_t flags = opts.Has("flags") ? argUint32(opts.Get("flags"), "writeSignal: flags") : 0;
    int64_t strength = opts.Has("strengthRaw")
                           ? argInt64(opts.Get("strengthRaw"), "writeSignal: strengthRaw")
                           : 0;
    std::string name = opts.Has("name") ? argString(opts.Get("name"), "writeSignal: name") : "";
    std::vector<uint32_t> sids;
    if (opts.Has("symbolIds"))
    {
      sids = detail_runtrace::readU32Array(argArray(opts.Get("symbolIds"), "writeSignal: symbolIds"));
    }
    std::vector<uint8_t> payload;
    if (opts.Has("payload"))
    {
      payload = detail_runtrace::readBytes(opts.Get("payload"), "writeSignal: payload");
    }
    lrvx_run_recorder_write_signal64(_h, run_ts, feed_ts, sid, flags, strength,
                                     name.empty() ? nullptr : name.data(), name.size(),
                                     sids.empty() ? nullptr : sids.data(), sids.size(),
                                     payload.empty() ? nullptr : payload.data(), payload.size());
  }

  void WriteOrderEvent(const Napi::CallbackInfo& info)
  {
    auto opts = argOptions(info, "writeOrderEvent");
    const auto u8 = [&opts](const char* key, const char* what, uint32_t fallback)
    {
      return static_cast<uint8_t>(opts.Has(key) ? argUint32(opts.Get(key), what) : fallback);
    };
    int64_t run_ts = toInt64Ns(opts.Get("runTsNs"), "writeOrderEvent: runTsNs");
    int64_t feed_ts = opts.Has("feedTsNs") ? toInt64Ns(opts.Get("feedTsNs"), "writeOrderEvent: feedTsNs") : 0;
    uint64_t oid =
        opts.Has("orderId") ? argUint64(opts.Get("orderId"), "writeOrderEvent: orderId") : 0;
    uint64_t pid = opts.Has("parentSignalId")
                       ? argUint64(opts.Get("parentSignalId"), "writeOrderEvent: parentSignalId")
                       : 0;
    int64_t price =
        opts.Has("priceRaw") ? argInt64(opts.Get("priceRaw"), "writeOrderEvent: priceRaw") : 0;
    int64_t qty =
        opts.Has("qtyRaw") ? argInt64(opts.Get("qtyRaw"), "writeOrderEvent: qtyRaw") : 0;
    uint32_t symid =
        opts.Has("symbolId") ? argUint32(opts.Get("symbolId"), "writeOrderEvent: symbolId") : 0;
    uint8_t kind = u8("eventKind", "writeOrderEvent: eventKind", 1);
    uint8_t side = u8("side", "writeOrderEvent: side", 0);
    uint8_t otype = u8("orderType", "writeOrderEvent: orderType", 0);
    uint32_t flags =
        opts.Has("flags") ? argUint32(opts.Get("flags"), "writeOrderEvent: flags") : 0;
    std::string reason = opts.Has("reason") ? argString(opts.Get("reason"), "writeOrderEvent: reason") : "";
    lrvx_run_recorder_write_order_event(_h, run_ts, feed_ts, oid, pid, price, qty, symid, kind,
                                        side, otype, flags,
                                        reason.empty() ? nullptr : reason.data(), reason.size());
  }

  void WriteFill(const Napi::CallbackInfo& info)
  {
    auto opts = argOptions(info, "writeFill");
    int64_t run_ts = toInt64Ns(opts.Get("runTsNs"), "writeFill: runTsNs");
    int64_t feed_ts = opts.Has("feedTsNs") ? toInt64Ns(opts.Get("feedTsNs"), "writeFill: feedTsNs") : 0;
    uint64_t oid = opts.Has("orderId") ? argUint64(opts.Get("orderId"), "writeFill: orderId") : 0;
    uint64_t fid = opts.Has("fillId") ? argUint64(opts.Get("fillId"), "writeFill: fillId") : 0;
    int64_t price =
        opts.Has("priceRaw") ? argInt64(opts.Get("priceRaw"), "writeFill: priceRaw") : 0;
    int64_t qty = opts.Has("qtyRaw") ? argInt64(opts.Get("qtyRaw"), "writeFill: qtyRaw") : 0;
    int64_t fee = opts.Has("feeRaw") ? argInt64(opts.Get("feeRaw"), "writeFill: feeRaw") : 0;
    uint32_t symid =
        opts.Has("symbolId") ? argUint32(opts.Get("symbolId"), "writeFill: symbolId") : 0;
    uint8_t side = static_cast<uint8_t>(
        opts.Has("side") ? argUint32(opts.Get("side"), "writeFill: side") : 0);
    uint8_t liq = static_cast<uint8_t>(
        opts.Has("liquidity") ? argUint32(opts.Get("liquidity"), "writeFill: liquidity") : 0);
    lrvx_run_recorder_write_fill(_h, run_ts, feed_ts, oid, fid, price, qty, fee, symid, side, liq);
  }

  void Close(const Napi::CallbackInfo&) { lrvx_run_recorder_close(_h); }

 public:
  LrvxRunRecorderHandle handle() const noexcept { return _h; }

 private:
  LrvxRunRecorderHandle _h{nullptr};
};

class TraceReaderWrap : public Napi::ObjectWrap<TraceReaderWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(
        env, "TraceReader",
        {InstanceMethod("strategyId", &TraceReaderWrap::StrategyId),
         InstanceMethod("strategyHash", &TraceReaderWrap::StrategyHash),
         InstanceMethod("runStartedNs", &TraceReaderWrap::RunStartedNs),
         InstanceMethod("runEndedNs", &TraceReaderWrap::RunEndedNs),
         InstanceMethod("tapeRefs", &TraceReaderWrap::TapeRefs),
         InstanceMethod("readAllSignals", &TraceReaderWrap::ReadAllSignals),
         InstanceMethod("readAllOrderEvents", &TraceReaderWrap::ReadAllOrderEvents),
         InstanceMethod("readAllFills", &TraceReaderWrap::ReadAllFills),
         InstanceMethod("close", &TraceReaderWrap::Close)});
  }

  TraceReaderWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<TraceReaderWrap>(info)
  {
    auto env = info.Env();
    if (info.Length() < 1 || !info[0].IsString())
    {
      Napi::TypeError::New(env, "TraceReader(path)").ThrowAsJavaScriptException();
      return;
    }
    std::string path = argString(info[0], "TraceReader: path");
    _h = lrvx_run_reader_open(path.c_str());
    if (!_h)
    {
      auto err = Napi::Error::New(env, "TraceReader: " + capiReason());
      err.Value().Set("code", Napi::String::New(env, "E_VAL_001"));
      err.Value().Set("name", Napi::String::New(env, "LrvxError"));
      throw err;
    }
  }

  ~TraceReaderWrap()
  {
    if (_h)
    {
      lrvx_run_reader_close(_h);
    }
  }

 private:
  Napi::Value StrategyId(const Napi::CallbackInfo& info)
  {
    uint64_t n = lrvx_run_reader_strategy_id(_h, nullptr, 0);
    std::string out(n, '\0');
    if (n > 0)
    {
      lrvx_run_reader_strategy_id(_h, out.data(), n);
    }
    return Napi::String::New(info.Env(), out);
  }

  Napi::Value StrategyHash(const Napi::CallbackInfo& info)
  {
    uint64_t n = lrvx_run_reader_strategy_hash(_h, nullptr, 0);
    std::string out(n, '\0');
    if (n > 0)
    {
      lrvx_run_reader_strategy_hash(_h, out.data(), n);
    }
    return Napi::String::New(info.Env(), out);
  }

  Napi::Value RunStartedNs(const Napi::CallbackInfo& info)
  {
    return Napi::BigInt::New(info.Env(), lrvx_run_reader_run_started_ns(_h));
  }

  Napi::Value RunEndedNs(const Napi::CallbackInfo& info)
  {
    return Napi::BigInt::New(info.Env(), lrvx_run_reader_run_ended_ns(_h));
  }

  Napi::Value TapeRefs(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    uint64_t n = lrvx_run_reader_tape_ref_count(_h);
    Napi::Array out = Napi::Array::New(env, n);
    for (uint64_t i = 0; i < n; ++i)
    {
      uint64_t plen = lrvx_run_reader_tape_ref_path(_h, i, nullptr, 0);
      std::string p(plen, '\0');
      if (plen > 0)
      {
        lrvx_run_reader_tape_ref_path(_h, i, p.data(), plen);
      }
      Napi::Object o = Napi::Object::New(env);
      o.Set("path", Napi::String::New(env, p));
      out.Set(static_cast<uint32_t>(i), o);
    }
    return out;
  }

  Napi::Value ReadAllSignals(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    uint64_t n = lrvx_run_reader_signal_count(_h);
    Napi::Array out = Napi::Array::New(env, n);
    for (uint64_t i = 0; i < n; ++i)
    {
      int64_t run_ts = 0, feed_ts = 0, strength = 0;
      uint32_t sid = 0, flags = 0;
      uint64_t name_len = 0, sym_count = 0, payload_len = 0;
      lrvx_run_reader_signal_header(_h, i, &run_ts, &feed_ts, &sid, &flags, &strength, &name_len,
                                    &sym_count, &payload_len);
      std::string name(name_len, '\0');
      if (name_len > 0)
      {
        lrvx_run_reader_signal_name(_h, i, name.data(), name_len);
      }
      std::vector<uint32_t> sids(sym_count);
      if (sym_count > 0)
      {
        lrvx_run_reader_signal_symbol_ids(_h, i, sids.data(), sym_count);
      }
      std::vector<uint8_t> payload(payload_len);
      if (payload_len > 0)
      {
        lrvx_run_reader_signal_payload(_h, i, payload.data(), payload_len);
      }
      Napi::Object o = Napi::Object::New(env);
      o.Set("runTsNs", Napi::BigInt::New(env, run_ts));
      o.Set("feedTsNs", Napi::BigInt::New(env, feed_ts));
      o.Set("signalId", Napi::Number::New(env, sid));
      // A BigInt, as every 64-bit id of a trace record is: an order id from
      // a strategy with a subscriber id of 2^21 or more does not fit a Number.
      o.Set("signalId64", idToBigInt(env, lrvx_run_reader_signal_id64(_h, i)));
      o.Set("flags", Napi::Number::New(env, flags));
      o.Set("strengthRaw", Napi::Number::New(env, static_cast<double>(strength)));
      o.Set("name", Napi::String::New(env, name));
      Napi::Array sidsJs = Napi::Array::New(env, sids.size());
      for (size_t k = 0; k < sids.size(); ++k)
      {
        sidsJs.Set(static_cast<uint32_t>(k), Napi::Number::New(env, sids[k]));
      }
      o.Set("symbolIds", sidsJs);
      o.Set("payload",
            Napi::Buffer<uint8_t>::Copy(env, payload.data(), payload.size()));
      out.Set(static_cast<uint32_t>(i), o);
    }
    return out;
  }

  Napi::Value ReadAllOrderEvents(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    uint64_t n = lrvx_run_reader_order_event_count(_h);
    Napi::Array out = Napi::Array::New(env, n);
    for (uint64_t i = 0; i < n; ++i)
    {
      int64_t run_ts = 0, feed_ts = 0, price = 0, qty = 0;
      uint64_t oid = 0, pid = 0, reason_len = 0;
      uint32_t sid = 0, flags = 0;
      uint8_t kind = 0, side = 0, otype = 0;
      lrvx_run_reader_order_event_header(_h, i, &run_ts, &feed_ts, &oid, &pid, &price, &qty, &sid,
                                         &kind, &side, &otype, &flags, &reason_len);
      std::string reason(reason_len, '\0');
      if (reason_len > 0)
      {
        lrvx_run_reader_order_event_reason(_h, i, reason.data(), reason_len);
      }
      Napi::Object o = Napi::Object::New(env);
      o.Set("runTsNs", Napi::BigInt::New(env, run_ts));
      o.Set("feedTsNs", Napi::BigInt::New(env, feed_ts));
      o.Set("orderId", idToBigInt(env, oid));
      o.Set("parentSignalId", idToBigInt(env, pid));
      o.Set("priceRaw", Napi::Number::New(env, static_cast<double>(price)));
      o.Set("qtyRaw", Napi::Number::New(env, static_cast<double>(qty)));
      o.Set("symbolId", Napi::Number::New(env, sid));
      o.Set("eventKind", Napi::Number::New(env, kind));
      o.Set("side", Napi::Number::New(env, side));
      o.Set("orderType", Napi::Number::New(env, otype));
      o.Set("flags", Napi::Number::New(env, flags));
      o.Set("reason", Napi::String::New(env, reason));
      out.Set(static_cast<uint32_t>(i), o);
    }
    return out;
  }

  Napi::Value ReadAllFills(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    uint64_t n = lrvx_run_reader_fill_count(_h);
    Napi::Array out = Napi::Array::New(env, n);
    for (uint64_t i = 0; i < n; ++i)
    {
      int64_t run_ts = 0, feed_ts = 0, price = 0, qty = 0, fee = 0;
      uint64_t oid = 0, fid = 0;
      uint32_t sid = 0;
      uint8_t side = 0, liq = 0;
      lrvx_run_reader_fill(_h, i, &run_ts, &feed_ts, &oid, &fid, &price, &qty, &fee, &sid, &side,
                           &liq);
      Napi::Object o = Napi::Object::New(env);
      o.Set("runTsNs", Napi::BigInt::New(env, run_ts));
      o.Set("feedTsNs", Napi::BigInt::New(env, feed_ts));
      o.Set("orderId", idToBigInt(env, oid));
      o.Set("fillId", idToBigInt(env, fid));
      o.Set("priceRaw", Napi::Number::New(env, static_cast<double>(price)));
      o.Set("qtyRaw", Napi::Number::New(env, static_cast<double>(qty)));
      o.Set("feeRaw", Napi::Number::New(env, static_cast<double>(fee)));
      o.Set("symbolId", Napi::Number::New(env, sid));
      o.Set("side", Napi::Number::New(env, side));
      o.Set("liquidity", Napi::Number::New(env, liq));
      out.Set(static_cast<uint32_t>(i), o);
    }
    return out;
  }

  void Close(const Napi::CallbackInfo&)
  {
    if (_h)
    {
      lrvx_run_reader_close(_h);
      _h = nullptr;
    }
  }

  LrvxRunReaderHandle _h{nullptr};
};

inline void registerRunTrace(Napi::Env env, Napi::Object exports)
{
  exports.Set("TraceRecorder", TraceRecorderWrap::Init(env));
  exports.Set("TraceReader", TraceReaderWrap::Init(env));
}

}  // namespace node_lrvx
