// node/src/tape_aggregators.h
//
// NAPI surface for the streaming tape aggregator framework.
// Five ObjectWrap classes thinly wrap the C ABI
// (`lrvx_*_aggregator_create` + `lrvx_*_read_result`); the matching
// `DataReader.run([...])` / `MergedTapeReader.run([...])` methods live
// on the reader wraps in data_ops.h and reach in via the public
// `collectAggregatorHandles` helper here.

#pragma once

#include <napi.h>

#include "lrvx/capi/lrvx_capi.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>

namespace node_lrvx
{

// ── Aggregator wrap helpers ─────────────────────────────────────────

namespace tape_agg_detail
{

inline LrvxAggregatorEventFilter readEventFilter(const Napi::CallbackInfo& info, size_t idx,
                                                 LrvxAggregatorEventFilter default_value)
{
  if (info.Length() <= idx || !info[idx].IsNumber())
  {
    return default_value;
  }
  int32_t raw = info[idx].As<Napi::Number>().Int32Value();
  switch (raw)
  {
    case LRVX_AGG_FILTER_TRADES:
    case LRVX_AGG_FILTER_BOOKS_ONLY:
    case LRVX_AGG_FILTER_BOTH:
      return static_cast<LrvxAggregatorEventFilter>(raw);
    default:
      return default_value;
  }
}

inline std::vector<uint32_t> readSymbolFilter(const Napi::CallbackInfo& info, size_t idx)
{
  std::vector<uint32_t> out;
  if (info.Length() <= idx || !info[idx].IsArray())
  {
    return out;
  }
  auto arr = info[idx].As<Napi::Array>();
  out.reserve(arr.Length());
  for (uint32_t i = 0; i < arr.Length(); ++i)
  {
    Napi::Value v = arr[i];
    // An element that is not a symbol id used to be dropped from the filter,
    // which then let through what the caller meant to keep out.
    out.push_back(argUint32(v, "a symbol id of the filter"));
  }
  return out;
}

inline std::vector<int64_t> readInt64Array(Napi::Array arr)
{
  std::vector<int64_t> out;
  out.reserve(arr.Length());
  for (uint32_t i = 0; i < arr.Length(); ++i)
  {
    Napi::Value v = arr[i];
    // An element of another type used to be dropped, shifting every later
    // one; a BigInt past 64 bits wrapped.
    out.push_back(argInteger(v, "an element of an int64 array"));
  }
  return out;
}

inline std::vector<double> readDoubleArray(Napi::Array arr)
{
  std::vector<double> out;
  out.reserve(arr.Length());
  for (uint32_t i = 0; i < arr.Length(); ++i)
  {
    Napi::Value v = arr[i];
    out.push_back(argNumber(v, "an element of a number array"));
  }
  return out;
}

}  // namespace tape_agg_detail

// ── EventTypeStatsAggregator ────────────────────────────────────────

class EventTypeStatsAggregatorWrap
    : public Napi::ObjectWrap<EventTypeStatsAggregatorWrap>
{
 public:
  static Napi::FunctionReference& Ctor()
  {
    static Napi::FunctionReference ref;
    return ref;
  }
  static Napi::Function Init(Napi::Env env)
  {
    Napi::Function fn = DefineClass(
        env, "EventTypeStatsAggregator",
        {InstanceMethod("result", &EventTypeStatsAggregatorWrap::Result)});
    Ctor() = Napi::Persistent(fn);
    Ctor().SuppressDestruct();
    return fn;
  }

  EventTypeStatsAggregatorWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<EventTypeStatsAggregatorWrap>(info)
  {
    auto filter = tape_agg_detail::readEventFilter(info, 0, LRVX_AGG_FILTER_BOTH);
    auto sf = tape_agg_detail::readSymbolFilter(info, 1);
    _h = lrvx_event_type_stats_aggregator_create(filter, sf.empty() ? nullptr : sf.data(),
                                                 static_cast<uint32_t>(sf.size()));
  }
  ~EventTypeStatsAggregatorWrap()
  {
    if (_h)
    {
      lrvx_aggregator_destroy(_h);
    }
  }

  LrvxAggregatorHandle nativeHandle() const { return _h; }

 private:
  Napi::Value Result(const Napi::CallbackInfo& info)
  {
    uint32_t n = lrvx_event_type_stats_read_result(_h, nullptr, 0);
    std::vector<LrvxEventTypeStatsRow> rows(n);
    if (n > 0)
    {
      lrvx_event_type_stats_read_result(_h, rows.data(), n);
    }
    auto env = info.Env();
    auto arr = Napi::Array::New(env, n);
    for (uint32_t i = 0; i < n; ++i)
    {
      auto o = Napi::Object::New(env);
      o.Set("symbolId", Napi::Number::New(env, rows[i].symbol_id));
      o.Set("trades", Napi::BigInt::New(env, rows[i].trades));
      o.Set("bookSnapshots", Napi::BigInt::New(env, rows[i].book_snapshots));
      o.Set("bookDeltas", Napi::BigInt::New(env, rows[i].book_deltas));
      o.Set("bookTops", Napi::BigInt::New(env, rows[i].book_tops));
      arr.Set(i, o);
    }
    return arr;
  }

  LrvxAggregatorHandle _h = nullptr;
};

// ── BinCountAggregator ──────────────────────────────────────────────

class BinCountAggregatorWrap : public Napi::ObjectWrap<BinCountAggregatorWrap>
{
 public:
  static Napi::FunctionReference& Ctor()
  {
    static Napi::FunctionReference ref;
    return ref;
  }
  static Napi::Function Init(Napi::Env env)
  {
    Napi::Function fn = DefineClass(env, "BinCountAggregator",
                                    {InstanceMethod("result", &BinCountAggregatorWrap::Result)});
    Ctor() = Napi::Persistent(fn);
    Ctor().SuppressDestruct();
    return fn;
  }

  BinCountAggregatorWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<BinCountAggregatorWrap>(info)
  {
    int64_t bucket_ns = 0;
    if (info.Length() > 0)
    {
      if (info[0].IsBigInt())
      {
        bool lossless = false;
        bucket_ns = argBigInt(info[0], "BinCountAggregator: argument 1").Int64Value(&lossless);
      }
      else if (info[0].IsNumber())
      {
        bucket_ns = argInteger(info[0], "BinCountAggregator: argument 1");
      }
    }
    uint8_t by_side = info.Length() > 1 && info[1].IsBoolean() && argBool(info[1], "BinCountAggregator: argument 2");
    uint8_t by_symbol = info.Length() > 2 && info[2].IsBoolean() && argBool(info[2], "BinCountAggregator: argument 3");
    auto filter = tape_agg_detail::readEventFilter(info, 3, LRVX_AGG_FILTER_BOTH);
    auto sf = tape_agg_detail::readSymbolFilter(info, 4);
    _h = lrvx_bin_count_aggregator_create(bucket_ns, by_side, by_symbol, filter,
                                          sf.empty() ? nullptr : sf.data(),
                                          static_cast<uint32_t>(sf.size()));
  }
  ~BinCountAggregatorWrap()
  {
    if (_h)
    {
      lrvx_aggregator_destroy(_h);
    }
  }

  LrvxAggregatorHandle nativeHandle() const { return _h; }

 private:
  Napi::Value Result(const Napi::CallbackInfo& info)
  {
    uint32_t n = lrvx_bin_count_read_result(_h, nullptr, 0);
    std::vector<LrvxBinCountRow> rows(n);
    if (n > 0)
    {
      lrvx_bin_count_read_result(_h, rows.data(), n);
    }
    auto env = info.Env();
    auto arr = Napi::Array::New(env, n);
    for (uint32_t i = 0; i < n; ++i)
    {
      auto o = Napi::Object::New(env);
      o.Set("bucketTsNs", Napi::BigInt::New(env, rows[i].bucket_ts_ns));
      o.Set("symbolId", Napi::Number::New(env, rows[i].symbol_id));
      o.Set("side", Napi::Number::New(env, rows[i].side));
      o.Set("count", Napi::BigInt::New(env, rows[i].count));
      arr.Set(i, o);
    }
    return arr;
  }

  LrvxAggregatorHandle _h = nullptr;
};

// ── VolumeBinAggregator ─────────────────────────────────────────────

class VolumeBinAggregatorWrap : public Napi::ObjectWrap<VolumeBinAggregatorWrap>
{
 public:
  static Napi::FunctionReference& Ctor()
  {
    static Napi::FunctionReference ref;
    return ref;
  }
  static Napi::Function Init(Napi::Env env)
  {
    Napi::Function fn = DefineClass(
        env, "VolumeBinAggregator",
        {InstanceMethod("result", &VolumeBinAggregatorWrap::Result)});
    Ctor() = Napi::Persistent(fn);
    Ctor().SuppressDestruct();
    return fn;
  }

  VolumeBinAggregatorWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<VolumeBinAggregatorWrap>(info)
  {
    int64_t bucket_ns = 0;
    if (info.Length() > 0)
    {
      if (info[0].IsBigInt())
      {
        bool lossless = false;
        bucket_ns = argBigInt(info[0], "VolumeBinAggregator: argument 1").Int64Value(&lossless);
      }
      else if (info[0].IsNumber())
      {
        bucket_ns = argInteger(info[0], "VolumeBinAggregator: argument 1");
      }
    }
    uint8_t by_side = info.Length() > 1 && info[1].IsBoolean() && argBool(info[1], "VolumeBinAggregator: argument 2");
    uint8_t by_symbol = info.Length() > 2 && info[2].IsBoolean() && argBool(info[2], "VolumeBinAggregator: argument 3");
    auto filter = tape_agg_detail::readEventFilter(info, 3, LRVX_AGG_FILTER_TRADES);
    auto sf = tape_agg_detail::readSymbolFilter(info, 4);
    _h = lrvx_volume_bin_aggregator_create(bucket_ns, by_side, by_symbol, filter,
                                           sf.empty() ? nullptr : sf.data(),
                                           static_cast<uint32_t>(sf.size()));
  }
  ~VolumeBinAggregatorWrap()
  {
    if (_h)
    {
      lrvx_aggregator_destroy(_h);
    }
  }

  LrvxAggregatorHandle nativeHandle() const { return _h; }

 private:
  Napi::Value Result(const Napi::CallbackInfo& info)
  {
    uint32_t n = lrvx_volume_bin_read_result(_h, nullptr, 0);
    std::vector<LrvxVolumeBinRow> rows(n);
    if (n > 0)
    {
      lrvx_volume_bin_read_result(_h, rows.data(), n);
    }
    auto env = info.Env();
    auto arr = Napi::Array::New(env, n);
    for (uint32_t i = 0; i < n; ++i)
    {
      auto o = Napi::Object::New(env);
      o.Set("bucketTsNs", Napi::BigInt::New(env, rows[i].bucket_ts_ns));
      o.Set("symbolId", Napi::Number::New(env, rows[i].symbol_id));
      o.Set("side", Napi::Number::New(env, rows[i].side));
      o.Set("qtyRaw", Napi::BigInt::New(env, rows[i].qty_raw));
      arr.Set(i, o);
    }
    return arr;
  }

  LrvxAggregatorHandle _h = nullptr;
};

// ── OHLCBinAggregator ───────────────────────────────────────────────

class OHLCBinAggregatorWrap : public Napi::ObjectWrap<OHLCBinAggregatorWrap>
{
 public:
  static Napi::FunctionReference& Ctor()
  {
    static Napi::FunctionReference ref;
    return ref;
  }
  static Napi::Function Init(Napi::Env env)
  {
    Napi::Function fn = DefineClass(
        env, "OHLCBinAggregator",
        {InstanceMethod("result", &OHLCBinAggregatorWrap::Result)});
    Ctor() = Napi::Persistent(fn);
    Ctor().SuppressDestruct();
    return fn;
  }

  OHLCBinAggregatorWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<OHLCBinAggregatorWrap>(info)
  {
    int64_t bucket_ns = 0;
    if (info.Length() > 0)
    {
      if (info[0].IsBigInt())
      {
        bool lossless = false;
        bucket_ns = argBigInt(info[0], "OHLCBinAggregator: argument 1").Int64Value(&lossless);
      }
      else if (info[0].IsNumber())
      {
        bucket_ns = argInteger(info[0], "OHLCBinAggregator: argument 1");
      }
    }
    uint8_t by_symbol = info.Length() > 1 && info[1].IsBoolean() && argBool(info[1], "OHLCBinAggregator: argument 2");
    auto filter = tape_agg_detail::readEventFilter(info, 2, LRVX_AGG_FILTER_TRADES);
    auto sf = tape_agg_detail::readSymbolFilter(info, 3);
    _h = lrvx_ohlc_bin_aggregator_create(bucket_ns, by_symbol, filter,
                                         sf.empty() ? nullptr : sf.data(),
                                         static_cast<uint32_t>(sf.size()));
  }
  ~OHLCBinAggregatorWrap()
  {
    if (_h)
    {
      lrvx_aggregator_destroy(_h);
    }
  }

  LrvxAggregatorHandle nativeHandle() const { return _h; }

 private:
  Napi::Value Result(const Napi::CallbackInfo& info)
  {
    uint32_t n = lrvx_ohlc_bin_read_result(_h, nullptr, 0);
    std::vector<LrvxOHLCBinRow> rows(n);
    if (n > 0)
    {
      lrvx_ohlc_bin_read_result(_h, rows.data(), n);
    }
    auto env = info.Env();
    auto arr = Napi::Array::New(env, n);
    for (uint32_t i = 0; i < n; ++i)
    {
      auto o = Napi::Object::New(env);
      o.Set("bucketTsNs", Napi::BigInt::New(env, rows[i].bucket_ts_ns));
      o.Set("symbolId", Napi::Number::New(env, rows[i].symbol_id));
      o.Set("openRaw", Napi::BigInt::New(env, rows[i].open_raw));
      o.Set("highRaw", Napi::BigInt::New(env, rows[i].high_raw));
      o.Set("lowRaw", Napi::BigInt::New(env, rows[i].low_raw));
      o.Set("closeRaw", Napi::BigInt::New(env, rows[i].close_raw));
      arr.Set(i, o);
    }
    return arr;
  }

  LrvxAggregatorHandle _h = nullptr;
};

// ── PeakAggregator ──────────────────────────────────────────────────

class PeakAggregatorWrap : public Napi::ObjectWrap<PeakAggregatorWrap>
{
 public:
  static Napi::FunctionReference& Ctor()
  {
    static Napi::FunctionReference ref;
    return ref;
  }
  static Napi::Function Init(Napi::Env env)
  {
    Napi::Function fn = DefineClass(env, "PeakAggregator",
                                    {InstanceMethod("result", &PeakAggregatorWrap::Result)});
    Ctor() = Napi::Persistent(fn);
    Ctor().SuppressDestruct();
    return fn;
  }

  PeakAggregatorWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<PeakAggregatorWrap>(info)
  {
    std::vector<int64_t> windows;
    if (info.Length() > 0 && info[0].IsArray())
    {
      windows = tape_agg_detail::readInt64Array(argArray(info[0], "PeakAggregator: argument 1"));
    }
    uint32_t top_n =
        info.Length() > 1 && info[1].IsNumber() ? argUint32(info[1], "PeakAggregator: argument 2") : 10;
    uint32_t oversample = info.Length() > 2 && info[2].IsNumber()
                              ? argUint32(info[2], "PeakAggregator: argument 3")
                              : 100;
    auto filter = tape_agg_detail::readEventFilter(info, 3, LRVX_AGG_FILTER_TRADES);
    auto sf = tape_agg_detail::readSymbolFilter(info, 4);
    _h = lrvx_peak_aggregator_create(windows.empty() ? nullptr : windows.data(),
                                     static_cast<uint32_t>(windows.size()), top_n,
                                     oversample, filter,
                                     sf.empty() ? nullptr : sf.data(),
                                     static_cast<uint32_t>(sf.size()));
  }
  ~PeakAggregatorWrap()
  {
    if (_h)
    {
      lrvx_aggregator_destroy(_h);
    }
  }

  LrvxAggregatorHandle nativeHandle() const { return _h; }

 private:
  Napi::Value Result(const Napi::CallbackInfo& info)
  {
    uint32_t n = lrvx_peak_read_result(_h, nullptr, 0);
    std::vector<LrvxPeakRow> rows(n);
    if (n > 0)
    {
      lrvx_peak_read_result(_h, rows.data(), n);
    }
    // Result shape: Array<{windowNs: bigint, peaks: Array<{count, startNs}>}>.
    // One outer entry per distinct window scale; peaks within each
    // entry are sorted by count descending (engine guarantees order).
    auto env = info.Env();
    std::vector<int64_t> seen_windows;
    std::vector<Napi::Array> per_window_arrays;
    for (uint32_t i = 0; i < n; ++i)
    {
      auto it = std::find(seen_windows.begin(), seen_windows.end(), rows[i].window_ns);
      Napi::Array arr;
      if (it == seen_windows.end())
      {
        arr = Napi::Array::New(env);
        seen_windows.push_back(rows[i].window_ns);
        per_window_arrays.push_back(arr);
      }
      else
      {
        arr = per_window_arrays[std::distance(seen_windows.begin(), it)];
      }
      auto entry = Napi::Object::New(env);
      entry.Set("count", Napi::BigInt::New(env, rows[i].count));
      entry.Set("startNs", Napi::BigInt::New(env, rows[i].start_ns));
      arr.Set(arr.Length(), entry);
    }
    auto outer = Napi::Array::New(env, seen_windows.size());
    for (uint32_t i = 0; i < seen_windows.size(); ++i)
    {
      auto o = Napi::Object::New(env);
      o.Set("windowNs", Napi::BigInt::New(env, seen_windows[i]));
      o.Set("peaks", per_window_arrays[i]);
      outer.Set(i, o);
    }
    return outer;
  }

  LrvxAggregatorHandle _h = nullptr;
};

// ── QuantileAggregator ──────────────────────────────────────────────

class QuantileAggregatorWrap : public Napi::ObjectWrap<QuantileAggregatorWrap>
{
 public:
  static Napi::FunctionReference& Ctor()
  {
    static Napi::FunctionReference ref;
    return ref;
  }
  static Napi::Function Init(Napi::Env env)
  {
    Napi::Function fn = DefineClass(
        env, "QuantileAggregator",
        {InstanceMethod("result", &QuantileAggregatorWrap::Result)});
    Ctor() = Napi::Persistent(fn);
    Ctor().SuppressDestruct();
    return fn;
  }

  QuantileAggregatorWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<QuantileAggregatorWrap>(info)
  {
    std::vector<int64_t> windows;
    if (info.Length() > 0 && info[0].IsArray())
    {
      windows = tape_agg_detail::readInt64Array(argArray(info[0], "QuantileAggregator: argument 1"));
    }
    std::vector<double> quantiles;
    if (info.Length() > 1 && info[1].IsArray())
    {
      quantiles = tape_agg_detail::readDoubleArray(argArray(info[1], "QuantileAggregator: argument 2"));
    }
    auto filter = tape_agg_detail::readEventFilter(info, 2, LRVX_AGG_FILTER_TRADES);
    auto sf = tape_agg_detail::readSymbolFilter(info, 3);
    _h = lrvx_quantile_aggregator_create(
        windows.empty() ? nullptr : windows.data(), static_cast<uint32_t>(windows.size()),
        quantiles.empty() ? nullptr : quantiles.data(),
        static_cast<uint32_t>(quantiles.size()), filter,
        sf.empty() ? nullptr : sf.data(), static_cast<uint32_t>(sf.size()));
  }
  ~QuantileAggregatorWrap()
  {
    if (_h)
    {
      lrvx_aggregator_destroy(_h);
    }
  }

  LrvxAggregatorHandle nativeHandle() const { return _h; }

 private:
  Napi::Value Result(const Napi::CallbackInfo& info)
  {
    uint32_t n = lrvx_quantile_read_result(_h, nullptr, 0);
    std::vector<LrvxQuantileRow> rows(n);
    if (n > 0)
    {
      lrvx_quantile_read_result(_h, rows.data(), n);
    }
    auto env = info.Env();
    // Result shape: Array<{windowNs: bigint, thresholds:
    // Array<{quantile: number, count: bigint}>}>. One outer entry per
    // distinct window; thresholds within each entry are ordered as
    // the engine returned them (ascending by quantile).
    std::vector<int64_t> seen_windows;
    std::vector<Napi::Array> per_window_arrays;
    for (uint32_t i = 0; i < n; ++i)
    {
      auto it = std::find(seen_windows.begin(), seen_windows.end(), rows[i].window_ns);
      Napi::Array arr;
      if (it == seen_windows.end())
      {
        arr = Napi::Array::New(env);
        seen_windows.push_back(rows[i].window_ns);
        per_window_arrays.push_back(arr);
      }
      else
      {
        arr = per_window_arrays[std::distance(seen_windows.begin(), it)];
      }
      auto entry = Napi::Object::New(env);
      entry.Set("quantile", Napi::Number::New(env, rows[i].quantile));
      entry.Set("count", Napi::BigInt::New(env, rows[i].count));
      arr.Set(arr.Length(), entry);
    }
    auto outer = Napi::Array::New(env, seen_windows.size());
    for (uint32_t i = 0; i < seen_windows.size(); ++i)
    {
      auto o = Napi::Object::New(env);
      o.Set("windowNs", Napi::BigInt::New(env, seen_windows[i]));
      o.Set("thresholds", per_window_arrays[i]);
      outer.Set(i, o);
    }
    return outer;
  }

  LrvxAggregatorHandle _h = nullptr;
};

// ── BookSnapshotBinAggregator ───────────────────────────────────────

class BookSnapshotBinAggregatorWrap
    : public Napi::ObjectWrap<BookSnapshotBinAggregatorWrap>
{
 public:
  static Napi::FunctionReference& Ctor()
  {
    static Napi::FunctionReference ref;
    return ref;
  }
  static Napi::Function Init(Napi::Env env)
  {
    Napi::Function fn = DefineClass(
        env, "BookSnapshotBinAggregator",
        {InstanceMethod("result", &BookSnapshotBinAggregatorWrap::Result)});
    Ctor() = Napi::Persistent(fn);
    Ctor().SuppressDestruct();
    return fn;
  }

  BookSnapshotBinAggregatorWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<BookSnapshotBinAggregatorWrap>(info)
  {
    int64_t bucket_ns = 0;
    if (info.Length() > 0)
    {
      if (info[0].IsBigInt())
      {
        bool lossless = false;
        bucket_ns = argBigInt(info[0], "BookSnapshotBinAggregator: argument 1").Int64Value(&lossless);
      }
      else if (info[0].IsNumber())
      {
        bucket_ns = argInteger(info[0], "BookSnapshotBinAggregator: argument 1");
      }
    }
    uint16_t levels = 20;
    if (info.Length() > 1 && info[1].IsNumber())
    {
      levels = static_cast<uint16_t>(argUint32(info[1], "BookSnapshotBinAggregator: argument 2"));
    }
    auto filter = tape_agg_detail::readEventFilter(info, 2, LRVX_AGG_FILTER_BOOKS_ONLY);
    auto sf = tape_agg_detail::readSymbolFilter(info, 3);
    _h = lrvx_book_snapshot_bin_aggregator_create(
        bucket_ns, levels, filter, sf.empty() ? nullptr : sf.data(),
        static_cast<uint32_t>(sf.size()));
  }
  ~BookSnapshotBinAggregatorWrap()
  {
    if (_h)
    {
      lrvx_aggregator_destroy(_h);
    }
  }

  LrvxAggregatorHandle nativeHandle() const { return _h; }

 private:
  Napi::Value Result(const Napi::CallbackInfo& info)
  {
    uint32_t n = lrvx_book_snapshot_bin_read_result(_h, nullptr, 0);
    std::vector<LrvxBookSnapshotBinRow> rows(n);
    if (n > 0)
    {
      lrvx_book_snapshot_bin_read_result(_h, rows.data(), n);
    }
    auto env = info.Env();
    auto arr = Napi::Array::New(env, n);
    for (uint32_t i = 0; i < n; ++i)
    {
      auto o = Napi::Object::New(env);
      o.Set("bucketTsNs", Napi::BigInt::New(env, rows[i].bucket_ts_ns));
      o.Set("symbolId", Napi::Number::New(env, rows[i].symbol_id));
      o.Set("level", Napi::Number::New(env, rows[i].level));
      o.Set("flags", Napi::Number::New(env, rows[i].flags));
      o.Set("bidPriceRaw", Napi::BigInt::New(env, rows[i].bid_price_raw));
      o.Set("bidQtyRaw", Napi::BigInt::New(env, rows[i].bid_qty_raw));
      o.Set("askPriceRaw", Napi::BigInt::New(env, rows[i].ask_price_raw));
      o.Set("askQtyRaw", Napi::BigInt::New(env, rows[i].ask_qty_raw));
      arr.Set(i, o);
    }
    return arr;
  }

  LrvxAggregatorHandle _h = nullptr;
};

// ── Public helpers + registration ──────────────────────────────────

// Walk a JS array of aggregator wraps and produce a flat list of
// LrvxAggregatorHandle pointers. Returns true on success; sets a JS
// TypeError and returns false if any element isn't one of the five
// recognised wrap classes.
inline bool collectAggregatorHandles(Napi::Env env, Napi::Array js_aggregators,
                                     std::vector<LrvxAggregatorHandle>& out)
{
  out.clear();
  out.reserve(js_aggregators.Length());
  for (uint32_t i = 0; i < js_aggregators.Length(); ++i)
  {
    Napi::Value v = js_aggregators[i];
    if (!v.IsObject())
    {
      Napi::TypeError::New(env, "Aggregator array element is not an object")
          .ThrowAsJavaScriptException();
      return false;
    }
    auto obj = v.As<Napi::Object>();
    LrvxAggregatorHandle h = nullptr;
    if (obj.InstanceOf(EventTypeStatsAggregatorWrap::Ctor().Value()))
    {
      h = Napi::ObjectWrap<EventTypeStatsAggregatorWrap>::Unwrap(obj)->nativeHandle();
    }
    else if (obj.InstanceOf(BinCountAggregatorWrap::Ctor().Value()))
    {
      h = Napi::ObjectWrap<BinCountAggregatorWrap>::Unwrap(obj)->nativeHandle();
    }
    else if (obj.InstanceOf(VolumeBinAggregatorWrap::Ctor().Value()))
    {
      h = Napi::ObjectWrap<VolumeBinAggregatorWrap>::Unwrap(obj)->nativeHandle();
    }
    else if (obj.InstanceOf(OHLCBinAggregatorWrap::Ctor().Value()))
    {
      h = Napi::ObjectWrap<OHLCBinAggregatorWrap>::Unwrap(obj)->nativeHandle();
    }
    else if (obj.InstanceOf(PeakAggregatorWrap::Ctor().Value()))
    {
      h = Napi::ObjectWrap<PeakAggregatorWrap>::Unwrap(obj)->nativeHandle();
    }
    else if (obj.InstanceOf(QuantileAggregatorWrap::Ctor().Value()))
    {
      h = Napi::ObjectWrap<QuantileAggregatorWrap>::Unwrap(obj)->nativeHandle();
    }
    else if (obj.InstanceOf(BookSnapshotBinAggregatorWrap::Ctor().Value()))
    {
      h = Napi::ObjectWrap<BookSnapshotBinAggregatorWrap>::Unwrap(obj)->nativeHandle();
    }
    else
    {
      Napi::TypeError::New(
          env,
          "Element is not a recognised tape aggregator wrap "
          "(EventTypeStats/BinCount/VolumeBin/OHLCBin/Peak/Quantile/BookSnapshotBin)")
          .ThrowAsJavaScriptException();
      return false;
    }
    out.push_back(h);
  }
  return true;
}

inline void registerTapeAggregators(Napi::Env env, Napi::Object exports)
{
  exports.Set("EventTypeStatsAggregator", EventTypeStatsAggregatorWrap::Init(env));
  exports.Set("BinCountAggregator", BinCountAggregatorWrap::Init(env));
  exports.Set("VolumeBinAggregator", VolumeBinAggregatorWrap::Init(env));
  exports.Set("OHLCBinAggregator", OHLCBinAggregatorWrap::Init(env));
  exports.Set("PeakAggregator", PeakAggregatorWrap::Init(env));
  exports.Set("QuantileAggregator", QuantileAggregatorWrap::Init(env));
  exports.Set("BookSnapshotBinAggregator", BookSnapshotBinAggregatorWrap::Init(env));

  // Mirror the C ABI enum values so JS callers can pass them positionally.
  auto filterEnum = Napi::Object::New(env);
  filterEnum.Set("Trades", Napi::Number::New(env, LRVX_AGG_FILTER_TRADES));
  filterEnum.Set("BooksOnly", Napi::Number::New(env, LRVX_AGG_FILTER_BOOKS_ONLY));
  filterEnum.Set("Both", Napi::Number::New(env, LRVX_AGG_FILTER_BOTH));
  exports.Set("AggregatorEventFilter", filterEnum);
}

}  // namespace node_lrvx
