// node/src/data_ops.h -- DataWriter, DataReader, BinaryLogRecorderHook, Partitioner, segment ops

#pragma once
#include <napi.h>
#include <cstring>
#include <vector>
#include "bindings_common.h"
#include "lrvx/capi/lrvx_capi.h"
#include "tape_aggregators.h"

namespace node_lrvx
{

// ── DataWriter ──────────────────────────────────────────────────────

class DataWriterWrap : public Napi::ObjectWrap<DataWriterWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "DataWriter",
                       {InstanceMethod("writeTrade", &DataWriterWrap::WriteTrade),
                        InstanceMethod("writeBook", &DataWriterWrap::WriteBook),
                        InstanceMethod("flush", &DataWriterWrap::Flush),
                        InstanceMethod("close", &DataWriterWrap::Close),
                        InstanceMethod("stats", &DataWriterWrap::Stats)});
  }
  DataWriterWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<DataWriterWrap>(info)
  {
    std::string dir = info[0].As<Napi::String>().Utf8Value();
    uint64_t maxMb = info.Length() > 1 ? info[1].As<Napi::Number>().Int64Value() : 256;
    uint8_t exId = info.Length() > 2 ? info[2].As<Napi::Number>().Uint32Value() : 0;
    _h = lrvx_data_writer_create(dir.c_str(), maxMb, exId);
  }
  ~DataWriterWrap()
  {
    if (_h)
    {
      lrvx_data_writer_close(_h);
      lrvx_data_writer_destroy(_h);
    }
  }

 private:
  Napi::Value WriteTrade(const Napi::CallbackInfo& info)
  {
    // index.d.ts declares `number | bigint` timestamps and `Side`
    // ("buy" | "sell"), but this read Number for all three, so every
    // declared form except an all-numeric call threw. writeBook next door
    // already used toInt64Ns; a real ns timestamp does not survive a double
    // (1765615835519000000 comes back as ...064), which is why bigint is the
    // documented form.
    const int64_t exchTs = toInt64Ns(info[0]);
    const int64_t recvTs = toInt64Ns(info[1]);
    const int64_t tradeId = toInt64Ns(info[4]);

    uint32_t side = 0;
    if (info[6].IsString())
    {
      const std::string s = info[6].As<Napi::String>().Utf8Value();
      if (s == "buy")
      {
        side = 0;
      }
      else if (s == "sell")
      {
        side = 1;
      }
      else
      {
        Napi::Error::New(info.Env(),
                         "unknown side: " + s + ". known: buy, sell")
            .ThrowAsJavaScriptException();
        return info.Env().Undefined();
      }
    }
    else
    {
      side = info[6].As<Napi::Number>().Uint32Value();
    }

    return Napi::Boolean::New(
        info.Env(),
        lrvx_data_writer_write_trade(_h, exchTs, recvTs,
                                     info[2].As<Napi::Number>().DoubleValue(),
                                     info[3].As<Napi::Number>().DoubleValue(),
                                     tradeId,
                                     info[5].As<Napi::Number>().Uint32Value(),
                                     side));
  }
  // bids/asks are flat BigInt64Array: [price_raw, qty_raw, price_raw, qty_raw, ...].
  // Reinterpret the buffer as LrvxBookLevel* — layout matches (two int64 per level).
  Napi::Value WriteBook(const Napi::CallbackInfo& info)
  {
    int64_t exchTs = toInt64Ns(info[0]);
    int64_t recvTs = toInt64Ns(info[1]);
    int64_t seq = toInt64Ns(info[2]);
    uint32_t symId = info[3].As<Napi::Number>().Uint32Value();
    // The C writer takes the recorder's byte (1 snapshot, 0 delta, 2 top);
    // updateType is the record byte the readers return (0, 1, 2).
    uint8_t isSnap = info[4].As<Napi::Boolean>().Value() ? 1u : 0u;
    if (info.Length() > 7 && !info[7].IsUndefined())
    {
      // Exactly 0, 1 or 2: no truncation of 1.9, no NaN read as 0.
      const double raw = info[7].IsNumber() ? info[7].As<Napi::Number>().DoubleValue() : -1.0;
      const int32_t updateType = (raw == 0.0 || raw == 1.0 || raw == 2.0) ? static_cast<int32_t>(raw) : -1;
      if (updateType < 0)
      {
        Napi::RangeError::New(info.Env(),
                              "writeBook: updateType takes 0 snapshot, 1 delta or 2 top")
            .ThrowAsJavaScriptException();
        return info.Env().Undefined();
      }
      if (isSnap == 1u && updateType != 0)
      {
        Napi::RangeError::New(info.Env(), "writeBook: isSnapshot true contradicts updateType " +
                                              std::to_string(updateType))
            .ThrowAsJavaScriptException();
        return info.Env().Undefined();
      }
      static constexpr uint8_t kRecorderByte[] = {1u, 0u, 2u};
      isSnap = kRecorderByte[updateType];
    }
    auto bidsArr = info[5].As<Napi::BigInt64Array>();
    auto asksArr = info[6].As<Napi::BigInt64Array>();
    const LrvxBookLevel* bids =
        reinterpret_cast<const LrvxBookLevel*>(bidsArr.Data());
    const LrvxBookLevel* asks =
        reinterpret_cast<const LrvxBookLevel*>(asksArr.Data());
    uint32_t nBids = static_cast<uint32_t>(bidsArr.ElementLength() / 2);
    uint32_t nAsks = static_cast<uint32_t>(asksArr.ElementLength() / 2);
    return Napi::Boolean::New(info.Env(),
                              lrvx_data_writer_write_book(_h, exchTs, recvTs, seq, symId,
                                                          isSnap, bids, nBids, asks, nAsks));
  }
  void Flush(const Napi::CallbackInfo&) { lrvx_data_writer_flush(_h); }
  void Close(const Napi::CallbackInfo&) { lrvx_data_writer_close(_h); }
  Napi::Value Stats(const Napi::CallbackInfo& info)
  {
    auto s = lrvx_data_writer_stats(_h);
    auto o = Napi::Object::New(info.Env());
    o.Set("bytesWritten", (double)s.bytes_written);
    o.Set("eventsWritten", (double)s.events_written);
    o.Set("segmentsCreated", (double)s.segments_created);
    o.Set("tradesWritten", (double)s.trades_written);
    return o;
  }
  LrvxDataWriterHandle _h = nullptr;
};

// ── DataReader ──────────────────────────────────────────────────────

class DataReaderWrap : public Napi::ObjectWrap<DataReaderWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "DataReader",
                       {InstanceAccessor("count", &DataReaderWrap::Count, nullptr),
                        InstanceMethod("summary", &DataReaderWrap::Summary),
                        InstanceMethod("stats", &DataReaderWrap::Stats),
                        InstanceMethod("readTrades", &DataReaderWrap::ReadTrades),
                        InstanceMethod("readTradesFrom", &DataReaderWrap::ReadTradesFrom),
                        InstanceMethod("readBBO", &DataReaderWrap::ReadBBO),
                        InstanceMethod("readBBOFrom", &DataReaderWrap::ReadBBOFrom),
                        InstanceMethod("readBookUpdates", &DataReaderWrap::ReadBookUpdates),
                        InstanceMethod("readBookUpdatesFrom", &DataReaderWrap::ReadBookUpdatesFrom),
                        InstanceMethod("run", &DataReaderWrap::Run)});
  }
  DataReaderWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<DataReaderWrap>(info)
  {
    std::string dir = info[0].As<Napi::String>().Utf8Value();
    int64_t from = info.Length() > 1 && (info[1].IsNumber() || info[1].IsBigInt()) ? toInt64Ns(info[1]) : 0;
    int64_t to = info.Length() > 2 && (info[2].IsNumber() || info[2].IsBigInt()) ? toInt64Ns(info[2]) : 0;
    // TODO: symbol filter array
    int64_t reorderWindowNs = 0;
    int32_t strictOrdering = 0;
    if (info.Length() > 3 && info[3].IsObject())
    {
      Napi::Object opts = info[3].As<Napi::Object>();
      if (opts.Has("reorderWindowNs"))
      {
        Napi::Value v = opts.Get("reorderWindowNs");
        if (v.IsNumber() || v.IsBigInt())
        {
          reorderWindowNs = toInt64Ns(v);
        }
      }
      if (opts.Has("strictOrdering"))
      {
        strictOrdering = opts.Get("strictOrdering").ToBoolean().Value() ? 1 : 0;
      }
    }
    _h = lrvx_data_reader_create_ordered(dir.c_str(), from, to, nullptr, 0, reorderWindowNs,
                                         strictOrdering);
  }
  ~DataReaderWrap()
  {
    if (_h)
    {
      lrvx_data_reader_destroy(_h);
    }
  }

 private:
  Napi::Value Count(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), (double)lrvx_data_reader_count(_h)); }
  Napi::Value Summary(const Napi::CallbackInfo& info)
  {
    auto s = lrvx_data_reader_summary(_h);
    auto o = Napi::Object::New(info.Env());
    o.Set("firstEventNs", (double)s.first_event_ns);
    o.Set("lastEventNs", (double)s.last_event_ns);
    o.Set("totalEvents", (double)s.total_events);
    o.Set("segmentCount", (double)s.segment_count);
    o.Set("totalBytes", (double)s.total_bytes);
    o.Set("durationSeconds", s.duration_seconds);
    return o;
  }
  Napi::Value Stats(const Napi::CallbackInfo& info)
  {
    auto s = lrvx_data_reader_stats(_h);
    auto o = Napi::Object::New(info.Env());
    o.Set("filesRead", (double)s.files_read);
    o.Set("eventsRead", (double)s.events_read);
    o.Set("tradesRead", (double)s.trades_read);
    o.Set("bookUpdatesRead", (double)s.book_updates_read);
    o.Set("bytesRead", (double)s.bytes_read);
    o.Set("crcErrors", (double)s.crc_errors);
    o.Set("lateDropped", (double)s.late_dropped);
    o.Set("unknownFramesSkipped", (double)s.unknown_frames_skipped);
    return o;
  }
  Napi::Value ReadTrades(const Napi::CallbackInfo& info)
  {
    uint64_t maxTrades = info.Length() > 0 && info[0].IsNumber()
                             ? info[0].As<Napi::Number>().Int64Value()
                             : 0;
    return readTradesImpl(info.Env(), /*startTsNs=*/0, /*useFrom=*/false, maxTrades);
  }
  Napi::Value ReadTradesFrom(const Napi::CallbackInfo& info)
  {
    int64_t startTsNs = toInt64Ns(info[0]);
    uint64_t maxTrades = info.Length() > 1 && info[1].IsNumber()
                             ? info[1].As<Napi::Number>().Int64Value()
                             : 0;
    return readTradesImpl(info.Env(), startTsNs, /*useFrom=*/true, maxTrades);
  }
  Napi::Value ReadBBO(const Napi::CallbackInfo& info)
  {
    uint64_t maxEvents = info.Length() > 0 && info[0].IsNumber()
                             ? info[0].As<Napi::Number>().Int64Value()
                             : 0;
    return readBboImpl(info.Env(), /*startTsNs=*/0, /*useFrom=*/false, maxEvents);
  }
  Napi::Value ReadBBOFrom(const Napi::CallbackInfo& info)
  {
    int64_t startTsNs = toInt64Ns(info[0]);
    uint64_t maxEvents = info.Length() > 1 && info[1].IsNumber()
                             ? info[1].As<Napi::Number>().Int64Value()
                             : 0;
    return readBboImpl(info.Env(), startTsNs, /*useFrom=*/true, maxEvents);
  }

 private:
  Napi::Value readTradesImpl(Napi::Env env, int64_t startTsNs, bool useFrom, uint64_t maxTrades)
  {
    if (maxTrades == 0)
    {
      maxTrades = useFrom ? lrvx_data_reader_read_trades_from(_h, startTsNs, nullptr, 0)
                          : lrvx_data_reader_read_trades(_h, nullptr, 0);
    }
    std::vector<LrvxTradeRecord> trades(maxTrades);
    uint64_t n = useFrom
                     ? lrvx_data_reader_read_trades_from(_h, startTsNs, trades.data(), maxTrades)
                     : lrvx_data_reader_read_trades(_h, trades.data(), maxTrades);
    auto arr = Napi::Array::New(env, n);
    for (uint64_t i = 0; i < n; i++)
    {
      auto o = Napi::Object::New(env);
      o.Set("exchangeTsNs", (double)trades[i].exchange_ts_ns);
      o.Set("recvTsNs", (double)trades[i].recv_ts_ns);
      o.Set("price", (double)trades[i].price_raw / 1e8);
      o.Set("qty", (double)trades[i].qty_raw / 1e8);
      o.Set("tradeId", (double)trades[i].trade_id);
      o.Set("symbolId", trades[i].symbol_id);
      // TradeRecord.side is declared "buy" | "sell", not a number.
      o.Set("side", Napi::String::New(env, trades[i].side == 0 ? "buy" : "sell"));
      arr.Set((uint32_t)i, o);
    }
    return arr;
  }

  Napi::Value readBboImpl(Napi::Env env, int64_t startTsNs, bool useFrom, uint64_t maxEvents)
  {
    if (maxEvents == 0)
    {
      maxEvents = useFrom ? lrvx_data_reader_read_bbo_from(_h, startTsNs, nullptr, 0)
                          : lrvx_data_reader_read_bbo(_h, nullptr, 0);
    }
    std::vector<LrvxBBO> bbos(maxEvents);
    uint64_t n = useFrom
                     ? lrvx_data_reader_read_bbo_from(_h, startTsNs, bbos.data(), maxEvents)
                     : lrvx_data_reader_read_bbo(_h, bbos.data(), maxEvents);
    auto arr = Napi::Array::New(env, n);
    for (uint64_t i = 0; i < n; i++)
    {
      auto o = Napi::Object::New(env);
      o.Set("exchangeTsNs", (double)bbos[i].exchange_ts_ns);
      o.Set("recvTsNs", (double)bbos[i].recv_ts_ns);
      o.Set("seq", (double)bbos[i].seq);
      o.Set("symbolId", bbos[i].symbol_id);
      o.Set("eventType", bbos[i].event_type);
      o.Set("bidPrice", (double)bbos[i].bid_price_raw / 1e8);
      o.Set("bidQty", (double)bbos[i].bid_qty_raw / 1e8);
      o.Set("askPrice", (double)bbos[i].ask_price_raw / 1e8);
      o.Set("askQty", (double)bbos[i].ask_qty_raw / 1e8);
      arr.Set((uint32_t)i, o);
    }
    return arr;
  }

 public:
  // Returns an array of book update events. Each event:
  //   { exchangeTsNs, recvTsNs, seq, symbolId, eventType,
  //     bids: [{price, qty}, ...], asks: [{price, qty}, ...] }
  Napi::Value ReadBookUpdates(const Napi::CallbackInfo& info)
  {
    return readBookUpdatesImpl(info.Env(), /*startTsNs=*/0, /*useFrom=*/false);
  }
  Napi::Value ReadBookUpdatesFrom(const Napi::CallbackInfo& info)
  {
    int64_t startTsNs = toInt64Ns(info[0]);
    return readBookUpdatesImpl(info.Env(), startTsNs, /*useFrom=*/true);
  }

 private:
  Napi::Value readBookUpdatesImpl(Napi::Env env, int64_t startTsNs, bool useFrom)
  {
    uint64_t totalLevels = 0;
    uint64_t maxEvents = useFrom
                             ? lrvx_data_reader_count_book_updates_from(_h, startTsNs, &totalLevels)
                             : lrvx_data_reader_count_book_updates(_h, &totalLevels);

    std::vector<LrvxBookUpdateHeader> headers(maxEvents);
    std::vector<LrvxLevel> levels(totalLevels);
    uint64_t n = useFrom
                     ? lrvx_data_reader_read_book_updates_from(_h, startTsNs, headers.data(),
                                                               maxEvents, levels.data(),
                                                               totalLevels)
                     : lrvx_data_reader_read_book_updates(_h, headers.data(), maxEvents,
                                                          levels.data(), totalLevels);

    auto arr = Napi::Array::New(env, n);
    for (uint64_t i = 0; i < n; i++)
    {
      const auto& h = headers[i];
      auto o = Napi::Object::New(env);
      o.Set("exchangeTsNs", (double)h.exchange_ts_ns);
      o.Set("recvTsNs", (double)h.recv_ts_ns);
      o.Set("seq", (double)h.seq);
      o.Set("symbolId", h.symbol_id);
      o.Set("eventType", h.event_type);

      auto bids = Napi::Array::New(env, h.bid_count);
      for (uint16_t k = 0; k < h.bid_count; k++)
      {
        const auto& l = levels[h.level_offset + k];
        auto lo = Napi::Object::New(env);
        lo.Set("price", (double)l.price_raw / 1e8);
        lo.Set("qty", (double)l.qty_raw / 1e8);
        bids.Set((uint32_t)k, lo);
      }
      o.Set("bids", bids);

      auto asks = Napi::Array::New(env, h.ask_count);
      for (uint16_t k = 0; k < h.ask_count; k++)
      {
        const auto& l = levels[h.level_offset + h.bid_count + k];
        auto lo = Napi::Object::New(env);
        lo.Set("price", (double)l.price_raw / 1e8);
        lo.Set("qty", (double)l.qty_raw / 1e8);
        asks.Set((uint32_t)k, lo);
      }
      o.Set("asks", asks);

      arr.Set((uint32_t)i, o);
    }
    return arr;
  }

  // Single-pass streaming aggregator dispatch over the tape. Takes a
  // JS array of aggregator wraps (any mix of the five tape aggregator
  // classes) plus an optional `n_threads` integer (default 1).
  // Unwraps each element to a C ABI handle and dispatches. Returns
  // `true` on success; an empty array is a no-op no-decompression
  // call returning `true`.
  // Accepts either a positional `n_threads` integer (back-compat) or
  // an options object `{ nThreads, onProgress, progressIntervalMs }`.
  // `onProgress(pct, tsNs)` fires from inside the run loop at most
  // once per `progressIntervalMs`; returning a falsy value cancels.
  // Progress is reported only on the single-thread path; multi-thread
  // runs ignore the callback because per-event JS reentry would
  // defeat the parallelism.
  Napi::Value Run(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    if (info.Length() == 0 || !info[0].IsArray())
    {
      Napi::TypeError::New(env, "DataReader.run expects an Array of aggregator wraps")
          .ThrowAsJavaScriptException();
      return env.Null();
    }
    auto js_arr = info[0].As<Napi::Array>();
    std::vector<LrvxAggregatorHandle> handles;
    if (!collectAggregatorHandles(env, js_arr, handles))
    {
      return env.Null();
    }
    uint32_t n_threads = 0;  // 0 = auto: min(segments, hardware_concurrency)
    Napi::Function on_progress;
    bool has_progress = false;
    uint32_t progress_interval_ms = 1000;
    if (info.Length() > 1)
    {
      if (info[1].IsNumber())
      {
        const int32_t v = info[1].As<Napi::Number>().Int32Value();
        n_threads = (v < 0) ? 0u : static_cast<uint32_t>(v);
      }
      else if (info[1].IsObject() && !info[1].IsNull())
      {
        auto opts = info[1].As<Napi::Object>();
        if (opts.Has("nThreads"))
        {
          const int32_t v = opts.Get("nThreads").As<Napi::Number>().Int32Value();
          n_threads = (v < 0) ? 0u : static_cast<uint32_t>(v);
        }
        if (opts.Has("onProgress") && opts.Get("onProgress").IsFunction())
        {
          on_progress = opts.Get("onProgress").As<Napi::Function>();
          has_progress = true;
        }
        if (opts.Has("progressIntervalMs"))
        {
          const int32_t v =
              opts.Get("progressIntervalMs").As<Napi::Number>().Int32Value();
          progress_interval_ms = (v <= 0) ? 1000u : static_cast<uint32_t>(v);
        }
      }
    }

    struct ProgressCtx
    {
      Napi::Env env;
      Napi::Function fn;
      bool cancelled = false;
    } ctx{env, on_progress, false};

    if (has_progress)
    {
      lrvx_data_reader_set_progress_callback(
          _h,
          [](void* user, double pct, int64_t cursor_ts_ns) -> uint8_t
          {
            auto* c = static_cast<ProgressCtx*>(user);
            Napi::HandleScope scope(c->env);
            try
            {
              auto result =
                  c->fn.Call({Napi::Number::New(c->env, pct),
                              Napi::Number::New(c->env,
                                                static_cast<double>(cursor_ts_ns))});
              if (result.IsBoolean() && !result.As<Napi::Boolean>().Value())
              {
                c->cancelled = true;
                return 0;
              }
              return 1;
            }
            catch (const Napi::Error&)
            {
              c->cancelled = true;
              return 0;
            }
          },
          &ctx, progress_interval_ms);
    }
    else
    {
      lrvx_data_reader_clear_progress_callback(_h);
    }

    uint8_t ok = lrvx_data_reader_run(_h, handles.empty() ? nullptr : handles.data(),
                                      static_cast<uint32_t>(handles.size()),
                                      n_threads);
    if (has_progress)
    {
      // Drop the callback so a follow-up run() without options does
      // not inherit the previous progress hook.
      lrvx_data_reader_clear_progress_callback(_h);
    }
    return Napi::Boolean::New(env, ok != 0);
  }

 public:
  LrvxDataReaderHandle _h = nullptr;
};

// ── BinaryLogRecorderHook ───────────────────────────────────────────
//
// Built-in .lrvx recorder. Owns a BinaryLogWriter on the engine
// thread — events stay in C++ on the hot path, no JS callbacks.
// Attach via `runner.setMarketDataRecorder(hook)`.

class BinaryLogRecorderHookWrap : public Napi::ObjectWrap<BinaryLogRecorderHookWrap>
{
 public:
  // Cached at module init; used by RunnerNode::setMarketDataRecorder for
  // an InstanceOf check before unwrapping.
  static Napi::FunctionReference& Ctor()
  {
    static Napi::FunctionReference ref;
    return ref;
  }
  static Napi::Function Init(Napi::Env env)
  {
    Napi::Function fn = DefineClass(env, "BinaryLogRecorderHook",
                                    {InstanceMethod("addSymbol", &BinaryLogRecorderHookWrap::AddSymbol),
                                     InstanceMethod("flush", &BinaryLogRecorderHookWrap::Flush),
                                     InstanceMethod("stats", &BinaryLogRecorderHookWrap::Stats)});
    Ctor() = Napi::Persistent(fn);
    Ctor().SuppressDestruct();
    return fn;
  }
  BinaryLogRecorderHookWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<BinaryLogRecorderHookWrap>(info)
  {
    auto env = info.Env();
    std::string dir = info[0].As<Napi::String>().Utf8Value();
    uint64_t maxMb = info.Length() > 1 && info[1].IsNumber()
                         ? info[1].As<Napi::Number>().Int64Value()
                         : 256;
    uint8_t exId = info.Length() > 2 && info[2].IsNumber()
                       ? info[2].As<Napi::Number>().Uint32Value()
                       : 0;
    uint8_t compression = 0;
    if (info.Length() > 3 && info[3].IsString())
    {
      auto s = info[3].As<Napi::String>().Utf8Value();
      if (s == "none")
      {
        compression = 0;
      }
      else if (s == "lz4")
      {
        compression = 1;
      }
      else
      {
        Napi::TypeError::New(env, "compression must be \"none\" or \"lz4\"")
            .ThrowAsJavaScriptException();
        return;
      }
    }
    std::string exchangeName;
    std::string instrumentType;
    if (info.Length() > 4 && info[4].IsString())
    {
      exchangeName = info[4].As<Napi::String>().Utf8Value();
    }
    if (info.Length() > 5 && info[5].IsString())
    {
      instrumentType = info[5].As<Napi::String>().Utf8Value();
    }
    _h = lrvx_binary_log_recorder_hook_create_ex(
        dir.c_str(), maxMb, exId, compression,
        exchangeName.empty() ? nullptr : exchangeName.c_str(),
        instrumentType.empty() ? nullptr : instrumentType.c_str());
  }
  ~BinaryLogRecorderHookWrap()
  {
    if (_h)
    {
      lrvx_binary_log_recorder_hook_destroy(_h);
    }
  }

  // Borrowed handle for runner attachment. Lifetime tied to this hook.
  LrvxMarketDataRecorderHandle asRecorder() const
  {
    return _h ? lrvx_binary_log_recorder_hook_as_recorder(_h) : nullptr;
  }

 private:
  void AddSymbol(const Napi::CallbackInfo& info)
  {
    lrvx_binary_log_recorder_hook_add_symbol(
        _h,
        info[0].As<Napi::Number>().Uint32Value(),
        info[1].As<Napi::String>().Utf8Value().c_str(),
        info.Length() > 2 ? info[2].As<Napi::String>().Utf8Value().c_str() : "",
        info.Length() > 3 ? info[3].As<Napi::String>().Utf8Value().c_str() : "",
        static_cast<int8_t>(info.Length() > 4 ? info[4].As<Napi::Number>().Int32Value() : 8),
        static_cast<int8_t>(info.Length() > 5 ? info[5].As<Napi::Number>().Int32Value() : 8));
  }
  void Flush(const Napi::CallbackInfo&) { lrvx_binary_log_recorder_hook_flush(_h); }
  Napi::Value Stats(const Napi::CallbackInfo& info)
  {
    auto s = lrvx_binary_log_recorder_hook_stats(_h);
    auto env = info.Env();
    auto o = Napi::Object::New(env);
    o.Set("tradesWritten", Napi::BigInt::New(env, s.trades_written));
    o.Set("bookUpdatesWritten",
          Napi::BigInt::New(env, s.events_written - s.trades_written));
    o.Set("bytesWritten", Napi::BigInt::New(env, s.bytes_written));
    o.Set("segmentsCreated", Napi::BigInt::New(env, s.segments_created));
    o.Set("errors", Napi::BigInt::New(env, uint64_t{0}));
    return o;
  }
  LrvxBinaryLogRecorderHookHandle _h = nullptr;
};

// ── MergedTapeReader ────────────────────────────────────────────────
//
// N-tape merge-on-read. Rekeys per-tape local symbol ids into a global
// id space keyed by (exchange, name). Tape order in the `paths` array
// is load-bearing — ties on identical exchange_ts_ns break by paths
// order. Throws on overlapping book streams for the same symbol; the
// merged book state is otherwise undefined.

class MergedTapeReaderWrap : public Napi::ObjectWrap<MergedTapeReaderWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "MergedTapeReader",
                       {InstanceMethod("symbolTable", &MergedTapeReaderWrap::SymbolTable),
                        InstanceMethod("timeRange", &MergedTapeReaderWrap::TimeRange),
                        InstanceMethod("perTapeStats", &MergedTapeReaderWrap::PerTapeStats),
                        InstanceMethod("readTrades", &MergedTapeReaderWrap::ReadTrades),
                        InstanceMethod("readBooks", &MergedTapeReaderWrap::ReadBooks),
                        InstanceMethod("run", &MergedTapeReaderWrap::Run)});
  }
  MergedTapeReaderWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<MergedTapeReaderWrap>(info)
  {
    auto env = info.Env();
    if (info.Length() < 1 || !info[0].IsArray())
    {
      Napi::TypeError::New(env, "MergedTapeReader: paths must be string[]")
          .ThrowAsJavaScriptException();
      return;
    }
    auto pathsArr = info[0].As<Napi::Array>();
    uint32_t n = pathsArr.Length();
    _pathStorage.reserve(n);
    std::vector<const char*> ptrs;
    ptrs.reserve(n);
    for (uint32_t i = 0; i < n; ++i)
    {
      auto v = pathsArr.Get(i);
      if (!v.IsString())
      {
        Napi::TypeError::New(env, "MergedTapeReader: paths must be string[]")
            .ThrowAsJavaScriptException();
        return;
      }
      _pathStorage.push_back(v.As<Napi::String>().Utf8Value());
      ptrs.push_back(_pathStorage.back().c_str());
    }

    int64_t fromNs = -1;
    int64_t toNs = -1;
    std::vector<uint32_t> filter;
    if (info.Length() > 1 && info[1].IsObject())
    {
      auto opts = info[1].As<Napi::Object>();
      if (opts.Has("fromNs"))
      {
        auto v = opts.Get("fromNs");
        if (!v.IsNull() && !v.IsUndefined())
        {
          fromNs = toInt64Ns(v);
        }
      }
      if (opts.Has("toNs"))
      {
        auto v = opts.Get("toNs");
        if (!v.IsNull() && !v.IsUndefined())
        {
          toNs = toInt64Ns(v);
        }
      }
      if (opts.Has("symbols"))
      {
        auto v = opts.Get("symbols");
        if (v.IsArray())
        {
          auto sa = v.As<Napi::Array>();
          uint32_t m = sa.Length();
          filter.reserve(m);
          for (uint32_t i = 0; i < m; ++i)
          {
            filter.push_back(sa.Get(i).As<Napi::Number>().Uint32Value());
          }
        }
      }
    }

    _h = lrvx_merged_tape_reader_create(
        ptrs.data(), n, fromNs, toNs,
        filter.empty() ? nullptr : filter.data(),
        static_cast<uint32_t>(filter.size()));
    if (!_h)
    {
      Napi::Error::New(env,
                       "MergedTapeReader: invalid paths or overlapping book streams")
          .ThrowAsJavaScriptException();
      return;
    }
  }
  ~MergedTapeReaderWrap()
  {
    if (_h)
    {
      lrvx_merged_tape_reader_destroy(_h);
    }
  }

 private:
  Napi::Value SymbolTable(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    uint32_t n = lrvx_merged_tape_reader_symbol_count(_h);
    std::vector<LrvxMergedSymbol> syms(n);
    uint32_t got = lrvx_merged_tape_reader_get_symbols(_h, syms.data(), n);
    auto arr = Napi::Array::New(env, got);
    for (uint32_t i = 0; i < got; ++i)
    {
      auto o = Napi::Object::New(env);
      o.Set("globalId", syms[i].global_id);
      // exchange / name are borrowed const char* — Napi::String::New copies.
      o.Set("exchange",
            syms[i].exchange ? Napi::String::New(env, syms[i].exchange)
                             : Napi::String::New(env, ""));
      o.Set("name",
            syms[i].name ? Napi::String::New(env, syms[i].name)
                         : Napi::String::New(env, ""));
      o.Set("pricePrecision", (int32_t)syms[i].price_precision);
      o.Set("qtyPrecision", (int32_t)syms[i].qty_precision);
      arr.Set(i, o);
    }
    return arr;
  }

  Napi::Value TimeRange(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    int64_t first = 0;
    int64_t last = 0;
    lrvx_merged_tape_reader_time_range(_h, &first, &last);
    auto arr = Napi::Array::New(env, 2);
    arr.Set((uint32_t)0, Napi::BigInt::New(env, first));
    arr.Set((uint32_t)1, Napi::BigInt::New(env, last));
    return arr;
  }

  Napi::Value PerTapeStats(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    uint32_t n = lrvx_merged_tape_reader_tape_count(_h);
    std::vector<LrvxMergedTapeStats> stats(n);
    uint32_t got = lrvx_merged_tape_reader_get_tape_stats(_h, stats.data(), n);
    auto arr = Napi::Array::New(env, got);
    for (uint32_t i = 0; i < got; ++i)
    {
      auto o = Napi::Object::New(env);
      o.Set("path",
            stats[i].path ? Napi::String::New(env, stats[i].path)
                          : Napi::String::New(env, ""));
      o.Set("trades", Napi::BigInt::New(env, stats[i].trades));
      o.Set("books", Napi::BigInt::New(env, stats[i].books));
      o.Set("firstEventNs", Napi::BigInt::New(env, stats[i].first_event_ns));
      o.Set("lastEventNs", Napi::BigInt::New(env, stats[i].last_event_ns));
      arr.Set(i, o);
    }
    return arr;
  }

  // Mirrors DataReaderWrap.ReadTrades shape: an Array of plain objects
  // with the same field names. symbol_id carries the post-rekey global id.
  Napi::Value ReadTrades(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    uint64_t maxTrades = lrvx_merged_tape_reader_count_trades(_h);
    std::vector<LrvxTradeRecord> trades(maxTrades);
    uint64_t n = lrvx_merged_tape_reader_read_trades(_h, trades.data(), maxTrades);
    auto arr = Napi::Array::New(env, n);
    for (uint64_t i = 0; i < n; ++i)
    {
      auto o = Napi::Object::New(env);
      // Mirrors DataReaderWrap.ReadTrades (above): same TradeRecord
      // interface, same field types. This used to hand back BigInt here
      // while DataReader handed back Number for the same declared
      // `number` fields, so `a.exchangeTsNs === b.exchangeTsNs` was
      // always false and arithmetic between the two readers threw.
      o.Set("exchangeTsNs", (double)trades[i].exchange_ts_ns);
      o.Set("recvTsNs", (double)trades[i].recv_ts_ns);
      o.Set("price", (double)trades[i].price_raw / 1e8);
      o.Set("qty", (double)trades[i].qty_raw / 1e8);
      o.Set("tradeId", (double)trades[i].trade_id);
      o.Set("symbolId", trades[i].symbol_id);
      o.Set("side", Napi::String::New(env, trades[i].side == 0 ? "buy" : "sell"));
      arr.Set((uint32_t)i, o);
    }
    return arr;
  }

  // Mirrors DataReaderWrap.ReadBookUpdates: returns { headers, levels }.
  // Each header carries the global symbol_id; levels[] is flat and
  // sliced by header.levelOffset / bidCount / askCount.
  Napi::Value ReadBooks(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    uint64_t totalLevels = 0;
    uint64_t maxEvents = lrvx_merged_tape_reader_count_books(_h, &totalLevels);
    std::vector<LrvxBookUpdateHeader> headers(maxEvents);
    std::vector<LrvxLevel> levels(totalLevels);
    uint64_t n = lrvx_merged_tape_reader_read_books(_h, headers.data(), maxEvents,
                                                    levels.data(), totalLevels);

    auto arr = Napi::Array::New(env, n);
    for (uint64_t i = 0; i < n; ++i)
    {
      const auto& h = headers[i];
      auto o = Napi::Object::New(env);
      // Mirrors DataReaderWrap.ReadBookUpdates: same BookUpdateRecord
      // interface (`number` fields), same reasoning as ReadTrades above.
      o.Set("exchangeTsNs", (double)h.exchange_ts_ns);
      o.Set("recvTsNs", (double)h.recv_ts_ns);
      o.Set("seq", (double)h.seq);
      o.Set("symbolId", h.symbol_id);
      o.Set("eventType", h.event_type);

      auto bids = Napi::Array::New(env, h.bid_count);
      for (uint16_t k = 0; k < h.bid_count; ++k)
      {
        const auto& l = levels[h.level_offset + k];
        auto lo = Napi::Object::New(env);
        lo.Set("price", (double)l.price_raw / 1e8);
        lo.Set("qty", (double)l.qty_raw / 1e8);
        bids.Set((uint32_t)k, lo);
      }
      o.Set("bids", bids);

      auto asks = Napi::Array::New(env, h.ask_count);
      for (uint16_t k = 0; k < h.ask_count; ++k)
      {
        const auto& l = levels[h.level_offset + h.bid_count + k];
        auto lo = Napi::Object::New(env);
        lo.Set("price", (double)l.price_raw / 1e8);
        lo.Set("qty", (double)l.qty_raw / 1e8);
        asks.Set((uint32_t)k, lo);
      }
      o.Set("asks", asks);

      arr.Set((uint32_t)i, o);
    }
    return arr;
  }

  // Streaming aggregator dispatch over the merged stream. Same semantics
  // as DataReader.run — events are seen with global-rewritten symbol
  // ids; per-tape provenance is not surfaced to aggregators.
  Napi::Value Run(const Napi::CallbackInfo& info)
  {
    auto env = info.Env();
    if (info.Length() == 0 || !info[0].IsArray())
    {
      Napi::TypeError::New(env, "MergedTapeReader.run expects an Array of aggregator wraps")
          .ThrowAsJavaScriptException();
      return env.Null();
    }
    auto js_arr = info[0].As<Napi::Array>();
    std::vector<LrvxAggregatorHandle> handles;
    if (!collectAggregatorHandles(env, js_arr, handles))
    {
      return env.Null();
    }
    // n_threads is reserved on MergedTapeReader for now (single-tape
    // partitioning gets symbol-rekey tangled across workers); accept
    // and pass through but the C ABI ignores it for the merged path.
    uint32_t n_threads = 0;  // 0 = auto: min(segments, hardware_concurrency)
    if (info.Length() > 1 && info[1].IsNumber())
    {
      const int32_t v = info[1].As<Napi::Number>().Int32Value();
      // Negative or zero from JS → auto (0). Otherwise clamp to uint32.
      n_threads = (v < 0) ? 0u : static_cast<uint32_t>(v);
    }
    uint8_t ok = lrvx_merged_tape_reader_run(
        _h, handles.empty() ? nullptr : handles.data(),
        static_cast<uint32_t>(handles.size()), n_threads);
    return Napi::Boolean::New(env, ok != 0);
  }

  LrvxMergedTapeReaderHandle _h = nullptr;
  // Path strings need to outlive lrvx_merged_tape_reader_create — it
  // copies internally but the const char* array must stay valid for the
  // duration of the call.
  std::vector<std::string> _pathStorage;
};

// ── Partitioner ─────────────────────────────────────────────────────

class PartitionerWrap : public Napi::ObjectWrap<PartitionerWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "Partitioner",
                       {InstanceMethod("byTime", &PartitionerWrap::ByTime),
                        InstanceMethod("byDuration", &PartitionerWrap::ByDuration),
                        InstanceMethod("byCalendar", &PartitionerWrap::ByCalendar),
                        InstanceMethod("bySymbol", &PartitionerWrap::BySymbol),
                        InstanceMethod("perSymbol", &PartitionerWrap::PerSymbol),
                        InstanceMethod("byEventCount", &PartitionerWrap::ByEventCount)});
  }
  PartitionerWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<PartitionerWrap>(info)
  {
    _h = lrvx_partitioner_create(info[0].As<Napi::String>().Utf8Value().c_str());
  }
  ~PartitionerWrap()
  {
    if (_h)
    {
      lrvx_partitioner_destroy(_h);
    }
  }

 private:
  Napi::Value toArray(const Napi::CallbackInfo& info, LrvxPartition* parts, uint32_t n)
  {
    auto arr = Napi::Array::New(info.Env(), n);
    for (uint32_t i = 0; i < n; i++)
    {
      auto o = Napi::Object::New(info.Env());
      o.Set("partitionId", parts[i].partition_id);
      o.Set("fromNs", (double)parts[i].from_ns);
      o.Set("toNs", (double)parts[i].to_ns);
      o.Set("warmupFromNs", (double)parts[i].warmup_from_ns);
      o.Set("estimatedEvents", (double)parts[i].estimated_events);
      o.Set("estimatedBytes", (double)parts[i].estimated_bytes);
      arr.Set(i, o);
    }
    return arr;
  }
  Napi::Value ByTime(const Napi::CallbackInfo& info)
  {
    uint32_t n = info[0].As<Napi::Number>().Uint32Value();
    int64_t warmup = info.Length() > 1 ? toInt64Ns(info[1]) : 0;
    uint32_t count = lrvx_partitioner_by_time(_h, n, warmup, nullptr, 0);
    std::vector<LrvxPartition> parts(count);
    lrvx_partitioner_by_time(_h, n, warmup, parts.data(), count);
    return toArray(info, parts.data(), count);
  }
  Napi::Value ByDuration(const Napi::CallbackInfo& info)
  {
    int64_t dur = toInt64Ns(info[0]);
    int64_t warmup = info.Length() > 1 ? toInt64Ns(info[1]) : 0;
    uint32_t count = lrvx_partitioner_by_duration(_h, dur, warmup, nullptr, 0);
    std::vector<LrvxPartition> parts(count);
    lrvx_partitioner_by_duration(_h, dur, warmup, parts.data(), count);
    return toArray(info, parts.data(), count);
  }
  Napi::Value ByCalendar(const Napi::CallbackInfo& info)
  {
    // CalendarUnit is Hour=0, Day=1, Week=2, Month=3
    // (replay/ops/partitioner.h). There used to be no `else`, so "hour"
    // worked by falling through to 0 -- and so did every typo, silently
    // partitioning by hour instead of by month.
    if (!info[0].IsString())
    {
      Napi::TypeError::New(info.Env(),
                           "byCalendar expects a unit string: hour, day, "
                           "week or month")
          .ThrowAsJavaScriptException();
      return info.Env().Undefined();
    }
    std::string u = info[0].As<Napi::String>().Utf8Value();
    uint8_t unit = 0;
    if (u == "hour")
    {
      unit = 0;
    }
    else if (u == "day")
    {
      unit = 1;
    }
    else if (u == "week")
    {
      unit = 2;
    }
    else if (u == "month")
    {
      unit = 3;
    }
    else
    {
      Napi::Error::New(info.Env(),
                       "unknown calendar unit: " + u +
                           ". known: hour, day, week, month")
          .ThrowAsJavaScriptException();
      return info.Env().Undefined();
    }
    int64_t warmup = info.Length() > 1 ? toInt64Ns(info[1]) : 0;
    uint32_t count = lrvx_partitioner_by_calendar(_h, unit, warmup, nullptr, 0);
    std::vector<LrvxPartition> parts(count);
    lrvx_partitioner_by_calendar(_h, unit, warmup, parts.data(), count);
    return toArray(info, parts.data(), count);
  }
  Napi::Value BySymbol(const Napi::CallbackInfo& info)
  {
    uint32_t n = info[0].As<Napi::Number>().Uint32Value();
    uint32_t count = lrvx_partitioner_by_symbol(_h, n, nullptr, 0);
    std::vector<LrvxPartition> parts(count);
    lrvx_partitioner_by_symbol(_h, n, parts.data(), count);
    return toArray(info, parts.data(), count);
  }
  Napi::Value PerSymbol(const Napi::CallbackInfo& info)
  {
    uint32_t count = lrvx_partitioner_per_symbol(_h, nullptr, 0);
    std::vector<LrvxPartition> parts(count);
    lrvx_partitioner_per_symbol(_h, parts.data(), count);
    return toArray(info, parts.data(), count);
  }
  Napi::Value ByEventCount(const Napi::CallbackInfo& info)
  {
    uint32_t n = info[0].As<Napi::Number>().Uint32Value();
    uint32_t count = lrvx_partitioner_by_event_count(_h, n, nullptr, 0);
    std::vector<LrvxPartition> parts(count);
    lrvx_partitioner_by_event_count(_h, n, parts.data(), count);
    return toArray(info, parts.data(), count);
  }
  LrvxPartitionerHandle _h = nullptr;
};

// ── Segment operations ──────────────────────────────────────────────

inline Napi::Value seg_validate(const Napi::CallbackInfo& info)
{
  return Napi::Boolean::New(info.Env(), lrvx_segment_validate(info[0].As<Napi::String>().Utf8Value().c_str()));
}

inline Napi::Value seg_validate_full(const Napi::CallbackInfo& info)
{
  auto r = lrvx_segment_validate_full(info[0].As<Napi::String>().Utf8Value().c_str(), 1, 1);
  auto o = Napi::Object::New(info.Env());
  o.Set("valid", (bool)r.valid);
  o.Set("headerValid", (bool)r.header_valid);
  o.Set("reportedEventCount", (double)r.reported_event_count);
  o.Set("actualEventCount", (double)r.actual_event_count);
  o.Set("hasIndex", (bool)r.has_index);
  o.Set("indexValid", (bool)r.index_valid);
  o.Set("tradesFound", (double)r.trades_found);
  o.Set("bookUpdatesFound", (double)r.book_updates_found);
  o.Set("crcErrors", r.crc_errors);
  o.Set("timestampAnomalies", r.timestamp_anomalies);
  return o;
}

inline Napi::Value seg_validate_dataset(const Napi::CallbackInfo& info)
{
  auto r = lrvx_dataset_validate(info[0].As<Napi::String>().Utf8Value().c_str());
  auto o = Napi::Object::New(info.Env());
  o.Set("valid", (bool)r.valid);
  o.Set("totalSegments", r.total_segments);
  o.Set("validSegments", r.valid_segments);
  o.Set("corruptedSegments", r.corrupted_segments);
  o.Set("totalEvents", (double)r.total_events);
  o.Set("totalBytes", (double)r.total_bytes);
  o.Set("firstTimestamp", (double)r.first_timestamp);
  o.Set("lastTimestamp", (double)r.last_timestamp);
  return o;
}

inline Napi::Value seg_merge(const Napi::CallbackInfo& info)
{
  return Napi::Boolean::New(info.Env(), lrvx_segment_merge(
                                            info[0].As<Napi::String>().Utf8Value().c_str(),
                                            info[1].As<Napi::String>().Utf8Value().c_str()));
}

inline Napi::Value seg_merge_dir(const Napi::CallbackInfo& info)
{
  auto r = lrvx_segment_merge_dir(info[0].As<Napi::String>().Utf8Value().c_str(),
                                  info[1].As<Napi::String>().Utf8Value().c_str());
  auto o = Napi::Object::New(info.Env());
  o.Set("success", (bool)r.success);
  o.Set("segmentsMerged", (double)r.segments_merged);
  o.Set("eventsWritten", (double)r.events_written);
  o.Set("bytesWritten", (double)r.bytes_written);
  return o;
}

inline Napi::Value seg_split(const Napi::CallbackInfo& info)
{
  uint8_t mode = 0;  // time
  std::string m = info.Length() > 2 ? info[2].As<Napi::String>().Utf8Value() : "time";
  if (m == "event_count")
  {
    mode = 1;
  }
  else if (m == "size")
  {
    mode = 2;
  }
  else if (m == "symbol")
  {
    mode = 3;
  }
  auto r = lrvx_segment_split(
      info[0].As<Napi::String>().Utf8Value().c_str(),
      info[1].As<Napi::String>().Utf8Value().c_str(),
      mode,
      info.Length() > 3 ? toInt64Ns(info[3]) : 3600000000000LL,
      info.Length() > 4 ? toInt64Ns(info[4]) : 1000000);
  auto o = Napi::Object::New(info.Env());
  o.Set("success", (bool)r.success);
  o.Set("segmentsCreated", r.segments_created);
  o.Set("eventsWritten", (double)r.events_written);
  return o;
}

inline Napi::Value seg_export(const Napi::CallbackInfo& info)
{
  uint8_t fmt = 0;
  if (info.Length() > 2 && info[2].IsString())
  {
    std::string f = info[2].As<Napi::String>().Utf8Value();
    if (f == "json")
    {
      fmt = 1;
    }
    else if (f == "jsonlines")
    {
      fmt = 2;
    }
    else if (f == "binary")
    {
      fmt = 3;
    }
  }
  auto r = lrvx_segment_export(
      info[0].As<Napi::String>().Utf8Value().c_str(),
      info[1].As<Napi::String>().Utf8Value().c_str(),
      fmt,
      info.Length() > 3 && (info[3].IsNumber() || info[3].IsBigInt()) ? toInt64Ns(info[3]) : 0,
      info.Length() > 4 && (info[4].IsNumber() || info[4].IsBigInt()) ? toInt64Ns(info[4]) : 0,
      nullptr, 0);
  auto o = Napi::Object::New(info.Env());
  o.Set("success", (bool)r.success);
  o.Set("eventsExported", (double)r.events_exported);
  o.Set("bytesWritten", (double)r.bytes_written);
  return o;
}

inline Napi::Value seg_recompress(const Napi::CallbackInfo& info)
{
  uint8_t c = 1;  // lz4
  if (info.Length() > 2)
  {
    std::string s = info[2].As<Napi::String>().Utf8Value();
    if (s == "none")
    {
      c = 0;
    }
  }
  return Napi::Boolean::New(info.Env(), lrvx_segment_recompress(
                                            info[0].As<Napi::String>().Utf8Value().c_str(),
                                            info[1].As<Napi::String>().Utf8Value().c_str(), c));
}

inline Napi::Value seg_extract_symbols(const Napi::CallbackInfo& info)
{
  auto syms = info[2].As<Napi::Uint32Array>();
  return Napi::Number::New(info.Env(), (double)lrvx_segment_extract_symbols(
                                           info[0].As<Napi::String>().Utf8Value().c_str(),
                                           info[1].As<Napi::String>().Utf8Value().c_str(),
                                           syms.Data(), syms.ElementLength()));
}

inline Napi::Value seg_extract_time_range(const Napi::CallbackInfo& info)
{
  return Napi::Number::New(info.Env(), (double)lrvx_segment_extract_time_range(
                                           info[0].As<Napi::String>().Utf8Value().c_str(),
                                           info[1].As<Napi::String>().Utf8Value().c_str(),
                                           toInt64Ns(info[2]),
                                           toInt64Ns(info[3])));
}

// ── Registration ────────────────────────────────────────────────────

inline Napi::Value seg_inspect(const Napi::CallbackInfo& info)
{
  auto h = lrvx_data_reader_create(info[0].As<Napi::String>().Utf8Value().c_str());
  auto s = lrvx_data_reader_summary(h);
  lrvx_data_reader_destroy(h);
  auto o = Napi::Object::New(info.Env());
  o.Set("firstEventNs", (double)s.first_event_ns);
  o.Set("lastEventNs", (double)s.last_event_ns);
  o.Set("totalEvents", (double)s.total_events);
  o.Set("segmentCount", (double)s.segment_count);
  o.Set("totalBytes", (double)s.total_bytes);
  o.Set("durationSeconds", s.duration_seconds);
  return o;
}

inline void registerDataOps(Napi::Env env, Napi::Object exports)
{
  exports.Set("DataWriter", DataWriterWrap::Init(env));
  exports.Set("DataReader", DataReaderWrap::Init(env));
  exports.Set("BinaryLogRecorderHook", BinaryLogRecorderHookWrap::Init(env));
  exports.Set("MergedTapeReader", MergedTapeReaderWrap::Init(env));
  exports.Set("Partitioner", PartitionerWrap::Init(env));

  exports.Set("validateSegment", Napi::Function::New(env, seg_validate));
  exports.Set("validate", Napi::Function::New(env, seg_validate_full));
  exports.Set("validateDataset", Napi::Function::New(env, seg_validate_dataset));
  exports.Set("mergeSegments", Napi::Function::New(env, seg_merge));
  exports.Set("mergeDir", Napi::Function::New(env, seg_merge_dir));
  exports.Set("split", Napi::Function::New(env, seg_split));
  exports.Set("exportData", Napi::Function::New(env, seg_export));
  exports.Set("recompress", Napi::Function::New(env, seg_recompress));
  exports.Set("extractSymbols", Napi::Function::New(env, seg_extract_symbols));
  exports.Set("extractTimeRange", Napi::Function::New(env, seg_extract_time_range));
  exports.Set("inspect", Napi::Function::New(env, seg_inspect));
}

}  // namespace node_lrvx
