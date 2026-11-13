#include "js_bindings.h"
#include "js_cstring.h"
#include "lrvx/capi/abi_check.hpp"
#include "lrvx/capi/lrvx_capi.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace lrvx
{

// A JS-visible count that turns directly into a C++ allocation size (e.g.
// `new std::vector<T>(count)`) bypasses JS_SetMemoryLimit entirely -- that
// limit only tracks QuickJS's own allocator. Bound every such count against
// this ceiling before it reaches an allocation. Generous enough for any
// realistic strategy use, small enough that even the largest element type
// in this file (a bar, 72 bytes) stays under a gigabyte.
static constexpr uint32_t kLrvxJsMaxAllocCount = 10'000'000;

// ============================================================
// Opaque handle class — safe way to pass a C handle through JS
// ============================================================
//
// A leak audit (triggered by an ASan LeakSanitizer report against
// js_order_group_create, CI-only: leak detection does not run locally on
// this project's development platform) found that this class's finalizer
// had always been nullptr. Every one of the ~35 handle-returning classes
// built on createHandleObject/getHandlePtr shares this one JSClassID, so
// that one nullptr meant every one of them leaked its underlying C object
// whenever a script dropped the wrapper without calling .destroy() --
// exactly the same shape of bug IndicatorGraph had (see the finalized
// handle classes below), just not yet fixed here because there are many
// more of these than there are dedicated classes.
//
// Rather than give each of the ~35 its own JSClassID and finalizer (which
// is what IndicatorGraph/StreamingIndicatorGraph/the feed clock did,
// because each of those needed extra per-type cleanup beyond a single
// destroy call), this class now carries a small per-object entry that
// pairs the handle with the destroy function that owns it. One finalizer
// serves every handle type; createHandleObject's caller supplies the
// right destroy function once, at creation.
struct LrvxHandleEntry
{
  void* handle;
  void (*destroy)(void*);
};

// Total live handle-bearing JS objects of this class: incremented on
// every createHandleObject, decremented exactly once per object either
// by an explicit destroy() or by the finalizer, whichever happens first.
// A leak test can create a batch, drop every JS reference, force a
// collection, and assert this returns to its starting value -- catching
// exactly this class of bug locally, without a sanitizer.
static std::atomic<size_t> g_lrvxHandleLiveCount{0};

size_t lrvxJsHandleLiveCountForTesting()
{
  return g_lrvxHandleLiveCount.load(std::memory_order_relaxed);
}

static JSClassID jsHandleClassId = 0;

static void js_handle_finalizer(JSRuntime*, JSValue val);

static JSClassDef jsHandleClassDef = {
    .class_name = "LrvxHandle",
    .finalizer = js_handle_finalizer,
};

// QuickJS class IDs are allocated from a process-wide counter: JS_NewClassID
// must run exactly once per process, not once per engine. Every
// LrvxJsStrategy construction used to call it again, silently reassigning
// this shared id out from under any other engine that happened to be alive
// at the same time (harmless when engines are strictly sequential, which is
// how every caller used this file until the threading fix below (see
// LrvxJsExecutor) made concurrent engines -- one per executor thread -- a
// normal, live configuration).
// JS_NewClass(rt, id, def) below still runs per-engine: that is what
// actually registers the class in a given runtime's class table.
static std::once_flag jsHandleClassIdOnce;

static void ensureHandleClassId()
{
  std::call_once(jsHandleClassIdOnce, []
                 { JS_NewClassID(&jsHandleClassId); });
}

// `destroyFn` is the C ABI destroy function that owns `handle` (e.g.
// lrvx_book_destroy). Pass nullptr only for a *borrowed* handle that JS
// does not own the lifetime of -- currently just the LrvxStrategyHandle
// LrvxJsStrategy::injectHandle hands to the script, which the C++ side
// (not JS) creates, keeps, and destroys.
JSValue createHandleObject(JSContext* ctx, void* handle, void (*destroyFn)(void*))
{
  JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(jsHandleClassId));
  auto* entry = new LrvxHandleEntry{handle, destroyFn};
  JS_SetOpaque(obj, entry);
  if (destroyFn)
  {
    g_lrvxHandleLiveCount.fetch_add(1, std::memory_order_relaxed);
  }
  return obj;
}

void* getHandlePtr(JSContext* ctx, JSValueConst val)
{
  auto* entry = static_cast<LrvxHandleEntry*>(JS_GetOpaque(val, jsHandleClassId));
  return entry ? entry->handle : nullptr;
}

// Shared destroy() implementation for every handle type built on
// createHandleObject with a real destroy function: every js_X_destroy
// binding below delegates to this rather than repeating the same
// null-check / destroy / free-entry / null-opaque sequence, which is
// exactly the sequence the original per-type bodies were missing the
// last two steps of.
static JSValue js_generic_handle_destroy(JSContext*, JSValueConst argv0)
{
  auto* entry = static_cast<LrvxHandleEntry*>(JS_GetOpaque(argv0, jsHandleClassId));
  if (!entry)
  {
    return JS_UNDEFINED;
  }
  if (entry->handle && entry->destroy)
  {
    entry->destroy(entry->handle);
    g_lrvxHandleLiveCount.fetch_sub(1, std::memory_order_relaxed);
  }
  delete entry;
  JS_SetOpaque(argv0, nullptr);
  return JS_UNDEFINED;
}

static void js_handle_finalizer(JSRuntime*, JSValue val)
{
  auto* entry = static_cast<LrvxHandleEntry*>(JS_GetOpaque(val, jsHandleClassId));
  if (!entry)
  {
    return;
  }
  if (entry->handle && entry->destroy)
  {
    entry->destroy(entry->handle);
    g_lrvxHandleLiveCount.fetch_sub(1, std::memory_order_relaxed);
  }
  delete entry;
}

// ============================================================
// Finalized handle classes — IndicatorGraph, StreamingIndicatorGraph,
// multi-feed clock.
//
// Unlike the generic LrvxHandle above, these three have a real finalizer:
// if a script drops the wrapper object without calling .destroy(), the
// underlying C++ state (and, for the two graphs, the JS function/thisObj
// references each node holds) is still freed when QuickJS collects the
// object, instead of leaking for the life of the process. Explicit
// destroy() still works and is still cheap -- it nulls the opaque pointer,
// which both makes a second destroy() a no-op instead of a double-free and
// makes the finalizer that runs later a no-op instead of a double-free.
static JSClassID jsGraphClassId = 0;
static JSClassID jsStreamingClassId = 0;
static JSClassID jsFeedClockClassId = 0;

static void js_graph_finalizer(JSRuntime* rt, JSValue val);
static void js_streaming_finalizer(JSRuntime* rt, JSValue val);
static void js_feed_clock_finalizer(JSRuntime* rt, JSValue val);

static JSClassDef jsGraphClassDef = {
    .class_name = "LrvxIndicatorGraph",
    .finalizer = js_graph_finalizer,
};
static JSClassDef jsStreamingClassDef = {
    .class_name = "LrvxStreamingIndicatorGraph",
    .finalizer = js_streaming_finalizer,
};
static JSClassDef jsFeedClockClassDef = {
    .class_name = "LrvxFeedClock",
    .finalizer = js_feed_clock_finalizer,
};

static std::once_flag jsGraphClassIdOnce;
static std::once_flag jsFeedClockClassIdOnce;

static void ensureGraphClassIds()
{
  std::call_once(jsGraphClassIdOnce, []
                 {
    JS_NewClassID(&jsGraphClassId);
    JS_NewClassID(&jsStreamingClassId); });
}

static void ensureFeedClockClassId()
{
  std::call_once(jsFeedClockClassIdOnce, []
                 { JS_NewClassID(&jsFeedClockClassId); });
}

// ============================================================
// Helpers
// ============================================================

static void* getHandle(JSContext* ctx, JSValueConst argv0)
{
  return getHandlePtr(ctx, argv0);
}

// These three ignore JS_To*'s return code by design: this file has ~100
// call sites built on them, most for non-money parameters (indicator
// periods, node names as numbers never occur, etc.) where the existing
// behavior -- convert what you can, default the rest to 0/NaN -- is long
// established and changing it everywhere at once is its own risk. What
// they must not do is leave a *pending* exception behind for an unrelated
// later call to trip over: a `valueOf()` that throws sets one, and
// nothing here was reading it, so it now clear it immediately instead.
// Order price/quantity -- the one place this silent-default behavior is
// actually dangerous -- go through toFinitePriceOrThrow /
// toFiniteQtyOrThrow below instead, which propagate the failure.
static void clearStrayException(JSContext* ctx)
{
  JSValue exc = JS_GetException(ctx);
  JS_FreeValue(ctx, exc);
}

static double toDouble(JSContext* ctx, JSValueConst val)
{
  double d = 0;
  if (JS_ToFloat64(ctx, &d, val) < 0)
  {
    clearStrayException(ctx);
    d = 0;
  }
  return d;
}

static uint32_t toUint32(JSContext* ctx, JSValueConst val)
{
  uint32_t u = 0;
  if (JS_ToUint32(ctx, &u, val) < 0)
  {
    clearStrayException(ctx);
    u = 0;
  }
  return u;
}

// JS_ToInt64Ext rather than JS_ToInt64: a nanosecond timestamp arrives from
// JS as a BigInt, and plain JS_ToInt64 would route it through ToNumber,
// which throws on a BigInt. Numbers still convert exactly as before, so an
// int64 parameter now takes either spelling.
static int64_t toInt64(JSContext* ctx, JSValueConst val)
{
  int64_t i = 0;
  if (JS_ToInt64Ext(ctx, &i, val) < 0)
  {
    clearStrayException(ctx);
    i = 0;
  }
  return i;
}

// Order price and quantity used to go through plain toDouble(),
// which turns NaN into 0, +-Infinity into +-INT64_MAX once the C ABI's
// fixed-point conversion saturates it, and a throwing valueOf() into a
// silently-swallowed exception plus a price of 0 -- an order that no real
// exchange would ever accept, submitted without a word to the script. Both
// helpers reject non-finite input and propagate JS_ToFloat64 failures as a
// real thrown TypeError instead of a number.
static bool toFinitePriceOrThrow(JSContext* ctx, JSValueConst val, double& out, const char* label)
{
  if (JS_ToFloat64(ctx, &out, val) < 0)
  {
    return false;  // JS_ToFloat64 already threw (e.g. a throwing valueOf())
  }
  if (!std::isfinite(out))
  {
    JS_ThrowTypeError(ctx, "%s must be a finite number, got %s", label,
                      std::isnan(out) ? "NaN" : (out > 0 ? "Infinity" : "-Infinity"));
    return false;
  }
  return true;
}

static bool toFiniteQtyOrThrow(JSContext* ctx, JSValueConst val, double& out, const char* label)
{
  if (!toFinitePriceOrThrow(ctx, val, out, label))
  {
    return false;
  }
  if (out <= 0.0)
  {
    JS_ThrowTypeError(ctx, "%s must be a finite positive number, got %g", label, out);
    return false;
  }
  return true;
}

#define TO_PRICE_OR_THROW(dst, ctx, val, label)            \
  double dst = 0;                                          \
  if (!toFinitePriceOrThrow((ctx), (val), (dst), (label))) \
  return JS_EXCEPTION

#define TO_QTY_OR_THROW(dst, ctx, val, label)            \
  double dst = 0;                                        \
  if (!toFiniteQtyOrThrow((ctx), (val), (dst), (label))) \
  return JS_EXCEPTION

#define GET_HANDLE_OR_THROW(ctx, argv)                                                    \
  auto h = static_cast<LrvxStrategyHandle>(getHandle((ctx), (argv)[0]));                  \
  if (!h)                                                                                 \
  {                                                                                       \
    return JS_ThrowTypeError((ctx), "Strategy handle is null (not connected to engine)"); \
  }

// ============================================================
// Signal emission bindings
// ============================================================

static JSValue js_emit_market_buy(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  TO_QTY_OR_THROW(qty, ctx, argv[2], "quantity");
  uint64_t oid = lrvx_emit_market_buy(h, sym, lrvx_quantity_from_double(qty));
  return JS_NewFloat64(ctx, static_cast<double>(oid));
}

static JSValue js_emit_market_sell(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  TO_QTY_OR_THROW(qty, ctx, argv[2], "quantity");
  uint64_t oid = lrvx_emit_market_sell(h, sym, lrvx_quantity_from_double(qty));
  return JS_NewFloat64(ctx, static_cast<double>(oid));
}

static JSValue js_emit_limit_buy_tif(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  TO_PRICE_OR_THROW(price, ctx, argv[2], "price");
  TO_QTY_OR_THROW(qty, ctx, argv[3], "quantity");
  uint32_t tif = toUint32(ctx, argv[4]);
  uint64_t oid = lrvx_emit_limit_buy_tif(h, sym, lrvx_price_from_double(price),
                                         lrvx_quantity_from_double(qty),
                                         static_cast<uint8_t>(tif));
  return JS_NewFloat64(ctx, static_cast<double>(oid));
}

static JSValue js_emit_limit_sell_tif(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  TO_PRICE_OR_THROW(price, ctx, argv[2], "price");
  TO_QTY_OR_THROW(qty, ctx, argv[3], "quantity");
  uint32_t tif = toUint32(ctx, argv[4]);
  uint64_t oid = lrvx_emit_limit_sell_tif(h, sym, lrvx_price_from_double(price),
                                          lrvx_quantity_from_double(qty),
                                          static_cast<uint8_t>(tif));
  return JS_NewFloat64(ctx, static_cast<double>(oid));
}

static JSValue js_emit_cancel(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  int64_t oid = toInt64(ctx, argv[1]);
  lrvx_emit_cancel(h, static_cast<uint64_t>(oid));
  return JS_UNDEFINED;
}

static JSValue js_emit_cancel_all(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  lrvx_emit_cancel_all(h, sym);
  return JS_UNDEFINED;
}

static JSValue js_emit_modify(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  int64_t oid = toInt64(ctx, argv[1]);
  TO_PRICE_OR_THROW(price, ctx, argv[2], "price");
  TO_QTY_OR_THROW(qty, ctx, argv[3], "quantity");
  lrvx_emit_modify(h, static_cast<uint64_t>(oid), lrvx_price_from_double(price),
                   lrvx_quantity_from_double(qty));
  return JS_UNDEFINED;
}

static JSValue js_emit_stop_market(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  uint32_t side = toUint32(ctx, argv[2]);
  TO_PRICE_OR_THROW(trigger, ctx, argv[3], "trigger price");
  TO_QTY_OR_THROW(qty, ctx, argv[4], "quantity");
  uint64_t oid = lrvx_emit_stop_market(h, sym, static_cast<uint8_t>(side),
                                       lrvx_price_from_double(trigger),
                                       lrvx_quantity_from_double(qty));
  return JS_NewFloat64(ctx, static_cast<double>(oid));
}

static JSValue js_emit_stop_limit(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  uint32_t side = toUint32(ctx, argv[2]);
  TO_PRICE_OR_THROW(trigger, ctx, argv[3], "trigger price");
  TO_PRICE_OR_THROW(limit, ctx, argv[4], "limit price");
  TO_QTY_OR_THROW(qty, ctx, argv[5], "quantity");
  uint64_t oid = lrvx_emit_stop_limit(h, sym, static_cast<uint8_t>(side),
                                      lrvx_price_from_double(trigger),
                                      lrvx_price_from_double(limit),
                                      lrvx_quantity_from_double(qty));
  return JS_NewFloat64(ctx, static_cast<double>(oid));
}

static JSValue js_emit_take_profit_market(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  uint32_t side = toUint32(ctx, argv[2]);
  TO_PRICE_OR_THROW(trigger, ctx, argv[3], "trigger price");
  TO_QTY_OR_THROW(qty, ctx, argv[4], "quantity");
  uint64_t oid = lrvx_emit_take_profit_market(h, sym, static_cast<uint8_t>(side),
                                              lrvx_price_from_double(trigger),
                                              lrvx_quantity_from_double(qty));
  return JS_NewFloat64(ctx, static_cast<double>(oid));
}

static JSValue js_emit_take_profit_limit(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  uint32_t side = toUint32(ctx, argv[2]);
  TO_PRICE_OR_THROW(trigger, ctx, argv[3], "trigger price");
  TO_PRICE_OR_THROW(limit, ctx, argv[4], "limit price");
  TO_QTY_OR_THROW(qty, ctx, argv[5], "quantity");
  uint64_t oid = lrvx_emit_take_profit_limit(h, sym, static_cast<uint8_t>(side),
                                             lrvx_price_from_double(trigger),
                                             lrvx_price_from_double(limit),
                                             lrvx_quantity_from_double(qty));
  return JS_NewFloat64(ctx, static_cast<double>(oid));
}

static JSValue js_emit_trailing_stop(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  uint32_t side = toUint32(ctx, argv[2]);
  TO_PRICE_OR_THROW(offset, ctx, argv[3], "offset");
  TO_QTY_OR_THROW(qty, ctx, argv[4], "quantity");
  uint64_t oid = lrvx_emit_trailing_stop(h, sym, static_cast<uint8_t>(side),
                                         lrvx_price_from_double(offset),
                                         lrvx_quantity_from_double(qty));
  return JS_NewFloat64(ctx, static_cast<double>(oid));
}

static JSValue js_emit_trailing_stop_pct(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  uint32_t side = toUint32(ctx, argv[2]);
  int32_t bps = static_cast<int32_t>(toInt64(ctx, argv[3]));
  TO_QTY_OR_THROW(qty, ctx, argv[4], "quantity");
  uint64_t oid = lrvx_emit_trailing_stop_percent(h, sym, static_cast<uint8_t>(side), bps,
                                                 lrvx_quantity_from_double(qty));
  return JS_NewFloat64(ctx, static_cast<double>(oid));
}

static JSValue js_emit_close_position(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  uint64_t oid = lrvx_emit_close_position(h, sym);
  return JS_NewFloat64(ctx, static_cast<double>(oid));
}

// ============================================================
// Context query bindings
// ============================================================

static JSValue js_position(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  int64_t raw = lrvx_position_raw(h, sym);
  return JS_NewFloat64(ctx, lrvx_quantity_to_double(raw));
}

static JSValue js_last_trade_price(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  int64_t raw = lrvx_last_trade_price_raw(h, sym);
  return JS_NewFloat64(ctx, lrvx_price_to_double(raw));
}

// null when the side is empty, matching the book bindings below. The raw
// accessors answer 0 for an empty side and for a best quote of exactly 0.0
// alike, and a market that trades through zero reaches that price, so the
// number 0 cannot carry both meanings on the way into JS.
static JSValue js_best_bid(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  int64_t raw = 0;
  if (!lrvx_best_bid_raw_opt(h, sym, &raw))
  {
    return JS_NULL;
  }
  return JS_NewFloat64(ctx, lrvx_price_to_double(raw));
}

static JSValue js_best_ask(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  int64_t raw = 0;
  if (!lrvx_best_ask_raw_opt(h, sym, &raw))
  {
    return JS_NULL;
  }
  return JS_NewFloat64(ctx, lrvx_price_to_double(raw));
}

static JSValue js_mid_price(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  int64_t raw = 0;
  if (!lrvx_mid_price_raw_opt(h, sym, &raw))
  {
    return JS_NULL;
  }
  return JS_NewFloat64(ctx, lrvx_price_to_double(raw));
}

static JSValue js_get_order_status(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  int64_t oid = toInt64(ctx, argv[1]);
  int32_t status = lrvx_get_order_status(h, static_cast<uint64_t>(oid));
  return JS_NewInt32(ctx, status);
}

// ============================================================
// Multi-timeframe alignment helpers
// ============================================================

static JSValue jsBarFromLrvx(JSContext* ctx, const LrvxBar& b)
{
  JSValue obj = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, obj, "open", JS_NewFloat64(ctx, lrvx_price_to_double(b.open_raw)));
  JS_SetPropertyStr(ctx, obj, "high", JS_NewFloat64(ctx, lrvx_price_to_double(b.high_raw)));
  JS_SetPropertyStr(ctx, obj, "low", JS_NewFloat64(ctx, lrvx_price_to_double(b.low_raw)));
  JS_SetPropertyStr(ctx, obj, "close", JS_NewFloat64(ctx, lrvx_price_to_double(b.close_raw)));
  JS_SetPropertyStr(ctx, obj, "volume",
                    JS_NewFloat64(ctx, lrvx_quantity_to_double(b.volume_raw)));
  JS_SetPropertyStr(ctx, obj, "startNs",
                    JS_NewBigInt64(ctx, static_cast<int64_t>(b.start_time_ns)));
  JS_SetPropertyStr(ctx, obj, "endNs",
                    JS_NewBigInt64(ctx, static_cast<int64_t>(b.end_time_ns)));
  return obj;
}

static JSValue js_strategy_last_closed_bar(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  uint32_t bt = toUint32(ctx, argv[2]);
  uint64_t param = static_cast<uint64_t>(toInt64(ctx, argv[3]));
  LrvxBar bar{};
  uint8_t ok = lrvx_strategy_last_closed_bar(h, sym, static_cast<uint8_t>(bt), param, &bar);
  if (!ok)
  {
    return JS_NULL;
  }
  return jsBarFromLrvx(ctx, bar);
}

static JSValue js_strategy_last_n_closed_bars(JSContext* ctx, JSValueConst, int,
                                              JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  uint32_t bt = toUint32(ctx, argv[2]);
  uint64_t param = static_cast<uint64_t>(toInt64(ctx, argv[3]));
  uint32_t n = toUint32(ctx, argv[4]);
  // `n` comes straight from JS and used to size this vector with no
  // cap, bypassing the engine's own memory limit (that limit only tracks
  // QuickJS's allocator, not a plain `new`). A bogus or malicious `n`
  // could otherwise request hundreds of gigabytes from one call.
  if (n > kLrvxJsMaxAllocCount)
  {
    return JS_ThrowTypeError(ctx, "lastNClosedBars: n=%u exceeds the maximum of %u", n,
                             kLrvxJsMaxAllocCount);
  }
  std::vector<LrvxBar> bars(n);
  uint32_t got = lrvx_strategy_last_n_closed_bars(h, sym, static_cast<uint8_t>(bt), param,
                                                  bars.data(), n);
  JSValue arr = JS_NewArray(ctx);
  for (uint32_t i = 0; i < got; ++i)
  {
    JS_SetPropertyUint32(ctx, arr, i, jsBarFromLrvx(ctx, bars[i]));
  }
  return arr;
}

static JSValue js_strategy_get_bar_ring_capacity(JSContext* ctx, JSValueConst, int,
                                                 JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  return JS_NewUint32(ctx, lrvx_strategy_get_bar_ring_capacity(h));
}

static JSValue js_strategy_set_bar_ring_capacity(JSContext* ctx, JSValueConst, int,
                                                 JSValueConst* argv)
{
  GET_HANDLE_OR_THROW(ctx, argv);
  uint32_t cap = toUint32(ctx, argv[1]);
  lrvx_strategy_set_bar_ring_capacity(h, cap);
  return JS_UNDEFINED;
}

// ============================================================
// Multi-feed clock
//
// This used to hand the raw heap pointer straight back to the script as
// a BigInt (visible in plaintext, defeating ASLR) and accept any BigInt
// back as a handle with no validation -- `destroy(12345n)` dereferenced
// address 0x3039. It also had no way to free one at all: the
// JS facade (quickjs/lrvx/feed_clock.js) never called
// __lrvx_feed_clock_destroy, so every clock leaked unconditionally. Both
// are fixed the same way every other handle-shaped binding in this file
// is: an opaque JSValue tagged with its own class id, `destroy()` on the
// facade, and nulling the opaque pointer on free so a second free (or a
// use after it) is a safe no-op instead of memory corruption.
// ============================================================

static void* getFeedClockPtr(JSValueConst val)
{
  return JS_GetOpaque(val, jsFeedClockClassId);
}

#define GET_FEED_CLOCK_OR_THROW(ctx, argv)                                             \
  auto* fc = getFeedClockPtr((argv)[0]);                                               \
  if (!fc)                                                                             \
  {                                                                                    \
    return JS_ThrowTypeError((ctx), "feed clock handle is null (already destroyed?)"); \
  }

static JSValue js_feed_clock_create(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  if (argc < 6 || !JS_IsArray(ctx, argv[0]))
  {
    return JS_ThrowTypeError(ctx, "feed_clock_create(symbols[], count, policy, timeoutMs, leader, budgetMs)");
  }
  uint32_t count = toUint32(ctx, argv[1]);
  uint32_t arrLen = 0;
  {
    JSValue lenVal = JS_GetPropertyStr(ctx, argv[0], "length");
    JS_ToUint32(ctx, &arrLen, lenVal);
    JS_FreeValue(ctx, lenVal);
  }
  // `count` is a caller-supplied number independent of the actual
  // array, and used to size a plain `std::vector` -- which bypasses
  // JS_SetMemoryLimit entirely. A mismatched count either reads garbage
  // past the array (for legitimate-looking calls) or requests an
  // unbounded allocation (11 GB and climbing from one line of script,
  // confirmed). Require it to match what was actually passed.
  if (count > arrLen)
  {
    return JS_ThrowTypeError(
        ctx, "feed_clock_create: count (%u) exceeds symbols.length (%u)", count, arrLen);
  }
  std::vector<uint32_t> sv(count);
  for (uint32_t i = 0; i < count; ++i)
  {
    JSValue v = JS_GetPropertyUint32(ctx, argv[0], i);
    sv[i] = toUint32(ctx, v);
    JS_FreeValue(ctx, v);
  }
  uint32_t policy = toUint32(ctx, argv[2]);
  int64_t timeout = toInt64(ctx, argv[3]);
  uint32_t leader = toUint32(ctx, argv[4]);
  int64_t budget = toInt64(ctx, argv[5]);
  void* h = lrvx_feed_clock_create(sv.data(), count, static_cast<uint8_t>(policy), timeout,
                                   leader, budget);
  JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(jsFeedClockClassId));
  JS_SetOpaque(obj, h);
  return obj;
}

static JSValue js_feed_clock_destroy(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  void* h = getFeedClockPtr(argv[0]);
  if (!h)
  {
    return JS_UNDEFINED;  // already destroyed (or never created) -- no-op, not a crash
  }
  lrvx_feed_clock_destroy(h);
  JS_SetOpaque(argv[0], nullptr);
  return JS_UNDEFINED;
}

static JSValue js_feed_clock_symbol_count(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_FEED_CLOCK_OR_THROW(ctx, argv);
  return JS_NewUint32(ctx, lrvx_feed_clock_symbol_count(fc));
}

static JSValue js_feed_clock_symbol_at(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_FEED_CLOCK_OR_THROW(ctx, argv);
  uint32_t idx = toUint32(ctx, argv[1]);
  return JS_NewUint32(ctx, lrvx_feed_clock_symbol_at(fc, idx));
}

static JSValue js_feed_clock_tick(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_FEED_CLOCK_OR_THROW(ctx, argv);
  int64_t ts = toInt64(ctx, argv[1]);
  uint32_t sym = toUint32(ctx, argv[2]);
  return JS_NewUint32(ctx, lrvx_feed_clock_tick(fc, ts, sym));
}

static JSValue js_feed_clock_last_fired(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_FEED_CLOCK_OR_THROW(ctx, argv);
  return JS_NewUint32(ctx, lrvx_feed_clock_last_fired(fc));
}

static JSValue js_feed_clock_last_triggered_by(JSContext* ctx, JSValueConst, int,
                                               JSValueConst* argv)
{
  GET_FEED_CLOCK_OR_THROW(ctx, argv);
  return JS_NewUint32(ctx, lrvx_feed_clock_last_triggered_by(fc));
}

static JSValue js_feed_clock_last_seen_at(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_FEED_CLOCK_OR_THROW(ctx, argv);
  uint32_t idx = toUint32(ctx, argv[1]);
  return JS_NewBigInt64(ctx, lrvx_feed_clock_last_seen_at(fc, idx));
}

static JSValue js_feed_clock_staleness_at(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_FEED_CLOCK_OR_THROW(ctx, argv);
  uint32_t idx = toUint32(ctx, argv[1]);
  return JS_NewBigInt64(ctx, lrvx_feed_clock_staleness_at(fc, idx));
}

static JSValue js_feed_clock_reset(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_FEED_CLOCK_OR_THROW(ctx, argv);
  lrvx_feed_clock_reset(fc);
  return JS_UNDEFINED;
}

static void js_feed_clock_finalizer(JSRuntime*, JSValue val)
{
  void* h = JS_GetOpaque(val, jsFeedClockClassId);
  if (h)
  {
    lrvx_feed_clock_destroy(h);
  }
}

// ============================================================
// Batch indicators
// ============================================================

static JSValue jsArrayFromDoubles(JSContext* ctx, const std::vector<double>& data)
{
  JSValue arr = JS_NewArray(ctx);
  for (size_t i = 0; i < data.size(); i++)
  {
    JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), JS_NewFloat64(ctx, data[i]));
  }
  return arr;
}

static std::vector<double> jsArrayToDoubles(JSContext* ctx, JSValueConst arr)
{
  JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
  uint32_t len = 0;
  JS_ToUint32(ctx, &len, lenVal);
  JS_FreeValue(ctx, lenVal);

  std::vector<double> result(len);
  for (uint32_t i = 0; i < len; i++)
  {
    JSValue elem = JS_GetPropertyUint32(ctx, arr, i);
    JS_ToFloat64(ctx, &result[i], elem);
    JS_FreeValue(ctx, elem);
  }
  return result;
}

// A registry batch call whose integer arguments are below their floor
// (lrvx/indicator/params.h) writes nothing and records E_IND_001; it is
// thrown here, as the streaming constructor throws the same refusal, instead
// of handing back an array nobody wrote. The error is cleared first because
// a successful call does not clear it.
#define LRVX_JS_INDICATOR_CALL(ctx, ...)                                \
  do                                                                    \
  {                                                                     \
    lrvx_clear_last_error();                                            \
    __VA_ARGS__;                                                        \
    if (lrvx_last_error_code() != 0)                                    \
    {                                                                   \
      return JS_ThrowTypeError((ctx), "%s", lrvx_last_error_message()); \
    }                                                                   \
  } while (0)

static JSValue js_indicator_sma(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto data = jsArrayToDoubles(ctx, argv[0]);
  uint32_t period = toUint32(ctx, argv[1]);
  std::vector<double> output(data.size());
  LRVX_JS_INDICATOR_CALL(ctx, lrvx_indicator_sma(data.data(), data.size(), period, output.data()));
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_ema(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto data = jsArrayToDoubles(ctx, argv[0]);
  uint32_t period = toUint32(ctx, argv[1]);
  std::vector<double> output(data.size());
  LRVX_JS_INDICATOR_CALL(ctx, lrvx_indicator_ema(data.data(), data.size(), period, output.data()));
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_rsi(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto data = jsArrayToDoubles(ctx, argv[0]);
  uint32_t period = toUint32(ctx, argv[1]);
  std::vector<double> output(data.size());
  LRVX_JS_INDICATOR_CALL(ctx, lrvx_indicator_rsi(data.data(), data.size(), period, output.data()));
  return jsArrayFromDoubles(ctx, output);
}

// Every high/low/close indicator below used to take its length
// from `high` alone and hand the other two arrays' `.data()` pointers to
// the C function unchecked. A mismatched `low`/`close` of size 0 gives a
// null `.data()` and a segfault inside the indicator; a mismatch that is
// merely *shorter* than `high` is worse -- a silent out-of-bounds read
// that produces a plausible-looking number instead of crashing.
static bool checkSameLength3(JSContext* ctx, size_t a, size_t b, size_t c, const char* fnName)
{
  if (a != b || a != c)
  {
    JS_ThrowTypeError(ctx, "%s: high/low/close must have the same length (got %zu/%zu/%zu)",
                      fnName, a, b, c);
    return false;
  }
  return true;
}

static JSValue js_indicator_atr(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto high = jsArrayToDoubles(ctx, argv[0]);
  auto low = jsArrayToDoubles(ctx, argv[1]);
  auto close = jsArrayToDoubles(ctx, argv[2]);
  if (!checkSameLength3(ctx, high.size(), low.size(), close.size(), "atr"))
  {
    return JS_EXCEPTION;
  }
  uint32_t period = toUint32(ctx, argv[3]);
  size_t len = high.size();
  std::vector<double> output(len);
  LRVX_JS_INDICATOR_CALL(ctx, lrvx_indicator_atr(high.data(), low.data(), close.data(), len, period, output.data()));
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_macd(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  auto data = jsArrayToDoubles(ctx, argv[0]);
  uint32_t fast = (argc > 1) ? toUint32(ctx, argv[1]) : 12;
  uint32_t slow = (argc > 2) ? toUint32(ctx, argv[2]) : 26;
  uint32_t signal = (argc > 3) ? toUint32(ctx, argv[3]) : 9;
  size_t len = data.size();
  std::vector<double> macdOut(len), signalOut(len), histOut(len);
  LRVX_JS_INDICATOR_CALL(ctx, lrvx_indicator_macd(data.data(), len, fast, slow, signal, macdOut.data(), signalOut.data(),
                                                  histOut.data()));
  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "line", jsArrayFromDoubles(ctx, macdOut));
  JS_SetPropertyStr(ctx, result, "signal", jsArrayFromDoubles(ctx, signalOut));
  JS_SetPropertyStr(ctx, result, "histogram", jsArrayFromDoubles(ctx, histOut));
  return result;
}

static JSValue js_indicator_bollinger(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  auto data = jsArrayToDoubles(ctx, argv[0]);
  uint32_t period = (argc > 1) ? toUint32(ctx, argv[1]) : 20;
  double multiplier = (argc > 2) ? toDouble(ctx, argv[2]) : 2.0;
  size_t len = data.size();
  std::vector<double> upper(len), middle(len), lower(len);
  LRVX_JS_INDICATOR_CALL(ctx, lrvx_indicator_bollinger(data.data(), len, period, multiplier, upper.data(), middle.data(),
                                                       lower.data()));
  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "upper", jsArrayFromDoubles(ctx, upper));
  JS_SetPropertyStr(ctx, result, "middle", jsArrayFromDoubles(ctx, middle));
  JS_SetPropertyStr(ctx, result, "lower", jsArrayFromDoubles(ctx, lower));
  return result;
}

static JSValue js_indicator_rma(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto data = jsArrayToDoubles(ctx, argv[0]);
  uint32_t period = toUint32(ctx, argv[1]);
  std::vector<double> output(data.size());
  LRVX_JS_INDICATOR_CALL(ctx, lrvx_indicator_rma(data.data(), data.size(), period, output.data()));
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_dema(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto data = jsArrayToDoubles(ctx, argv[0]);
  uint32_t period = toUint32(ctx, argv[1]);
  std::vector<double> output(data.size());
  LRVX_JS_INDICATOR_CALL(ctx, lrvx_indicator_dema(data.data(), data.size(), period, output.data()));
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_tema(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto data = jsArrayToDoubles(ctx, argv[0]);
  uint32_t period = toUint32(ctx, argv[1]);
  std::vector<double> output(data.size());
  LRVX_JS_INDICATOR_CALL(ctx, lrvx_indicator_tema(data.data(), data.size(), period, output.data()));
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_kama(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  auto data = jsArrayToDoubles(ctx, argv[0]);
  uint32_t period = toUint32(ctx, argv[1]);
  uint32_t fast = (argc > 2) ? toUint32(ctx, argv[2]) : 2;
  uint32_t slow = (argc > 3) ? toUint32(ctx, argv[3]) : 30;
  std::vector<double> output(data.size());
  LRVX_JS_INDICATOR_CALL(ctx, lrvx_indicator_kama(data.data(), data.size(), period, fast, slow, output.data()));
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_slope(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto data = jsArrayToDoubles(ctx, argv[0]);
  uint32_t length = toUint32(ctx, argv[1]);
  std::vector<double> output(data.size());
  LRVX_JS_INDICATOR_CALL(ctx, lrvx_indicator_slope(data.data(), data.size(), length, output.data()));
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_adx(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto high = jsArrayToDoubles(ctx, argv[0]);
  auto low = jsArrayToDoubles(ctx, argv[1]);
  auto close = jsArrayToDoubles(ctx, argv[2]);
  if (!checkSameLength3(ctx, high.size(), low.size(), close.size(), "adx"))
  {
    return JS_EXCEPTION;
  }
  uint32_t period = toUint32(ctx, argv[3]);
  size_t len = high.size();
  std::vector<double> adxOut(len), plusDi(len), minusDi(len);
  lrvx_indicator_adx(high.data(), low.data(), close.data(), len, period, adxOut.data(),
                     plusDi.data(), minusDi.data());
  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "adx", jsArrayFromDoubles(ctx, adxOut));
  JS_SetPropertyStr(ctx, result, "plusDi", jsArrayFromDoubles(ctx, plusDi));
  JS_SetPropertyStr(ctx, result, "minusDi", jsArrayFromDoubles(ctx, minusDi));
  return result;
}

static JSValue js_indicator_cci(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto high = jsArrayToDoubles(ctx, argv[0]);
  auto low = jsArrayToDoubles(ctx, argv[1]);
  auto close = jsArrayToDoubles(ctx, argv[2]);
  if (!checkSameLength3(ctx, high.size(), low.size(), close.size(), "cci"))
  {
    return JS_EXCEPTION;
  }
  uint32_t period = toUint32(ctx, argv[3]);
  size_t len = high.size();
  std::vector<double> output(len);
  LRVX_JS_INDICATOR_CALL(ctx, lrvx_indicator_cci(high.data(), low.data(), close.data(), len, period, output.data()));
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_stochastic(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  auto high = jsArrayToDoubles(ctx, argv[0]);
  auto low = jsArrayToDoubles(ctx, argv[1]);
  auto close = jsArrayToDoubles(ctx, argv[2]);
  if (!checkSameLength3(ctx, high.size(), low.size(), close.size(), "stochastic"))
  {
    return JS_EXCEPTION;
  }
  uint32_t kPeriod = (argc > 3) ? toUint32(ctx, argv[3]) : 14;
  uint32_t dPeriod = (argc > 4) ? toUint32(ctx, argv[4]) : 3;
  size_t len = high.size();
  std::vector<double> kOut(len), dOut(len);
  LRVX_JS_INDICATOR_CALL(ctx, lrvx_indicator_stochastic(high.data(), low.data(), close.data(), len, kPeriod, dPeriod,
                                                        kOut.data(), dOut.data()));
  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "k", jsArrayFromDoubles(ctx, kOut));
  JS_SetPropertyStr(ctx, result, "d", jsArrayFromDoubles(ctx, dOut));
  return result;
}

static JSValue js_indicator_chop(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto high = jsArrayToDoubles(ctx, argv[0]);
  auto low = jsArrayToDoubles(ctx, argv[1]);
  auto close = jsArrayToDoubles(ctx, argv[2]);
  uint32_t period = toUint32(ctx, argv[3]);
  size_t len = high.size();
  std::vector<double> output(len);
  LRVX_JS_INDICATOR_CALL(ctx, lrvx_indicator_chop(high.data(), low.data(), close.data(), len, period, output.data()));
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_obv(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto close = jsArrayToDoubles(ctx, argv[0]);
  auto volume = jsArrayToDoubles(ctx, argv[1]);
  size_t len = close.size();
  std::vector<double> output(len);
  lrvx_indicator_obv(close.data(), volume.data(), len, output.data());
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_vwap(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto close = jsArrayToDoubles(ctx, argv[0]);
  auto volume = jsArrayToDoubles(ctx, argv[1]);
  uint32_t window = toUint32(ctx, argv[2]);
  size_t len = close.size();
  std::vector<double> output(len);
  lrvx_indicator_vwap(close.data(), volume.data(), len, window, output.data());
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_cvd(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto open = jsArrayToDoubles(ctx, argv[0]);
  auto high = jsArrayToDoubles(ctx, argv[1]);
  auto low = jsArrayToDoubles(ctx, argv[2]);
  auto close = jsArrayToDoubles(ctx, argv[3]);
  auto volume = jsArrayToDoubles(ctx, argv[4]);
  size_t len = open.size();
  std::vector<double> output(len);
  lrvx_indicator_cvd(open.data(), high.data(), low.data(), close.data(), volume.data(), len,
                     output.data());
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_skewness(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto data = jsArrayToDoubles(ctx, argv[0]);
  uint32_t period = toUint32(ctx, argv[1]);
  std::vector<double> output(data.size());
  LRVX_JS_INDICATOR_CALL(ctx, lrvx_indicator_skewness(data.data(), data.size(), period, output.data()));
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_kurtosis(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto data = jsArrayToDoubles(ctx, argv[0]);
  uint32_t period = toUint32(ctx, argv[1]);
  std::vector<double> output(data.size());
  LRVX_JS_INDICATOR_CALL(ctx, lrvx_indicator_kurtosis(data.data(), data.size(), period, output.data()));
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_rolling_zscore(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto data = jsArrayToDoubles(ctx, argv[0]);
  uint32_t period = toUint32(ctx, argv[1]);
  std::vector<double> output(data.size());
  LRVX_JS_INDICATOR_CALL(ctx, lrvx_indicator_rolling_zscore(data.data(), data.size(), period, output.data()));
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_shannon_entropy(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto data = jsArrayToDoubles(ctx, argv[0]);
  uint32_t period = toUint32(ctx, argv[1]);
  uint32_t bins = toUint32(ctx, argv[2]);
  std::vector<double> output(data.size());
  LRVX_JS_INDICATOR_CALL(ctx, lrvx_indicator_shannon_entropy(data.data(), data.size(), period, bins, output.data()));
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_parkinson_vol(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto high = jsArrayToDoubles(ctx, argv[0]);
  auto low = jsArrayToDoubles(ctx, argv[1]);
  uint32_t period = toUint32(ctx, argv[2]);
  size_t len = high.size();
  std::vector<double> output(len);
  LRVX_JS_INDICATOR_CALL(ctx, lrvx_indicator_parkinson_vol(high.data(), low.data(), len, period, output.data()));
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_rogers_satchell_vol(JSContext* ctx, JSValueConst, int,
                                                JSValueConst* argv)
{
  auto open = jsArrayToDoubles(ctx, argv[0]);
  auto high = jsArrayToDoubles(ctx, argv[1]);
  auto low = jsArrayToDoubles(ctx, argv[2]);
  auto close = jsArrayToDoubles(ctx, argv[3]);
  uint32_t period = toUint32(ctx, argv[4]);
  size_t len = open.size();
  std::vector<double> output(len);
  LRVX_JS_INDICATOR_CALL(ctx, lrvx_indicator_rogers_satchell_vol(open.data(), high.data(), low.data(), close.data(), len,
                                                                 period, output.data()));
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_correlation(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto x = jsArrayToDoubles(ctx, argv[0]);
  auto y = jsArrayToDoubles(ctx, argv[1]);
  uint32_t period = toUint32(ctx, argv[2]);
  size_t len = x.size();
  std::vector<double> output(len);
  LRVX_JS_INDICATOR_CALL(ctx, lrvx_indicator_correlation(x.data(), y.data(), len, period, output.data()));
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_indicator_adf(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  auto input = jsArrayToDoubles(ctx, argv[0]);
  uint32_t maxLag = argc > 1 ? toUint32(ctx, argv[1]) : 4;
  lrvx::JsCString regArg = lrvx::jsOptionalCString(ctx, argc, argv, 2);
  if (regArg.present() && !regArg.ok())
  {
    return JS_EXCEPTION;
  }
  const char* reg = regArg.or_else("c");
  double testStat = 0.0;
  double pValue = 0.0;
  size_t usedLag = 0;
  lrvx_indicator_adf(input.data(), input.size(), maxLag, reg, &testStat, &pValue, &usedLag);
  JSValue obj = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, obj, "test_stat", JS_NewFloat64(ctx, testStat));
  JS_SetPropertyStr(ctx, obj, "p_value", JS_NewFloat64(ctx, pValue));
  JS_SetPropertyStr(ctx, obj, "used_lag", JS_NewUint32(ctx, static_cast<uint32_t>(usedLag)));
  return obj;
}

static JSValue js_indicator_autocorrelation(JSContext* ctx, JSValueConst, int,
                                            JSValueConst* argv)
{
  auto in = jsArrayToDoubles(ctx, argv[0]);
  uint32_t window = toUint32(ctx, argv[1]);
  uint32_t lag = toUint32(ctx, argv[2]);
  size_t len = in.size();
  std::vector<double> output(len);
  LRVX_JS_INDICATOR_CALL(ctx, lrvx_indicator_autocorrelation(in.data(), len, window, lag, output.data()));
  return jsArrayFromDoubles(ctx, output);
}

// ============================================================
// Streaming indicators
// ============================================================
//
// The streaming classes in quickjs/lrvx/indicators.js each hold one
// lrvx_streaming_indicator handle, on the generic LrvxHandle class, so the
// finalizer destroys it when a script drops the object without calling
// destroy(). An update is one call and one kernel step; nothing here keeps
// history. A destroyed handle reads NaN (the classes show null), not ready,
// count 0.

static JSValue js_streaming_indicator_create(JSContext* ctx, JSValueConst, int,
                                             JSValueConst* argv)
{
  LRVX_JS_CSTRING_OR_THROW(name, ctx, argv[0]);
  uint32_t n = 0;
  if (JS_IsArray(ctx, argv[1]))
  {
    JSValue lenVal = JS_GetPropertyStr(ctx, argv[1], "length");
    JS_ToUint32(ctx, &n, lenVal);
    JS_FreeValue(ctx, lenVal);
  }
  // Constructor arguments are a handful of numbers; anything longer is a
  // mistake, and the length must not reach an allocation unchecked.
  if (n > 16)
  {
    return JS_ThrowRangeError(ctx, "streaming indicator: %u constructor arguments", n);
  }
  std::vector<double> args = n ? jsArrayToDoubles(ctx, argv[1]) : std::vector<double>{};
  LrvxStreamingIndicatorHandle h =
      lrvx_streaming_indicator_create(name, args.data(), static_cast<uint32_t>(args.size()));
  if (!h)
  {
    return JS_ThrowTypeError(ctx, "%s", lrvx_last_error_message());
  }
  return createHandleObject(ctx, h, lrvx_streaming_indicator_destroy);
}

static JSValue js_streaming_indicator_destroy(JSContext* ctx, JSValueConst, int,
                                              JSValueConst* argv)
{
  return js_generic_handle_destroy(ctx, argv[0]);
}

static JSValue js_streaming_indicator_update(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = getHandle(ctx, argv[0]);
  if (!h)
  {
    return JS_NewFloat64(ctx, std::nan(""));
  }
  return JS_NewFloat64(ctx, lrvx_streaming_indicator_update(h, toDouble(ctx, argv[1])));
}

static JSValue js_streaming_indicator_update_bar(JSContext* ctx, JSValueConst, int,
                                                 JSValueConst* argv)
{
  auto h = getHandle(ctx, argv[0]);
  if (!h)
  {
    return JS_NewFloat64(ctx, std::nan(""));
  }
  return JS_NewFloat64(ctx, lrvx_streaming_indicator_update_bar(h, toDouble(ctx, argv[1]),
                                                                toDouble(ctx, argv[2]),
                                                                toDouble(ctx, argv[3])));
}

static JSValue js_streaming_indicator_update_high_low(JSContext* ctx, JSValueConst, int,
                                                      JSValueConst* argv)
{
  auto h = getHandle(ctx, argv[0]);
  if (!h)
  {
    return JS_NewFloat64(ctx, std::nan(""));
  }
  return JS_NewFloat64(ctx, lrvx_streaming_indicator_update_high_low(h, toDouble(ctx, argv[1]),
                                                                     toDouble(ctx, argv[2])));
}

static JSValue js_streaming_indicator_update_ohlc(JSContext* ctx, JSValueConst, int,
                                                  JSValueConst* argv)
{
  auto h = getHandle(ctx, argv[0]);
  if (!h)
  {
    return JS_NewFloat64(ctx, std::nan(""));
  }
  return JS_NewFloat64(
      ctx, lrvx_streaming_indicator_update_ohlc(h, toDouble(ctx, argv[1]), toDouble(ctx, argv[2]),
                                                toDouble(ctx, argv[3]), toDouble(ctx, argv[4])));
}

static JSValue js_streaming_indicator_update_pair(JSContext* ctx, JSValueConst, int,
                                                  JSValueConst* argv)
{
  auto h = getHandle(ctx, argv[0]);
  if (!h)
  {
    return JS_NewFloat64(ctx, std::nan(""));
  }
  return JS_NewFloat64(ctx, lrvx_streaming_indicator_update_pair(h, toDouble(ctx, argv[1]),
                                                                 toDouble(ctx, argv[2])));
}

static JSValue js_streaming_indicator_value(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = getHandle(ctx, argv[0]);
  return JS_NewFloat64(ctx, h ? lrvx_streaming_indicator_value(h) : std::nan(""));
}

static JSValue js_streaming_indicator_output(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = getHandle(ctx, argv[0]);
  return JS_NewFloat64(ctx,
                       h ? lrvx_streaming_indicator_output(h, toUint32(ctx, argv[1])) : std::nan(""));
}

static JSValue js_streaming_indicator_ready(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = getHandle(ctx, argv[0]);
  return JS_NewBool(ctx, h != nullptr && lrvx_streaming_indicator_ready(h) != 0);
}

static JSValue js_streaming_indicator_count(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = getHandle(ctx, argv[0]);
  return JS_NewFloat64(ctx, h ? static_cast<double>(lrvx_streaming_indicator_count(h)) : 0.0);
}

static JSValue js_streaming_indicator_reset(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  if (auto h = getHandle(ctx, argv[0]))
  {
    lrvx_streaming_indicator_reset(h);
  }
  return JS_UNDEFINED;
}

// ============================================================
// Targets (forward-looking labels, batch only)
// ============================================================

static JSValue js_target_future_return(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto close = jsArrayToDoubles(ctx, argv[0]);
  uint32_t horizon = toUint32(ctx, argv[1]);
  size_t len = close.size();
  std::vector<double> output(len);
  lrvx_target_future_return(close.data(), len, horizon, output.data());
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_target_future_ctc_volatility(JSContext* ctx, JSValueConst, int,
                                               JSValueConst* argv)
{
  auto close = jsArrayToDoubles(ctx, argv[0]);
  uint32_t horizon = toUint32(ctx, argv[1]);
  size_t len = close.size();
  std::vector<double> output(len);
  lrvx_target_future_ctc_volatility(close.data(), len, horizon, output.data());
  return jsArrayFromDoubles(ctx, output);
}

static JSValue js_target_future_linear_slope(JSContext* ctx, JSValueConst, int,
                                             JSValueConst* argv)
{
  auto close = jsArrayToDoubles(ctx, argv[0]);
  uint32_t horizon = toUint32(ctx, argv[1]);
  size_t len = close.size();
  std::vector<double> output(len);
  lrvx_target_future_linear_slope(close.data(), len, horizon, output.data());
  return jsArrayFromDoubles(ctx, output);
}

// ============================================================
// IndicatorGraph (batch) bindings
// ============================================================

namespace
{

struct JsGraphNodeState
{
  JSContext* ctx;
  JSValue fn;
  JSValue thisObj;
  std::vector<double> buffer;
};

struct JsGraphState
{
  LrvxIndicatorGraphHandle handle;
  std::vector<std::unique_ptr<JsGraphNodeState>> nodes;
};

static const double* js_graph_node_trampoline(void* user_data, LrvxIndicatorGraphHandle,
                                              uint32_t symbol, size_t* out_len)
{
  auto* st = static_cast<JsGraphNodeState*>(user_data);
  JSValue args[2] = {JS_DupValue(st->ctx, st->thisObj), JS_NewUint32(st->ctx, symbol)};
  JSValue result = JS_Call(st->ctx, st->fn, JS_UNDEFINED, 2, args);
  JS_FreeValue(st->ctx, args[0]);
  JS_FreeValue(st->ctx, args[1]);
  if (JS_IsException(result))
  {
    JS_FreeValue(st->ctx, result);
    *out_len = 0;
    return nullptr;
  }
  st->buffer = jsArrayToDoubles(st->ctx, result);
  JS_FreeValue(st->ctx, result);
  *out_len = st->buffer.size();
  return st->buffer.data();
}

}  // namespace

// IndicatorGraph used to share the generic LrvxHandle class, whose
// finalizer is nullptr -- a graph a script drops without calling
// .destroy() leaked the C++ state (and every JS function reference each
// node holds) for the life of the process (measured: ~2.6 KB per graph).
// Its own class id gives it a real finalizer. destroy() (and the
// finalizer, for the same reason) null the opaque pointer, so a second
// destroy() -- or a call through the raw `__lrvx_graph_destroy` global on
// a handle a script squirreled away -- is a safe no-op instead of a
// double-free or a use-after-free.
static JsGraphState* getGraphPtr(JSValueConst val)
{
  return static_cast<JsGraphState*>(JS_GetOpaque(val, jsGraphClassId));
}

#define GET_GRAPH_OR_THROW(ctx, argv)                                             \
  auto* st = getGraphPtr((argv)[0]);                                              \
  if (!st)                                                                        \
  {                                                                               \
    return JS_ThrowTypeError((ctx), "graph handle is null (already destroyed?)"); \
  }

static void freeGraphState(JsGraphState* st)
{
  for (auto& node : st->nodes)
  {
    JS_FreeValueRT(JS_GetRuntime(node->ctx), node->fn);
    JS_FreeValueRT(JS_GetRuntime(node->ctx), node->thisObj);
  }
  lrvx_indicator_graph_destroy(st->handle);
  delete st;
}

static void js_graph_finalizer(JSRuntime*, JSValue val)
{
  auto* st = static_cast<JsGraphState*>(JS_GetOpaque(val, jsGraphClassId));
  if (st)
  {
    freeGraphState(st);
  }
}

static JSValue js_graph_create(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
  auto* st = new JsGraphState{lrvx_indicator_graph_create(), {}};
  JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(jsGraphClassId));
  JS_SetOpaque(obj, st);
  return obj;
}

static JSValue js_graph_destroy(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto* st = getGraphPtr(argv[0]);
  if (!st)
  {
    return JS_UNDEFINED;
  }
  freeGraphState(st);
  JS_SetOpaque(argv[0], nullptr);
  return JS_UNDEFINED;
}

static JSValue js_graph_set_bars(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  GET_GRAPH_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  auto close = jsArrayToDoubles(ctx, argv[2]);
  std::vector<double> high, low, volume;
  const double* hp = nullptr;
  const double* lp = nullptr;
  const double* vp = nullptr;
  if (argc > 3 && !JS_IsNull(argv[3]) && !JS_IsUndefined(argv[3]))
  {
    high = jsArrayToDoubles(ctx, argv[3]);
    if (high.size() != close.size())
    {
      return JS_ThrowTypeError(ctx, "graph.setBars: high length must match close");
    }
    hp = high.data();
  }
  if (argc > 4 && !JS_IsNull(argv[4]) && !JS_IsUndefined(argv[4]))
  {
    low = jsArrayToDoubles(ctx, argv[4]);
    if (low.size() != close.size())
    {
      return JS_ThrowTypeError(ctx, "graph.setBars: low length must match close");
    }
    lp = low.data();
  }
  if (argc > 5 && !JS_IsNull(argv[5]) && !JS_IsUndefined(argv[5]))
  {
    volume = jsArrayToDoubles(ctx, argv[5]);
    if (volume.size() != close.size())
    {
      return JS_ThrowTypeError(ctx, "graph.setBars: volume length must match close");
    }
    vp = volume.data();
  }
  lrvx_indicator_graph_set_bars(st->handle, sym, close.data(), hp, lp, vp, close.size());
  return JS_UNDEFINED;
}

static JSValue js_graph_add_node(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_GRAPH_OR_THROW(ctx, argv);
  // A non-string node name (a Symbol, or an object whose toString
  // throws) makes JS_ToCString return null; the string ctor below then
  // calls strlen(nullptr) and segfaults. Same for each dependency name.
  lrvx::JsCString name(ctx, argv[1]);
  if (!name)
  {
    return JS_EXCEPTION;
  }
  std::string nameStr(name);

  std::vector<std::string> deps;
  if (JS_IsArray(ctx, argv[2]))
  {
    uint32_t depsLen = 0;
    {
      JSValue lenVal = JS_GetPropertyStr(ctx, argv[2], "length");
      JS_ToUint32(ctx, &depsLen, lenVal);
      JS_FreeValue(ctx, lenVal);
    }
    for (uint32_t i = 0; i < depsLen; ++i)
    {
      JSValue v = JS_GetPropertyUint32(ctx, argv[2], i);
      lrvx::JsCString s(ctx, v);
      if (!s)
      {
        JS_FreeValue(ctx, v);
        return JS_EXCEPTION;
      }
      deps.emplace_back(s);
      JS_FreeValue(ctx, v);
    }
  }

  std::vector<const char*> depPtrs;
  depPtrs.reserve(deps.size());
  for (const auto& d : deps)
  {
    depPtrs.push_back(d.c_str());
  }

  // Capture the JS callable + the thisObj (graph wrapper).
  auto node = std::make_unique<JsGraphNodeState>();
  node->ctx = ctx;
  node->fn = JS_DupValue(ctx, argv[3]);
  node->thisObj = JS_DupValue(ctx, argv[4]);

  lrvx_indicator_graph_add_node(st->handle, nameStr.c_str(),
                                deps.empty() ? nullptr : depPtrs.data(), deps.size(),
                                js_graph_node_trampoline, node.get());
  st->nodes.push_back(std::move(node));
  return JS_UNDEFINED;
}

static JSValue js_graph_require(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_GRAPH_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  lrvx::JsCString name(ctx, argv[2]);
  if (!name)
  {
    return JS_EXCEPTION;
  }
  size_t len = 0;
  const double* p = lrvx_indicator_graph_require(st->handle, sym, name, &len);
  if (!p)
  {
    // `!p` also happens when a dependency node's own function
    // threw during this require() -- js_graph_node_trampoline above
    // leaves that exception pending rather than converting it to a
    // return value. Throwing a fresh generic error here would silently
    // replace it, and the caller would see "graph: require failed"
    // instead of whatever their node actually threw. Propagate the real
    // exception when there is one; only synthesize this generic message
    // when there truly isn't (name/symbol not found).
    JSValue pending = JS_GetException(ctx);
    if (!JS_IsNull(pending))
    {
      JS_Throw(ctx, pending);
      return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, pending);
    return JS_ThrowTypeError(ctx, "graph: require failed");
  }
  return jsArrayFromDoubles(ctx, std::vector<double>(p, p + len));
}

static JSValue js_graph_get(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_GRAPH_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  lrvx::JsCString name(ctx, argv[2]);
  if (!name)
  {
    return JS_EXCEPTION;
  }
  size_t len = 0;
  const double* p = lrvx_indicator_graph_get(st->handle, sym, name, &len);
  if (!p)
  {
    return JS_NULL;
  }
  return jsArrayFromDoubles(ctx, std::vector<double>(p, p + len));
}

#define GRAPH_FIELD(field)                                                               \
  static JSValue js_graph_##field(JSContext* ctx, JSValueConst, int, JSValueConst* argv) \
  {                                                                                      \
    GET_GRAPH_OR_THROW(ctx, argv);                                                       \
    uint32_t sym = toUint32(ctx, argv[1]);                                               \
    size_t len = 0;                                                                      \
    const double* p = lrvx_indicator_graph_##field(st->handle, sym, &len);               \
    return jsArrayFromDoubles(ctx, std::vector<double>(p, p + len));                     \
  }
GRAPH_FIELD(close)
GRAPH_FIELD(high)
GRAPH_FIELD(low)
GRAPH_FIELD(volume)
#undef GRAPH_FIELD

static JSValue js_graph_invalidate(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_GRAPH_OR_THROW(ctx, argv);
  lrvx_indicator_graph_invalidate(st->handle, toUint32(ctx, argv[1]));
  return JS_UNDEFINED;
}

static JSValue js_graph_invalidate_all(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_GRAPH_OR_THROW(ctx, argv);
  lrvx_indicator_graph_invalidate_all(st->handle);
  return JS_UNDEFINED;
}

// ============================================================
// StreamingIndicatorGraph bindings
// ============================================================

namespace
{

struct JsStreamingState
{
  LrvxStreamingGraphHandle handle;
  std::vector<std::unique_ptr<JsGraphNodeState>> nodes;
};

static const double* js_streaming_node_trampoline(void* user_data, LrvxIndicatorGraphHandle,
                                                  uint32_t symbol, size_t* out_len)
{
  auto* st = static_cast<JsGraphNodeState*>(user_data);
  JSValue args[2] = {JS_DupValue(st->ctx, st->thisObj), JS_NewUint32(st->ctx, symbol)};
  JSValue result = JS_Call(st->ctx, st->fn, JS_UNDEFINED, 2, args);
  JS_FreeValue(st->ctx, args[0]);
  JS_FreeValue(st->ctx, args[1]);
  if (JS_IsException(result))
  {
    JS_FreeValue(st->ctx, result);
    *out_len = 0;
    return nullptr;
  }
  st->buffer = jsArrayToDoubles(st->ctx, result);
  JS_FreeValue(st->ctx, result);
  *out_len = st->buffer.size();
  return st->buffer.data();
}

}  // namespace

static JsStreamingState* getStreamingPtr(JSValueConst val)
{
  return static_cast<JsStreamingState*>(JS_GetOpaque(val, jsStreamingClassId));
}

#define GET_STREAMING_OR_THROW(ctx, argv)                                                   \
  auto* st = getStreamingPtr((argv)[0]);                                                    \
  if (!st)                                                                                  \
  {                                                                                         \
    return JS_ThrowTypeError((ctx), "streaming graph handle is null (already destroyed?)"); \
  }

static void freeStreamingState(JsStreamingState* st)
{
  for (auto& node : st->nodes)
  {
    JS_FreeValueRT(JS_GetRuntime(node->ctx), node->fn);
    JS_FreeValueRT(JS_GetRuntime(node->ctx), node->thisObj);
  }
  lrvx_streaming_graph_destroy(st->handle);
  delete st;
}

static void js_streaming_finalizer(JSRuntime*, JSValue val)
{
  auto* st = static_cast<JsStreamingState*>(JS_GetOpaque(val, jsStreamingClassId));
  if (st)
  {
    freeStreamingState(st);
  }
}

static JSValue js_streaming_create(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
  auto* st = new JsStreamingState{lrvx_streaming_graph_create(), {}};
  JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(jsStreamingClassId));
  JS_SetOpaque(obj, st);
  return obj;
}

static JSValue js_streaming_destroy(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto* st = getStreamingPtr(argv[0]);
  if (!st)
  {
    return JS_UNDEFINED;
  }
  freeStreamingState(st);
  JS_SetOpaque(argv[0], nullptr);
  return JS_UNDEFINED;
}

static JSValue js_streaming_add_node(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_STREAMING_OR_THROW(ctx, argv);
  lrvx::JsCString name(ctx, argv[1]);
  if (!name)
  {
    return JS_EXCEPTION;
  }
  std::string nameStr(name);

  std::vector<std::string> deps;
  if (JS_IsArray(ctx, argv[2]))
  {
    uint32_t depsLen = 0;
    {
      JSValue lenVal = JS_GetPropertyStr(ctx, argv[2], "length");
      JS_ToUint32(ctx, &depsLen, lenVal);
      JS_FreeValue(ctx, lenVal);
    }
    for (uint32_t i = 0; i < depsLen; ++i)
    {
      JSValue v = JS_GetPropertyUint32(ctx, argv[2], i);
      lrvx::JsCString s(ctx, v);
      if (!s)
      {
        JS_FreeValue(ctx, v);
        return JS_EXCEPTION;
      }
      deps.emplace_back(s);
      JS_FreeValue(ctx, v);
    }
  }

  std::vector<const char*> depPtrs;
  depPtrs.reserve(deps.size());
  for (const auto& d : deps)
  {
    depPtrs.push_back(d.c_str());
  }

  auto node = std::make_unique<JsGraphNodeState>();
  node->ctx = ctx;
  node->fn = JS_DupValue(ctx, argv[3]);
  node->thisObj = JS_DupValue(ctx, argv[4]);

  lrvx_streaming_graph_add_node(st->handle, nameStr.c_str(),
                                deps.empty() ? nullptr : depPtrs.data(), deps.size(),
                                js_streaming_node_trampoline, node.get());
  st->nodes.push_back(std::move(node));
  return JS_UNDEFINED;
}

static JSValue js_streaming_step(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  GET_STREAMING_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  double c = toDouble(ctx, argv[2]);
  double h = argc > 3 && !JS_IsUndefined(argv[3]) && !JS_IsNull(argv[3])
                 ? toDouble(ctx, argv[3])
                 : c;
  double l = argc > 4 && !JS_IsUndefined(argv[4]) && !JS_IsNull(argv[4])
                 ? toDouble(ctx, argv[4])
                 : c;
  double v = argc > 5 && !JS_IsUndefined(argv[5]) && !JS_IsNull(argv[5])
                 ? toDouble(ctx, argv[5])
                 : 0.0;
  lrvx_streaming_graph_step(st->handle, sym, c, h, l, c, v);
  return JS_UNDEFINED;
}

static JSValue js_streaming_current(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_STREAMING_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  lrvx::JsCString name(ctx, argv[2]);
  if (!name)
  {
    return JS_EXCEPTION;
  }
  double val = lrvx_streaming_graph_current(st->handle, sym, name);
  return JS_NewFloat64(ctx, val);
}

static JSValue js_streaming_bar_count(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_STREAMING_OR_THROW(ctx, argv);
  uint32_t sym = toUint32(ctx, argv[1]);
  return JS_NewUint32(ctx, lrvx_streaming_graph_bar_count(st->handle, sym));
}

static JSValue js_streaming_reset(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_STREAMING_OR_THROW(ctx, argv);
  lrvx_streaming_graph_reset(st->handle, toUint32(ctx, argv[1]));
  return JS_UNDEFINED;
}

static JSValue js_streaming_reset_all(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  GET_STREAMING_OR_THROW(ctx, argv);
  lrvx_streaming_graph_reset_all(st->handle);
  return JS_UNDEFINED;
}

#define STREAMING_FIELD(field)                                                               \
  static JSValue js_streaming_##field(JSContext* ctx, JSValueConst, int, JSValueConst* argv) \
  {                                                                                          \
    GET_STREAMING_OR_THROW(ctx, argv);                                                       \
    uint32_t sym = toUint32(ctx, argv[1]);                                                   \
    size_t len = 0;                                                                          \
    const double* p = lrvx_streaming_graph_##field(st->handle, sym, &len);                   \
    return jsArrayFromDoubles(ctx, std::vector<double>(p, p + len));                         \
  }
STREAMING_FIELD(close)
STREAMING_FIELD(high)
STREAMING_FIELD(low)
STREAMING_FIELD(volume)
#undef STREAMING_FIELD

// ============================================================
// Order book bindings
// ============================================================

static JSValue js_book_create(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = lrvx_book_create(toDouble(ctx, argv[0]));
  if (!h)
  {
    return JS_ThrowRangeError(ctx, "book_create: tick size must be positive");
  }
  return createHandleObject(ctx, h, lrvx_book_destroy);
}
static JSValue js_book_destroy(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return js_generic_handle_destroy(ctx, argv[0]);
}
static JSValue js_book_best_bid(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  double p = 0;
  if (lrvx_book_best_bid(static_cast<LrvxBookHandle>(getHandle(ctx, argv[0])), &p))
  {
    return JS_NewFloat64(ctx, p);
  }
  return JS_NULL;
}
static JSValue js_book_best_ask(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  double p = 0;
  if (lrvx_book_best_ask(static_cast<LrvxBookHandle>(getHandle(ctx, argv[0])), &p))
  {
    return JS_NewFloat64(ctx, p);
  }
  return JS_NULL;
}
static JSValue js_book_mid(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  double p = 0;
  if (lrvx_book_mid(static_cast<LrvxBookHandle>(getHandle(ctx, argv[0])), &p))
  {
    return JS_NewFloat64(ctx, p);
  }
  return JS_NULL;
}
static JSValue js_book_spread(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  double s = 0;
  if (lrvx_book_spread(static_cast<LrvxBookHandle>(getHandle(ctx, argv[0])), &s))
  {
    return JS_NewFloat64(ctx, s);
  }
  return JS_NULL;
}
static JSValue js_book_clear(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  lrvx_book_clear(static_cast<LrvxBookHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}
static JSValue js_book_is_crossed(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewBool(
      ctx, lrvx_book_is_crossed(static_cast<LrvxBookHandle>(getHandle(ctx, argv[0]))) != 0);
}
static JSValue js_book_apply_snapshot(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<LrvxBookHandle>(getHandle(ctx, argv[0]));
  auto bp = jsArrayToDoubles(ctx, argv[1]);
  auto bq = jsArrayToDoubles(ctx, argv[2]);
  auto ap = jsArrayToDoubles(ctx, argv[3]);
  auto aq = jsArrayToDoubles(ctx, argv[4]);
  lrvx_book_apply_snapshot(h, bp.data(), bq.data(), bp.size(), ap.data(), aq.data(), ap.size());
  return JS_UNDEFINED;
}
static JSValue js_book_apply_delta(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<LrvxBookHandle>(getHandle(ctx, argv[0]));
  auto bp = jsArrayToDoubles(ctx, argv[1]);
  auto bq = jsArrayToDoubles(ctx, argv[2]);
  auto ap = jsArrayToDoubles(ctx, argv[3]);
  auto aq = jsArrayToDoubles(ctx, argv[4]);
  lrvx_book_apply_delta(h, bp.data(), bq.data(), bp.size(), ap.data(), aq.data(), ap.size());
  return JS_UNDEFINED;
}

// ============================================================
// Executor bindings
// ============================================================

static JSValue js_executor_create(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
  return createHandleObject(ctx, lrvx_simulated_executor_create(), lrvx_simulated_executor_destroy);
}
static JSValue js_executor_destroy(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return js_generic_handle_destroy(ctx, argv[0]);
}
static JSValue js_executor_submit(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0]));
  lrvx_simulated_executor_submit_order(h, static_cast<uint64_t>(toInt64(ctx, argv[1])),
                                       static_cast<uint8_t>(toUint32(ctx, argv[2])),
                                       toDouble(ctx, argv[3]), toDouble(ctx, argv[4]),
                                       static_cast<uint8_t>(toUint32(ctx, argv[5])),
                                       toUint32(ctx, argv[6]));
  return JS_UNDEFINED;
}
static JSValue js_executor_submit_ex(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0]));
  lrvx_simulated_executor_submit_order_ex(
      h, static_cast<uint64_t>(toInt64(ctx, argv[1])),
      static_cast<uint8_t>(toUint32(ctx, argv[2])), toDouble(ctx, argv[3]),
      toDouble(ctx, argv[4]), static_cast<uint8_t>(toUint32(ctx, argv[5])),
      toUint32(ctx, argv[6]), static_cast<uint8_t>(toUint32(ctx, argv[7])),
      static_cast<uint8_t>(toUint32(ctx, argv[8])), toInt64(ctx, argv[9]));
  return JS_UNDEFINED;
}
static JSValue js_executor_submit_bracket(JSContext* ctx, JSValueConst, int,
                                          JSValueConst* argv)
{
  lrvx_simulated_executor_submit_bracket(
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      static_cast<uint64_t>(toInt64(ctx, argv[1])),
      toUint32(ctx, argv[2]),
      static_cast<uint8_t>(toUint32(ctx, argv[3])),
      static_cast<uint8_t>(toUint32(ctx, argv[4])),
      toDouble(ctx, argv[5]), toDouble(ctx, argv[6]),
      static_cast<uint8_t>(toUint32(ctx, argv[7])),
      static_cast<uint8_t>(toUint32(ctx, argv[8])),
      toDouble(ctx, argv[9]),
      static_cast<uint8_t>(toUint32(ctx, argv[10])),
      static_cast<uint8_t>(toUint32(ctx, argv[11])),
      toDouble(ctx, argv[12]));
  return JS_UNDEFINED;
}
static JSValue js_executor_cancel_bracket(JSContext* ctx, JSValueConst, int,
                                          JSValueConst* argv)
{
  lrvx_simulated_executor_cancel_bracket(
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      static_cast<uint64_t>(toInt64(ctx, argv[1])));
  return JS_UNDEFINED;
}
static JSValue js_executor_bracket_state(JSContext* ctx, JSValueConst, int,
                                         JSValueConst* argv)
{
  return JS_NewUint32(ctx, lrvx_simulated_executor_bracket_state(
                               static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
                               static_cast<uint64_t>(toInt64(ctx, argv[1]))));
}
static JSValue js_executor_set_bracket_child_arm_mode(JSContext* ctx, JSValueConst, int,
                                                      JSValueConst* argv)
{
  lrvx_simulated_executor_set_bracket_child_arm_mode(
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      static_cast<uint8_t>(toUint32(ctx, argv[1])));
  return JS_UNDEFINED;
}
static JSValue js_executor_submit_iceberg(JSContext* ctx, JSValueConst, int,
                                          JSValueConst* argv)
{
  lrvx_simulated_executor_submit_iceberg(
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      static_cast<uint64_t>(toInt64(ctx, argv[1])),
      static_cast<uint8_t>(toUint32(ctx, argv[2])), toDouble(ctx, argv[3]),
      toDouble(ctx, argv[4]), toDouble(ctx, argv[5]), toUint32(ctx, argv[6]));
  return JS_UNDEFINED;
}
static JSValue js_executor_set_iceberg_refresh_latency(JSContext* ctx, JSValueConst, int,
                                                       JSValueConst* argv)
{
  lrvx_simulated_executor_set_iceberg_refresh_latency(
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      toInt64(ctx, argv[1]));
  return JS_UNDEFINED;
}
static JSValue js_executor_iceberg_hidden_remaining_raw(JSContext* ctx, JSValueConst, int,
                                                        JSValueConst* argv)
{
  return JS_NewInt64(ctx, lrvx_simulated_executor_iceberg_hidden_remaining_raw(
                              static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
                              static_cast<uint64_t>(toInt64(ctx, argv[1]))));
}
static JSValue js_executor_set_iceberg_size_randomisation_pct(JSContext* ctx,
                                                              JSValueConst, int,
                                                              JSValueConst* argv)
{
  double pct = 0.0;
  JS_ToFloat64(ctx, &pct, argv[1]);
  lrvx_simulated_executor_set_iceberg_size_randomisation_pct(
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])), pct);
  return JS_UNDEFINED;
}
static JSValue js_executor_set_iceberg_priority_mode(JSContext* ctx, JSValueConst, int,
                                                     JSValueConst* argv)
{
  uint8_t code = 0;
  if (JS_IsString(argv[1]))
  {
    lrvx::JsCString s(ctx, argv[1]);
    if (s != nullptr)
    {
      std::string name(s);
      if (name == "retain")
      {
        code = 1;
      }
    }
  }
  else
  {
    code = static_cast<uint8_t>(toUint32(ctx, argv[1]));
  }
  lrvx_simulated_executor_set_iceberg_priority_mode(
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])), code);
  return JS_UNDEFINED;
}
static JSValue js_executor_set_iceberg_jitter_seed(JSContext* ctx, JSValueConst, int,
                                                   JSValueConst* argv)
{
  lrvx_simulated_executor_set_iceberg_jitter_seed(
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      static_cast<uint64_t>(toInt64(ctx, argv[1])));
  return JS_UNDEFINED;
}
static JSValue js_executor_on_bar(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  lrvx_simulated_executor_on_bar(static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
                                 toUint32(ctx, argv[1]), toDouble(ctx, argv[2]));
  return JS_UNDEFINED;
}
// Open-aware form: moves the market to the open (releasing any order held
// from the previous bar's callback there), then walks low -> high -> close.
// Use this, not on_bar, to drive the executor by hand to the same fills
// BacktestRunner produces on the same bars.
static JSValue js_executor_on_bar_ohlc(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  lrvx_simulated_executor_on_bar_ohlc(
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      toUint32(ctx, argv[1]), toDouble(ctx, argv[2]), toDouble(ctx, argv[3]),
      toDouble(ctx, argv[4]), toDouble(ctx, argv[5]));
  return JS_UNDEFINED;
}
static JSValue js_executor_begin_bar_callback_window(JSContext* ctx, JSValueConst, int,
                                                     JSValueConst* argv)
{
  lrvx_simulated_executor_begin_bar_callback_window(
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}
static JSValue js_executor_end_bar_callback_window(JSContext* ctx, JSValueConst, int,
                                                   JSValueConst* argv)
{
  lrvx_simulated_executor_end_bar_callback_window(
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}
static JSValue js_executor_reset(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  lrvx_simulated_executor_reset(static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}
static JSValue js_executor_on_trade(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  lrvx_simulated_executor_on_trade(static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
                                   toUint32(ctx, argv[1]), toDouble(ctx, argv[2]),
                                   static_cast<uint8_t>(toUint32(ctx, argv[3])));
  return JS_UNDEFINED;
}
static JSValue js_executor_advance(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  lrvx_simulated_executor_advance_clock(static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
                                        toInt64(ctx, argv[1]));
  return JS_UNDEFINED;
}
static JSValue js_executor_fill_count(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewUint32(
      ctx, lrvx_simulated_executor_fill_count(static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0]))));
}

// An out-of-range model code is refused by the C API; surface that as a throw.
static JSValue js_executor_set_default_slippage(JSContext* ctx, JSValueConst, int,
                                                JSValueConst* argv)
{
  lrvx_clear_last_error();
  lrvx_simulated_executor_set_default_slippage(
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      static_cast<int32_t>(toInt64(ctx, argv[1])),
      static_cast<int32_t>(toInt64(ctx, argv[2])), toDouble(ctx, argv[3]),
      toDouble(ctx, argv[4]), toDouble(ctx, argv[5]));
  if (lrvx_last_error_code() != 0)
  {
    return JS_ThrowTypeError(ctx, "setDefaultSlippage: %s", lrvx_last_error_message());
  }
  return JS_UNDEFINED;
}
static JSValue js_executor_set_symbol_slippage(JSContext* ctx, JSValueConst, int,
                                               JSValueConst* argv)
{
  lrvx_clear_last_error();
  lrvx_simulated_executor_set_symbol_slippage(
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])), toUint32(ctx, argv[1]),
      static_cast<int32_t>(toInt64(ctx, argv[2])),
      static_cast<int32_t>(toInt64(ctx, argv[3])), toDouble(ctx, argv[4]),
      toDouble(ctx, argv[5]), toDouble(ctx, argv[6]));
  if (lrvx_last_error_code() != 0)
  {
    return JS_ThrowTypeError(ctx, "setSymbolSlippage: %s", lrvx_last_error_message());
  }
  return JS_UNDEFINED;
}
static JSValue js_executor_set_queue_model(JSContext* ctx, JSValueConst, int,
                                           JSValueConst* argv)
{
  lrvx_clear_last_error();
  lrvx_simulated_executor_set_queue_model(static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
                                          static_cast<int32_t>(toInt64(ctx, argv[1])),
                                          toUint32(ctx, argv[2]));
  if (lrvx_last_error_code() != 0)
  {
    return JS_ThrowTypeError(ctx, "setQueueModel: %s", lrvx_last_error_message());
  }
  return JS_UNDEFINED;
}
static JSValue js_executor_set_queue_fifo_top_n(JSContext* ctx, JSValueConst, int,
                                                JSValueConst* argv)
{
  lrvx_simulated_executor_set_queue_fifo_top_n(
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      toUint32(ctx, argv[1]));
  return JS_UNDEFINED;
}
static JSValue js_executor_set_top_priority_share(JSContext* ctx, JSValueConst, int,
                                                  JSValueConst* argv)
{
  double share = 0.40;
  JS_ToFloat64(ctx, &share, argv[1]);
  lrvx_simulated_executor_set_top_priority_share(
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])), share);
  return JS_UNDEFINED;
}
static JSValue js_executor_set_lmm_orders(JSContext* ctx, JSValueConst, int,
                                          JSValueConst* argv)
{
  auto h = static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0]));
  uint32_t n = 0;
  JSValue lenVal = JS_GetPropertyStr(ctx, argv[1], "length");
  JS_ToUint32(ctx, &n, lenVal);
  JS_FreeValue(ctx, lenVal);
  std::vector<uint64_t> ids(n);
  for (uint32_t i = 0; i < n; ++i)
  {
    JSValue v = JS_GetPropertyUint32(ctx, argv[1], i);
    JS_ToInt64Ext(ctx, reinterpret_cast<int64_t*>(&ids[i]), v);
    JS_FreeValue(ctx, v);
  }
  lrvx_simulated_executor_set_lmm_orders(h, ids.data(), n);
  return JS_UNDEFINED;
}
static JSValue js_executor_set_lmm_bonus_multiplier(JSContext* ctx, JSValueConst, int,
                                                    JSValueConst* argv)
{
  double m = 1.5;
  JS_ToFloat64(ctx, &m, argv[1]);
  lrvx_simulated_executor_set_lmm_bonus_multiplier(
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])), m);
  return JS_UNDEFINED;
}
static JSValue js_executor_set_order_priority_multiplier(JSContext* ctx, JSValueConst,
                                                         int, JSValueConst* argv)
{
  double m = 1.0;
  JS_ToFloat64(ctx, &m, argv[2]);
  lrvx_simulated_executor_set_order_priority_multiplier(
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      static_cast<uint64_t>(toInt64(ctx, argv[1])), m);
  return JS_UNDEFINED;
}
static JSValue js_executor_set_submit_ack(JSContext* ctx, JSValueConst, int argc,
                                          JSValueConst* argv)
{
  lrvx_simulated_executor_set_submit_ack_latency(
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      toInt64(ctx, argv[1]), argc > 2 ? toInt64(ctx, argv[2]) : 0);
  return JS_UNDEFINED;
}
static JSValue js_executor_set_cancel_ack(JSContext* ctx, JSValueConst, int argc,
                                          JSValueConst* argv)
{
  lrvx_simulated_executor_set_cancel_ack_latency(
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      toInt64(ctx, argv[1]), argc > 2 ? toInt64(ctx, argv[2]) : 0);
  return JS_UNDEFINED;
}
static JSValue js_executor_set_replace_ack(JSContext* ctx, JSValueConst, int argc,
                                           JSValueConst* argv)
{
  lrvx_simulated_executor_set_replace_ack_latency(
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      toInt64(ctx, argv[1]), argc > 2 ? toInt64(ctx, argv[2]) : 0);
  return JS_UNDEFINED;
}
static JSValue js_latency_dist_create(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
  return createHandleObject(ctx, lrvx_latency_distribution_create(), lrvx_latency_distribution_destroy);
}
static JSValue js_latency_dist_destroy(JSContext* ctx, JSValueConst, int,
                                       JSValueConst* argv)
{
  return js_generic_handle_destroy(ctx, argv[0]);
}
static JSValue js_latency_dist_set_constant(JSContext* ctx, JSValueConst, int,
                                            JSValueConst* argv)
{
  lrvx_latency_distribution_set_constant(
      static_cast<LrvxLatencyDistributionHandle>(getHandle(ctx, argv[0])),
      toInt64(ctx, argv[1]));
  return JS_UNDEFINED;
}
static JSValue js_latency_dist_set_uniform(JSContext* ctx, JSValueConst, int,
                                           JSValueConst* argv)
{
  lrvx_latency_distribution_set_uniform(
      static_cast<LrvxLatencyDistributionHandle>(getHandle(ctx, argv[0])),
      toInt64(ctx, argv[1]), toInt64(ctx, argv[2]));
  return JS_UNDEFINED;
}
static JSValue js_latency_dist_set_lognormal(JSContext* ctx, JSValueConst, int,
                                             JSValueConst* argv)
{
  double sigma = 0.0;
  JS_ToFloat64(ctx, &sigma, argv[2]);
  lrvx_latency_distribution_set_lognormal(
      static_cast<LrvxLatencyDistributionHandle>(getHandle(ctx, argv[0])),
      toInt64(ctx, argv[1]), sigma);
  return JS_UNDEFINED;
}
static JSValue js_latency_dist_set_empirical(JSContext* ctx, JSValueConst, int,
                                             JSValueConst* argv)
{
  auto h = static_cast<LrvxLatencyDistributionHandle>(getHandle(ctx, argv[0]));
  uint32_t n = 0;
  JSValue lenVal = JS_GetPropertyStr(ctx, argv[1], "length");
  JS_ToUint32(ctx, &n, lenVal);
  JS_FreeValue(ctx, lenVal);
  std::vector<int64_t> samples;
  samples.reserve(n);
  for (uint32_t i = 0; i < n; ++i)
  {
    JSValue v = JS_GetPropertyUint32(ctx, argv[1], i);
    samples.push_back(toInt64(ctx, v));
    JS_FreeValue(ctx, v);
  }
  lrvx_latency_distribution_set_empirical(
      h, samples.empty() ? nullptr : samples.data(), n);
  return JS_UNDEFINED;
}
static JSValue js_latency_dist_set_burst(JSContext* ctx, JSValueConst, int,
                                         JSValueConst* argv)
{
  double rho = 0.0;
  JS_ToFloat64(ctx, &rho, argv[1]);
  lrvx_latency_distribution_set_burst_correlation(
      static_cast<LrvxLatencyDistributionHandle>(getHandle(ctx, argv[0])), rho);
  return JS_UNDEFINED;
}
static JSValue js_latency_dist_median_ns(JSContext* ctx, JSValueConst, int,
                                         JSValueConst* argv)
{
  return JS_NewInt64(ctx, lrvx_latency_distribution_median_ns(
                              static_cast<LrvxLatencyDistributionHandle>(
                                  getHandle(ctx, argv[0]))));
}
static JSValue js_executor_set_submit_ack_dist(JSContext* ctx, JSValueConst, int,
                                               JSValueConst* argv)
{
  lrvx_simulated_executor_set_submit_ack_latency_distribution(
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      static_cast<LrvxLatencyDistributionHandle>(getHandle(ctx, argv[1])));
  return JS_UNDEFINED;
}
static JSValue js_executor_set_cancel_ack_dist(JSContext* ctx, JSValueConst, int,
                                               JSValueConst* argv)
{
  lrvx_simulated_executor_set_cancel_ack_latency_distribution(
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      static_cast<LrvxLatencyDistributionHandle>(getHandle(ctx, argv[1])));
  return JS_UNDEFINED;
}
static JSValue js_executor_set_replace_ack_dist(JSContext* ctx, JSValueConst, int,
                                                JSValueConst* argv)
{
  lrvx_simulated_executor_set_replace_ack_latency_distribution(
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      static_cast<LrvxLatencyDistributionHandle>(getHandle(ctx, argv[1])));
  return JS_UNDEFINED;
}

static JSValue js_executor_apply_latency_profile(JSContext* ctx, JSValueConst, int,
                                                 JSValueConst* argv)
{
  LRVX_JS_CSTRING_OR_THROW(name, ctx, argv[1]);
  const int ok = lrvx_simulated_executor_apply_latency_profile(
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])), name);
  std::string nameCopy = name ? name : "";
  if (!ok)
  {
    return JS_ThrowTypeError(ctx, "unknown latency profile: %s", nameCopy.c_str());
  }
  return JS_UNDEFINED;
}
static JSValue js_executor_set_stp_mode(JSContext* ctx, JSValueConst, int,
                                        JSValueConst* argv)
{
  uint8_t mode = static_cast<uint8_t>(toUint32(ctx, argv[1]));
  lrvx_simulated_executor_set_stp_mode(
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])), mode);
  return JS_UNDEFINED;
}
static JSValue js_executor_set_stp_group_membership(JSContext* ctx, JSValueConst, int,
                                                    JSValueConst* argv)
{
  lrvx_simulated_executor_set_stp_group_membership(
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      static_cast<uint64_t>(toInt64(ctx, argv[1])),
      static_cast<uint64_t>(toInt64(ctx, argv[2])));
  return JS_UNDEFINED;
}
static JSValue js_executor_stp_group_for(JSContext* ctx, JSValueConst, int,
                                         JSValueConst* argv)
{
  uint64_t g = lrvx_simulated_executor_stp_group_for(
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      static_cast<uint64_t>(toInt64(ctx, argv[1])));
  return JS_NewFloat64(ctx, static_cast<double>(g));
}
static JSValue js_executor_set_fok_mode(JSContext* ctx, JSValueConst, int,
                                        JSValueConst* argv)
{
  uint8_t code = 0;
  if (JS_IsString(argv[1]))
  {
    lrvx::JsCString s(ctx, argv[1]);
    if (s != nullptr)
    {
      std::string name(s);
      if (name == "single_price")
      {
        code = 1;
      }
    }
  }
  else
  {
    code = static_cast<uint8_t>(toUint32(ctx, argv[1]));
  }
  lrvx_simulated_executor_set_fok_mode(
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])), code);
  return JS_UNDEFINED;
}
static JSValue js_executor_fok_mode(JSContext* ctx, JSValueConst, int,
                                    JSValueConst* argv)
{
  const uint8_t code = lrvx_simulated_executor_fok_mode(
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])));
  return JS_NewUint32(ctx, code);
}
static JSValue js_rate_limit_create(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
  return createHandleObject(ctx, lrvx_rate_limit_policy_create(), lrvx_rate_limit_policy_destroy);
}
static JSValue js_rate_limit_destroy(JSContext* ctx, JSValueConst, int,
                                     JSValueConst* argv)
{
  return js_generic_handle_destroy(ctx, argv[0]);
}
static JSValue js_rate_limit_add_bucket(JSContext* ctx, JSValueConst, int,
                                        JSValueConst* argv)
{
  auto h = static_cast<LrvxRateLimitPolicyHandle>(getHandle(ctx, argv[0]));
  lrvx::JsCString name(ctx, argv[1]);
  int64_t window = toInt64(ctx, argv[2]);
  uint32_t cap = toUint32(ctx, argv[3]);
  uint32_t sw = toUint32(ctx, argv[4]);
  uint32_t cw = toUint32(ctx, argv[5]);
  uint32_t rw = toUint32(ctx, argv[6]);
  lrvx_rate_limit_policy_add_bucket(h, name ? name : "bucket", window, cap, sw, cw, rw);
  return JS_UNDEFINED;
}
static JSValue js_rate_limit_add_bucket_family(JSContext* ctx, JSValueConst, int,
                                               JSValueConst* argv)
{
  // (handle, name, window_ns, capacity, submit_w, cancel_w, replace_w, family, query_w)
  auto h = static_cast<LrvxRateLimitPolicyHandle>(getHandle(ctx, argv[0]));
  lrvx::JsCString name(ctx, argv[1]);
  int64_t window = toInt64(ctx, argv[2]);
  uint32_t cap = toUint32(ctx, argv[3]);
  uint32_t sw = toUint32(ctx, argv[4]);
  uint32_t cw = toUint32(ctx, argv[5]);
  uint32_t rw = toUint32(ctx, argv[6]);
  uint8_t family = static_cast<uint8_t>(toUint32(ctx, argv[7]));
  uint32_t qw = toUint32(ctx, argv[8]);
  lrvx_rate_limit_policy_add_bucket_family(h, name ? name : "bucket", window, cap,
                                           sw, cw, rw, family, qw);
  return JS_UNDEFINED;
}
static JSValue js_rate_limit_set_ban(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  lrvx_rate_limit_policy_set_ban(
      static_cast<LrvxRateLimitPolicyHandle>(getHandle(ctx, argv[0])),
      toUint32(ctx, argv[1]), toInt64(ctx, argv[2]));
  return JS_UNDEFINED;
}
static JSValue js_rate_limit_load_profile(JSContext* ctx, JSValueConst, int,
                                          JSValueConst* argv)
{
  LRVX_JS_CSTRING_OR_THROW(name, ctx, argv[1]);
  const int ok = lrvx_rate_limit_policy_load_profile(
      static_cast<LrvxRateLimitPolicyHandle>(getHandle(ctx, argv[0])), name);
  std::string nameCopy = name ? name : "";
  if (!ok)
  {
    return JS_ThrowTypeError(ctx, "unknown rate-limit profile: %s", nameCopy.c_str());
  }
  return JS_UNDEFINED;
}
static JSValue js_rate_limit_ban_until_ns(JSContext* ctx, JSValueConst, int,
                                          JSValueConst* argv)
{
  return JS_NewBigInt64(ctx, lrvx_rate_limit_policy_ban_until_ns(
                                 static_cast<LrvxRateLimitPolicyHandle>(
                                     getHandle(ctx, argv[0]))));
}
static JSValue js_rate_limit_consecutive_rejects(JSContext* ctx, JSValueConst, int,
                                                 JSValueConst* argv)
{
  return JS_NewUint32(ctx, lrvx_rate_limit_policy_consecutive_rejects(
                               static_cast<LrvxRateLimitPolicyHandle>(
                                   getHandle(ctx, argv[0]))));
}
static JSValue js_executor_set_rate_limit_policy(JSContext* ctx, JSValueConst, int,
                                                 JSValueConst* argv)
{
  lrvx_simulated_executor_set_rate_limit_policy(
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      static_cast<LrvxRateLimitPolicyHandle>(getHandle(ctx, argv[1])));
  return JS_UNDEFINED;
}
static JSValue js_executor_clear_rate_limit_policy(JSContext* ctx, JSValueConst, int,
                                                   JSValueConst* argv)
{
  lrvx_simulated_executor_clear_rate_limit_policy(
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}

// Venue availability.
static JSValue js_venue_availability_create(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
  return createHandleObject(ctx, lrvx_venue_availability_create(), lrvx_venue_availability_destroy);
}
static JSValue js_venue_availability_destroy(JSContext* ctx, JSValueConst, int,
                                             JSValueConst* argv)
{
  return js_generic_handle_destroy(ctx, argv[0]);
}
static JSValue js_venue_availability_schedule_outage(JSContext* ctx, JSValueConst, int argc,
                                                     JSValueConst* argv)
{
  lrvx_venue_availability_schedule_outage(
      static_cast<LrvxVenueAvailabilityHandle>(getHandle(ctx, argv[0])),
      toInt64(ctx, argv[1]), toInt64(ctx, argv[2]),
      argc > 3 ? static_cast<uint8_t>(toInt64(ctx, argv[3])) : 0,
      argc > 4 ? toInt64(ctx, argv[4]) : 0);
  return JS_UNDEFINED;
}
static JSValue js_venue_availability_auto_random_outages(JSContext* ctx, JSValueConst,
                                                         int argc, JSValueConst* argv)
{
  lrvx_venue_availability_auto_random_outages(
      static_cast<LrvxVenueAvailabilityHandle>(getHandle(ctx, argv[0])),
      toDouble(ctx, argv[1]), toInt64(ctx, argv[2]),
      argc > 3 ? static_cast<uint8_t>(toInt64(ctx, argv[3])) : 0,
      argc > 4 ? static_cast<uint64_t>(toInt64(ctx, argv[4])) : 0xC0FFEEULL);
  return JS_UNDEFINED;
}
static JSValue js_venue_availability_is_up(JSContext* ctx, JSValueConst, int,
                                           JSValueConst* argv)
{
  const uint8_t up = lrvx_venue_availability_is_up(
      static_cast<LrvxVenueAvailabilityHandle>(getHandle(ctx, argv[0])),
      toInt64(ctx, argv[1]));
  return JS_NewBool(ctx, up != 0);
}
static JSValue js_venue_availability_schedule_outage_ex(JSContext* ctx, JSValueConst,
                                                        int, JSValueConst* argv)
{
  // (handle, start_ns, duration_ns, outage_type, policy, gtc_ttl_ns,
  //  degradation_latency_multiplier, wrong_side_recovery_bps)
  auto h = static_cast<LrvxVenueAvailabilityHandle>(getHandle(ctx, argv[0]));
  const int64_t start = toInt64(ctx, argv[1]);
  const int64_t dur = toInt64(ctx, argv[2]);
  const uint8_t outageType = static_cast<uint8_t>(toUint32(ctx, argv[3]));
  const uint8_t policy = static_cast<uint8_t>(toUint32(ctx, argv[4]));
  const int64_t ttl = toInt64(ctx, argv[5]);
  double degr = 1.0;
  JS_ToFloat64(ctx, &degr, argv[6]);
  double wsBps = 0.0;
  JS_ToFloat64(ctx, &wsBps, argv[7]);
  lrvx_venue_availability_schedule_outage_ex(h, start, dur, outageType, policy, ttl,
                                             degr, wsBps);
  return JS_UNDEFINED;
}
static JSValue js_venue_availability_submits_allowed(JSContext* ctx, JSValueConst,
                                                     int, JSValueConst* argv)
{
  const uint8_t v = lrvx_venue_availability_submits_allowed(
      static_cast<LrvxVenueAvailabilityHandle>(getHandle(ctx, argv[0])),
      toInt64(ctx, argv[1]));
  return JS_NewBool(ctx, v != 0);
}
static JSValue js_venue_availability_cancels_allowed(JSContext* ctx, JSValueConst,
                                                     int, JSValueConst* argv)
{
  const uint8_t v = lrvx_venue_availability_cancels_allowed(
      static_cast<LrvxVenueAvailabilityHandle>(getHandle(ctx, argv[0])),
      toInt64(ctx, argv[1]));
  return JS_NewBool(ctx, v != 0);
}
static JSValue js_venue_availability_book_updates_allowed(JSContext* ctx, JSValueConst,
                                                          int, JSValueConst* argv)
{
  const uint8_t v = lrvx_venue_availability_book_updates_allowed(
      static_cast<LrvxVenueAvailabilityHandle>(getHandle(ctx, argv[0])),
      toInt64(ctx, argv[1]));
  return JS_NewBool(ctx, v != 0);
}
static JSValue js_venue_availability_trades_allowed(JSContext* ctx, JSValueConst,
                                                    int, JSValueConst* argv)
{
  const uint8_t v = lrvx_venue_availability_trades_allowed(
      static_cast<LrvxVenueAvailabilityHandle>(getHandle(ctx, argv[0])),
      toInt64(ctx, argv[1]));
  return JS_NewBool(ctx, v != 0);
}
static JSValue js_venue_availability_latency_multiplier(JSContext* ctx, JSValueConst,
                                                        int, JSValueConst* argv)
{
  return JS_NewFloat64(
      ctx,
      lrvx_venue_availability_latency_multiplier(
          static_cast<LrvxVenueAvailabilityHandle>(getHandle(ctx, argv[0])),
          toInt64(ctx, argv[1])));
}
static JSValue js_venue_availability_consume_wrong_side_recovery_bps(
    JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewFloat64(
      ctx, lrvx_venue_availability_consume_wrong_side_recovery_bps(
               static_cast<LrvxVenueAvailabilityHandle>(getHandle(ctx, argv[0]))));
}
static JSValue js_executor_set_venue_availability(JSContext* ctx, JSValueConst, int,
                                                  JSValueConst* argv)
{
  lrvx_simulated_executor_set_venue_availability(
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
      static_cast<LrvxVenueAvailabilityHandle>(getHandle(ctx, argv[1])));
  return JS_UNDEFINED;
}

// Fee schedule.
static JSValue js_fee_schedule_create(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
  return createHandleObject(ctx, lrvx_fee_schedule_create(), lrvx_fee_schedule_destroy);
}
static JSValue js_fee_schedule_destroy(JSContext* ctx, JSValueConst, int,
                                       JSValueConst* argv)
{
  return js_generic_handle_destroy(ctx, argv[0]);
}
static JSValue js_fee_schedule_add_tier(JSContext* ctx, JSValueConst, int,
                                        JSValueConst* argv)
{
  double min_notional = 0.0, maker = 0.0, taker = 0.0;
  JS_ToFloat64(ctx, &min_notional, argv[1]);
  JS_ToFloat64(ctx, &maker, argv[2]);
  JS_ToFloat64(ctx, &taker, argv[3]);
  lrvx_fee_schedule_add_tier(
      static_cast<LrvxFeeScheduleHandle>(getHandle(ctx, argv[0])), min_notional, maker,
      taker);
  return JS_UNDEFINED;
}
static JSValue js_fee_schedule_load_profile(JSContext* ctx, JSValueConst, int,
                                            JSValueConst* argv)
{
  LRVX_JS_CSTRING_OR_THROW(name, ctx, argv[1]);
  const int ok = lrvx_fee_schedule_load_profile(
      static_cast<LrvxFeeScheduleHandle>(getHandle(ctx, argv[0])), name);
  std::string nameCopy = name ? name : "";
  if (!ok)
  {
    return JS_ThrowTypeError(ctx, "unknown fee-schedule profile: %s", nameCopy.c_str());
  }
  return JS_UNDEFINED;
}
// Liquidation engine.
static JSValue js_liquidation_engine_create(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
  return createHandleObject(ctx, lrvx_liquidation_engine_create(), lrvx_liquidation_engine_destroy);
}
static JSValue js_liquidation_engine_destroy(JSContext* ctx, JSValueConst, int,
                                             JSValueConst* argv)
{
  return js_generic_handle_destroy(ctx, argv[0]);
}
static JSValue js_liquidation_engine_add_tier(JSContext* ctx, JSValueConst, int,
                                              JSValueConst* argv)
{
  lrvx_liquidation_engine_add_tier(
      static_cast<LrvxLiquidationEngineHandle>(getHandle(ctx, argv[0])),
      toDouble(ctx, argv[1]), toDouble(ctx, argv[2]));
  return JS_UNDEFINED;
}
static JSValue js_liquidation_engine_set_insurance_fund_capital(
    JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  lrvx_liquidation_engine_set_insurance_fund_capital(
      static_cast<LrvxLiquidationEngineHandle>(getHandle(ctx, argv[0])),
      toDouble(ctx, argv[1]));
  return JS_UNDEFINED;
}
static JSValue js_liquidation_engine_insurance_fund_balance(
    JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewFloat64(
      ctx, lrvx_liquidation_engine_insurance_fund_balance(
               static_cast<LrvxLiquidationEngineHandle>(getHandle(ctx, argv[0]))));
}
static JSValue js_liquidation_engine_set_adl_enabled(JSContext* ctx, JSValueConst, int,
                                                     JSValueConst* argv)
{
  lrvx_liquidation_engine_set_adl_enabled(
      static_cast<LrvxLiquidationEngineHandle>(getHandle(ctx, argv[0])),
      static_cast<uint8_t>(toUint32(ctx, argv[1])));
  return JS_UNDEFINED;
}
static JSValue js_liquidation_engine_set_adl_ranking(JSContext* ctx, JSValueConst, int,
                                                     JSValueConst* argv)
{
  uint8_t code = 0;
  if (JS_IsString(argv[1]))
  {
    lrvx::JsCString s(ctx, argv[1]);
    if (s != nullptr)
    {
      std::string name(s);
      if (name == "binance")
      {
        code = 1;
      }
      else if (name == "bybit")
      {
        code = 2;
      }
      else if (name == "position_size")
      {
        code = 3;
      }
    }
  }
  else
  {
    code = static_cast<uint8_t>(toUint32(ctx, argv[1]));
  }
  lrvx_liquidation_engine_set_adl_ranking(
      static_cast<LrvxLiquidationEngineHandle>(getHandle(ctx, argv[0])), code);
  return JS_UNDEFINED;
}
static JSValue js_liquidation_engine_adl_ranking(JSContext* ctx, JSValueConst, int,
                                                 JSValueConst* argv)
{
  const uint8_t code = lrvx_liquidation_engine_adl_ranking(
      static_cast<LrvxLiquidationEngineHandle>(getHandle(ctx, argv[0])));
  return JS_NewUint32(ctx, code);
}
static JSValue js_liquidation_engine_set_liquidation_slippage_bps(
    JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  lrvx_liquidation_engine_set_liquidation_slippage_bps(
      static_cast<LrvxLiquidationEngineHandle>(getHandle(ctx, argv[0])),
      toDouble(ctx, argv[1]));
  return JS_UNDEFINED;
}
static JSValue js_liquidation_engine_open_position(JSContext* ctx, JSValueConst, int,
                                                   JSValueConst* argv)
{
  lrvx_liquidation_engine_open_position(
      static_cast<LrvxLiquidationEngineHandle>(getHandle(ctx, argv[0])),
      static_cast<uint64_t>(toInt64(ctx, argv[1])),
      toUint32(ctx, argv[2]), toDouble(ctx, argv[3]),
      toDouble(ctx, argv[4]), toDouble(ctx, argv[5]));
  return JS_UNDEFINED;
}
static JSValue js_liquidation_engine_close_position(JSContext* ctx, JSValueConst, int,
                                                    JSValueConst* argv)
{
  lrvx_liquidation_engine_close_position(
      static_cast<LrvxLiquidationEngineHandle>(getHandle(ctx, argv[0])),
      static_cast<uint64_t>(toInt64(ctx, argv[1])),
      toUint32(ctx, argv[2]));
  return JS_UNDEFINED;
}
static JSValue js_liquidation_engine_on_mark(JSContext* ctx, JSValueConst, int,
                                             JSValueConst* argv)
{
  return JS_NewUint32(ctx, lrvx_liquidation_engine_on_mark(
                               static_cast<LrvxLiquidationEngineHandle>(getHandle(ctx, argv[0])),
                               toUint32(ctx, argv[1]), toDouble(ctx, argv[2])));
}
static JSValue js_liquidation_engine_liquidations_count(JSContext* ctx, JSValueConst, int,
                                                        JSValueConst* argv)
{
  return JS_NewFloat64(
      ctx, static_cast<double>(lrvx_liquidation_engine_liquidations_count(
               static_cast<LrvxLiquidationEngineHandle>(getHandle(ctx, argv[0])))));
}
static JSValue js_liquidation_engine_insurance_payments_count(JSContext* ctx, JSValueConst, int,
                                                              JSValueConst* argv)
{
  return JS_NewFloat64(
      ctx, static_cast<double>(lrvx_liquidation_engine_insurance_payments_count(
               static_cast<LrvxLiquidationEngineHandle>(getHandle(ctx, argv[0])))));
}
static JSValue js_liquidation_engine_adl_closeouts_count(JSContext* ctx, JSValueConst, int,
                                                         JSValueConst* argv)
{
  return JS_NewFloat64(
      ctx, static_cast<double>(lrvx_liquidation_engine_adl_closeouts_count(
               static_cast<LrvxLiquidationEngineHandle>(getHandle(ctx, argv[0])))));
}
static JSValue js_liquidation_engine_load_profile(JSContext* ctx, JSValueConst, int,
                                                  JSValueConst* argv)
{
  lrvx_liquidation_engine_load_profile(
      static_cast<LrvxLiquidationEngineHandle>(getHandle(ctx, argv[0])),
      static_cast<uint8_t>(toUint32(ctx, argv[1])));
  return JS_UNDEFINED;
}
static JSValue js_liquidation_engine_set_executor(JSContext* ctx, JSValueConst, int argc,
                                                  JSValueConst* argv)
{
  LrvxSimulatedExecutorHandle exec_h = nullptr;
  if (argc >= 2 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1]))
  {
    exec_h = static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[1]));
  }
  lrvx_liquidation_engine_set_executor(
      static_cast<LrvxLiquidationEngineHandle>(getHandle(ctx, argv[0])), exec_h);
  return JS_UNDEFINED;
}
static JSValue js_liquidation_engine_ticks_to_first_adl(JSContext* ctx, JSValueConst, int,
                                                        JSValueConst* argv)
{
  return JS_NewFloat64(
      ctx, static_cast<double>(lrvx_liquidation_engine_ticks_to_first_adl(
               static_cast<LrvxLiquidationEngineHandle>(getHandle(ctx, argv[0])))));
}
static JSValue js_liquidation_engine_reset_stats(JSContext* ctx, JSValueConst, int,
                                                 JSValueConst* argv)
{
  lrvx_liquidation_engine_reset_stats(
      static_cast<LrvxLiquidationEngineHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}
static JSValue js_liquidation_engine_set_mark_impact_model(
    JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  uint8_t code = 0;
  if (argc >= 2 && JS_IsString(argv[1]))
  {
    lrvx::JsCString s(ctx, argv[1]);
    if (s != nullptr)
    {
      const std::string name(s);
      if (name == "book_anchored")
      {
        code = 1;
      }
      else if (name == "book_only")
      {
        code = 2;
      }
    }
  }
  else if (argc >= 2 && JS_IsNumber(argv[1]))
  {
    uint32_t v = 0;
    JS_ToUint32(ctx, &v, argv[1]);
    code = static_cast<uint8_t>(v);
  }
  double weight = 0.3;
  if (argc >= 3 && JS_IsNumber(argv[2]))
  {
    JS_ToFloat64(ctx, &weight, argv[2]);
  }
  lrvx_liquidation_engine_set_mark_impact_model(
      static_cast<LrvxLiquidationEngineHandle>(getHandle(ctx, argv[0])), code,
      weight);
  return JS_UNDEFINED;
}
static JSValue js_liquidation_engine_mark_impact_model(JSContext* ctx,
                                                       JSValueConst, int,
                                                       JSValueConst* argv)
{
  const uint8_t code = lrvx_liquidation_engine_mark_impact_model(
      static_cast<LrvxLiquidationEngineHandle>(getHandle(ctx, argv[0])));
  const char* name = "none";
  switch (code)
  {
    case 1:
      name = "book_anchored";
      break;
    case 2:
      name = "book_only";
      break;
    default:
      name = "none";
      break;
  }
  return JS_NewString(ctx, name);
}
static JSValue js_liquidation_engine_mark_impact_weight(JSContext* ctx,
                                                        JSValueConst, int,
                                                        JSValueConst* argv)
{
  return JS_NewFloat64(
      ctx, lrvx_liquidation_engine_mark_impact_weight(
               static_cast<LrvxLiquidationEngineHandle>(getHandle(ctx, argv[0]))));
}
static JSValue js_liquidation_engine_set_max_cascade_depth(
    JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  uint32_t depth = 0;
  JS_ToUint32(ctx, &depth, argv[1]);
  lrvx_liquidation_engine_set_max_cascade_depth(
      static_cast<LrvxLiquidationEngineHandle>(getHandle(ctx, argv[0])), depth);
  return JS_UNDEFINED;
}
static JSValue js_liquidation_engine_max_cascade_depth(JSContext* ctx,
                                                       JSValueConst, int,
                                                       JSValueConst* argv)
{
  return JS_NewUint32(
      ctx, lrvx_liquidation_engine_max_cascade_depth(
               static_cast<LrvxLiquidationEngineHandle>(getHandle(ctx, argv[0]))));
}

// ===== Account =====
static JSValue js_account_create(JSContext* ctx, JSValueConst, int argc,
                                 JSValueConst* argv)
{
  int64_t id = 0;
  double equity = 0.0;
  if (argc >= 1)
  {
    JS_ToInt64Ext(ctx, &id, argv[0]);
  }
  if (argc >= 2)
  {
    JS_ToFloat64(ctx, &equity, argv[1]);
  }
  return createHandleObject(
      ctx, lrvx_account_create(static_cast<uint64_t>(id), equity), lrvx_account_destroy);
}
static JSValue js_account_destroy(JSContext* ctx, JSValueConst, int,
                                  JSValueConst* argv)
{
  return js_generic_handle_destroy(ctx, argv[0]);
}
static JSValue js_account_id(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewInt64(ctx, static_cast<int64_t>(lrvx_account_id(
                              static_cast<LrvxAccountHandle>(
                                  getHandle(ctx, argv[0])))));
}
static JSValue js_account_equity(JSContext* ctx, JSValueConst, int,
                                 JSValueConst* argv)
{
  return JS_NewFloat64(
      ctx, lrvx_account_equity(
               static_cast<LrvxAccountHandle>(getHandle(ctx, argv[0]))));
}
static JSValue js_account_set_equity(JSContext* ctx, JSValueConst, int,
                                     JSValueConst* argv)
{
  double v = 0.0;
  JS_ToFloat64(ctx, &v, argv[1]);
  lrvx_account_set_equity(
      static_cast<LrvxAccountHandle>(getHandle(ctx, argv[0])), v);
  return JS_UNDEFINED;
}
static JSValue js_account_add_equity(JSContext* ctx, JSValueConst, int,
                                     JSValueConst* argv)
{
  double v = 0.0;
  JS_ToFloat64(ctx, &v, argv[1]);
  lrvx_account_add_equity(
      static_cast<LrvxAccountHandle>(getHandle(ctx, argv[0])), v);
  return JS_UNDEFINED;
}
static JSValue js_account_margin_mode(JSContext* ctx, JSValueConst, int,
                                      JSValueConst* argv)
{
  const uint8_t m = lrvx_account_margin_mode(
      static_cast<LrvxAccountHandle>(getHandle(ctx, argv[0])));
  return JS_NewString(ctx, m == 1 ? "isolated" : "cross");
}
static JSValue js_account_set_margin_mode(JSContext* ctx, JSValueConst, int argc,
                                          JSValueConst* argv)
{
  uint8_t code = 0;
  if (argc >= 2 && JS_IsString(argv[1]))
  {
    lrvx::JsCString s(ctx, argv[1]);
    if (s != nullptr)
    {
      if (std::string(s) == "isolated")
      {
        code = 1;
      }
    }
  }
  else if (argc >= 2 && JS_IsNumber(argv[1]))
  {
    uint32_t v = 0;
    JS_ToUint32(ctx, &v, argv[1]);
    code = static_cast<uint8_t>(v);
  }
  lrvx_account_set_margin_mode(
      static_cast<LrvxAccountHandle>(getHandle(ctx, argv[0])), code);
  return JS_UNDEFINED;
}
static JSValue js_account_open_position(JSContext* ctx, JSValueConst, int argc,
                                        JSValueConst* argv)
{
  uint32_t sym = 0;
  double qty = 0.0, entry = 0.0;
  JS_ToUint32(ctx, &sym, argv[1]);
  JS_ToFloat64(ctx, &qty, argv[2]);
  JS_ToFloat64(ctx, &entry, argv[3]);
  if (argc >= 5 && JS_IsNumber(argv[4]))
  {
    double iso = 0.0;
    JS_ToFloat64(ctx, &iso, argv[4]);
    lrvx_account_open_position_isolated(
        static_cast<LrvxAccountHandle>(getHandle(ctx, argv[0])), sym, qty,
        entry, iso);
  }
  else
  {
    lrvx_account_open_position(
        static_cast<LrvxAccountHandle>(getHandle(ctx, argv[0])), sym, qty,
        entry);
  }
  return JS_UNDEFINED;
}
static JSValue js_account_close_position(JSContext* ctx, JSValueConst, int,
                                         JSValueConst* argv)
{
  uint32_t sym = 0;
  JS_ToUint32(ctx, &sym, argv[1]);
  lrvx_account_close_position(
      static_cast<LrvxAccountHandle>(getHandle(ctx, argv[0])), sym);
  return JS_UNDEFINED;
}
static JSValue js_account_position_count(JSContext* ctx, JSValueConst, int,
                                         JSValueConst* argv)
{
  return JS_NewUint32(
      ctx, lrvx_account_position_count(
               static_cast<LrvxAccountHandle>(getHandle(ctx, argv[0]))));
}
// An optional clock-reading argument that was given: a Number or a BigInt.
static bool isClockReading(JSContext* ctx, JSValueConst v)
{
  return JS_IsNumber(v) || JS_IsBigInt(ctx, v);
}
static JSValue js_account_set_mark(JSContext* ctx, JSValueConst, int argc,
                                   JSValueConst* argv)
{
  uint32_t sym = 0;
  double px = 0.0;
  JS_ToUint32(ctx, &sym, argv[1]);
  JS_ToFloat64(ctx, &px, argv[2]);
  if (argc >= 4 && isClockReading(ctx, argv[3]))
  {
    int64_t ts = 0;
    JS_ToInt64Ext(ctx, &ts, argv[3]);
    lrvx_account_set_mark_at(
        static_cast<LrvxAccountHandle>(getHandle(ctx, argv[0])), sym, px, ts);
  }
  else
  {
    lrvx_account_set_mark(
        static_cast<LrvxAccountHandle>(getHandle(ctx, argv[0])), sym, px);
  }
  return JS_UNDEFINED;
}
static JSValue js_account_mark_ts(JSContext* ctx, JSValueConst, int,
                                  JSValueConst* argv)
{
  uint32_t sym = 0;
  JS_ToUint32(ctx, &sym, argv[1]);
  return JS_NewBigInt64(
      ctx, lrvx_account_mark_ts(
               static_cast<LrvxAccountHandle>(getHandle(ctx, argv[0])), sym));
}
static JSValue js_account_has_stale_marks(JSContext* ctx, JSValueConst, int,
                                          JSValueConst* argv)
{
  int64_t now = 0, budget = 0;
  JS_ToInt64Ext(ctx, &now, argv[1]);
  JS_ToInt64Ext(ctx, &budget, argv[2]);
  return JS_NewBool(
      ctx, lrvx_account_has_stale_marks(
               static_cast<LrvxAccountHandle>(getHandle(ctx, argv[0])), now,
               budget) != 0);
}
static JSValue js_liquidation_engine_on_marks(JSContext* ctx, JSValueConst,
                                              int argc, JSValueConst* argv)
{
  // argv[0]: engine handle, argv[1]: array of [sym, price] pairs,
  // argv[2]: optional ts_ns.
  uint32_t n = 0;
  JSValue lenVal = JS_GetPropertyStr(ctx, argv[1], "length");
  JS_ToUint32(ctx, &n, lenVal);
  JS_FreeValue(ctx, lenVal);
  std::vector<uint32_t> syms(n);
  std::vector<double> prices(n);
  for (uint32_t i = 0; i < n; ++i)
  {
    JSValue pair = JS_GetPropertyUint32(ctx, argv[1], i);
    JSValue s = JS_GetPropertyUint32(ctx, pair, 0);
    JSValue p = JS_GetPropertyUint32(ctx, pair, 1);
    JS_ToUint32(ctx, &syms[i], s);
    JS_ToFloat64(ctx, &prices[i], p);
    JS_FreeValue(ctx, s);
    JS_FreeValue(ctx, p);
    JS_FreeValue(ctx, pair);
  }
  int64_t ts = 0;
  if (argc >= 3 && isClockReading(ctx, argv[2]))
  {
    JS_ToInt64Ext(ctx, &ts, argv[2]);
  }
  const uint32_t total = lrvx_liquidation_engine_on_marks(
      static_cast<LrvxLiquidationEngineHandle>(getHandle(ctx, argv[0])), n,
      syms.data(), prices.data(), ts);
  return JS_NewUint32(ctx, total);
}
static JSValue js_account_total_notional(JSContext* ctx, JSValueConst, int,
                                         JSValueConst* argv)
{
  return JS_NewFloat64(
      ctx, lrvx_account_total_notional(
               static_cast<LrvxAccountHandle>(getHandle(ctx, argv[0]))));
}
static JSValue js_account_total_upnl(JSContext* ctx, JSValueConst, int,
                                     JSValueConst* argv)
{
  return JS_NewFloat64(
      ctx, lrvx_account_total_unrealised_pnl(
               static_cast<LrvxAccountHandle>(getHandle(ctx, argv[0]))));
}
static JSValue js_account_record_fill(JSContext* ctx, JSValueConst, int argc,
                                      JSValueConst* argv)
{
  int64_t ts = 0;
  double n = 0.0;
  JS_ToInt64Ext(ctx, &ts, argv[1]);
  JS_ToFloat64(ctx, &n, argv[2]);
  if (argc >= 4 && JS_IsNumber(argv[3]))
  {
    uint32_t sym = 0;
    JS_ToUint32(ctx, &sym, argv[3]);
    lrvx_account_record_fill_ex(
        static_cast<LrvxAccountHandle>(getHandle(ctx, argv[0])), ts, n, sym);
  }
  else
  {
    lrvx_account_record_fill(
        static_cast<LrvxAccountHandle>(getHandle(ctx, argv[0])), ts, n);
  }
  return JS_UNDEFINED;
}
static JSValue js_account_rolling_notional_by_symbol(JSContext* ctx,
                                                     JSValueConst, int,
                                                     JSValueConst* argv)
{
  auto h = static_cast<LrvxAccountHandle>(getHandle(ctx, argv[0]));
  const uint32_t n = lrvx_account_rolling_notional_by_symbol_size(h);
  std::vector<uint32_t> syms(n);
  std::vector<double> totals(n);
  lrvx_account_rolling_notional_by_symbol_copy(h, syms.data(), totals.data(), n);
  JSValue arr = JS_NewArray(ctx);
  for (uint32_t i = 0; i < n; ++i)
  {
    JSValue pair = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, pair, "symbol", JS_NewUint32(ctx, syms[i]));
    JS_SetPropertyStr(ctx, pair, "notional", JS_NewFloat64(ctx, totals[i]));
    JS_SetPropertyUint32(ctx, arr, i, pair);
  }
  return arr;
}
static JSValue js_account_rolling_notional_30d(JSContext* ctx, JSValueConst, int,
                                               JSValueConst* argv)
{
  return JS_NewFloat64(
      ctx, lrvx_account_rolling_notional_30d(
               static_cast<LrvxAccountHandle>(getHandle(ctx, argv[0]))));
}
static JSValue js_account_reset_rolling(JSContext* ctx, JSValueConst, int,
                                        JSValueConst* argv)
{
  lrvx_account_reset_rolling(
      static_cast<LrvxAccountHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}
static JSValue js_liquidation_engine_attach_account(JSContext* ctx, JSValueConst,
                                                    int, JSValueConst* argv)
{
  lrvx_liquidation_engine_attach_account(
      static_cast<LrvxLiquidationEngineHandle>(getHandle(ctx, argv[0])),
      static_cast<LrvxAccountHandle>(getHandle(ctx, argv[1])));
  return JS_UNDEFINED;
}
static JSValue js_liquidation_engine_detach_account(JSContext* ctx, JSValueConst,
                                                    int, JSValueConst* argv)
{
  int64_t id = 0;
  JS_ToInt64Ext(ctx, &id, argv[1]);
  lrvx_liquidation_engine_detach_account(
      static_cast<LrvxLiquidationEngineHandle>(getHandle(ctx, argv[0])),
      static_cast<uint64_t>(id));
  return JS_UNDEFINED;
}
static JSValue js_fee_schedule_bind_account(JSContext* ctx, JSValueConst, int,
                                            JSValueConst* argv)
{
  lrvx_fee_schedule_bind_account(
      static_cast<LrvxFeeScheduleHandle>(getHandle(ctx, argv[0])),
      static_cast<LrvxAccountHandle>(getHandle(ctx, argv[1])));
  return JS_UNDEFINED;
}
static JSValue js_fee_schedule_clear_account_binding(JSContext* ctx, JSValueConst,
                                                     int, JSValueConst* argv)
{
  lrvx_fee_schedule_clear_account_binding(
      static_cast<LrvxFeeScheduleHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}

// ===== VenueStack =====
static JSValue js_venue_stack_create(JSContext* ctx, JSValueConst, int argc,
                                     JSValueConst* argv)
{
  uint32_t venueCode = 0;
  int64_t accountId = 0;
  double equity = 0.0;
  if (argc >= 1)
  {
    JS_ToUint32(ctx, &venueCode, argv[0]);
  }
  if (argc >= 2)
  {
    JS_ToInt64Ext(ctx, &accountId, argv[1]);
  }
  if (argc >= 3)
  {
    JS_ToFloat64(ctx, &equity, argv[2]);
  }
  return createHandleObject(
      ctx, lrvx_venue_stack_create(static_cast<uint8_t>(venueCode), static_cast<uint64_t>(accountId), equity), lrvx_venue_stack_destroy);
}
static JSValue js_venue_stack_destroy(JSContext* ctx, JSValueConst, int,
                                      JSValueConst* argv)
{
  return js_generic_handle_destroy(ctx, argv[0]);
}
static JSValue js_venue_stack_executor(JSContext* ctx, JSValueConst, int,
                                       JSValueConst* argv)
{
  // Borrowed: a view into the venue stack's own member, not a
  // separate allocation -- its lifetime is the stack's.
  return createHandleObject(
      ctx, lrvx_venue_stack_executor(static_cast<LrvxVenueStackHandle>(getHandle(ctx, argv[0]))),
      nullptr);
}
static JSValue js_venue_stack_account(JSContext* ctx, JSValueConst, int,
                                      JSValueConst* argv)
{
  // Borrowed: a view into the venue stack's own member, not a
  // separate allocation -- its lifetime is the stack's.
  return createHandleObject(
      ctx, lrvx_venue_stack_account(static_cast<LrvxVenueStackHandle>(getHandle(ctx, argv[0]))),
      nullptr);
}
static JSValue js_venue_stack_liquidation(JSContext* ctx, JSValueConst, int,
                                          JSValueConst* argv)
{
  // Borrowed: a view into the venue stack's own member, not a
  // separate allocation -- its lifetime is the stack's.
  return createHandleObject(
      ctx, lrvx_venue_stack_liquidation(static_cast<LrvxVenueStackHandle>(getHandle(ctx, argv[0]))),
      nullptr);
}
static JSValue js_venue_stack_fees(JSContext* ctx, JSValueConst, int,
                                   JSValueConst* argv)
{
  // Borrowed: a view into the venue stack's own member, not a
  // separate allocation -- its lifetime is the stack's.
  return createHandleObject(
      ctx, lrvx_venue_stack_fees(static_cast<LrvxVenueStackHandle>(getHandle(ctx, argv[0]))),
      nullptr);
}
static JSValue js_venue_stack_funding(JSContext* ctx, JSValueConst, int,
                                      JSValueConst* argv)
{
  // Borrowed: a view into the venue stack's own member, not a
  // separate allocation -- its lifetime is the stack's.
  return createHandleObject(
      ctx, lrvx_venue_stack_funding(static_cast<LrvxVenueStackHandle>(getHandle(ctx, argv[0]))),
      nullptr);
}
static JSValue js_venue_stack_venue(JSContext* ctx, JSValueConst, int,
                                    JSValueConst* argv)
{
  // Borrowed: a view into the venue stack's own member, not a
  // separate allocation -- its lifetime is the stack's.
  return createHandleObject(
      ctx, lrvx_venue_stack_venue(static_cast<LrvxVenueStackHandle>(getHandle(ctx, argv[0]))),
      nullptr);
}
static JSValue js_venue_stack_venue_name(JSContext* ctx, JSValueConst, int,
                                         JSValueConst* argv)
{
  const char* name = lrvx_venue_stack_venue_name(
      static_cast<LrvxVenueStackHandle>(getHandle(ctx, argv[0])));
  return JS_NewString(ctx, name ? name : "");
}
template <typename T, typename SizeFn, typename CopyFn>
static JSValue copyVecToJs(JSContext* ctx, void* handle, SizeFn sizeFn, CopyFn copyFn)
{
  const uint32_t n = sizeFn(handle);
  std::vector<T> buf(n);
  copyFn(handle, buf.data(), n);
  JSValue arr = JS_NewArray(ctx);
  for (uint32_t i = 0; i < n; ++i)
  {
    JS_SetPropertyUint32(ctx, arr, i, JS_NewFloat64(ctx, static_cast<double>(buf[i])));
  }
  return arr;
}
static JSValue js_liquidation_engine_deficits_paid_by_fund(JSContext* ctx, JSValueConst, int,
                                                           JSValueConst* argv)
{
  return copyVecToJs<double>(
      ctx, getHandle(ctx, argv[0]),
      [](void* h)
      { return lrvx_liquidation_engine_deficits_paid_by_fund_size(
            static_cast<LrvxLiquidationEngineHandle>(h)); },
      [](void* h, double* o, uint32_t n)
      {
        lrvx_liquidation_engine_deficits_paid_by_fund_copy(
            static_cast<LrvxLiquidationEngineHandle>(h), o, n);
      });
}
static JSValue js_liquidation_engine_deficits_paid_by_adl(JSContext* ctx, JSValueConst, int,
                                                          JSValueConst* argv)
{
  return copyVecToJs<double>(
      ctx, getHandle(ctx, argv[0]),
      [](void* h)
      { return lrvx_liquidation_engine_deficits_paid_by_adl_size(
            static_cast<LrvxLiquidationEngineHandle>(h)); },
      [](void* h, double* o, uint32_t n)
      {
        lrvx_liquidation_engine_deficits_paid_by_adl_copy(
            static_cast<LrvxLiquidationEngineHandle>(h), o, n);
      });
}
static JSValue js_liquidation_engine_cascade_sizes(JSContext* ctx, JSValueConst, int,
                                                   JSValueConst* argv)
{
  return copyVecToJs<uint32_t>(
      ctx, getHandle(ctx, argv[0]),
      [](void* h)
      { return lrvx_liquidation_engine_cascade_sizes_size(
            static_cast<LrvxLiquidationEngineHandle>(h)); },
      [](void* h, uint32_t* o, uint32_t n)
      {
        lrvx_liquidation_engine_cascade_sizes_copy(
            static_cast<LrvxLiquidationEngineHandle>(h), o, n);
      });
}
static JSValue js_liquidation_engine_fund_balance_history(JSContext* ctx, JSValueConst, int,
                                                          JSValueConst* argv)
{
  return copyVecToJs<double>(
      ctx, getHandle(ctx, argv[0]),
      [](void* h)
      { return lrvx_liquidation_engine_fund_balance_history_size(
            static_cast<LrvxLiquidationEngineHandle>(h)); },
      [](void* h, double* o, uint32_t n)
      {
        lrvx_liquidation_engine_fund_balance_history_copy(
            static_cast<LrvxLiquidationEngineHandle>(h), o, n);
      });
}

static JSValue js_fee_schedule_record_fill(JSContext* ctx, JSValueConst, int,
                                           JSValueConst* argv)
{
  double notional = 0.0;
  JS_ToFloat64(ctx, &notional, argv[2]);
  lrvx_fee_schedule_record_fill(
      static_cast<LrvxFeeScheduleHandle>(getHandle(ctx, argv[0])),
      toInt64(ctx, argv[1]), notional);
  return JS_UNDEFINED;
}
static JSValue js_fee_schedule_fee_for(JSContext* ctx, JSValueConst, int,
                                       JSValueConst* argv)
{
  double notional = 0.0;
  JS_ToFloat64(ctx, &notional, argv[2]);
  uint8_t is_maker = static_cast<uint8_t>(toUint32(ctx, argv[3]));
  return JS_NewFloat64(ctx, lrvx_fee_schedule_fee_for(
                                static_cast<LrvxFeeScheduleHandle>(
                                    getHandle(ctx, argv[0])),
                                toInt64(ctx, argv[1]), notional, is_maker));
}
static JSValue js_fee_schedule_current_tier(JSContext* ctx, JSValueConst, int,
                                            JSValueConst* argv)
{
  return JS_NewUint32(ctx, lrvx_fee_schedule_current_tier(
                               static_cast<LrvxFeeScheduleHandle>(
                                   getHandle(ctx, argv[0]))));
}
static JSValue js_fee_schedule_rolling_notional(JSContext* ctx, JSValueConst, int,
                                                JSValueConst* argv)
{
  return JS_NewFloat64(ctx, lrvx_fee_schedule_rolling_notional(
                                static_cast<LrvxFeeScheduleHandle>(
                                    getHandle(ctx, argv[0]))));
}
static JSValue js_fee_schedule_reset(JSContext* ctx, JSValueConst, int,
                                     JSValueConst* argv)
{
  lrvx_fee_schedule_reset_rolling(
      static_cast<LrvxFeeScheduleHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}

// Funding schedule.
static JSValue js_funding_schedule_create(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
  return createHandleObject(ctx, lrvx_funding_schedule_create(), lrvx_funding_schedule_destroy);
}
static JSValue js_funding_schedule_destroy(JSContext* ctx, JSValueConst, int,
                                           JSValueConst* argv)
{
  return js_generic_handle_destroy(ctx, argv[0]);
}
static JSValue js_funding_schedule_set_constant(JSContext* ctx, JSValueConst, int,
                                                JSValueConst* argv)
{
  auto h = static_cast<LrvxFundingScheduleHandle>(getHandle(ctx, argv[0]));
  int64_t interval = toInt64(ctx, argv[1]);
  double rate = 0.0;
  JS_ToFloat64(ctx, &rate, argv[2]);
  lrvx_funding_schedule_set_constant(h, interval, rate);
  return JS_UNDEFINED;
}
static JSValue js_funding_schedule_load_profile(JSContext* ctx, JSValueConst, int,
                                                JSValueConst* argv)
{
  auto h = static_cast<LrvxFundingScheduleHandle>(getHandle(ctx, argv[0]));
  LRVX_JS_CSTRING_OR_THROW(name, ctx, argv[1]);
  const int ok = lrvx_funding_schedule_load_profile(h, name);
  std::string nameCopy = name ? name : "";
  if (!ok)
  {
    return JS_ThrowTypeError(ctx, "unknown funding-schedule profile: %s", nameCopy.c_str());
  }
  return JS_UNDEFINED;
}
static JSValue js_funding_schedule_load_tape(JSContext* ctx, JSValueConst, int,
                                             JSValueConst* argv)
{
  auto h = static_cast<LrvxFundingScheduleHandle>(getHandle(ctx, argv[0]));
  LRVX_JS_CSTRING_OR_THROW(path, ctx, argv[1]);
  lrvx_clear_last_error();
  const uint8_t ok = lrvx_funding_schedule_load_tape(h, path);
  return JS_NewBool(ctx, ok != 0);
}
// Why the last call that reports failure through its return value failed.
static JSValue js_last_error(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
  return JS_NewString(ctx, lrvx_last_error_message());
}
static JSValue js_funding_schedule_set_tape_by_symbol(JSContext* ctx, JSValueConst,
                                                      int, JSValueConst* argv)
{
  // (handle, timestamps_array, symbols_array, rates_array)
  auto h = static_cast<LrvxFundingScheduleHandle>(getHandle(ctx, argv[0]));
  uint32_t n = 0;
  JSValue lenVal = JS_GetPropertyStr(ctx, argv[1], "length");
  JS_ToUint32(ctx, &n, lenVal);
  JS_FreeValue(ctx, lenVal);
  std::vector<int64_t> ts(n);
  std::vector<uint32_t> sy(n);
  std::vector<double> rates(n);
  for (uint32_t i = 0; i < n; ++i)
  {
    JSValue tv = JS_GetPropertyUint32(ctx, argv[1], i);
    JS_ToInt64Ext(ctx, &ts[i], tv);
    JS_FreeValue(ctx, tv);
    JSValue sv = JS_GetPropertyUint32(ctx, argv[2], i);
    JS_ToUint32(ctx, &sy[i], sv);
    JS_FreeValue(ctx, sv);
    JSValue rv = JS_GetPropertyUint32(ctx, argv[3], i);
    JS_ToFloat64(ctx, &rates[i], rv);
    JS_FreeValue(ctx, rv);
  }
  lrvx_funding_schedule_set_tape_by_symbol(h, ts.data(), sy.data(), rates.data(), n);
  return JS_UNDEFINED;
}
static JSValue js_funding_schedule_set_constant_rate(JSContext* ctx, JSValueConst, int,
                                                     JSValueConst* argv)
{
  auto h = static_cast<LrvxFundingScheduleHandle>(getHandle(ctx, argv[0]));
  double rate = 0.0;
  JS_ToFloat64(ctx, &rate, argv[1]);
  lrvx_funding_schedule_set_constant_rate(h, rate);
  return JS_UNDEFINED;
}
static JSValue js_funding_schedule_reset(JSContext* ctx, JSValueConst, int,
                                         JSValueConst* argv)
{
  lrvx_funding_schedule_reset(
      static_cast<LrvxFundingScheduleHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}
static JSValue js_funding_schedule_tick(JSContext* ctx, JSValueConst, int,
                                        JSValueConst* argv)
{
  auto h = static_cast<LrvxFundingScheduleHandle>(getHandle(ctx, argv[0]));
  int64_t nowNs = toInt64(ctx, argv[1]);
  auto syms = argv[2];
  auto pos = argv[3];
  auto mk = argv[4];
  uint32_t n = 0;
  JSValue lenVal = JS_GetPropertyStr(ctx, syms, "length");
  JS_ToUint32(ctx, &n, lenVal);
  JS_FreeValue(ctx, lenVal);
  std::vector<uint32_t> sy(n);
  std::vector<double> ps(n);
  std::vector<double> mp(n);
  for (uint32_t i = 0; i < n; ++i)
  {
    JSValue v = JS_GetPropertyUint32(ctx, syms, i);
    sy[i] = toUint32(ctx, v);
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyUint32(ctx, pos, i);
    double d = 0.0;
    JS_ToFloat64(ctx, &d, v);
    ps[i] = d;
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyUint32(ctx, mk, i);
    d = 0.0;
    JS_ToFloat64(ctx, &d, v);
    mp[i] = d;
    JS_FreeValue(ctx, v);
  }
  // One call settles and hands over; a second is made only when more events
  // were due than the first had room for, and that first one settled nothing.
  uint32_t room = 16;
  std::vector<int64_t> ts(room);
  std::vector<double> buf(room * 6, 0.0);
  uint32_t count = lrvx_funding_schedule_tick_exact(h, nowNs, sy.data(), ps.data(), mp.data(), n,
                                                    ts.data(), buf.data(), room);
  if (count > room)
  {
    room = count;
    ts.assign(room, 0);
    buf.assign(room * 6, 0.0);
    count = lrvx_funding_schedule_tick_exact(h, nowNs, sy.data(), ps.data(), mp.data(), n,
                                             ts.data(), buf.data(), room);
  }
  JSValue arr = JS_NewArray(ctx);
  for (uint32_t i = 0; i < count; ++i)
  {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "timestampNs", JS_NewBigInt64(ctx, ts[i]));
    JS_SetPropertyStr(ctx, obj, "symbol", JS_NewFloat64(ctx, buf[i * 6 + 1]));
    JS_SetPropertyStr(ctx, obj, "rate", JS_NewFloat64(ctx, buf[i * 6 + 2]));
    JS_SetPropertyStr(ctx, obj, "markPrice", JS_NewFloat64(ctx, buf[i * 6 + 3]));
    JS_SetPropertyStr(ctx, obj, "positionSigned", JS_NewFloat64(ctx, buf[i * 6 + 4]));
    JS_SetPropertyStr(ctx, obj, "amount", JS_NewFloat64(ctx, buf[i * 6 + 5]));
    JS_SetPropertyUint32(ctx, arr, i, obj);
  }
  return arr;
}
static JSValue js_executor_on_trade_qty(JSContext* ctx, JSValueConst, int,
                                        JSValueConst* argv)
{
  lrvx_simulated_executor_on_trade_qty(static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
                                       toUint32(ctx, argv[1]), toDouble(ctx, argv[2]),
                                       toDouble(ctx, argv[3]),
                                       static_cast<uint8_t>(toUint32(ctx, argv[4])));
  return JS_UNDEFINED;
}
static JSValue js_executor_on_best_levels(JSContext* ctx, JSValueConst, int,
                                          JSValueConst* argv)
{
  lrvx_simulated_executor_on_best_levels(static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
                                         toUint32(ctx, argv[1]), toDouble(ctx, argv[2]),
                                         toDouble(ctx, argv[3]), toDouble(ctx, argv[4]),
                                         toDouble(ctx, argv[5]));
  return JS_UNDEFINED;
}
static JSValue js_executor_on_top_of_book(JSContext* ctx, JSValueConst, int,
                                          JSValueConst* argv)
{
  lrvx_simulated_executor_on_top_of_book(static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[0])),
                                         toUint32(ctx, argv[1]),
                                         static_cast<uint8_t>(toUint32(ctx, argv[2])),
                                         toDouble(ctx, argv[3]), toDouble(ctx, argv[4]),
                                         static_cast<uint8_t>(toUint32(ctx, argv[5])),
                                         toDouble(ctx, argv[6]), toDouble(ctx, argv[7]));
  return JS_UNDEFINED;
}

static JSValue js_backtest_result_create(JSContext* ctx, JSValueConst, int,
                                         JSValueConst* argv)
{
  return createHandleObject(
      ctx, lrvx_backtest_result_create(toDouble(ctx, argv[0]), toDouble(ctx, argv[1]), static_cast<uint8_t>(toUint32(ctx, argv[2])), toDouble(ctx, argv[3]), toDouble(ctx, argv[4]), toDouble(ctx, argv[5])), lrvx_backtest_result_destroy);
}
static JSValue js_backtest_result_destroy(JSContext* ctx, JSValueConst, int,
                                          JSValueConst* argv)
{
  return js_generic_handle_destroy(ctx, argv[0]);
}
static JSValue js_backtest_result_record_fill(JSContext* ctx, JSValueConst, int,
                                              JSValueConst* argv)
{
  lrvx_backtest_result_record_fill(
      static_cast<LrvxBacktestResultHandle>(getHandle(ctx, argv[0])),
      static_cast<uint64_t>(toInt64(ctx, argv[1])), toUint32(ctx, argv[2]),
      static_cast<uint8_t>(toUint32(ctx, argv[3])), toDouble(ctx, argv[4]),
      toDouble(ctx, argv[5]), toInt64(ctx, argv[6]));
  return JS_UNDEFINED;
}
static JSValue js_backtest_result_ingest(JSContext* ctx, JSValueConst, int,
                                         JSValueConst* argv)
{
  lrvx_backtest_result_ingest_executor(
      static_cast<LrvxBacktestResultHandle>(getHandle(ctx, argv[0])),
      static_cast<LrvxSimulatedExecutorHandle>(getHandle(ctx, argv[1])));
  return JS_UNDEFINED;
}
static JSValue js_backtest_result_stats(JSContext* ctx, JSValueConst, int,
                                        JSValueConst* argv)
{
  LrvxBacktestStats s{};
  lrvx_backtest_result_stats(
      static_cast<LrvxBacktestResultHandle>(getHandle(ctx, argv[0])), &s);
  JSValue obj = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, obj, "totalTrades", JS_NewInt64(ctx, (int64_t)s.totalTrades));
  JS_SetPropertyStr(ctx, obj, "winningTrades", JS_NewInt64(ctx, (int64_t)s.winningTrades));
  JS_SetPropertyStr(ctx, obj, "losingTrades", JS_NewInt64(ctx, (int64_t)s.losingTrades));
  JS_SetPropertyStr(ctx, obj, "maxConsecutiveWins",
                    JS_NewInt64(ctx, (int64_t)s.maxConsecutiveWins));
  JS_SetPropertyStr(ctx, obj, "maxConsecutiveLosses",
                    JS_NewInt64(ctx, (int64_t)s.maxConsecutiveLosses));
  JS_SetPropertyStr(ctx, obj, "initialCapital", JS_NewFloat64(ctx, s.initialCapital));
  JS_SetPropertyStr(ctx, obj, "finalCapital", JS_NewFloat64(ctx, s.finalCapital));
  JS_SetPropertyStr(ctx, obj, "totalPnl", JS_NewFloat64(ctx, s.totalPnl));
  JS_SetPropertyStr(ctx, obj, "totalFees", JS_NewFloat64(ctx, s.totalFees));
  JS_SetPropertyStr(ctx, obj, "netPnl", JS_NewFloat64(ctx, s.netPnl));
  JS_SetPropertyStr(ctx, obj, "grossProfit", JS_NewFloat64(ctx, s.grossProfit));
  JS_SetPropertyStr(ctx, obj, "grossLoss", JS_NewFloat64(ctx, s.grossLoss));
  JS_SetPropertyStr(ctx, obj, "maxDrawdown", JS_NewFloat64(ctx, s.maxDrawdown));
  JS_SetPropertyStr(ctx, obj, "maxDrawdownPct", JS_NewFloat64(ctx, s.maxDrawdownPct));
  JS_SetPropertyStr(ctx, obj, "winRate", JS_NewFloat64(ctx, s.winRate));
  JS_SetPropertyStr(ctx, obj, "profitFactor", JS_NewFloat64(ctx, s.profitFactor));
  JS_SetPropertyStr(ctx, obj, "avgWin", JS_NewFloat64(ctx, s.avgWin));
  JS_SetPropertyStr(ctx, obj, "avgLoss", JS_NewFloat64(ctx, s.avgLoss));
  JS_SetPropertyStr(ctx, obj, "avgWinLossRatio", JS_NewFloat64(ctx, s.avgWinLossRatio));
  JS_SetPropertyStr(ctx, obj, "avgTradeDurationNs",
                    JS_NewFloat64(ctx, s.avgTradeDurationNs));
  JS_SetPropertyStr(ctx, obj, "medianTradeDurationNs",
                    JS_NewFloat64(ctx, s.medianTradeDurationNs));
  JS_SetPropertyStr(ctx, obj, "maxTradeDurationNs",
                    JS_NewFloat64(ctx, s.maxTradeDurationNs));
  JS_SetPropertyStr(ctx, obj, "sharpeRatio", JS_NewFloat64(ctx, s.sharpeRatio));
  JS_SetPropertyStr(ctx, obj, "sortinoRatio", JS_NewFloat64(ctx, s.sortinoRatio));
  JS_SetPropertyStr(ctx, obj, "calmarRatio", JS_NewFloat64(ctx, s.calmarRatio));
  JS_SetPropertyStr(ctx, obj, "timeWeightedReturn",
                    JS_NewFloat64(ctx, s.timeWeightedReturn));
  JS_SetPropertyStr(ctx, obj, "returnPct", JS_NewFloat64(ctx, s.returnPct));
  JS_SetPropertyStr(ctx, obj, "startTimeNs", JS_NewBigInt64(ctx, s.startTimeNs));
  JS_SetPropertyStr(ctx, obj, "endTimeNs", JS_NewBigInt64(ctx, s.endTimeNs));
  return obj;
}
static JSValue js_backtest_result_equity_curve(JSContext* ctx, JSValueConst, int,
                                               JSValueConst* argv)
{
  auto h = static_cast<LrvxBacktestResultHandle>(getHandle(ctx, argv[0]));
  const uint32_t n = lrvx_backtest_result_equity_curve(h, nullptr, 0);
  std::vector<LrvxEquityPoint> pts(n);
  if (n > 0)
  {
    lrvx_backtest_result_equity_curve(h, pts.data(), n);
  }
  JSValue arr = JS_NewArray(ctx);
  for (uint32_t i = 0; i < n; ++i)
  {
    JSValue pt = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, pt, "timestampNs", JS_NewBigInt64(ctx, pts[i].timestamp_ns));
    JS_SetPropertyStr(ctx, pt, "equity", JS_NewFloat64(ctx, pts[i].equity));
    JS_SetPropertyStr(ctx, pt, "drawdownPct", JS_NewFloat64(ctx, pts[i].drawdown_pct));
    JS_SetPropertyUint32(ctx, arr, i, pt);
  }
  return arr;
}
static JSValue js_backtest_result_write_csv(JSContext* ctx, JSValueConst, int,
                                            JSValueConst* argv)
{
  lrvx::JsCString path(ctx, argv[1]);
  if (!path)
  {
    return JS_NewBool(ctx, 0);
  }
  uint8_t ok = lrvx_backtest_result_write_equity_curve_csv(
      static_cast<LrvxBacktestResultHandle>(getHandle(ctx, argv[0])), path);
  return JS_NewBool(ctx, ok != 0);
}

// ============================================================
// Position tracker bindings
// ============================================================

static JSValue js_pos_create(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  uint8_t basis = (argc > 0) ? static_cast<uint8_t>(toUint32(ctx, argv[0])) : 0;
  return createHandleObject(ctx, lrvx_position_tracker_create(basis), lrvx_position_tracker_destroy);
}
static JSValue js_pos_destroy(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return js_generic_handle_destroy(ctx, argv[0]);
}
static JSValue js_pos_on_fill(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  lrvx_position_tracker_on_fill(
      static_cast<LrvxPositionTrackerHandle>(getHandle(ctx, argv[0])), toUint32(ctx, argv[1]),
      static_cast<uint8_t>(toUint32(ctx, argv[2])), toDouble(ctx, argv[3]),
      toDouble(ctx, argv[4]));
  return JS_UNDEFINED;
}
static JSValue js_pos_position(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewFloat64(ctx, lrvx_position_tracker_position(
                                static_cast<LrvxPositionTrackerHandle>(getHandle(ctx, argv[0])),
                                toUint32(ctx, argv[1])));
}
static JSValue js_pos_avg_entry(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewFloat64(ctx, lrvx_position_tracker_avg_entry(
                                static_cast<LrvxPositionTrackerHandle>(getHandle(ctx, argv[0])),
                                toUint32(ctx, argv[1])));
}
static JSValue js_pos_pnl(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewFloat64(
      ctx, lrvx_position_tracker_realized_pnl(
               static_cast<LrvxPositionTrackerHandle>(getHandle(ctx, argv[0])),
               toUint32(ctx, argv[1])));
}
static JSValue js_pos_total_pnl(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewFloat64(ctx, lrvx_position_tracker_total_pnl(
                                static_cast<LrvxPositionTrackerHandle>(getHandle(ctx, argv[0]))));
}

// ============================================================
// Delta book compression bindings
// ============================================================

static std::vector<LrvxBookLevel> readDeltaBookLevels(JSContext* ctx, JSValueConst arr)
{
  std::vector<LrvxBookLevel> out;
  if (!JS_IsArray(ctx, arr))
  {
    return out;
  }
  uint32_t len = 0;
  JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
  JS_ToUint32(ctx, &len, lenVal);
  JS_FreeValue(ctx, lenVal);
  out.reserve(len);
  for (uint32_t i = 0; i < len; ++i)
  {
    JSValue entry = JS_GetPropertyUint32(ctx, arr, i);
    LrvxBookLevel l{};
    JSValue p = JS_GetPropertyStr(ctx, entry, "priceRaw");
    JSValue q = JS_GetPropertyStr(ctx, entry, "qtyRaw");
    l.price_raw = toInt64(ctx, p);
    l.quantity_raw = toInt64(ctx, q);
    JS_FreeValue(ctx, p);
    JS_FreeValue(ctx, q);
    JS_FreeValue(ctx, entry);
    out.push_back(l);
  }
  return out;
}

static JSValue deltaBookLevelsToJs(JSContext* ctx, const std::vector<LrvxBookLevel>& levels)
{
  JSValue arr = JS_NewArray(ctx);
  for (size_t i = 0; i < levels.size(); ++i)
  {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "priceRaw", JS_NewInt64(ctx, levels[i].price_raw));
    JS_SetPropertyStr(ctx, o, "qtyRaw", JS_NewInt64(ctx, levels[i].quantity_raw));
    JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), o);
  }
  return arr;
}

static JSValue js_delta_book_encoder_create(JSContext* ctx, JSValueConst, int argc,
                                            JSValueConst* argv)
{
  uint32_t anchor_every = (argc > 0) ? toUint32(ctx, argv[0]) : 100;
  auto h = lrvx_delta_book_encoder_create(anchor_every);
  if (!h)
  {
    return JS_ThrowTypeError(ctx, "DeltaBookEncoder: construction failed");
  }
  return createHandleObject(ctx, h, lrvx_delta_book_encoder_destroy);
}

static JSValue js_delta_book_encoder_destroy(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return js_generic_handle_destroy(ctx, argv[0]);
}

static JSValue js_delta_book_encoder_encode(JSContext* ctx, JSValueConst, int argc,
                                            JSValueConst* argv)
{
  if (argc < 4)
  {
    return JS_ThrowTypeError(ctx, "encode(handle, symbol, bids, asks)");
  }
  auto h = static_cast<LrvxDeltaBookEncoderHandle>(getHandle(ctx, argv[0]));
  uint32_t sym = toUint32(ctx, argv[1]);
  auto bids = readDeltaBookLevels(ctx, argv[2]);
  auto asks = readDeltaBookLevels(ctx, argv[3]);
  uint8_t is_delta = 0;
  uint64_t bcnt = 0, acnt = 0;
  lrvx_delta_book_encoder_encode(h, sym,
                                 bids.empty() ? nullptr : bids.data(), bids.size(),
                                 asks.empty() ? nullptr : asks.data(), asks.size(),
                                 &is_delta, &bcnt, &acnt);
  std::vector<LrvxBookLevel> out_bids(bcnt), out_asks(acnt);
  if (bcnt)
  {
    lrvx_delta_book_encoder_copy_bids(h, out_bids.data(), bcnt);
  }
  if (acnt)
  {
    lrvx_delta_book_encoder_copy_asks(h, out_asks.data(), acnt);
  }
  JSValue out = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, out, "isDelta", JS_NewBool(ctx, is_delta != 0));
  JS_SetPropertyStr(ctx, out, "bids", deltaBookLevelsToJs(ctx, out_bids));
  JS_SetPropertyStr(ctx, out, "asks", deltaBookLevelsToJs(ctx, out_asks));
  return out;
}

static JSValue js_delta_book_replayer_create(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
  return createHandleObject(ctx, lrvx_delta_book_replayer_create(), lrvx_delta_book_replayer_destroy);
}

static JSValue js_delta_book_replayer_destroy(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return js_generic_handle_destroy(ctx, argv[0]);
}

static JSValue js_delta_book_replayer_apply(JSContext* ctx, JSValueConst, int argc,
                                            JSValueConst* argv)
{
  if (argc < 5)
  {
    return JS_ThrowTypeError(ctx, "apply(handle, type, symbol, bids, asks)");
  }
  auto h = static_cast<LrvxDeltaBookReplayerHandle>(getHandle(ctx, argv[0]));
  uint8_t type = static_cast<uint8_t>(toUint32(ctx, argv[1]));
  uint32_t sym = toUint32(ctx, argv[2]);
  auto bids = readDeltaBookLevels(ctx, argv[3]);
  auto asks = readDeltaBookLevels(ctx, argv[4]);
  uint64_t bcnt = 0, acnt = 0;
  lrvx_delta_book_replayer_apply(h, type, sym,
                                 bids.empty() ? nullptr : bids.data(), bids.size(),
                                 asks.empty() ? nullptr : asks.data(), asks.size(),
                                 &bcnt, &acnt);
  std::vector<LrvxBookLevel> out_bids(bcnt), out_asks(acnt);
  if (bcnt)
  {
    lrvx_delta_book_replayer_copy_bids(h, out_bids.data(), bcnt);
  }
  if (acnt)
  {
    lrvx_delta_book_replayer_copy_asks(h, out_asks.data(), acnt);
  }
  JSValue out = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, out, "bids", deltaBookLevelsToJs(ctx, out_bids));
  JS_SetPropertyStr(ctx, out, "asks", deltaBookLevelsToJs(ctx, out_asks));
  return out;
}

// ============================================================
// .lrvxrun trace recorder / reader bindings
// ============================================================

static std::vector<uint32_t> readU32Array(JSContext* ctx, JSValueConst arr)
{
  std::vector<uint32_t> out;
  if (!JS_IsArray(ctx, arr))
  {
    return out;
  }
  uint32_t len = 0;
  JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
  JS_ToUint32(ctx, &len, lenVal);
  JS_FreeValue(ctx, lenVal);
  out.reserve(len);
  for (uint32_t i = 0; i < len; ++i)
  {
    JSValue v = JS_GetPropertyUint32(ctx, arr, i);
    uint32_t n = 0;
    JS_ToUint32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    out.push_back(n);
  }
  return out;
}

static JSValue u32VecToJs(JSContext* ctx, const std::vector<uint32_t>& v)
{
  JSValue out = JS_NewArray(ctx);
  for (size_t i = 0; i < v.size(); ++i)
  {
    JS_SetPropertyUint32(ctx, out, static_cast<uint32_t>(i),
                         JS_NewUint32(ctx, v[i]));
  }
  return out;
}

static JSValue js_run_recorder_create(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  if (argc < 1)
  {
    return JS_ThrowTypeError(ctx, "TraceRecorder: need options object");
  }
  auto getStr = [&](const char* key, const char* dflt = "") -> std::string
  {
    JSValue v = JS_GetPropertyStr(ctx, argv[0], key);
    if (JS_IsUndefined(v) || JS_IsNull(v))
    {
      JS_FreeValue(ctx, v);
      return dflt;
    }
    lrvx::JsCString s(ctx, v);
    std::string out = s ? s : "";
    JS_FreeValue(ctx, v);
    return out;
  };
  auto getI64 = [&](const char* key, int64_t dflt = 0) -> int64_t
  {
    JSValue v = JS_GetPropertyStr(ctx, argv[0], key);
    int64_t out = dflt;
    if (!JS_IsUndefined(v) && !JS_IsNull(v))
    {
      JS_ToInt64Ext(ctx, &out, v);
    }
    JS_FreeValue(ctx, v);
    return out;
  };
  std::string path = getStr("path");
  std::string sid = getStr("strategyId");
  std::string shash = getStr("strategyHash");
  int64_t started = getI64("runStartedNs");
  auto h = lrvx_run_recorder_create(path.c_str(), sid.c_str(), shash.c_str(), started);
  if (!h)
  {
    return JS_ThrowTypeError(ctx, "TraceRecorder: %s", lrvx_last_error_message());
  }
  return createHandleObject(ctx, h, lrvx_run_recorder_destroy);
}

static JSValue js_run_recorder_destroy(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return js_generic_handle_destroy(ctx, argv[0]);
}

static JSValue js_run_recorder_add_tape_ref(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  if (argc < 2)
  {
    return JS_ThrowTypeError(ctx, "addTapeRef(handle, opts)");
  }
  auto h = static_cast<LrvxRunRecorderHandle>(getHandle(ctx, argv[0]));
  auto getStr = [&](const char* key) -> std::string
  {
    JSValue v = JS_GetPropertyStr(ctx, argv[1], key);
    if (JS_IsUndefined(v) || JS_IsNull(v))
    {
      JS_FreeValue(ctx, v);
      return "";
    }
    lrvx::JsCString s(ctx, v);
    std::string out = s ? s : "";
    JS_FreeValue(ctx, v);
    return out;
  };
  auto getI64 = [&](const char* key) -> int64_t
  {
    JSValue v = JS_GetPropertyStr(ctx, argv[1], key);
    int64_t out = 0;
    if (!JS_IsUndefined(v) && !JS_IsNull(v))
    {
      JS_ToInt64Ext(ctx, &out, v);
    }
    JS_FreeValue(ctx, v);
    return out;
  };
  std::string path = getStr("path");
  std::string ch = getStr("contentHash");
  lrvx_run_recorder_add_tape_ref(h, path.c_str(), ch.c_str(), getI64("firstEventNs"),
                                 getI64("lastEventNs"));
  return JS_UNDEFINED;
}

static JSValue js_run_recorder_set_run_ended_ns(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<LrvxRunRecorderHandle>(getHandle(ctx, argv[0]));
  lrvx_run_recorder_set_run_ended_ns(h, toInt64(ctx, argv[1]));
  return JS_UNDEFINED;
}

static JSValue js_run_recorder_write_signal(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  if (argc < 2)
  {
    return JS_ThrowTypeError(ctx, "writeSignal(handle, opts)");
  }
  auto h = static_cast<LrvxRunRecorderHandle>(getHandle(ctx, argv[0]));
  auto opts = argv[1];
  auto getI64 = [&](const char* k, int64_t d = 0) -> int64_t
  {
    JSValue v = JS_GetPropertyStr(ctx, opts, k);
    int64_t out = d;
    if (!JS_IsUndefined(v) && !JS_IsNull(v))
    {
      JS_ToInt64Ext(ctx, &out, v);
    }
    JS_FreeValue(ctx, v);
    return out;
  };
  auto getU32 = [&](const char* k, uint32_t d = 0) -> uint32_t
  {
    JSValue v = JS_GetPropertyStr(ctx, opts, k);
    uint32_t out = d;
    if (!JS_IsUndefined(v) && !JS_IsNull(v))
    {
      JS_ToUint32(ctx, &out, v);
    }
    JS_FreeValue(ctx, v);
    return out;
  };
  int64_t run_ts = getI64("runTsNs");
  int64_t feed_ts = getI64("feedTsNs");
  // A Number or a BigInt; the whole 64-bit id is written.
  uint64_t signal_id = static_cast<uint64_t>(getI64("signalId"));
  uint32_t flags = getU32("flags");
  int64_t strength = getI64("strengthRaw");
  std::string name;
  {
    JSValue v = JS_GetPropertyStr(ctx, opts, "name");
    if (!JS_IsUndefined(v) && !JS_IsNull(v))
    {
      lrvx::JsCString s(ctx, v);
      if (s)
      {
        name = s;
      }
    }
    JS_FreeValue(ctx, v);
  }
  std::vector<uint32_t> sids;
  {
    JSValue v = JS_GetPropertyStr(ctx, opts, "symbolIds");
    if (JS_IsArray(ctx, v))
    {
      sids = readU32Array(ctx, v);
    }
    JS_FreeValue(ctx, v);
  }
  std::vector<uint8_t> payload;
  {
    JSValue v = JS_GetPropertyStr(ctx, opts, "payload");
    if (JS_IsString(v))
    {
      lrvx::JsCString s(ctx, v);
      if (s)
      {
        payload.assign(s.get(), s.get() + std::strlen(s.get()));
      }
    }
    JS_FreeValue(ctx, v);
  }
  lrvx_run_recorder_write_signal64(h, run_ts, feed_ts, signal_id, flags, strength,
                                   name.empty() ? nullptr : name.data(), name.size(),
                                   sids.empty() ? nullptr : sids.data(), sids.size(),
                                   payload.empty() ? nullptr : payload.data(), payload.size());
  return JS_UNDEFINED;
}

static JSValue js_run_recorder_write_order_event(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  if (argc < 2)
  {
    return JS_ThrowTypeError(ctx, "writeOrderEvent(handle, opts)");
  }
  auto h = static_cast<LrvxRunRecorderHandle>(getHandle(ctx, argv[0]));
  auto opts = argv[1];
  auto getI64 = [&](const char* k, int64_t d = 0) -> int64_t
  {
    JSValue v = JS_GetPropertyStr(ctx, opts, k);
    int64_t out = d;
    if (!JS_IsUndefined(v) && !JS_IsNull(v))
    {
      JS_ToInt64Ext(ctx, &out, v);
    }
    JS_FreeValue(ctx, v);
    return out;
  };
  auto getU32 = [&](const char* k, uint32_t d = 0) -> uint32_t
  {
    JSValue v = JS_GetPropertyStr(ctx, opts, k);
    uint32_t out = d;
    if (!JS_IsUndefined(v) && !JS_IsNull(v))
    {
      JS_ToUint32(ctx, &out, v);
    }
    JS_FreeValue(ctx, v);
    return out;
  };
  std::string reason;
  {
    JSValue v = JS_GetPropertyStr(ctx, opts, "reason");
    if (!JS_IsUndefined(v) && !JS_IsNull(v))
    {
      lrvx::JsCString s(ctx, v);
      if (s)
      {
        reason = s;
      }
    }
    JS_FreeValue(ctx, v);
  }
  lrvx_run_recorder_write_order_event(h, getI64("runTsNs"), getI64("feedTsNs"),
                                      static_cast<uint64_t>(getI64("orderId")),
                                      static_cast<uint64_t>(getI64("parentSignalId")),
                                      getI64("priceRaw"), getI64("qtyRaw"),
                                      getU32("symbolId"),
                                      static_cast<uint8_t>(getU32("eventKind", 1)),
                                      static_cast<uint8_t>(getU32("side")),
                                      static_cast<uint8_t>(getU32("orderType")),
                                      getU32("flags"),
                                      reason.empty() ? nullptr : reason.data(), reason.size());
  return JS_UNDEFINED;
}

static JSValue js_run_recorder_write_fill(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  if (argc < 2)
  {
    return JS_ThrowTypeError(ctx, "writeFill(handle, opts)");
  }
  auto h = static_cast<LrvxRunRecorderHandle>(getHandle(ctx, argv[0]));
  auto opts = argv[1];
  auto getI64 = [&](const char* k, int64_t d = 0) -> int64_t
  {
    JSValue v = JS_GetPropertyStr(ctx, opts, k);
    int64_t out = d;
    if (!JS_IsUndefined(v) && !JS_IsNull(v))
    {
      JS_ToInt64Ext(ctx, &out, v);
    }
    JS_FreeValue(ctx, v);
    return out;
  };
  auto getU32 = [&](const char* k, uint32_t d = 0) -> uint32_t
  {
    JSValue v = JS_GetPropertyStr(ctx, opts, k);
    uint32_t out = d;
    if (!JS_IsUndefined(v) && !JS_IsNull(v))
    {
      JS_ToUint32(ctx, &out, v);
    }
    JS_FreeValue(ctx, v);
    return out;
  };
  lrvx_run_recorder_write_fill(h, getI64("runTsNs"), getI64("feedTsNs"),
                               static_cast<uint64_t>(getI64("orderId")),
                               static_cast<uint64_t>(getI64("fillId")),
                               getI64("priceRaw"), getI64("qtyRaw"), getI64("feeRaw"),
                               getU32("symbolId"),
                               static_cast<uint8_t>(getU32("side")),
                               static_cast<uint8_t>(getU32("liquidity")));
  return JS_UNDEFINED;
}

static JSValue js_run_recorder_close(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  lrvx_run_recorder_close(static_cast<LrvxRunRecorderHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}

static JSValue js_run_reader_open(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  if (argc < 1)
  {
    return JS_ThrowTypeError(ctx, "TraceReader(path)");
  }
  lrvx::JsCString path(ctx, argv[0]);
  auto h = lrvx_run_reader_open(path ? path : "");
  if (!h)
  {
    return JS_ThrowTypeError(ctx, "TraceReader: %s", lrvx_last_error_message());
  }
  return createHandleObject(ctx, h, lrvx_run_reader_close);
}

static JSValue js_run_reader_close(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return js_generic_handle_destroy(ctx, argv[0]);
}

static JSValue js_run_reader_strategy_id(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<LrvxRunReaderHandle>(getHandle(ctx, argv[0]));
  uint64_t n = lrvx_run_reader_strategy_id(h, nullptr, 0);
  std::string out(n, '\0');
  if (n)
  {
    lrvx_run_reader_strategy_id(h, out.data(), n);
  }
  return JS_NewStringLen(ctx, out.data(), out.size());
}

static JSValue js_run_reader_run_started_ns(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<LrvxRunReaderHandle>(getHandle(ctx, argv[0]));
  return JS_NewBigInt64(ctx, lrvx_run_reader_run_started_ns(h));
}

static JSValue js_run_reader_run_ended_ns(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<LrvxRunReaderHandle>(getHandle(ctx, argv[0]));
  return JS_NewBigInt64(ctx, lrvx_run_reader_run_ended_ns(h));
}

static JSValue js_run_reader_signals(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<LrvxRunReaderHandle>(getHandle(ctx, argv[0]));
  uint64_t n = lrvx_run_reader_signal_count(h);
  JSValue out = JS_NewArray(ctx);
  for (uint64_t i = 0; i < n; ++i)
  {
    int64_t run_ts = 0, feed_ts = 0, strength = 0;
    uint32_t sid = 0, flags = 0;
    uint64_t name_len = 0, sym_count = 0, payload_len = 0;
    lrvx_run_reader_signal_header(h, i, &run_ts, &feed_ts, &sid, &flags, &strength, &name_len,
                                  &sym_count, &payload_len);
    std::string name(name_len, '\0');
    if (name_len)
    {
      lrvx_run_reader_signal_name(h, i, name.data(), name_len);
    }
    std::vector<uint32_t> sids(sym_count);
    if (sym_count)
    {
      lrvx_run_reader_signal_symbol_ids(h, i, sids.data(), sym_count);
    }
    std::vector<uint8_t> payload(payload_len);
    if (payload_len)
    {
      lrvx_run_reader_signal_payload(h, i, payload.data(), payload_len);
    }
    JSValue rec = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, rec, "runTsNs", JS_NewBigInt64(ctx, run_ts));
    JS_SetPropertyStr(ctx, rec, "feedTsNs", JS_NewBigInt64(ctx, feed_ts));
    JS_SetPropertyStr(ctx, rec, "signalId", JS_NewUint32(ctx, sid));
    // A Number, as orderId and parentSignalId are on an order event record.
    JS_SetPropertyStr(
        ctx, rec, "signalId64",
        JS_NewInt64(ctx, static_cast<int64_t>(lrvx_run_reader_signal_id64(h, i))));
    JS_SetPropertyStr(ctx, rec, "flags", JS_NewUint32(ctx, flags));
    JS_SetPropertyStr(ctx, rec, "strengthRaw", JS_NewInt64(ctx, strength));
    JS_SetPropertyStr(ctx, rec, "name", JS_NewStringLen(ctx, name.data(), name.size()));
    JS_SetPropertyStr(ctx, rec, "symbolIds", u32VecToJs(ctx, sids));
    JS_SetPropertyStr(ctx, rec, "payload",
                      JS_NewStringLen(ctx, reinterpret_cast<const char*>(payload.data()),
                                      payload.size()));
    JS_SetPropertyUint32(ctx, out, static_cast<uint32_t>(i), rec);
  }
  return out;
}

static JSValue js_run_reader_orders(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<LrvxRunReaderHandle>(getHandle(ctx, argv[0]));
  uint64_t n = lrvx_run_reader_order_event_count(h);
  JSValue out = JS_NewArray(ctx);
  for (uint64_t i = 0; i < n; ++i)
  {
    int64_t run_ts = 0, feed_ts = 0, price = 0, qty = 0;
    uint64_t oid = 0, pid = 0, reason_len = 0;
    uint32_t sid = 0, flags = 0;
    uint8_t kind = 0, side = 0, otype = 0;
    lrvx_run_reader_order_event_header(h, i, &run_ts, &feed_ts, &oid, &pid, &price, &qty, &sid,
                                       &kind, &side, &otype, &flags, &reason_len);
    std::string reason(reason_len, '\0');
    if (reason_len)
    {
      lrvx_run_reader_order_event_reason(h, i, reason.data(), reason_len);
    }
    JSValue rec = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, rec, "runTsNs", JS_NewBigInt64(ctx, run_ts));
    JS_SetPropertyStr(ctx, rec, "feedTsNs", JS_NewBigInt64(ctx, feed_ts));
    JS_SetPropertyStr(ctx, rec, "orderId", JS_NewInt64(ctx, static_cast<int64_t>(oid)));
    JS_SetPropertyStr(ctx, rec, "parentSignalId", JS_NewInt64(ctx, static_cast<int64_t>(pid)));
    JS_SetPropertyStr(ctx, rec, "priceRaw", JS_NewInt64(ctx, price));
    JS_SetPropertyStr(ctx, rec, "qtyRaw", JS_NewInt64(ctx, qty));
    JS_SetPropertyStr(ctx, rec, "symbolId", JS_NewUint32(ctx, sid));
    JS_SetPropertyStr(ctx, rec, "eventKind", JS_NewUint32(ctx, kind));
    JS_SetPropertyStr(ctx, rec, "side", JS_NewUint32(ctx, side));
    JS_SetPropertyStr(ctx, rec, "orderType", JS_NewUint32(ctx, otype));
    JS_SetPropertyStr(ctx, rec, "flags", JS_NewUint32(ctx, flags));
    JS_SetPropertyStr(ctx, rec, "reason", JS_NewStringLen(ctx, reason.data(), reason.size()));
    JS_SetPropertyUint32(ctx, out, static_cast<uint32_t>(i), rec);
  }
  return out;
}

static JSValue js_run_reader_fills(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<LrvxRunReaderHandle>(getHandle(ctx, argv[0]));
  uint64_t n = lrvx_run_reader_fill_count(h);
  JSValue out = JS_NewArray(ctx);
  for (uint64_t i = 0; i < n; ++i)
  {
    int64_t run_ts = 0, feed_ts = 0, price = 0, qty = 0, fee = 0;
    uint64_t oid = 0, fid = 0;
    uint32_t sid = 0;
    uint8_t side = 0, liq = 0;
    lrvx_run_reader_fill(h, i, &run_ts, &feed_ts, &oid, &fid, &price, &qty, &fee, &sid, &side, &liq);
    JSValue rec = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, rec, "runTsNs", JS_NewBigInt64(ctx, run_ts));
    JS_SetPropertyStr(ctx, rec, "feedTsNs", JS_NewBigInt64(ctx, feed_ts));
    JS_SetPropertyStr(ctx, rec, "orderId", JS_NewInt64(ctx, static_cast<int64_t>(oid)));
    JS_SetPropertyStr(ctx, rec, "fillId", JS_NewInt64(ctx, static_cast<int64_t>(fid)));
    JS_SetPropertyStr(ctx, rec, "priceRaw", JS_NewInt64(ctx, price));
    JS_SetPropertyStr(ctx, rec, "qtyRaw", JS_NewInt64(ctx, qty));
    JS_SetPropertyStr(ctx, rec, "feeRaw", JS_NewInt64(ctx, fee));
    JS_SetPropertyStr(ctx, rec, "symbolId", JS_NewUint32(ctx, sid));
    JS_SetPropertyStr(ctx, rec, "side", JS_NewUint32(ctx, side));
    JS_SetPropertyStr(ctx, rec, "liquidity", JS_NewUint32(ctx, liq));
    JS_SetPropertyUint32(ctx, out, static_cast<uint32_t>(i), rec);
  }
  return out;
}

// ============================================================
// Order group (multi-leg state machine + risk gate + pair-latency)
// ============================================================
//
// Thin shim over the C ABI. The high-level OrderGroup class lives in
// quickjs/lrvx/order_group.js and wraps these globals so the four
// bindings (pybind11 / NAPI / QuickJS / Codon) all reach the same
// C++ engine.

static JSValue js_order_group_create(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  int64_t parent = 0;
  uint32_t policy = 0;
  if (argv)
  {
    JS_ToInt64Ext(ctx, &parent, argv[0]);
    JS_ToUint32(ctx, &policy, argv[1]);
  }
  auto h = lrvx_order_group_create(static_cast<uint64_t>(parent), static_cast<uint8_t>(policy));
  return createHandleObject(ctx, h, lrvx_order_group_destroy);
}

static JSValue js_order_group_destroy(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return js_generic_handle_destroy(ctx, argv[0]);
}

static JSValue js_order_group_add_market_leg(JSContext* ctx, JSValueConst, int,
                                             JSValueConst* argv)
{
  auto h = static_cast<LrvxOrderGroupHandle>(getHandle(ctx, argv[0]));
  uint32_t symbol = toUint32(ctx, argv[1]);
  uint32_t side = toUint32(ctx, argv[2]);
  double qty = toDouble(ctx, argv[3]);
  int64_t qty_raw = static_cast<int64_t>(qty * 100'000'000LL);
  return JS_NewUint32(ctx,
                      lrvx_order_group_add_market_leg(h, symbol, static_cast<uint8_t>(side),
                                                      qty_raw));
}

static JSValue js_order_group_add_limit_leg(JSContext* ctx, JSValueConst, int,
                                            JSValueConst* argv)
{
  auto h = static_cast<LrvxOrderGroupHandle>(getHandle(ctx, argv[0]));
  uint32_t symbol = toUint32(ctx, argv[1]);
  uint32_t side = toUint32(ctx, argv[2]);
  double price = toDouble(ctx, argv[3]);
  double qty = toDouble(ctx, argv[4]);
  int64_t price_raw = static_cast<int64_t>(price * 100'000'000LL);
  int64_t qty_raw = static_cast<int64_t>(qty * 100'000'000LL);
  return JS_NewUint32(
      ctx, lrvx_order_group_add_limit_leg(h, symbol, static_cast<uint8_t>(side), price_raw,
                                          qty_raw));
}

static JSValue js_order_group_leg_count(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<LrvxOrderGroupHandle>(getHandle(ctx, argv[0]));
  return JS_NewUint32(ctx, lrvx_order_group_leg_count(h));
}

static JSValue js_order_group_leg_state(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<LrvxOrderGroupHandle>(getHandle(ctx, argv[0]));
  uint32_t i = toUint32(ctx, argv[1]);
  return JS_NewUint32(ctx, lrvx_order_group_leg_state(h, i));
}

static JSValue js_order_group_leg_filled(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<LrvxOrderGroupHandle>(getHandle(ctx, argv[0]));
  uint32_t i = toUint32(ctx, argv[1]);
  int64_t raw = lrvx_order_group_leg_filled_raw(h, i);
  return JS_NewFloat64(ctx, static_cast<double>(raw) / 1e8);
}

static JSValue js_order_group_leg_order_id(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<LrvxOrderGroupHandle>(getHandle(ctx, argv[0]));
  uint32_t i = toUint32(ctx, argv[1]);
  return JS_NewInt64(ctx, static_cast<int64_t>(lrvx_order_group_leg_order_id(h, i)));
}

static JSValue js_order_group_record_submit(JSContext* ctx, JSValueConst, int,
                                            JSValueConst* argv)
{
  auto h = static_cast<LrvxOrderGroupHandle>(getHandle(ctx, argv[0]));
  uint32_t i = toUint32(ctx, argv[1]);
  int64_t order_id = 0;
  JS_ToInt64Ext(ctx, &order_id, argv[2]);
  lrvx_order_group_record_submit(h, i, static_cast<uint64_t>(order_id));
  return JS_UNDEFINED;
}

static JSValue js_order_group_record_fill(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<LrvxOrderGroupHandle>(getHandle(ctx, argv[0]));
  uint32_t i = toUint32(ctx, argv[1]);
  double qty = toDouble(ctx, argv[2]);
  lrvx_order_group_record_fill(h, i, static_cast<int64_t>(qty * 100'000'000LL));
  return JS_UNDEFINED;
}

static JSValue js_order_group_record_cancel(JSContext* ctx, JSValueConst, int,
                                            JSValueConst* argv)
{
  auto h = static_cast<LrvxOrderGroupHandle>(getHandle(ctx, argv[0]));
  uint32_t i = toUint32(ctx, argv[1]);
  lrvx_order_group_record_cancel(h, i);
  return JS_UNDEFINED;
}

static JSValue js_order_group_record_failure(JSContext* ctx, JSValueConst, int,
                                             JSValueConst* argv)
{
  auto h = static_cast<LrvxOrderGroupHandle>(getHandle(ctx, argv[0]));
  uint32_t i = toUint32(ctx, argv[1]);
  lrvx_order_group_record_failure(h, i);
  return JS_UNDEFINED;
}

static JSValue js_order_group_record_replace_accepted(JSContext* ctx, JSValueConst, int,
                                                      JSValueConst* argv)
{
  auto h = static_cast<LrvxOrderGroupHandle>(getHandle(ctx, argv[0]));
  uint32_t i = toUint32(ctx, argv[1]);
  uint64_t new_id = static_cast<uint64_t>(toInt64(ctx, argv[2]));
  lrvx_order_group_record_replace_accepted(h, i, new_id);
  return JS_UNDEFINED;
}

static JSValue js_order_group_record_replace_rejected(JSContext* ctx, JSValueConst, int,
                                                      JSValueConst* argv)
{
  auto h = static_cast<LrvxOrderGroupHandle>(getHandle(ctx, argv[0]));
  uint32_t i = toUint32(ctx, argv[1]);
  lrvx_order_group_record_replace_rejected(h, i);
  return JS_UNDEFINED;
}

static JSValue js_order_group_find_leg_by_order_id(JSContext* ctx, JSValueConst, int,
                                                   JSValueConst* argv)
{
  auto h = static_cast<LrvxOrderGroupHandle>(getHandle(ctx, argv[0]));
  uint64_t order_id = static_cast<uint64_t>(toInt64(ctx, argv[1]));
  uint32_t idx = lrvx_order_group_find_leg_by_order_id(h, order_id);
  if (idx == UINT32_MAX)
  {
    return JS_NULL;
  }
  return JS_NewUint32(ctx, idx);
}

static JSValue js_order_group_state(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<LrvxOrderGroupHandle>(getHandle(ctx, argv[0]));
  return JS_NewUint32(ctx, lrvx_order_group_state(h));
}

static JSValue js_order_group_recommended_actions(JSContext* ctx, JSValueConst, int,
                                                  JSValueConst* argv)
{
  auto h = static_cast<LrvxOrderGroupHandle>(getHandle(ctx, argv[0]));
  constexpr uint32_t kMax = 32;
  int64_t buf[kMax * 5];
  uint32_t n = lrvx_order_group_recommended_actions(h, buf, kMax);
  JSValue out = JS_NewArray(ctx);
  for (uint32_t i = 0; i < n; ++i)
  {
    int64_t* row = buf + i * 5;
    JSValue rec = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, rec, "kind",
                      JS_NewString(ctx, row[0] == 0 ? "cancel" : "revert"));
    JS_SetPropertyStr(ctx, rec, "legIndex", JS_NewInt32(ctx, static_cast<int32_t>(row[1])));
    if (row[0] == 0)
    {
      JS_SetPropertyStr(ctx, rec, "orderId", JS_NewInt64(ctx, row[2]));
    }
    else
    {
      JS_SetPropertyStr(ctx, rec, "symbol", JS_NewInt32(ctx, static_cast<int32_t>(row[2])));
      JS_SetPropertyStr(ctx, rec, "side", JS_NewInt32(ctx, static_cast<int32_t>(row[3])));
      JS_SetPropertyStr(ctx, rec, "qty", JS_NewFloat64(ctx, static_cast<double>(row[4]) / 1e8));
    }
    JS_SetPropertyUint32(ctx, out, i, rec);
  }
  return out;
}

static JSValue js_order_group_mark_action_dispatched(JSContext* ctx, JSValueConst, int,
                                                     JSValueConst* argv)
{
  auto h = static_cast<LrvxOrderGroupHandle>(getHandle(ctx, argv[0]));
  uint32_t leg = toUint32(ctx, argv[1]);
  uint32_t kind = toUint32(ctx, argv[2]);
  lrvx_order_group_mark_action_dispatched(h, leg, static_cast<uint8_t>(kind));
  return JS_UNDEFINED;
}

static JSValue js_order_group_set_risk_limits(JSContext* ctx, JSValueConst, int,
                                              JSValueConst* argv)
{
  auto h = static_cast<LrvxOrderGroupHandle>(getHandle(ctx, argv[0]));
  double max_gross = toDouble(ctx, argv[1]);
  double max_conc = toDouble(ctx, argv[2]);
  double max_leg = toDouble(ctx, argv[3]);
  int64_t gross_raw = static_cast<int64_t>(max_gross * 100'000'000LL);
  int64_t leg_raw = static_cast<int64_t>(max_leg * 100'000'000LL);
  lrvx_order_group_set_risk_limits(h, gross_raw, max_conc, leg_raw);
  return JS_UNDEFINED;
}

static JSValue js_order_group_precheck_submission(JSContext* ctx, JSValueConst, int,
                                                  JSValueConst* argv)
{
  auto h = static_cast<LrvxOrderGroupHandle>(getHandle(ctx, argv[0]));
  double equity = toDouble(ctx, argv[1]);

  std::vector<int64_t> prices_raw;
  uint32_t plen = 0;
  if (JS_IsArray(ctx, argv[2]))
  {
    JSValue lenVal = JS_GetPropertyStr(ctx, argv[2], "length");
    JS_ToUint32(ctx, &plen, lenVal);
    JS_FreeValue(ctx, lenVal);
    prices_raw.reserve(plen);
    for (uint32_t i = 0; i < plen; ++i)
    {
      JSValue v = JS_GetPropertyUint32(ctx, argv[2], i);
      double p = 0;
      JS_ToFloat64(ctx, &p, v);
      JS_FreeValue(ctx, v);
      prices_raw.push_back(static_cast<int64_t>(p * 100'000'000LL));
    }
  }

  char rule[64] = {};
  char detail[256] = {};
  uint8_t denied = lrvx_order_group_precheck_submission(
      h, equity, prices_raw.empty() ? nullptr : prices_raw.data(), plen, rule, sizeof(rule),
      detail, sizeof(detail));

  JSValue out = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, out, "denied", JS_NewBool(ctx, denied != 0));
  JS_SetPropertyStr(ctx, out, "rule", JS_NewString(ctx, rule));
  JS_SetPropertyStr(ctx, out, "detail", JS_NewString(ctx, detail));
  return out;
}

static JSValue js_order_group_set_pair_latency_budget_ns(JSContext* ctx, JSValueConst, int,
                                                         JSValueConst* argv)
{
  auto h = static_cast<LrvxOrderGroupHandle>(getHandle(ctx, argv[0]));
  int64_t budget = 0;
  JS_ToInt64Ext(ctx, &budget, argv[1]);
  lrvx_order_group_set_pair_latency_budget_ns(h, budget);
  return JS_UNDEFINED;
}

static JSValue js_order_group_pair_latency_decision(JSContext* ctx, JSValueConst, int,
                                                    JSValueConst* argv)
{
  auto h = static_cast<LrvxOrderGroupHandle>(getHandle(ctx, argv[0]));
  int64_t submit_ts = 0, ack_ts = 0;
  JS_ToInt64Ext(ctx, &submit_ts, argv[1]);
  JS_ToInt64Ext(ctx, &ack_ts, argv[2]);
  uint32_t ack_received = toUint32(ctx, argv[3]);
  uint8_t d = lrvx_order_group_pair_latency_decision(h, submit_ts, ack_ts,
                                                     static_cast<uint8_t>(ack_received));
  const char* name = d == 0 ? "wait" : (d == 1 ? "submit_follower" : "cancel_leader");
  return JS_NewString(ctx, name);
}

// ============================================================
// Live queue position estimator
// ============================================================

static JSValue js_live_queue_position_create(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
  return createHandleObject(ctx, lrvx_live_queue_position_create(), lrvx_live_queue_position_destroy);
}

static JSValue js_live_queue_position_destroy(JSContext* ctx, JSValueConst, int,
                                              JSValueConst* argv)
{
  return js_generic_handle_destroy(ctx, argv[0]);
}

static JSValue js_live_queue_position_set_half_life(JSContext* ctx, JSValueConst, int,
                                                    JSValueConst* argv)
{
  auto h = static_cast<LrvxLiveQueuePositionHandle>(getHandle(ctx, argv[0]));
  lrvx_live_queue_position_set_confidence_half_life_ns(h, toInt64(ctx, argv[1]));
  return JS_UNDEFINED;
}

static JSValue js_live_queue_position_set_shrink_factor(JSContext* ctx, JSValueConst, int,
                                                        JSValueConst* argv)
{
  auto h = static_cast<LrvxLiveQueuePositionHandle>(getHandle(ctx, argv[0]));
  double factor = 0.0;
  JS_ToFloat64(ctx, &factor, argv[1]);
  lrvx_live_queue_position_set_shrink_factor(h, factor);
  return JS_UNDEFINED;
}

static JSValue js_live_queue_position_on_order_placed(JSContext* ctx, JSValueConst, int,
                                                      JSValueConst* argv)
{
  auto h = static_cast<LrvxLiveQueuePositionHandle>(getHandle(ctx, argv[0]));
  uint32_t symbol = toUint32(ctx, argv[1]);
  uint8_t side = static_cast<uint8_t>(toUint32(ctx, argv[2]));
  double price = 0.0, order_qty = 0.0, level_qty = 0.0;
  JS_ToFloat64(ctx, &price, argv[3]);
  uint64_t order_id = static_cast<uint64_t>(toInt64(ctx, argv[4]));
  JS_ToFloat64(ctx, &order_qty, argv[5]);
  JS_ToFloat64(ctx, &level_qty, argv[6]);
  int64_t ts_ns = toInt64(ctx, argv[7]);
  lrvx_live_queue_position_on_order_placed(
      h, symbol, side, lrvx_price_from_double(price), order_id,
      lrvx_quantity_from_double(order_qty), lrvx_quantity_from_double(level_qty),
      ts_ns);
  return JS_UNDEFINED;
}

static JSValue js_live_queue_position_on_order_cancelled(JSContext* ctx, JSValueConst,
                                                         int, JSValueConst* argv)
{
  auto h = static_cast<LrvxLiveQueuePositionHandle>(getHandle(ctx, argv[0]));
  uint64_t order_id = static_cast<uint64_t>(toInt64(ctx, argv[1]));
  int64_t ts_ns = toInt64(ctx, argv[2]);
  lrvx_live_queue_position_on_order_cancelled(h, order_id, ts_ns);
  return JS_UNDEFINED;
}

static JSValue js_live_queue_position_on_order_filled(JSContext* ctx, JSValueConst, int,
                                                      JSValueConst* argv)
{
  auto h = static_cast<LrvxLiveQueuePositionHandle>(getHandle(ctx, argv[0]));
  uint64_t order_id = static_cast<uint64_t>(toInt64(ctx, argv[1]));
  double cum = 0.0;
  JS_ToFloat64(ctx, &cum, argv[2]);
  int64_t ts_ns = toInt64(ctx, argv[3]);
  lrvx_live_queue_position_on_order_filled(h, order_id,
                                           lrvx_quantity_from_double(cum), ts_ns);
  return JS_UNDEFINED;
}

static JSValue js_live_queue_position_on_trade(JSContext* ctx, JSValueConst, int,
                                               JSValueConst* argv)
{
  auto h = static_cast<LrvxLiveQueuePositionHandle>(getHandle(ctx, argv[0]));
  uint32_t symbol = toUint32(ctx, argv[1]);
  double price = 0.0, qty = 0.0;
  JS_ToFloat64(ctx, &price, argv[2]);
  JS_ToFloat64(ctx, &qty, argv[3]);
  int64_t ts_ns = toInt64(ctx, argv[4]);
  lrvx_live_queue_position_on_trade(h, symbol, lrvx_price_from_double(price),
                                    lrvx_quantity_from_double(qty), ts_ns);
  return JS_UNDEFINED;
}

static JSValue js_live_queue_position_on_trade_with_flag(JSContext* ctx, JSValueConst,
                                                         int, JSValueConst* argv)
{
  auto h = static_cast<LrvxLiveQueuePositionHandle>(getHandle(ctx, argv[0]));
  uint32_t symbol = toUint32(ctx, argv[1]);
  double price = 0.0, qty = 0.0;
  JS_ToFloat64(ctx, &price, argv[2]);
  JS_ToFloat64(ctx, &qty, argv[3]);
  int64_t ts_ns = toInt64(ctx, argv[4]);
  uint8_t is_hidden = static_cast<uint8_t>(toUint32(ctx, argv[5]));
  lrvx_live_queue_position_on_trade_with_flag(
      h, symbol, lrvx_price_from_double(price), lrvx_quantity_from_double(qty), ts_ns,
      is_hidden);
  return JS_UNDEFINED;
}

static JSValue js_live_queue_position_set_hidden_order_policy(JSContext* ctx,
                                                              JSValueConst, int,
                                                              JSValueConst* argv)
{
  auto h = static_cast<LrvxLiveQueuePositionHandle>(getHandle(ctx, argv[0]));
  uint8_t policy = static_cast<uint8_t>(toUint32(ctx, argv[1]));
  lrvx_live_queue_position_set_hidden_order_policy(h, policy);
  return JS_UNDEFINED;
}

static JSValue js_live_queue_position_on_level_update(JSContext* ctx, JSValueConst, int,
                                                      JSValueConst* argv)
{
  auto h = static_cast<LrvxLiveQueuePositionHandle>(getHandle(ctx, argv[0]));
  uint32_t symbol = toUint32(ctx, argv[1]);
  uint8_t side = static_cast<uint8_t>(toUint32(ctx, argv[2]));
  double price = 0.0, new_qty = 0.0;
  JS_ToFloat64(ctx, &price, argv[3]);
  JS_ToFloat64(ctx, &new_qty, argv[4]);
  int64_t ts_ns = toInt64(ctx, argv[5]);
  lrvx_live_queue_position_on_level_update(h, symbol, side,
                                           lrvx_price_from_double(price),
                                           lrvx_quantity_from_double(new_qty), ts_ns);
  return JS_UNDEFINED;
}

static JSValue js_live_queue_position_snapshot(JSContext* ctx, JSValueConst, int,
                                               JSValueConst* argv)
{
  auto h = static_cast<LrvxLiveQueuePositionHandle>(getHandle(ctx, argv[0]));
  uint64_t order_id = static_cast<uint64_t>(toInt64(ctx, argv[1]));
  int64_t now_ns = toInt64(ctx, argv[2]);
  int64_t slots[6] = {0};
  uint8_t ok = lrvx_live_queue_position_snapshot(h, order_id, now_ns, slots);
  if (!ok)
  {
    return JS_NULL;
  }
  double conf = 0.0;
  std::memcpy(&conf, &slots[4], sizeof(double));
  JSValue obj = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, obj, "orderId", JS_NewInt64(ctx, slots[0]));
  JS_SetPropertyStr(ctx, obj, "queueAheadEst",
                    JS_NewFloat64(ctx, lrvx_quantity_to_double(slots[1])));
  JS_SetPropertyStr(ctx, obj, "total",
                    JS_NewFloat64(ctx, lrvx_quantity_to_double(slots[2])));
  JS_SetPropertyStr(ctx, obj, "lastUpdateNs", JS_NewBigInt64(ctx, slots[3]));
  JS_SetPropertyStr(ctx, obj, "confidence", JS_NewFloat64(ctx, conf));
  JS_SetPropertyStr(ctx, obj, "hiddenVolumeSeen",
                    JS_NewFloat64(ctx, lrvx_quantity_to_double(slots[5])));
  return obj;
}

static JSValue js_live_queue_position_tracked_count(JSContext* ctx, JSValueConst, int,
                                                    JSValueConst* argv)
{
  auto h = static_cast<LrvxLiveQueuePositionHandle>(getHandle(ctx, argv[0]));
  return JS_NewUint32(ctx, lrvx_live_queue_position_tracked_count(h));
}

// ============================================================
// Bar dispatch recorder (cross-binding parity test fixture)
// ============================================================

static JSValue js_bar_dispatch_recorder_create(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
  return createHandleObject(ctx, lrvx_bar_dispatch_recorder_create(), lrvx_bar_dispatch_recorder_destroy);
}

static JSValue js_bar_dispatch_recorder_destroy(JSContext* ctx, JSValueConst, int,
                                                JSValueConst* argv)
{
  return js_generic_handle_destroy(ctx, argv[0]);
}

static JSValue js_bar_dispatch_recorder_add_time_seconds(JSContext* ctx, JSValueConst, int,
                                                         JSValueConst* argv)
{
  auto h = static_cast<LrvxBarDispatchRecorderHandle>(getHandle(ctx, argv[0]));
  uint32_t seconds = toUint32(ctx, argv[1]);
  return JS_NewUint32(ctx, lrvx_bar_dispatch_recorder_add_time_seconds(h, seconds));
}

static JSValue js_bar_dispatch_recorder_on_trade(JSContext* ctx, JSValueConst, int,
                                                 JSValueConst* argv)
{
  auto h = static_cast<LrvxBarDispatchRecorderHandle>(getHandle(ctx, argv[0]));
  uint32_t symbol = toUint32(ctx, argv[1]);
  double price = toDouble(ctx, argv[2]);
  double qty = toDouble(ctx, argv[3]);
  int64_t ts_ns = 0;
  JS_ToInt64Ext(ctx, &ts_ns, argv[4]);
  lrvx_bar_dispatch_recorder_on_trade(h, symbol, price, qty, ts_ns);
  return JS_UNDEFINED;
}

static JSValue js_bar_dispatch_recorder_finalize(JSContext* ctx, JSValueConst, int,
                                                 JSValueConst* argv)
{
  lrvx_bar_dispatch_recorder_finalize(
      static_cast<LrvxBarDispatchRecorderHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}

static JSValue js_bar_dispatch_recorder_count(JSContext* ctx, JSValueConst, int,
                                              JSValueConst* argv)
{
  auto h = static_cast<LrvxBarDispatchRecorderHandle>(getHandle(ctx, argv[0]));
  return JS_NewUint32(ctx, lrvx_bar_dispatch_recorder_count(h));
}

static JSValue js_bar_dispatch_recorder_type_at(JSContext* ctx, JSValueConst, int,
                                                JSValueConst* argv)
{
  auto h = static_cast<LrvxBarDispatchRecorderHandle>(getHandle(ctx, argv[0]));
  uint32_t i = toUint32(ctx, argv[1]);
  return JS_NewUint32(ctx, lrvx_bar_dispatch_recorder_type_at(h, i));
}

static JSValue js_bar_dispatch_recorder_param_at(JSContext* ctx, JSValueConst, int,
                                                 JSValueConst* argv)
{
  auto h = static_cast<LrvxBarDispatchRecorderHandle>(getHandle(ctx, argv[0]));
  uint32_t i = toUint32(ctx, argv[1]);
  return JS_NewInt64(ctx, static_cast<int64_t>(lrvx_bar_dispatch_recorder_param_at(h, i)));
}

// ============================================================
// Tape diff bindings (replay-equivalence localization)
// ============================================================

static JSValue js_tape_diff(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  if (argc < 2)
  {
    return JS_ThrowTypeError(ctx, "tapeDiff: need leftPath and rightPath");
  }
  lrvx::JsCString left(ctx, argv[0]);
  lrvx::JsCString right(ctx, argv[1]);
  if (!left || !right)
  {
    return JS_ThrowTypeError(ctx, "tapeDiff: paths must be strings");
  }
  uint32_t max_mismatches = 16;
  int64_t tolerance_ns = 0;
  if (argc >= 3 && JS_IsObject(argv[2]))
  {
    JSValue v = JS_GetPropertyStr(ctx, argv[2], "maxMismatches");
    if (!JS_IsUndefined(v))
    {
      JS_ToUint32(ctx, &max_mismatches, v);
    }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[2], "fieldToleranceNs");
    if (!JS_IsUndefined(v))
    {
      JS_ToInt64Ext(ctx, &tolerance_ns, v);
    }
    JS_FreeValue(ctx, v);
  }

  LrvxTapeDiffHandle h =
      lrvx_tape_diff_create(left, right, max_mismatches, tolerance_ns);
  std::string leftStr = left.str();
  std::string rightStr = right.str();
  if (!h)
  {
    return JS_ThrowTypeError(ctx, "tapeDiff: %s", lrvx_last_error_message());
  }

  JSValue out = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, out, "leftPath", JS_NewString(ctx, leftStr.c_str()));
  JS_SetPropertyStr(ctx, out, "rightPath", JS_NewString(ctx, rightStr.c_str()));
  JS_SetPropertyStr(ctx, out, "leftCount",
                    JS_NewInt64(ctx, static_cast<int64_t>(lrvx_tape_diff_left_count(h))));
  JS_SetPropertyStr(ctx, out, "rightCount",
                    JS_NewInt64(ctx, static_cast<int64_t>(lrvx_tape_diff_right_count(h))));

  uint64_t div_idx = 0;
  if (lrvx_tape_diff_first_divergence(h, &div_idx))
  {
    JS_SetPropertyStr(ctx, out, "firstDivergenceIndex",
                      JS_NewInt64(ctx, static_cast<int64_t>(div_idx)));
  }
  else
  {
    JS_SetPropertyStr(ctx, out, "firstDivergenceIndex", JS_NULL);
  }
  JS_SetPropertyStr(ctx, out, "equal",
                    JS_NewBool(ctx, lrvx_tape_diff_equal(h) != 0));

  const uint64_t mcount = lrvx_tape_diff_mismatch_count(h);
  JSValue mlist = JS_NewArray(ctx);
  if (mcount > 0)
  {
    std::vector<LrvxTapeDiffMismatch> buf(mcount);
    lrvx_tape_diff_copy_mismatches(h, buf.data(), mcount);
    for (uint64_t i = 0; i < mcount; ++i)
    {
      JSValue entry = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, entry, "index",
                        JS_NewInt64(ctx, static_cast<int64_t>(buf[i].index)));
      auto putSide = [&](const char* key, const LrvxTapeDiffTrade& t)
      {
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "exchangeTsNs", JS_NewBigInt64(ctx, t.exchange_ts_ns));
        JS_SetPropertyStr(ctx, o, "symbolId", JS_NewUint32(ctx, t.symbol_id));
        JS_SetPropertyStr(ctx, o, "priceRaw", JS_NewInt64(ctx, t.price_raw));
        JS_SetPropertyStr(ctx, o, "qtyRaw", JS_NewInt64(ctx, t.qty_raw));
        JS_SetPropertyStr(ctx, o, "side", JS_NewUint32(ctx, t.side));
        JS_SetPropertyStr(ctx, entry, key, o);
      };
      putSide("left", buf[i].left);
      putSide("right", buf[i].right);
      JS_SetPropertyUint32(ctx, mlist, static_cast<uint32_t>(i), entry);
    }
  }
  JS_SetPropertyStr(ctx, out, "mismatches", mlist);

  lrvx_tape_diff_destroy(h);
  return out;
}

// ============================================================
// Execution algorithm bindings (TWAP / VWAP / Iceberg / POV)
// ============================================================

static JSValue js_exec_twap_create(JSContext* ctx, JSValueConst, int argc,
                                   JSValueConst* argv)
{
  if (argc < 8)
  {
    return JS_ThrowTypeError(ctx, "twap_create: need 8 args");
  }
  double target_qty;
  JS_ToFloat64(ctx, &target_qty, argv[0]);
  uint8_t side = static_cast<uint8_t>(toUint32(ctx, argv[1]));
  uint32_t symbol = toUint32(ctx, argv[2]);
  uint8_t type = static_cast<uint8_t>(toUint32(ctx, argv[3]));
  double limit_price;
  JS_ToFloat64(ctx, &limit_price, argv[4]);
  int64_t duration_ns = toInt64(ctx, argv[5]);
  uint32_t slice_count = toUint32(ctx, argv[6]);
  int64_t start_time_ns = toInt64(ctx, argv[7]);
  auto h = lrvx_exec_twap_create(target_qty, side, symbol, type, limit_price,
                                 duration_ns, slice_count, start_time_ns);
  if (!h)
  {
    return JS_ThrowTypeError(ctx, "twap_create: invalid args");
  }
  return createHandleObject(ctx, h, lrvx_exec_destroy);
}

static JSValue js_exec_vwap_create(JSContext* ctx, JSValueConst, int argc,
                                   JSValueConst* argv)
{
  if (argc < 6)
  {
    return JS_ThrowTypeError(ctx, "vwap_create: need 6 args");
  }
  double target_qty;
  JS_ToFloat64(ctx, &target_qty, argv[0]);
  uint8_t side = static_cast<uint8_t>(toUint32(ctx, argv[1]));
  uint32_t symbol = toUint32(ctx, argv[2]);
  uint8_t type = static_cast<uint8_t>(toUint32(ctx, argv[3]));
  double limit_price;
  JS_ToFloat64(ctx, &limit_price, argv[4]);
  std::vector<int64_t> ts;
  std::vector<double> vol;
  JSValue lenVal = JS_GetPropertyStr(ctx, argv[5], "length");
  uint32_t len = 0;
  JS_ToUint32(ctx, &len, lenVal);
  JS_FreeValue(ctx, lenVal);
  for (uint32_t i = 0; i < len; ++i)
  {
    JSValue row = JS_GetPropertyUint32(ctx, argv[5], i);
    JSValue tsv = JS_GetPropertyUint32(ctx, row, 0);
    JSValue volv = JS_GetPropertyUint32(ctx, row, 1);
    ts.push_back(toInt64(ctx, tsv));
    double v;
    JS_ToFloat64(ctx, &v, volv);
    vol.push_back(v);
    JS_FreeValue(ctx, tsv);
    JS_FreeValue(ctx, volv);
    JS_FreeValue(ctx, row);
  }
  auto h = lrvx_exec_vwap_create(target_qty, side, symbol, type, limit_price,
                                 ts.empty() ? nullptr : ts.data(),
                                 vol.empty() ? nullptr : vol.data(),
                                 ts.size());
  if (!h)
  {
    return JS_ThrowTypeError(ctx, "vwap_create: invalid args");
  }
  return createHandleObject(ctx, h, lrvx_exec_destroy);
}

static JSValue js_exec_iceberg_create(JSContext* ctx, JSValueConst, int argc,
                                      JSValueConst* argv)
{
  if (argc < 6)
  {
    return JS_ThrowTypeError(ctx, "iceberg_create: need 6 args");
  }
  double target_qty;
  JS_ToFloat64(ctx, &target_qty, argv[0]);
  uint8_t side = static_cast<uint8_t>(toUint32(ctx, argv[1]));
  uint32_t symbol = toUint32(ctx, argv[2]);
  uint8_t type = static_cast<uint8_t>(toUint32(ctx, argv[3]));
  double limit_price, visible_qty;
  JS_ToFloat64(ctx, &limit_price, argv[4]);
  JS_ToFloat64(ctx, &visible_qty, argv[5]);
  auto h = lrvx_exec_iceberg_create(target_qty, side, symbol, type, limit_price,
                                    visible_qty);
  if (!h)
  {
    return JS_ThrowTypeError(ctx, "iceberg_create: invalid args");
  }
  return createHandleObject(ctx, h, lrvx_exec_destroy);
}

static JSValue js_exec_pov_create(JSContext* ctx, JSValueConst, int argc,
                                  JSValueConst* argv)
{
  if (argc < 7)
  {
    return JS_ThrowTypeError(ctx, "pov_create: need 7 args");
  }
  double target_qty;
  JS_ToFloat64(ctx, &target_qty, argv[0]);
  uint8_t side = static_cast<uint8_t>(toUint32(ctx, argv[1]));
  uint32_t symbol = toUint32(ctx, argv[2]);
  uint8_t type = static_cast<uint8_t>(toUint32(ctx, argv[3]));
  double limit_price, rate, min_slice;
  JS_ToFloat64(ctx, &limit_price, argv[4]);
  JS_ToFloat64(ctx, &rate, argv[5]);
  JS_ToFloat64(ctx, &min_slice, argv[6]);
  auto h = lrvx_exec_pov_create(target_qty, side, symbol, type, limit_price, rate,
                                min_slice);
  if (!h)
  {
    return JS_ThrowTypeError(ctx, "pov_create: invalid args");
  }
  return createHandleObject(ctx, h, lrvx_exec_destroy);
}

static JSValue js_exec_destroy(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return js_generic_handle_destroy(ctx, argv[0]);
}

static JSValue js_exec_step(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto h = static_cast<LrvxExecAlgoHandle>(getHandle(ctx, argv[0]));
  lrvx_exec_step(h, toInt64(ctx, argv[1]));
  size_t n = lrvx_exec_pending_count(h);
  JSValue arr = JS_NewArray(ctx);
  for (size_t i = 0; i < n; ++i)
  {
    LrvxExecChildOrder c{};
    if (lrvx_exec_pending_at(h, i, &c))
    {
      JSValue o = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, o, "orderId", JS_NewInt64(ctx, static_cast<int64_t>(c.order_id)));
      JS_SetPropertyStr(ctx, o, "timestampNs", JS_NewBigInt64(ctx, c.timestamp_ns));
      JS_SetPropertyStr(ctx, o, "qty", JS_NewFloat64(ctx, c.qty));
      JS_SetPropertyStr(ctx, o, "price", JS_NewFloat64(ctx, c.price));
      JS_SetPropertyStr(ctx, o, "type",
                        JS_NewString(ctx, c.type == 1 ? "limit" : "market"));
      JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), o);
    }
  }
  lrvx_exec_clear_pending(h);
  return arr;
}

static JSValue js_exec_report_fill(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  double q;
  JS_ToFloat64(ctx, &q, argv[1]);
  lrvx_exec_report_fill(static_cast<LrvxExecAlgoHandle>(getHandle(ctx, argv[0])), q);
  return JS_UNDEFINED;
}

static JSValue js_exec_observe_volume(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  double q;
  JS_ToFloat64(ctx, &q, argv[1]);
  lrvx_exec_observe_volume(static_cast<LrvxExecAlgoHandle>(getHandle(ctx, argv[0])), q);
  return JS_UNDEFINED;
}

static JSValue js_exec_submitted_qty(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewFloat64(
      ctx, lrvx_exec_submitted_qty(static_cast<LrvxExecAlgoHandle>(getHandle(ctx, argv[0]))));
}

static JSValue js_exec_filled_qty(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewFloat64(
      ctx, lrvx_exec_filled_qty(static_cast<LrvxExecAlgoHandle>(getHandle(ctx, argv[0]))));
}

static JSValue js_exec_remaining_qty(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewFloat64(
      ctx, lrvx_exec_remaining_qty(static_cast<LrvxExecAlgoHandle>(getHandle(ctx, argv[0]))));
}

static JSValue js_exec_is_done(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewBool(
      ctx, lrvx_exec_is_done(static_cast<LrvxExecAlgoHandle>(getHandle(ctx, argv[0]))) != 0);
}

// ============================================================
// Portfolio risk aggregator bindings
// ============================================================

static JSValue js_portfolio_risk_create(JSContext* ctx, JSValueConst, int argc,
                                        JSValueConst* argv)
{
  LrvxPortfolioRiskRules rules{};
  double initial_equity = 0.0;
  auto readOpt = [&](JSValueConst obj, const char* key, uint8_t& has, double& v)
  {
    JSValue val = JS_GetPropertyStr(ctx, obj, key);
    if (!JS_IsUndefined(val) && !JS_IsNull(val))
    {
      has = 1;
      JS_ToFloat64(ctx, &v, val);
    }
    JS_FreeValue(ctx, val);
  };
  if (argc > 0 && JS_IsObject(argv[0]))
  {
    readOpt(argv[0], "maxDrawdownPct", rules.has_max_drawdown_pct, rules.max_drawdown_pct);
    readOpt(argv[0], "maxDailyLoss", rules.has_max_daily_loss, rules.max_daily_loss);
    readOpt(argv[0], "maxGrossExposure", rules.has_max_gross_exposure, rules.max_gross_exposure);
    readOpt(argv[0], "maxConcentrationPct", rules.has_max_concentration_pct, rules.max_concentration_pct);
  }
  if (argc > 1)
  {
    JS_ToFloat64(ctx, &initial_equity, argv[1]);
  }
  LrvxPortfolioRiskHandle h = lrvx_portfolio_risk_create(&rules, initial_equity);
  if (!h)
  {
    return JS_ThrowTypeError(ctx,
                             "PortfolioRiskAggregator: construction failed. A "
                             "maxDrawdownPct rule needs a positive "
                             "initialEquity to measure against.");
  }
  return createHandleObject(ctx, h, lrvx_portfolio_risk_destroy);
}

static JSValue js_portfolio_risk_destroy(JSContext* ctx, JSValueConst, int,
                                         JSValueConst* argv)
{
  return js_generic_handle_destroy(ctx, argv[0]);
}

static JSValue js_portfolio_risk_update(JSContext* ctx, JSValueConst, int argc,
                                        JSValueConst* argv)
{
  if (argc < 3)
  {
    return JS_ThrowTypeError(ctx, "update(handle, name, fields)");
  }
  auto h = static_cast<LrvxPortfolioRiskHandle>(getHandle(ctx, argv[0]));
  lrvx::JsCString name(ctx, argv[1]);
  if (!name)
  {
    return JS_ThrowTypeError(ctx, "update: name must be a string");
  }
  LrvxStrategyAccountFields f{};
  uint8_t mask = 0;
  auto putDouble = [&](const char* key, uint8_t bit, double& out)
  {
    JSValue v = JS_GetPropertyStr(ctx, argv[2], key);
    if (!JS_IsUndefined(v))
    {
      JS_ToFloat64(ctx, &out, v);
      mask |= bit;
    }
    JS_FreeValue(ctx, v);
  };
  auto putInt = [&](const char* key, uint8_t bit, uint64_t& out)
  {
    JSValue v = JS_GetPropertyStr(ctx, argv[2], key);
    if (!JS_IsUndefined(v))
    {
      int64_t tmp = 0;
      JS_ToInt64Ext(ctx, &tmp, v);
      out = static_cast<uint64_t>(tmp);
      mask |= bit;
    }
    JS_FreeValue(ctx, v);
  };
  putDouble("realizedPnl", 1u << 0, f.realized_pnl);
  putDouble("unrealizedPnl", 1u << 1, f.unrealized_pnl);
  putDouble("fees", 1u << 2, f.fees);
  putDouble("grossExposure", 1u << 3, f.gross_exposure);
  putDouble("netExposure", 1u << 4, f.net_exposure);
  putInt("tradeCount", 1u << 5, f.trade_count);
  lrvx_portfolio_risk_update(h, name, &f, mask);
  return JS_UNDEFINED;
}

static JSValue js_portfolio_risk_remove(JSContext* ctx, JSValueConst, int,
                                        JSValueConst* argv)
{
  auto h = static_cast<LrvxPortfolioRiskHandle>(getHandle(ctx, argv[0]));
  lrvx::JsCString name(ctx, argv[1]);
  if (name)
  {
    lrvx_portfolio_risk_remove(h, name);
  }
  return JS_UNDEFINED;
}

static JSValue js_portfolio_risk_reset_kill_switch(JSContext* ctx, JSValueConst, int,
                                                   JSValueConst* argv)
{
  lrvx_portfolio_risk_reset_kill_switch(
      static_cast<LrvxPortfolioRiskHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}

static JSValue breachToJsObject(JSContext* ctx, const LrvxBreach& b)
{
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "rule", JS_NewString(ctx, b.rule ? b.rule : ""));
  JS_SetPropertyStr(ctx, o, "value", JS_NewFloat64(ctx, b.value));
  JS_SetPropertyStr(ctx, o, "limit", JS_NewFloat64(ctx, b.limit));
  JS_SetPropertyStr(ctx, o, "detail", JS_NewString(ctx, b.detail ? b.detail : ""));
  return o;
}

static JSValue js_portfolio_risk_check_order(JSContext* ctx, JSValueConst, int argc,
                                             JSValueConst* argv)
{
  if (argc < 4)
  {
    return JS_ThrowTypeError(ctx, "checkOrder(handle, strategy, notional, side)");
  }
  auto h = static_cast<LrvxPortfolioRiskHandle>(getHandle(ctx, argv[0]));
  lrvx::JsCString strat(ctx, argv[1]);
  double notional = 0.0;
  JS_ToFloat64(ctx, &notional, argv[2]);
  lrvx::JsCString side(ctx, argv[3]);
  LrvxBreach b{};
  uint8_t hit = lrvx_portfolio_risk_check_order(h, strat ? strat : "",
                                                notional, side ? side : "", &b);
  JSValue out = hit ? breachToJsObject(ctx, b) : JS_NULL;
  return out;
}

static JSValue js_portfolio_risk_snapshot(JSContext* ctx, JSValueConst, int,
                                          JSValueConst* argv)
{
  auto h = static_cast<LrvxPortfolioRiskHandle>(getHandle(ctx, argv[0]));
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "totalDailyPnl",
                    JS_NewFloat64(ctx, lrvx_portfolio_risk_total_daily_pnl(h)));
  JS_SetPropertyStr(ctx, o, "totalGrossExposure",
                    JS_NewFloat64(ctx, lrvx_portfolio_risk_total_gross_exposure(h)));
  JS_SetPropertyStr(ctx, o, "currentEquity",
                    JS_NewFloat64(ctx, lrvx_portfolio_risk_current_equity(h)));
  JS_SetPropertyStr(ctx, o, "drawdownPct",
                    JS_NewFloat64(ctx, lrvx_portfolio_risk_drawdown_pct(h)));
  JS_SetPropertyStr(ctx, o, "killSwitchActive",
                    JS_NewBool(ctx, lrvx_portfolio_risk_kill_switch_active(h) != 0));
  const uint64_t bn = lrvx_portfolio_risk_breach_count(h);
  JSValue arr = JS_NewArray(ctx);
  for (uint64_t i = 0; i < bn; ++i)
  {
    LrvxBreach b{};
    if (lrvx_portfolio_risk_breach_at(h, i, &b))
    {
      JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), breachToJsObject(ctx, b));
    }
  }
  JS_SetPropertyStr(ctx, o, "breaches", arr);
  JS_SetPropertyStr(ctx, o, "accountCount",
                    JS_NewInt64(ctx, static_cast<int64_t>(lrvx_portfolio_risk_account_count(h))));
  return o;
}

// ============================================================
// Latency model bindings
// ============================================================

static JSValue js_lat_constant_create(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  int64_t feed = (argc > 0) ? toInt64(ctx, argv[0]) : 0;
  int64_t order = (argc > 1) ? toInt64(ctx, argv[1]) : 0;
  int64_t fill = (argc > 2) ? toInt64(ctx, argv[2]) : 0;
  LrvxLatencyModelHandle h = lrvx_latency_constant_create(feed, order, fill);
  if (!h)
  {
    return JS_ThrowTypeError(ctx, "ConstantLatency: feed/order/fill must be non-negative");
  }
  return createHandleObject(ctx, h, lrvx_latency_destroy);
}

static JSValue js_lat_gaussian_create(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  double fm = (argc > 0) ? toDouble(ctx, argv[0]) : 0.0;
  double fs = (argc > 1) ? toDouble(ctx, argv[1]) : 0.0;
  double om = (argc > 2) ? toDouble(ctx, argv[2]) : 0.0;
  double os = (argc > 3) ? toDouble(ctx, argv[3]) : 0.0;
  double lm = (argc > 4) ? toDouble(ctx, argv[4]) : 0.0;
  double ls = (argc > 5) ? toDouble(ctx, argv[5]) : 0.0;
  uint64_t seed = (argc > 6) ? static_cast<uint64_t>(toInt64(ctx, argv[6])) : 0;
  LrvxLatencyModelHandle h = lrvx_latency_gaussian_create(fm, fs, om, os, lm, ls, seed);
  if (!h)
  {
    return JS_ThrowTypeError(ctx, "GaussianLatency: means and stddevs must be non-negative");
  }
  return createHandleObject(ctx, h, lrvx_latency_destroy);
}

static JSValue js_lat_exponential_create(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  double fm = (argc > 0) ? toDouble(ctx, argv[0]) : 0.0;
  double om = (argc > 1) ? toDouble(ctx, argv[1]) : 0.0;
  double lm = (argc > 2) ? toDouble(ctx, argv[2]) : 0.0;
  uint64_t seed = (argc > 3) ? static_cast<uint64_t>(toInt64(ctx, argv[3])) : 0;
  LrvxLatencyModelHandle h = lrvx_latency_exponential_create(fm, om, lm, seed);
  if (!h)
  {
    return JS_ThrowTypeError(ctx, "ExponentialLatency: means must be non-negative");
  }
  return createHandleObject(ctx, h, lrvx_latency_destroy);
}

static std::vector<int64_t> readJsInt64Array(JSContext* ctx, JSValueConst arr)
{
  std::vector<int64_t> out;
  if (!JS_IsArray(ctx, arr))
  {
    return out;
  }
  uint32_t len = 0;
  JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
  JS_ToUint32(ctx, &len, lenVal);
  JS_FreeValue(ctx, lenVal);
  out.reserve(len);
  for (uint32_t i = 0; i < len; ++i)
  {
    JSValue v = JS_GetPropertyUint32(ctx, arr, i);
    out.push_back(toInt64(ctx, v));
    JS_FreeValue(ctx, v);
  }
  return out;
}

static JSValue js_lat_empirical_create(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  std::vector<int64_t> feed = (argc > 0) ? readJsInt64Array(ctx, argv[0]) : std::vector<int64_t>{};
  std::vector<int64_t> order = (argc > 1) ? readJsInt64Array(ctx, argv[1]) : std::vector<int64_t>{};
  std::vector<int64_t> fill = (argc > 2) ? readJsInt64Array(ctx, argv[2]) : std::vector<int64_t>{};
  uint64_t seed = (argc > 3) ? static_cast<uint64_t>(toInt64(ctx, argv[3])) : 0;
  LrvxLatencyModelHandle h = lrvx_latency_empirical_create(
      feed.empty() ? nullptr : feed.data(), feed.size(),
      order.empty() ? nullptr : order.data(), order.size(),
      fill.empty() ? nullptr : fill.data(), fill.size(),
      seed);
  if (!h)
  {
    return JS_ThrowTypeError(ctx, "EmpiricalLatency: provide non-empty samples and non-negative values");
  }
  return createHandleObject(ctx, h, lrvx_latency_destroy);
}

static JSValue js_lat_destroy(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return js_generic_handle_destroy(ctx, argv[0]);
}
static JSValue js_lat_feed_delay(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewInt64(ctx, lrvx_latency_feed_delay(
                              static_cast<LrvxLatencyModelHandle>(getHandle(ctx, argv[0]))));
}
static JSValue js_lat_order_delay(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewInt64(ctx, lrvx_latency_order_delay(
                              static_cast<LrvxLatencyModelHandle>(getHandle(ctx, argv[0]))));
}
static JSValue js_lat_fill_delay(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewInt64(ctx, lrvx_latency_fill_delay(
                              static_cast<LrvxLatencyModelHandle>(getHandle(ctx, argv[0]))));
}
static JSValue js_lat_sample(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  LrvxLatencySample s{};
  lrvx_latency_sample(static_cast<LrvxLatencyModelHandle>(getHandle(ctx, argv[0])), &s);
  JSValue obj = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, obj, "feedNs", JS_NewInt64(ctx, s.feed_ns));
  JS_SetPropertyStr(ctx, obj, "orderNs", JS_NewInt64(ctx, s.order_ns));
  JS_SetPropertyStr(ctx, obj, "fillNs", JS_NewInt64(ctx, s.fill_ns));
  return obj;
}
static JSValue js_lat_reset(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  uint64_t seed = (argc > 1) ? static_cast<uint64_t>(toInt64(ctx, argv[1])) : 0;
  lrvx_latency_reset(static_cast<LrvxLatencyModelHandle>(getHandle(ctx, argv[0])), seed);
  return JS_UNDEFINED;
}

// ============================================================
// Volume profile bindings
// ============================================================

static JSValue js_vprofile_create(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return createHandleObject(ctx, lrvx_volume_profile_create(toDouble(ctx, argv[0])), lrvx_volume_profile_destroy);
}
static JSValue js_vprofile_destroy(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return js_generic_handle_destroy(ctx, argv[0]);
}
static JSValue js_vprofile_add_trade(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  lrvx_volume_profile_add_trade(
      static_cast<LrvxVolumeProfileHandle>(getHandle(ctx, argv[0])), toDouble(ctx, argv[1]),
      toDouble(ctx, argv[2]), static_cast<uint8_t>(toUint32(ctx, argv[3])));
  return JS_UNDEFINED;
}
static JSValue js_vprofile_poc(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewFloat64(
      ctx, lrvx_volume_profile_poc(static_cast<LrvxVolumeProfileHandle>(getHandle(ctx, argv[0]))));
}
static JSValue js_vprofile_vah(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewFloat64(
      ctx, lrvx_volume_profile_vah(static_cast<LrvxVolumeProfileHandle>(getHandle(ctx, argv[0]))));
}
static JSValue js_vprofile_val(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewFloat64(
      ctx, lrvx_volume_profile_val(static_cast<LrvxVolumeProfileHandle>(getHandle(ctx, argv[0]))));
}
static JSValue js_vprofile_total_vol(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  return JS_NewFloat64(ctx, lrvx_volume_profile_total_volume(
                                static_cast<LrvxVolumeProfileHandle>(getHandle(ctx, argv[0]))));
}
static JSValue js_vprofile_clear(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  lrvx_volume_profile_clear(static_cast<LrvxVolumeProfileHandle>(getHandle(ctx, argv[0])));
  return JS_UNDEFINED;
}

// ============================================================
// Stats
// ============================================================

static JSValue js_stat_correlation(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
  auto x = jsArrayToDoubles(ctx, argv[0]);
  auto y = jsArrayToDoubles(ctx, argv[1]);
  return JS_NewFloat64(ctx, lrvx_stat_correlation(x.data(), y.data(),
                                                  std::min(x.size(), y.size())));
}

// ============================================================
// Console shim
// ============================================================

static JSValue js_console_impl(JSContext* ctx, int argc, JSValueConst* argv, FILE* stream)
{
  for (int i = 0; i < argc; i++)
  {
    if (i > 0)
    {
      fputc(' ', stream);
    }
    bool printed = false;
    if (JS_IsObject(argv[i]) && !JS_IsFunction(ctx, argv[i]))
    {
      // JSONStringify refuses an object holding a BigInt, which every
      // event object now does (its nanosecond fields). Fall through to
      // the plain string conversion rather than printing nothing.
      JSValue json = JS_JSONStringify(ctx, argv[i], JS_UNDEFINED, JS_UNDEFINED);
      if (JS_IsException(json))
      {
        clearStrayException(ctx);
      }
      else
      {
        lrvx::JsCString s(ctx, json);
        if (s.ok())
        {
          fputs(s.get(), stream);
          printed = true;
        }
      }
      JS_FreeValue(ctx, json);
    }
    if (!printed)
    {
      lrvx::JsCString str(ctx, argv[i]);
      if (str.ok())
      {
        fputs(str.get(), stream);
      }
      else
      {
        // A Symbol, or a toString that threw. Logging must not swallow the
        // value silently, and must not leave the exception pending for
        // whatever the script does next.
        clearStrayException(ctx);
        fputs("[unprintable]", stream);
      }
    }
  }
  fputc('\n', stream);
  return JS_UNDEFINED;
}

static JSValue js_console_log(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  return js_console_impl(ctx, argc, argv, stdout);
}

static JSValue js_console_warn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  fputs("[warn] ", stderr);
  return js_console_impl(ctx, argc, argv, stderr);
}

static JSValue js_console_error(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  fputs("[error] ", stderr);
  return js_console_impl(ctx, argc, argv, stderr);
}

// ============================================================
// Registration
// ============================================================

static void addGlobalFunc(JSContext* ctx, const char* name, JSCFunction* func, int argc)
{
  JSValue global = JS_GetGlobalObject(ctx);
  JS_SetPropertyStr(ctx, global, name, JS_NewCFunction(ctx, func, name, argc));
  JS_FreeValue(ctx, global);
}

// ============================================================
// MarketProfile
// ============================================================

static JSValue js_mp_create(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return createHandleObject(c, lrvx_market_profile_create(toDouble(c, a[0]), toUint32(c, a[1]), toInt64(c, a[2])), lrvx_market_profile_destroy);
}

static JSValue js_mp_destroy(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return js_generic_handle_destroy(c, a[0]);
}

static JSValue js_mp_add_trade(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  lrvx_market_profile_add_trade(static_cast<LrvxMarketProfileHandle>(getHandle(c, a[0])),
                                toInt64(c, a[1]), toDouble(c, a[2]), toDouble(c, a[3]), static_cast<uint8_t>(toUint32(c, a[4])));
  return JS_UNDEFINED;
}

static JSValue js_mp_poc(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewFloat64(c, lrvx_market_profile_poc(static_cast<LrvxMarketProfileHandle>(getHandle(c, a[0]))));
}

static JSValue js_mp_vah(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewFloat64(c, lrvx_market_profile_vah(static_cast<LrvxMarketProfileHandle>(getHandle(c, a[0]))));
}

static JSValue js_mp_val(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewFloat64(c, lrvx_market_profile_val(static_cast<LrvxMarketProfileHandle>(getHandle(c, a[0]))));
}

static JSValue js_mp_ib_high(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewFloat64(c, lrvx_market_profile_ib_high(static_cast<LrvxMarketProfileHandle>(getHandle(c, a[0]))));
}

static JSValue js_mp_ib_low(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewFloat64(c, lrvx_market_profile_ib_low(static_cast<LrvxMarketProfileHandle>(getHandle(c, a[0]))));
}

static JSValue js_mp_is_poor_high(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewBool(c, lrvx_market_profile_is_poor_high(static_cast<LrvxMarketProfileHandle>(getHandle(c, a[0]))));
}

static JSValue js_mp_is_poor_low(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewBool(c, lrvx_market_profile_is_poor_low(static_cast<LrvxMarketProfileHandle>(getHandle(c, a[0]))));
}

static JSValue js_mp_clear(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  lrvx_market_profile_clear(static_cast<LrvxMarketProfileHandle>(getHandle(c, a[0])));
  return JS_UNDEFINED;
}

// ============================================================
// CompositeBook
// ============================================================

static JSValue js_cb_create(JSContext* c, JSValueConst, int, JSValueConst*)
{
  return createHandleObject(c, lrvx_composite_book_create(), lrvx_composite_book_destroy);
}

static JSValue js_cb_destroy(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return js_generic_handle_destroy(c, a[0]);
}

// Same shape as js_book_apply_snapshot/js_book_apply_delta above, plus the
// exchange/symbol slot and the receive timestamp the composite matrix keys
// staleness off of. is_delta=false (apply_snapshot) replaces both sides of
// that exchange's top-of-book wholesale; apply_delta updates only the
// side(s) present here and leaves the other side untouched -- see
// lrvx_composite_book_apply_snapshot/_apply_delta in lrvx_capi.h.
static JSValue js_cb_apply_snapshot(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto h = static_cast<LrvxCompositeBookHandle>(getHandle(c, a[0]));
  uint32_t exchange = toUint32(c, a[1]);
  uint32_t symbol = toUint32(c, a[2]);
  auto bp = jsArrayToDoubles(c, a[3]);
  auto bq = jsArrayToDoubles(c, a[4]);
  auto ap = jsArrayToDoubles(c, a[5]);
  auto aq = jsArrayToDoubles(c, a[6]);
  int64_t recvNs = toInt64(c, a[7]);
  lrvx_composite_book_apply_snapshot(h, exchange, symbol, bp.data(), bq.data(), bp.size(),
                                     ap.data(), aq.data(), ap.size(), recvNs);
  return JS_UNDEFINED;
}

static JSValue js_cb_apply_delta(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto h = static_cast<LrvxCompositeBookHandle>(getHandle(c, a[0]));
  uint32_t exchange = toUint32(c, a[1]);
  uint32_t symbol = toUint32(c, a[2]);
  auto bp = jsArrayToDoubles(c, a[3]);
  auto bq = jsArrayToDoubles(c, a[4]);
  auto ap = jsArrayToDoubles(c, a[5]);
  auto aq = jsArrayToDoubles(c, a[6]);
  int64_t recvNs = toInt64(c, a[7]);
  lrvx_composite_book_apply_delta(h, exchange, symbol, bp.data(), bq.data(), bp.size(),
                                  ap.data(), aq.data(), ap.size(), recvNs);
  return JS_UNDEFINED;
}

static JSValue js_cb_best_bid(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  double p = 0, q = 0;
  if (lrvx_composite_book_best_bid(static_cast<LrvxCompositeBookHandle>(getHandle(c, a[0])),
                                   toUint32(c, a[1]), &p, &q))
  {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "price", JS_NewFloat64(c, p));
    JS_SetPropertyStr(c, o, "qty", JS_NewFloat64(c, q));
    return o;
  }
  return JS_NULL;
}

static JSValue js_cb_best_ask(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  double p = 0, q = 0;
  if (lrvx_composite_book_best_ask(static_cast<LrvxCompositeBookHandle>(getHandle(c, a[0])),
                                   toUint32(c, a[1]), &p, &q))
  {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "price", JS_NewFloat64(c, p));
    JS_SetPropertyStr(c, o, "qty", JS_NewFloat64(c, q));
    return o;
  }
  return JS_NULL;
}

static JSValue js_cb_has_arb(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewBool(c, lrvx_composite_book_has_arb(static_cast<LrvxCompositeBookHandle>(getHandle(c, a[0])), toUint32(c, a[1])));
}

static JSValue js_cb_mark_stale(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  lrvx_composite_book_mark_stale(static_cast<LrvxCompositeBookHandle>(getHandle(c, a[0])), toUint32(c, a[1]), toUint32(c, a[2]));
  return JS_UNDEFINED;
}

static JSValue js_cb_check_staleness(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  lrvx_composite_book_check_staleness(static_cast<LrvxCompositeBookHandle>(getHandle(c, a[0])), toInt64(c, a[1]), toInt64(c, a[2]));
  return JS_UNDEFINED;
}

// ============================================================
// OrderTracker
// ============================================================

static JSValue js_ot_create(JSContext* c, JSValueConst, int, JSValueConst*)
{
  return createHandleObject(c, lrvx_order_tracker_create(), lrvx_order_tracker_destroy);
}

static JSValue js_ot_destroy(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return js_generic_handle_destroy(c, a[0]);
}

static JSValue js_ot_submit(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewBool(c, lrvx_order_tracker_on_submitted(static_cast<LrvxOrderTrackerHandle>(getHandle(c, a[0])),
                                                       static_cast<uint64_t>(toInt64(c, a[1])), toUint32(c, a[2]), static_cast<uint8_t>(toUint32(c, a[3])),
                                                       toDouble(c, a[4]), toDouble(c, a[5])));
}

static JSValue js_ot_filled(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewBool(c, lrvx_order_tracker_on_filled(static_cast<LrvxOrderTrackerHandle>(getHandle(c, a[0])),
                                                    static_cast<uint64_t>(toInt64(c, a[1])), toDouble(c, a[2])));
}

static JSValue js_ot_canceled(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewBool(c, lrvx_order_tracker_on_canceled(static_cast<LrvxOrderTrackerHandle>(getHandle(c, a[0])),
                                                      static_cast<uint64_t>(toInt64(c, a[1]))));
}

static JSValue js_ot_is_active(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewBool(c, lrvx_order_tracker_is_active(static_cast<LrvxOrderTrackerHandle>(getHandle(c, a[0])),
                                                    static_cast<uint64_t>(toInt64(c, a[1]))));
}

static JSValue js_ot_active_count(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewUint32(c, lrvx_order_tracker_active_count(static_cast<LrvxOrderTrackerHandle>(getHandle(c, a[0]))));
}

static JSValue js_ot_total_count(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewUint32(c, lrvx_order_tracker_total_count(static_cast<LrvxOrderTrackerHandle>(getHandle(c, a[0]))));
}

static JSValue js_ot_prune(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  lrvx_order_tracker_prune(static_cast<LrvxOrderTrackerHandle>(getHandle(c, a[0])));
  return JS_UNDEFINED;
}

// ============================================================
// OrderJourneyTracer
// ============================================================

static JSValue js_ojt_create(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  uint64_t maxOrders = (argc > 0) ? static_cast<uint64_t>(toInt64(c, a[0])) : 1000000;
  uint64_t maxRecords = (argc > 1) ? static_cast<uint64_t>(toInt64(c, a[1])) : 64;
  double sampleRate = (argc > 2) ? toDouble(c, a[2]) : 1.0;
  uint64_t sampleSalt =
      (argc > 3) ? static_cast<uint64_t>(toInt64(c, a[3])) : 0x9E3779B97F4A7C15ULL;
  return createHandleObject(c, lrvx_order_journey_tracer_create(maxOrders, maxRecords, sampleRate, sampleSalt), lrvx_order_journey_tracer_destroy);
}

static JSValue js_ojt_destroy(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return js_generic_handle_destroy(c, a[0]);
}

static JSValue js_ojt_order_count(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewInt64(c, static_cast<int64_t>(lrvx_order_journey_tracer_order_count(
                            static_cast<LrvxOrderJourneyTracerHandle>(getHandle(c, a[0])))));
}

static JSValue js_ojt_record_count(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewInt64(c, static_cast<int64_t>(lrvx_order_journey_tracer_record_count(
                            static_cast<LrvxOrderJourneyTracerHandle>(getHandle(c, a[0])))));
}

static JSValue js_ojt_median_ack(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewFloat64(
      c, lrvx_order_journey_tracer_median_ack_latency_ns(
             static_cast<LrvxOrderJourneyTracerHandle>(getHandle(c, a[0]))));
}

static JSValue js_ojt_median_ttf(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewFloat64(
      c, lrvx_order_journey_tracer_median_time_to_first_fill_ns(
             static_cast<LrvxOrderJourneyTracerHandle>(getHandle(c, a[0]))));
}

static JSValue js_ojt_maker_ratio(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewFloat64(c, lrvx_order_journey_tracer_maker_fill_ratio(
                              static_cast<LrvxOrderJourneyTracerHandle>(getHandle(c, a[0]))));
}

static JSValue js_ojt_cancel_race(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewFloat64(c, lrvx_order_journey_tracer_cancel_race_loss_rate(
                              static_cast<LrvxOrderJourneyTracerHandle>(getHandle(c, a[0]))));
}

static JSValue js_ojt_row_to_obj(JSContext* c, const LrvxOrderTraceRow& r)
{
  JSValue o = JS_NewObject(c);
  JS_SetPropertyStr(c, o, "orderId", JS_NewInt64(c, static_cast<int64_t>(r.order_id)));
  JS_SetPropertyStr(c, o, "seq", JS_NewUint32(c, r.seq));
  JS_SetPropertyStr(c, o, "status", JS_NewUint32(c, r.status));
  JS_SetPropertyStr(c, o, "isMaker", JS_NewBool(c, r.is_maker != 0));
  JS_SetPropertyStr(c, o, "tsNs", JS_NewBigInt64(c, r.ts_ns));
  JS_SetPropertyStr(c, o, "fillQty",
                    JS_NewFloat64(c, static_cast<double>(r.fill_qty_raw) / 1e8));
  JS_SetPropertyStr(c, o, "fillPrice",
                    JS_NewFloat64(c, static_cast<double>(r.fill_price_raw) / 1e8));
  JS_SetPropertyStr(c, o, "queueAhead",
                    JS_NewFloat64(c, static_cast<double>(r.queue_ahead_raw) / 1e8));
  JS_SetPropertyStr(c, o, "queueTotal",
                    JS_NewFloat64(c, static_cast<double>(r.queue_total_raw) / 1e8));
  JS_SetPropertyStr(c, o, "submittedAtNs", JS_NewBigInt64(c, r.submitted_at_ns));
  JS_SetPropertyStr(c, o, "acceptedAtNs", JS_NewBigInt64(c, r.accepted_at_ns));
  JS_SetPropertyStr(c, o, "firstFillAtNs", JS_NewBigInt64(c, r.first_fill_at_ns));
  JS_SetPropertyStr(c, o, "lastFillAtNs", JS_NewBigInt64(c, r.last_fill_at_ns));
  JS_SetPropertyStr(c, o, "canceledAtNs", JS_NewBigInt64(c, r.canceled_at_ns));
  JS_SetPropertyStr(c, o, "rejectedAtNs", JS_NewBigInt64(c, r.rejected_at_ns));
  JS_SetPropertyStr(c, o, "triggeredAtNs", JS_NewBigInt64(c, r.triggered_at_ns));
  JS_SetPropertyStr(c, o, "expiredAtNs", JS_NewBigInt64(c, r.expired_at_ns));
  return o;
}

static JSValue js_ojt_result(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto h = static_cast<LrvxOrderJourneyTracerHandle>(getHandle(c, a[0]));
  const uint64_t n = lrvx_order_journey_tracer_result(h, nullptr, 0);
  std::vector<LrvxOrderTraceRow> buf(n);
  if (n > 0)
  {
    lrvx_order_journey_tracer_result(h, buf.data(), n);
  }
  JSValue arr = JS_NewArray(c);
  for (uint64_t i = 0; i < n; ++i)
  {
    JS_SetPropertyUint32(c, arr, static_cast<uint32_t>(i), js_ojt_row_to_obj(c, buf[i]));
  }
  return arr;
}

static JSValue js_ojt_journey(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto h = static_cast<LrvxOrderJourneyTracerHandle>(getHandle(c, a[0]));
  uint64_t orderId = static_cast<uint64_t>(toInt64(c, a[1]));
  const uint64_t n = lrvx_order_journey_tracer_journey(h, orderId, nullptr, 0);
  std::vector<LrvxOrderTraceRow> buf(n);
  if (n > 0)
  {
    lrvx_order_journey_tracer_journey(h, orderId, buf.data(), n);
  }
  JSValue arr = JS_NewArray(c);
  for (uint64_t i = 0; i < n; ++i)
  {
    JS_SetPropertyUint32(c, arr, static_cast<uint32_t>(i), js_ojt_row_to_obj(c, buf[i]));
  }
  return arr;
}

static JSValue js_ojt_clear(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  lrvx_order_journey_tracer_clear(
      static_cast<LrvxOrderJourneyTracerHandle>(getHandle(c, a[0])));
  return JS_UNDEFINED;
}

// ============================================================
// PositionGroupTracker
// ============================================================

static JSValue js_pg_create(JSContext* c, JSValueConst, int, JSValueConst*)
{
  return createHandleObject(c, lrvx_position_group_create(), lrvx_position_group_destroy);
}

static JSValue js_pg_destroy(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return js_generic_handle_destroy(c, a[0]);
}

static JSValue js_pg_open(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewFloat64(c, static_cast<double>(lrvx_position_group_open(
                              static_cast<LrvxPositionGroupHandle>(getHandle(c, a[0])),
                              static_cast<uint64_t>(toInt64(c, a[1])), toUint32(c, a[2]),
                              static_cast<uint8_t>(toUint32(c, a[3])), toDouble(c, a[4]), toDouble(c, a[5]))));
}

static JSValue js_pg_close(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  lrvx_position_group_close(static_cast<LrvxPositionGroupHandle>(getHandle(c, a[0])),
                            static_cast<uint64_t>(toInt64(c, a[1])), toDouble(c, a[2]));
  return JS_UNDEFINED;
}

static JSValue js_pg_partial_close(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  lrvx_position_group_partial_close(static_cast<LrvxPositionGroupHandle>(getHandle(c, a[0])),
                                    static_cast<uint64_t>(toInt64(c, a[1])), toDouble(c, a[2]), toDouble(c, a[3]));
  return JS_UNDEFINED;
}

static JSValue js_pg_net(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewFloat64(c, lrvx_position_group_net_position(
                              static_cast<LrvxPositionGroupHandle>(getHandle(c, a[0])), toUint32(c, a[1])));
}

static JSValue js_pg_pnl(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewFloat64(c, lrvx_position_group_realized_pnl(
                              static_cast<LrvxPositionGroupHandle>(getHandle(c, a[0])), toUint32(c, a[1])));
}

static JSValue js_pg_total_pnl(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewFloat64(c, lrvx_position_group_total_pnl(
                              static_cast<LrvxPositionGroupHandle>(getHandle(c, a[0]))));
}

static JSValue js_pg_open_count(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewUint32(c, lrvx_position_group_open_count(
                             static_cast<LrvxPositionGroupHandle>(getHandle(c, a[0])), toUint32(c, a[1])));
}

static JSValue js_pg_prune(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  lrvx_position_group_prune(static_cast<LrvxPositionGroupHandle>(getHandle(c, a[0])));
  return JS_UNDEFINED;
}

// ============================================================
// DataWriter
// ============================================================

static JSValue js_dw_create(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  LRVX_JS_CSTRING_OR_THROW(dir, c, a[0]);
  // Megabytes. Zero is not a size: a writer with a 0-byte limit rolls to a
  // new segment before every event and leaves an empty first one, so 0 means
  // the default the other bindings use, as leaving the argument out does.
  constexpr uint64_t kDefaultSegmentMb = 256;
  uint64_t mb = (JS_IsUndefined(a[1]) || JS_IsNull(a[1])) ? kDefaultSegmentMb : static_cast<uint64_t>(toInt64(c, a[1]));
  if (mb == 0)
  {
    mb = kDefaultSegmentMb;
  }
  uint8_t eid = (JS_IsUndefined(a[2]) || JS_IsNull(a[2])) ? 0 : static_cast<uint8_t>(toUint32(c, a[2]));
  JSValue ret = createHandleObject(c, lrvx_data_writer_create(dir, mb, eid), lrvx_data_writer_destroy);
  return ret;
}

static JSValue js_dw_destroy(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return js_generic_handle_destroy(c, a[0]);
}

static JSValue js_dw_write_trade(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewBool(c, lrvx_data_writer_write_trade(
                           static_cast<LrvxDataWriterHandle>(getHandle(c, a[0])),
                           toInt64(c, a[1]), toInt64(c, a[2]), toDouble(c, a[3]), toDouble(c, a[4]),
                           static_cast<uint64_t>(toInt64(c, a[5])), toUint32(c, a[6]),
                           static_cast<uint8_t>(toUint32(c, a[7]))));
}

static JSValue js_dw_flush(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  lrvx_data_writer_flush(static_cast<LrvxDataWriterHandle>(getHandle(c, a[0])));
  return JS_UNDEFINED;
}

static JSValue js_dw_close(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  lrvx_data_writer_close(static_cast<LrvxDataWriterHandle>(getHandle(c, a[0])));
  return JS_UNDEFINED;
}

static JSValue js_dw_stats(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  LrvxWriterStats s = lrvx_data_writer_stats(static_cast<LrvxDataWriterHandle>(getHandle(c, a[0])));
  JSValue o = JS_NewObject(c);
  JS_SetPropertyStr(c, o, "bytesWritten", JS_NewInt64(c, static_cast<int64_t>(s.bytes_written)));
  JS_SetPropertyStr(c, o, "eventsWritten", JS_NewInt64(c, static_cast<int64_t>(s.events_written)));
  JS_SetPropertyStr(c, o, "segmentsCreated", JS_NewInt64(c, static_cast<int64_t>(s.segments_created)));
  JS_SetPropertyStr(c, o, "tradesWritten", JS_NewInt64(c, static_cast<int64_t>(s.trades_written)));
  return o;
}

// Extract raw int64 [price,qty,...] pairs from a BigInt64Array (or ArrayBuffer) into a
// vector<LrvxBookLevel>. Returns true on success; throws on type/size error.
static bool extractBookLevels(JSContext* c, JSValueConst arr, uint32_t expectedPairs,
                              std::vector<LrvxBookLevel>& out)
{
  if (expectedPairs == 0)
  {
    out.clear();
    return true;
  }
  if (JS_IsUndefined(arr) || JS_IsNull(arr))
  {
    JS_ThrowTypeError(c, "writeBook: levels buffer is null but count > 0");
    return false;
  }
  size_t byteOffset = 0;
  size_t byteLength = 0;
  size_t bytesPerElement = 0;
  JSValue ab = JS_GetTypedArrayBuffer(c, arr, &byteOffset, &byteLength, &bytesPerElement);
  uint8_t* base = nullptr;
  size_t totalSize = 0;
  if (!JS_IsException(ab))
  {
    base = JS_GetArrayBuffer(c, &totalSize, ab);
    JS_FreeValue(c, ab);
  }
  else
  {
    // Maybe a plain ArrayBuffer.
    JS_GetException(c);  // clear pending
    base = JS_GetArrayBuffer(c, &totalSize, arr);
    byteOffset = 0;
    byteLength = totalSize;
    bytesPerElement = 8;
  }
  if (!base)
  {
    JS_ThrowTypeError(c, "writeBook: expected BigInt64Array or ArrayBuffer");
    return false;
  }
  if (bytesPerElement != 0 && bytesPerElement != 8)
  {
    JS_ThrowTypeError(c, "writeBook: typed array must be BigInt64Array (8-byte elements)");
    return false;
  }
  size_t needed = static_cast<size_t>(expectedPairs) * 2 * 8;
  if (byteLength < needed)
  {
    JS_ThrowRangeError(c, "writeBook: buffer too small for declared level count");
    return false;
  }
  const int64_t* p = reinterpret_cast<const int64_t*>(base + byteOffset);
  out.resize(expectedPairs);
  for (uint32_t i = 0; i < expectedPairs; i++)
  {
    out[i].price_raw = p[i * 2];
    out[i].quantity_raw = p[i * 2 + 1];
  }
  return true;
}

static JSValue js_dw_write_book(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto h = static_cast<LrvxDataWriterHandle>(getHandle(c, a[0]));
  int64_t exchangeTsNs = toInt64(c, a[1]);
  int64_t recvTsNs = toInt64(c, a[2]);
  int64_t seq = toInt64(c, a[3]);
  uint32_t symbolId = toUint32(c, a[4]);
  uint8_t isSnapshot = static_cast<uint8_t>(toUint32(c, a[5]));
  uint32_t nBids = toUint32(c, a[7]);
  uint32_t nAsks = toUint32(c, a[9]);

  std::vector<LrvxBookLevel> bids;
  std::vector<LrvxBookLevel> asks;
  if (!extractBookLevels(c, a[6], nBids, bids))
  {
    return JS_EXCEPTION;
  }
  if (!extractBookLevels(c, a[8], nAsks, asks))
  {
    return JS_EXCEPTION;
  }
  uint8_t ok = lrvx_data_writer_write_book(h, exchangeTsNs, recvTsNs, seq, symbolId, isSnapshot,
                                           bids.empty() ? nullptr : bids.data(), nBids,
                                           asks.empty() ? nullptr : asks.data(), nAsks);
  return JS_NewBool(c, ok);
}

// ============================================================
// BinaryLogRecorderHook
// ============================================================

static JSValue js_blrh_create(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  LRVX_JS_CSTRING_OR_THROW(dir, c, a[0]);
  uint64_t mb = (argc < 2 || JS_IsUndefined(a[1]) || JS_IsNull(a[1]))
                    ? 256
                    : static_cast<uint64_t>(toInt64(c, a[1]));
  uint8_t eid = (argc < 3 || JS_IsUndefined(a[2]) || JS_IsNull(a[2]))
                    ? 0
                    : static_cast<uint8_t>(toUint32(c, a[2]));
  uint8_t comp = (argc < 4 || JS_IsUndefined(a[3]) || JS_IsNull(a[3]))
                     ? 0
                     : static_cast<uint8_t>(toUint32(c, a[3]));
  // Optional a[4] = exchange_name, a[5] = instrument_type. Both feed the
  // RecordingMetadata stamp the merged-tape reader keys on.
  lrvx::JsCString exch = lrvx::jsOptionalCString(c, argc, a, 4);
  lrvx::JsCString itype = lrvx::jsOptionalCString(c, argc, a, 5);
  if ((exch.present() && !exch.ok()) || (itype.present() && !itype.ok()))
  {
    return JS_EXCEPTION;
  }
  return createHandleObject(
      c, lrvx_binary_log_recorder_hook_create_ex(dir, mb, eid, comp, exch, itype),
      lrvx_binary_log_recorder_hook_destroy);
}

// ── Recorder-handle drivers (used by smoke tests + bindings that want
//    to push events directly through a BinaryLogRecorderHook without
//    spinning up a Runner). The recorder handle's first member is the
//    callbacks struct, so the cast is layout-stable.
static JSValue js_recorder_on_start(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto* cb = static_cast<LrvxMarketDataRecorderCallbacks*>(getHandle(c, a[0]));
  if (cb && cb->on_start)
  {
    cb->on_start(cb->user_data);
  }
  return JS_UNDEFINED;
}

static JSValue js_recorder_on_stop(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto* cb = static_cast<LrvxMarketDataRecorderCallbacks*>(getHandle(c, a[0]));
  if (cb && cb->on_stop)
  {
    cb->on_stop(cb->user_data);
  }
  return JS_UNDEFINED;
}

static JSValue js_recorder_on_trade(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  // (handle, symbolId, price, qty, isBuy, exchangeTsNs)
  auto* cb = static_cast<LrvxMarketDataRecorderCallbacks*>(getHandle(c, a[0]));
  if (!cb || !cb->on_trade)
  {
    return JS_UNDEFINED;
  }
  LrvxTradeData td{};
  td.symbol = toUint32(c, a[1]);
  double px = toDouble(c, a[2]);
  double qty = toDouble(c, a[3]);
  td.price_raw = static_cast<int64_t>(std::llround(px * 1e8));
  td.quantity_raw = static_cast<int64_t>(std::llround(qty * 1e8));
  td.is_buy = JS_ToBool(c, a[4]) ? 1 : 0;
  td.exchange_ts_ns = toInt64(c, a[5]);
  cb->on_trade(cb->user_data, &td);
  return JS_UNDEFINED;
}

static JSValue js_blrh_destroy(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return js_generic_handle_destroy(c, a[0]);
}

static JSValue js_blrh_as_recorder(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto rec = lrvx_binary_log_recorder_hook_as_recorder(
      static_cast<LrvxBinaryLogRecorderHookHandle>(getHandle(c, a[0])));
  // Borrowed: this is &hook->recorder_view, a member of the hook object
  // itself, not a separate allocation -- destroying it would free part of
  // the hook out from under it. Its lifetime is the hook's.
  return createHandleObject(c, rec, nullptr);
}

static JSValue js_blrh_add_symbol(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  auto h = static_cast<LrvxBinaryLogRecorderHookHandle>(getHandle(c, a[0]));
  uint32_t symbolId = toUint32(c, a[1]);
  LRVX_JS_CSTRING_OR_THROW(name, c, a[2]);
  lrvx::JsCString base = lrvx::jsOptionalCString(c, argc, a, 3);
  lrvx::JsCString quote = lrvx::jsOptionalCString(c, argc, a, 4);
  if ((base.present() && !base.ok()) || (quote.present() && !quote.ok()))
  {
    return JS_EXCEPTION;
  }
  int8_t pp = argc > 5 ? static_cast<int8_t>(toInt64(c, a[5])) : 8;
  int8_t qp = argc > 6 ? static_cast<int8_t>(toInt64(c, a[6])) : 8;
  lrvx_binary_log_recorder_hook_add_symbol(h, symbolId, name.get(), base.or_else(""),
                                           quote.or_else(""), pp, qp);
  return JS_UNDEFINED;
}

static JSValue js_blrh_flush(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  lrvx_binary_log_recorder_hook_flush(
      static_cast<LrvxBinaryLogRecorderHookHandle>(getHandle(c, a[0])));
  return JS_UNDEFINED;
}

static JSValue js_blrh_stats(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  LrvxWriterStats s = lrvx_binary_log_recorder_hook_stats(
      static_cast<LrvxBinaryLogRecorderHookHandle>(getHandle(c, a[0])));
  JSValue o = JS_NewObject(c);
  JS_SetPropertyStr(c, o, "tradesWritten", JS_NewInt64(c, static_cast<int64_t>(s.trades_written)));
  JS_SetPropertyStr(c, o, "bookUpdatesWritten",
                    JS_NewInt64(c, static_cast<int64_t>(s.events_written - s.trades_written)));
  JS_SetPropertyStr(c, o, "bytesWritten", JS_NewInt64(c, static_cast<int64_t>(s.bytes_written)));
  JS_SetPropertyStr(c, o, "segmentsCreated",
                    JS_NewInt64(c, static_cast<int64_t>(s.segments_created)));
  return o;
}

// ============================================================
// DataReader
// ============================================================

static JSValue js_dr_create(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  LRVX_JS_CSTRING_OR_THROW(dir, c, a[0]);
  JSValue ret = createHandleObject(c, lrvx_data_reader_create(dir), lrvx_data_reader_destroy);
  return ret;
}

static JSValue js_dr_create_filtered(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  LRVX_JS_CSTRING_OR_THROW(dir, c, a[0]);
  int64_t from_ns = toInt64(c, a[1]);
  int64_t to_ns = toInt64(c, a[2]);
  // a[3] optional: array of symbol ids
  std::vector<uint32_t> syms;
  if (argc > 3 && JS_IsArray(c, a[3]))
  {
    JSValue lenVal = JS_GetPropertyStr(c, a[3], "length");
    uint32_t n = 0;
    JS_ToUint32(c, &n, lenVal);
    JS_FreeValue(c, lenVal);
    for (uint32_t i = 0; i < n; i++)
    {
      JSValue e = JS_GetPropertyUint32(c, a[3], i);
      syms.push_back(toUint32(c, e));
      JS_FreeValue(c, e);
    }
  }
  JSValue ret = createHandleObject(
      c,
      lrvx_data_reader_create_filtered(dir, from_ns, to_ns, syms.empty() ? nullptr : syms.data(),
                                       static_cast<uint32_t>(syms.size())),
      lrvx_data_reader_destroy);
  return ret;
}

// (dir, fromNs, toNs, symbols, reorderWindowNs, strictOrdering,
//  orderSegmentsByTime, skipSegmentsByTime). The prelude's DataReader passes
// every argument; the two segment switches default to on.
static JSValue js_dr_create_ex(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  LRVX_JS_CSTRING_OR_THROW(dir, c, a[0]);
  int64_t from_ns = toInt64(c, a[1]);
  int64_t to_ns = toInt64(c, a[2]);
  std::vector<uint32_t> syms;
  if (argc > 3 && JS_IsArray(c, a[3]))
  {
    JSValue lenVal = JS_GetPropertyStr(c, a[3], "length");
    uint32_t n = 0;
    JS_ToUint32(c, &n, lenVal);
    JS_FreeValue(c, lenVal);
    for (uint32_t i = 0; i < n; i++)
    {
      JSValue e = JS_GetPropertyUint32(c, a[3], i);
      syms.push_back(toUint32(c, e));
      JS_FreeValue(c, e);
    }
  }
  const int64_t reorder_window_ns = argc > 4 ? toInt64(c, a[4]) : 0;
  auto flag = [&](int i, bool fallback)
  {
    if (argc <= i || JS_IsUndefined(a[i]) || JS_IsNull(a[i]))
    {
      return fallback ? 1 : 0;
    }
    return JS_ToBool(c, a[i]) > 0 ? 1 : 0;
  };
  JSValue ret = createHandleObject(
      c,
      lrvx_data_reader_create_ex(dir, from_ns, to_ns, syms.empty() ? nullptr : syms.data(),
                                 static_cast<uint32_t>(syms.size()), reorder_window_ns,
                                 flag(5, false), flag(6, true), flag(7, true)),
      lrvx_data_reader_destroy);
  return ret;
}

static JSValue js_dr_destroy(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return js_generic_handle_destroy(c, a[0]);
}

static JSValue js_dr_count(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewInt64(c, static_cast<int64_t>(lrvx_data_reader_count(
                            static_cast<LrvxDataReaderHandle>(getHandle(c, a[0])))));
}

static JSValue js_dr_summary(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  LrvxDatasetSummary s = lrvx_data_reader_summary(static_cast<LrvxDataReaderHandle>(getHandle(c, a[0])));
  JSValue o = JS_NewObject(c);
  JS_SetPropertyStr(c, o, "firstEventNs", JS_NewBigInt64(c, s.first_event_ns));
  JS_SetPropertyStr(c, o, "lastEventNs", JS_NewBigInt64(c, s.last_event_ns));
  JS_SetPropertyStr(c, o, "totalEvents", JS_NewInt64(c, static_cast<int64_t>(s.total_events)));
  JS_SetPropertyStr(c, o, "segmentCount", JS_NewUint32(c, s.segment_count));
  JS_SetPropertyStr(c, o, "totalBytes", JS_NewInt64(c, static_cast<int64_t>(s.total_bytes)));
  JS_SetPropertyStr(c, o, "durationSeconds", JS_NewFloat64(c, s.duration_seconds));
  return o;
}

static JSValue js_dr_stats(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  LrvxReaderStats s = lrvx_data_reader_stats(static_cast<LrvxDataReaderHandle>(getHandle(c, a[0])));
  JSValue o = JS_NewObject(c);
  JS_SetPropertyStr(c, o, "filesRead", JS_NewInt64(c, static_cast<int64_t>(s.files_read)));
  JS_SetPropertyStr(c, o, "eventsRead", JS_NewInt64(c, static_cast<int64_t>(s.events_read)));
  JS_SetPropertyStr(c, o, "tradesRead", JS_NewInt64(c, static_cast<int64_t>(s.trades_read)));
  JS_SetPropertyStr(c, o, "bookUpdatesRead", JS_NewInt64(c, static_cast<int64_t>(s.book_updates_read)));
  JS_SetPropertyStr(c, o, "bytesRead", JS_NewInt64(c, static_cast<int64_t>(s.bytes_read)));
  JS_SetPropertyStr(c, o, "crcErrors", JS_NewInt64(c, static_cast<int64_t>(s.crc_errors)));
  JS_SetPropertyStr(c, o, "lateDropped", JS_NewInt64(c, static_cast<int64_t>(s.late_dropped)));
  JS_SetPropertyStr(c, o, "unknownFramesSkipped",
                    JS_NewInt64(c, static_cast<int64_t>(s.unknown_frames_skipped)));
  JS_SetPropertyStr(c, o, "segmentsSkipped",
                    JS_NewInt64(c, static_cast<int64_t>(s.segments_skipped)));
  JS_SetPropertyStr(c, o, "segmentsReordered",
                    JS_NewInt64(c, static_cast<int64_t>(s.segments_reordered)));
  JS_SetPropertyStr(c, o, "segmentsInterleaved",
                    JS_NewInt64(c, static_cast<int64_t>(s.segments_interleaved)));
  JS_SetPropertyStr(c, o, "segmentsTruncated",
                    JS_NewInt64(c, static_cast<int64_t>(s.segments_truncated)));
  return o;
}

// ── helpers shared by read_X / read_X_from ───────────────────────────────

static JSValue js_dr_build_trades_array(JSContext* c, const std::vector<LrvxTradeRecord>& trades,
                                        uint64_t got)
{
  JSValue arr = JS_NewArray(c);
  for (uint64_t i = 0; i < got; i++)
  {
    const auto& t = trades[i];
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "exchangeTsNs", JS_NewBigInt64(c, t.exchange_ts_ns));
    JS_SetPropertyStr(c, o, "recvTsNs", JS_NewBigInt64(c, t.recv_ts_ns));
    JS_SetPropertyStr(c, o, "price", JS_NewFloat64(c, static_cast<double>(t.price_raw) / 1e8));
    JS_SetPropertyStr(c, o, "qty", JS_NewFloat64(c, static_cast<double>(t.qty_raw) / 1e8));
    JS_SetPropertyStr(c, o, "tradeId", JS_NewInt64(c, static_cast<int64_t>(t.trade_id)));
    JS_SetPropertyStr(c, o, "symbolId", JS_NewUint32(c, t.symbol_id));
    JS_SetPropertyStr(c, o, "side", JS_NewString(c, t.side == 0 ? "buy" : "sell"));
    JS_SetPropertyUint32(c, arr, static_cast<uint32_t>(i), o);
  }
  return arr;
}

static JSValue js_dr_build_bbos_array(JSContext* c, const std::vector<LrvxBBO>& bbos, uint64_t got)
{
  JSValue arr = JS_NewArray(c);
  for (uint64_t i = 0; i < got; i++)
  {
    const auto& b = bbos[i];
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "exchangeTsNs", JS_NewBigInt64(c, b.exchange_ts_ns));
    JS_SetPropertyStr(c, o, "recvTsNs", JS_NewBigInt64(c, b.recv_ts_ns));
    JS_SetPropertyStr(c, o, "seq", JS_NewInt64(c, b.seq));
    JS_SetPropertyStr(c, o, "symbolId", JS_NewUint32(c, b.symbol_id));
    JS_SetPropertyStr(c, o, "eventType", JS_NewUint32(c, b.event_type));
    JS_SetPropertyStr(c, o, "bidPrice",
                      JS_NewFloat64(c, static_cast<double>(b.bid_price_raw) / 1e8));
    JS_SetPropertyStr(c, o, "bidQty",
                      JS_NewFloat64(c, static_cast<double>(b.bid_qty_raw) / 1e8));
    JS_SetPropertyStr(c, o, "askPrice",
                      JS_NewFloat64(c, static_cast<double>(b.ask_price_raw) / 1e8));
    JS_SetPropertyStr(c, o, "askQty",
                      JS_NewFloat64(c, static_cast<double>(b.ask_qty_raw) / 1e8));
    JS_SetPropertyUint32(c, arr, static_cast<uint32_t>(i), o);
  }
  return arr;
}

static JSValue js_dr_read_trades(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  auto h = static_cast<LrvxDataReaderHandle>(getHandle(c, a[0]));
  uint64_t max = argc > 1 ? static_cast<uint64_t>(toInt64(c, a[1])) : 0;
  uint64_t n = lrvx_data_reader_count(h);
  if (max > 0 && max < n)
  {
    n = max;
  }
  if (n == 0)
  {
    return JS_NewArray(c);
  }
  std::vector<LrvxTradeRecord> trades(n);
  uint64_t got = lrvx_data_reader_read_trades(h, trades.data(), n);
  return js_dr_build_trades_array(c, trades, got);
}

static JSValue js_dr_read_trades_from(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  auto h = static_cast<LrvxDataReaderHandle>(getHandle(c, a[0]));
  int64_t startTsNs = toInt64(c, a[1]);
  uint64_t max = argc > 2 ? static_cast<uint64_t>(toInt64(c, a[2])) : 0;
  uint64_t n = lrvx_data_reader_read_trades_from(h, startTsNs, nullptr, 0);
  if (max > 0 && max < n)
  {
    n = max;
  }
  if (n == 0)
  {
    return JS_NewArray(c);
  }
  std::vector<LrvxTradeRecord> trades(n);
  uint64_t got = lrvx_data_reader_read_trades_from(h, startTsNs, trades.data(), n);
  return js_dr_build_trades_array(c, trades, got);
}

static JSValue js_dr_read_bbo(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  auto h = static_cast<LrvxDataReaderHandle>(getHandle(c, a[0]));
  uint64_t max = argc > 1 ? static_cast<uint64_t>(toInt64(c, a[1])) : 0;
  uint64_t n = lrvx_data_reader_read_bbo(h, nullptr, 0);
  if (max > 0 && max < n)
  {
    n = max;
  }
  if (n == 0)
  {
    return JS_NewArray(c);
  }
  std::vector<LrvxBBO> bbos(n);
  uint64_t got = lrvx_data_reader_read_bbo(h, bbos.data(), n);
  return js_dr_build_bbos_array(c, bbos, got);
}

static JSValue js_dr_read_bbo_from(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  auto h = static_cast<LrvxDataReaderHandle>(getHandle(c, a[0]));
  int64_t startTsNs = toInt64(c, a[1]);
  uint64_t max = argc > 2 ? static_cast<uint64_t>(toInt64(c, a[2])) : 0;
  uint64_t n = lrvx_data_reader_read_bbo_from(h, startTsNs, nullptr, 0);
  if (max > 0 && max < n)
  {
    n = max;
  }
  if (n == 0)
  {
    return JS_NewArray(c);
  }
  std::vector<LrvxBBO> bbos(n);
  uint64_t got = lrvx_data_reader_read_bbo_from(h, startTsNs, bbos.data(), n);
  return js_dr_build_bbos_array(c, bbos, got);
}

static JSValue js_dr_build_book_updates_array(JSContext* c,
                                              const std::vector<LrvxBookUpdateHeader>& headers,
                                              const std::vector<LrvxLevel>& levels, uint64_t got)
{
  JSValue arr = JS_NewArray(c);
  for (uint64_t i = 0; i < got; i++)
  {
    const auto& hdr = headers[i];
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "exchangeTsNs", JS_NewBigInt64(c, hdr.exchange_ts_ns));
    JS_SetPropertyStr(c, o, "recvTsNs", JS_NewBigInt64(c, hdr.recv_ts_ns));
    JS_SetPropertyStr(c, o, "seq", JS_NewInt64(c, hdr.seq));
    JS_SetPropertyStr(c, o, "symbolId", JS_NewUint32(c, hdr.symbol_id));
    JS_SetPropertyStr(c, o, "eventType", JS_NewUint32(c, hdr.event_type));

    JSValue bids = JS_NewArray(c);
    for (uint16_t k = 0; k < hdr.bid_count; k++)
    {
      const auto& l = levels[hdr.level_offset + k];
      JSValue lo = JS_NewObject(c);
      JS_SetPropertyStr(c, lo, "price", JS_NewFloat64(c, static_cast<double>(l.price_raw) / 1e8));
      JS_SetPropertyStr(c, lo, "qty", JS_NewFloat64(c, static_cast<double>(l.qty_raw) / 1e8));
      JS_SetPropertyUint32(c, bids, k, lo);
    }
    JS_SetPropertyStr(c, o, "bids", bids);

    JSValue asks = JS_NewArray(c);
    for (uint16_t k = 0; k < hdr.ask_count; k++)
    {
      const auto& l = levels[hdr.level_offset + hdr.bid_count + k];
      JSValue lo = JS_NewObject(c);
      JS_SetPropertyStr(c, lo, "price", JS_NewFloat64(c, static_cast<double>(l.price_raw) / 1e8));
      JS_SetPropertyStr(c, lo, "qty", JS_NewFloat64(c, static_cast<double>(l.qty_raw) / 1e8));
      JS_SetPropertyUint32(c, asks, k, lo);
    }
    JS_SetPropertyStr(c, o, "asks", asks);

    JS_SetPropertyUint32(c, arr, static_cast<uint32_t>(i), o);
  }
  return arr;
}

static JSValue js_dr_read_book_updates(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto h = static_cast<LrvxDataReaderHandle>(getHandle(c, a[0]));
  uint64_t total_levels = 0;
  uint64_t n_events = lrvx_data_reader_count_book_updates(h, &total_levels);
  if (n_events == 0)
  {
    return JS_NewArray(c);
  }
  std::vector<LrvxBookUpdateHeader> headers(n_events);
  std::vector<LrvxLevel> levels(total_levels);
  uint64_t got = lrvx_data_reader_read_book_updates(h, headers.data(), n_events, levels.data(),
                                                    total_levels);
  return js_dr_build_book_updates_array(c, headers, levels, got);
}

static JSValue js_dr_read_book_updates_from(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto h = static_cast<LrvxDataReaderHandle>(getHandle(c, a[0]));
  int64_t startTsNs = toInt64(c, a[1]);
  uint64_t total_levels = 0;
  uint64_t n_events = lrvx_data_reader_count_book_updates_from(h, startTsNs, &total_levels);
  if (n_events == 0)
  {
    return JS_NewArray(c);
  }
  std::vector<LrvxBookUpdateHeader> headers(n_events);
  std::vector<LrvxLevel> levels(total_levels);
  uint64_t got = lrvx_data_reader_read_book_updates_from(
      h, startTsNs, headers.data(), n_events, levels.data(), total_levels);
  return js_dr_build_book_updates_array(c, headers, levels, got);
}

// ============================================================
// MergedTapeReader — N-tape merged consumption
// ============================================================

static JSValue js_mtr_create(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  // a[0]: array of paths (string[])
  // a[1]: fromNs (int64, -1 = none)
  // a[2]: toNs   (int64, -1 = none)
  // a[3]: symbol filter (uint32[], optional)
  // The paths are copied out of QuickJS rather than borrowed: the C call
  // below wants a stable array of pointers, and a converted string only
  // lives as long as the holder that owns it.
  std::vector<std::string> path_owned;
  std::vector<const char*> path_cstrs;
  if (JS_IsArray(c, a[0]))
  {
    JSValue lenVal = JS_GetPropertyStr(c, a[0], "length");
    uint32_t n = 0;
    JS_ToUint32(c, &n, lenVal);
    JS_FreeValue(c, lenVal);
    path_owned.reserve(n);
    for (uint32_t i = 0; i < n; i++)
    {
      JSValue e = JS_GetPropertyUint32(c, a[0], i);
      lrvx::JsCString s(c, e);
      if (!s.ok())
      {
        JS_FreeValue(c, e);
        return JS_EXCEPTION;
      }
      path_owned.emplace_back(s.get());
      JS_FreeValue(c, e);
    }
    path_cstrs.reserve(path_owned.size());
    for (const auto& p : path_owned)
    {
      path_cstrs.push_back(p.c_str());
    }
  }
  int64_t from_ns = argc > 1 ? toInt64(c, a[1]) : -1;
  int64_t to_ns = argc > 2 ? toInt64(c, a[2]) : -1;

  std::vector<uint32_t> syms;
  if (argc > 3 && JS_IsArray(c, a[3]))
  {
    JSValue lenVal = JS_GetPropertyStr(c, a[3], "length");
    uint32_t n = 0;
    JS_ToUint32(c, &n, lenVal);
    JS_FreeValue(c, lenVal);
    syms.reserve(n);
    for (uint32_t i = 0; i < n; i++)
    {
      JSValue e = JS_GetPropertyUint32(c, a[3], i);
      syms.push_back(toUint32(c, e));
      JS_FreeValue(c, e);
    }
  }

  auto handle = lrvx_merged_tape_reader_create(
      path_cstrs.empty() ? nullptr : path_cstrs.data(),
      static_cast<uint32_t>(path_cstrs.size()), from_ns, to_ns,
      syms.empty() ? nullptr : syms.data(), static_cast<uint32_t>(syms.size()));
  if (!handle)
  {
    return JS_ThrowTypeError(c, "%s", lrvx_last_error_message());
  }

  return createHandleObject(c, handle, lrvx_merged_tape_reader_destroy);
}

static JSValue js_mtr_destroy(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return js_generic_handle_destroy(c, a[0]);
}

static JSValue js_mtr_symbol_count(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewUint32(c, lrvx_merged_tape_reader_symbol_count(
                             static_cast<LrvxMergedTapeReaderHandle>(getHandle(c, a[0]))));
}

static JSValue js_mtr_get_symbols(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto h = static_cast<LrvxMergedTapeReaderHandle>(getHandle(c, a[0]));
  uint32_t n = lrvx_merged_tape_reader_symbol_count(h);
  if (n == 0)
  {
    return JS_NewArray(c);
  }
  std::vector<LrvxMergedSymbol> syms(n);
  uint32_t got = lrvx_merged_tape_reader_get_symbols(h, syms.data(), n);
  JSValue arr = JS_NewArray(c);
  for (uint32_t i = 0; i < got; i++)
  {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "globalId", JS_NewUint32(c, syms[i].global_id));
    JS_SetPropertyStr(c, o, "pricePrecision",
                      JS_NewInt32(c, static_cast<int32_t>(syms[i].price_precision)));
    JS_SetPropertyStr(c, o, "qtyPrecision",
                      JS_NewInt32(c, static_cast<int32_t>(syms[i].qty_precision)));
    JS_SetPropertyStr(c, o, "exchange", JS_NewString(c, syms[i].exchange ? syms[i].exchange : ""));
    JS_SetPropertyStr(c, o, "name", JS_NewString(c, syms[i].name ? syms[i].name : ""));
    JS_SetPropertyUint32(c, arr, i, o);
  }
  return arr;
}

static JSValue js_mtr_tape_count(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewUint32(c, lrvx_merged_tape_reader_tape_count(
                             static_cast<LrvxMergedTapeReaderHandle>(getHandle(c, a[0]))));
}

static JSValue js_mtr_get_tape_stats(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto h = static_cast<LrvxMergedTapeReaderHandle>(getHandle(c, a[0]));
  uint32_t n = lrvx_merged_tape_reader_tape_count(h);
  if (n == 0)
  {
    return JS_NewArray(c);
  }
  std::vector<LrvxMergedTapeStats> stats(n);
  uint32_t got = lrvx_merged_tape_reader_get_tape_stats(h, stats.data(), n);
  JSValue arr = JS_NewArray(c);
  for (uint32_t i = 0; i < got; i++)
  {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "firstEventNs", JS_NewBigInt64(c, stats[i].first_event_ns));
    JS_SetPropertyStr(c, o, "lastEventNs", JS_NewBigInt64(c, stats[i].last_event_ns));
    JS_SetPropertyStr(c, o, "trades",
                      JS_NewBigInt64(c, static_cast<int64_t>(stats[i].trades)));
    JS_SetPropertyStr(c, o, "books", JS_NewBigInt64(c, static_cast<int64_t>(stats[i].books)));
    JS_SetPropertyStr(c, o, "path", JS_NewString(c, stats[i].path ? stats[i].path : ""));
    JS_SetPropertyUint32(c, arr, i, o);
  }
  return arr;
}

static JSValue js_mtr_time_range(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto h = static_cast<LrvxMergedTapeReaderHandle>(getHandle(c, a[0]));
  int64_t lo = 0, hi = 0;
  lrvx_merged_tape_reader_time_range(h, &lo, &hi);
  JSValue o = JS_NewObject(c);
  JS_SetPropertyStr(c, o, "minFirstNs", JS_NewBigInt64(c, lo));
  JS_SetPropertyStr(c, o, "maxLastNs", JS_NewBigInt64(c, hi));
  return o;
}

static JSValue js_mtr_count_trades(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return JS_NewBigInt64(c, static_cast<int64_t>(lrvx_merged_tape_reader_count_trades(
                               static_cast<LrvxMergedTapeReaderHandle>(getHandle(c, a[0])))));
}

static JSValue js_mtr_read_trades(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  auto h = static_cast<LrvxMergedTapeReaderHandle>(getHandle(c, a[0]));
  uint64_t max = argc > 1 ? static_cast<uint64_t>(toInt64(c, a[1])) : 0;
  uint64_t n = lrvx_merged_tape_reader_count_trades(h);
  if (max > 0 && max < n)
  {
    n = max;
  }
  if (n == 0)
  {
    return JS_NewArray(c);
  }
  std::vector<LrvxTradeRecord> trades(n);
  uint64_t got = lrvx_merged_tape_reader_read_trades(h, trades.data(), n);
  return js_dr_build_trades_array(c, trades, got);
}

static JSValue js_mtr_count_books(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto h = static_cast<LrvxMergedTapeReaderHandle>(getHandle(c, a[0]));
  uint64_t total_levels = 0;
  uint64_t n_events = lrvx_merged_tape_reader_count_books(h, &total_levels);
  JSValue o = JS_NewObject(c);
  JS_SetPropertyStr(c, o, "events", JS_NewBigInt64(c, static_cast<int64_t>(n_events)));
  JS_SetPropertyStr(c, o, "levels", JS_NewBigInt64(c, static_cast<int64_t>(total_levels)));
  return o;
}

static JSValue js_mtr_read_books(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto h = static_cast<LrvxMergedTapeReaderHandle>(getHandle(c, a[0]));
  uint64_t total_levels = 0;
  uint64_t n_events = lrvx_merged_tape_reader_count_books(h, &total_levels);
  if (n_events == 0)
  {
    return JS_NewArray(c);
  }
  std::vector<LrvxBookUpdateHeader> headers(n_events);
  std::vector<LrvxLevel> levels(total_levels);
  uint64_t got = lrvx_merged_tape_reader_read_books(h, headers.data(), n_events,
                                                    levels.data(), total_levels);
  return js_dr_build_book_updates_array(c, headers, levels, got);
}

// ============================================================
// Partitioner
// ============================================================

static JSValue partitionArrayToJs(JSContext* ctx, const std::vector<LrvxPartition>& parts)
{
  JSValue arr = JS_NewArray(ctx);
  for (size_t i = 0; i < parts.size(); i++)
  {
    const auto& p = parts[i];
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "partitionId", JS_NewUint32(ctx, p.partition_id));
    JS_SetPropertyStr(ctx, o, "fromNs", JS_NewBigInt64(ctx, p.from_ns));
    JS_SetPropertyStr(ctx, o, "toNs", JS_NewBigInt64(ctx, p.to_ns));
    JS_SetPropertyStr(ctx, o, "warmupFromNs", JS_NewBigInt64(ctx, p.warmup_from_ns));
    JS_SetPropertyStr(ctx, o, "estimatedEvents", JS_NewInt64(ctx, static_cast<int64_t>(p.estimated_events)));
    JS_SetPropertyStr(ctx, o, "estimatedBytes", JS_NewInt64(ctx, static_cast<int64_t>(p.estimated_bytes)));
    JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), o);
  }
  return arr;
}

static std::vector<LrvxPartition> doPartition(
    JSContext* c, LrvxPartitionerHandle h,
    uint32_t (*fn)(LrvxPartitionerHandle, uint32_t, int64_t, LrvxPartition*, uint32_t),
    uint32_t num, int64_t warmup)
{
  uint32_t n = fn(h, num, warmup, nullptr, 0);
  if (n == 0)
  {
    return {};
  }
  std::vector<LrvxPartition> out(n);
  fn(h, num, warmup, out.data(), n);
  return out;
}

static JSValue js_part_create(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  LRVX_JS_CSTRING_OR_THROW(dir, c, a[0]);
  JSValue ret = createHandleObject(c, lrvx_partitioner_create(dir), lrvx_partitioner_destroy);
  return ret;
}

static JSValue js_part_destroy(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return js_generic_handle_destroy(c, a[0]);
}

static JSValue js_part_by_time(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  auto h = static_cast<LrvxPartitionerHandle>(getHandle(c, a[0]));
  auto parts = doPartition(c, h, lrvx_partitioner_by_time, toUint32(c, a[1]),
                           argc > 2 ? toInt64(c, a[2]) : 0);
  return partitionArrayToJs(c, parts);
}

static JSValue js_part_by_duration(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  auto h = static_cast<LrvxPartitionerHandle>(getHandle(c, a[0]));
  uint32_t n = lrvx_partitioner_by_duration(h, toInt64(c, a[1]),
                                            argc > 2 ? toInt64(c, a[2]) : 0, nullptr, 0);
  if (n == 0)
  {
    return JS_NewArray(c);
  }
  std::vector<LrvxPartition> out(n);
  lrvx_partitioner_by_duration(h, toInt64(c, a[1]), argc > 2 ? toInt64(c, a[2]) : 0, out.data(), n);
  return partitionArrayToJs(c, out);
}

static JSValue js_part_by_calendar(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  auto h = static_cast<LrvxPartitionerHandle>(getHandle(c, a[0]));
  uint8_t unit = argc > 1 ? static_cast<uint8_t>(toUint32(c, a[1])) : 2;
  int64_t warmup = argc > 2 ? toInt64(c, a[2]) : 0;
  uint32_t n = lrvx_partitioner_by_calendar(h, unit, warmup, nullptr, 0);
  if (n == 0)
  {
    return JS_NewArray(c);
  }
  std::vector<LrvxPartition> out(n);
  lrvx_partitioner_by_calendar(h, unit, warmup, out.data(), n);
  return partitionArrayToJs(c, out);
}

static JSValue js_part_by_symbol(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto h = static_cast<LrvxPartitionerHandle>(getHandle(c, a[0]));
  uint32_t n = lrvx_partitioner_by_symbol(h, toUint32(c, a[1]), nullptr, 0);
  if (n == 0)
  {
    return JS_NewArray(c);
  }
  std::vector<LrvxPartition> out(n);
  lrvx_partitioner_by_symbol(h, toUint32(c, a[1]), out.data(), n);
  return partitionArrayToJs(c, out);
}

static JSValue js_part_per_symbol(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto h = static_cast<LrvxPartitionerHandle>(getHandle(c, a[0]));
  uint32_t n = lrvx_partitioner_per_symbol(h, nullptr, 0);
  if (n == 0)
  {
    return JS_NewArray(c);
  }
  std::vector<LrvxPartition> out(n);
  lrvx_partitioner_per_symbol(h, out.data(), n);
  return partitionArrayToJs(c, out);
}

static JSValue js_part_by_event_count(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto h = static_cast<LrvxPartitionerHandle>(getHandle(c, a[0]));
  uint32_t n = lrvx_partitioner_by_event_count(h, toUint32(c, a[1]), nullptr, 0);
  if (n == 0)
  {
    return JS_NewArray(c);
  }
  std::vector<LrvxPartition> out(n);
  lrvx_partitioner_by_event_count(h, toUint32(c, a[1]), out.data(), n);
  return partitionArrayToJs(c, out);
}

// ============================================================
// Bar aggregators
// ============================================================

#include <algorithm>
#include <fstream>
#include <string>

static std::vector<int64_t> jsArrayToInt64s(JSContext* ctx, JSValueConst arr)
{
  JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
  uint32_t n = 0;
  JS_ToUint32(ctx, &n, lenVal);
  JS_FreeValue(ctx, lenVal);
  std::vector<int64_t> out(n);
  for (uint32_t i = 0; i < n; i++)
  {
    JSValue e = JS_GetPropertyUint32(ctx, arr, i);
    int64_t v = 0;
    JS_ToInt64Ext(ctx, &v, e);
    JS_FreeValue(ctx, e);
    out[i] = v;
  }
  return out;
}

static JSValue barsToJsArray(JSContext* ctx, const std::vector<LrvxBar>& bars)
{
  JSValue arr = JS_NewArray(ctx);
  for (size_t i = 0; i < bars.size(); i++)
  {
    const auto& b = bars[i];
    JSValue o = JS_NewObject(ctx);
    static constexpr double kScale = 1e8;
    JS_SetPropertyStr(ctx, o, "ts", JS_NewBigInt64(ctx, b.start_time_ns));
    JS_SetPropertyStr(ctx, o, "open", JS_NewFloat64(ctx, b.open_raw / kScale));
    JS_SetPropertyStr(ctx, o, "high", JS_NewFloat64(ctx, b.high_raw / kScale));
    JS_SetPropertyStr(ctx, o, "low", JS_NewFloat64(ctx, b.low_raw / kScale));
    JS_SetPropertyStr(ctx, o, "close", JS_NewFloat64(ctx, b.close_raw / kScale));
    JS_SetPropertyStr(ctx, o, "volume", JS_NewFloat64(ctx, b.volume_raw / kScale));
    JS_SetPropertyStr(ctx, o, "buyVolume", JS_NewFloat64(ctx, b.buy_volume_raw / kScale));
    JS_SetPropertyStr(ctx, o, "trades", JS_NewUint32(ctx, b.trade_count));
    // lrvx::Bar::reason, carried through so a batch-aggregated bar says why
    // it closed the same way the live callback path's bar object does.
    JS_SetPropertyStr(ctx, o, "closeReason", JS_NewUint32(ctx, b.close_reason));
    JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), o);
  }
  return arr;
}

// Generic aggregator: takes (timestamps, prices, qtys, is_buy, param, maxBars)
// Returns JS array of bar objects.
using AggTimeFn = uint32_t (*)(const int64_t*, const double*, const double*, const uint8_t*,
                               size_t, double, LrvxBar*, uint32_t);

// Read the optional `sides` array into per-trade buy flags. Shared, because
// js_agg_tick used to inline an all-zero vector instead and so reported
// buyVolume == 0 on every tick bar -- silently, and only on that one
// aggregator.
static std::vector<uint8_t> aggSides(JSContext* c, JSValueConst sidesArg, uint32_t n)
{
  std::vector<uint8_t> side(n, 0);
  if (JS_IsArray(c, sidesArg))
  {
    JSValue lenVal = JS_GetPropertyStr(c, sidesArg, "length");
    uint32_t sn = 0;
    JS_ToUint32(c, &sn, lenVal);
    JS_FreeValue(c, lenVal);
    for (uint32_t i = 0; i < std::min(sn, n); i++)
    {
      JSValue e = JS_GetPropertyUint32(c, sidesArg, i);
      int boolVal = JS_ToBool(c, e);
      JS_FreeValue(c, e);
      side[i] = boolVal ? 1 : 0;
    }
  }
  return side;
}

static JSValue doAgg(JSContext* c, JSValueConst* a, AggTimeFn fn, double param)
{
  auto ts = jsArrayToInt64s(c, a[0]);
  auto px = jsArrayToDoubles(c, a[1]);
  auto qty = jsArrayToDoubles(c, a[2]);
  uint32_t n = static_cast<uint32_t>(ts.size());
  std::vector<uint8_t> side = aggSides(c, a[3], n);
  std::vector<LrvxBar> bars(n);
  uint32_t got = fn(ts.data(), px.data(), qty.data(), side.data(), n, param, bars.data(), n);
  // Every aggregator routed through doAgg() except Renko closes at most one
  // bar per input trade, so `n` is always a safe upper bound and `got` is
  // never more than that. Renko can synthesize several bricks from a
  // single trade that gaps past more than one brick width (see
  // closeAndReopen() in renko_bar_policy.h), so `got` can come back larger than
  // `n` there. The call above is bounds-checked and never overruns `bars`,
  // but it also only wrote the first `n` bars in that case -- naively
  // resizing to `got` would pad the result with zero-filled bars instead
  // of the real ones that did not fit. Redo the call with a buffer sized
  // to the real count instead.
  if (got > n)
  {
    bars.assign(got, LrvxBar{});
    got = fn(ts.data(), px.data(), qty.data(), side.data(), n, param, bars.data(), got);
  }
  bars.resize(got);
  return barsToJsArray(c, bars);
}

// lrvx.timeBars and lrvx.heikinBars document their interval in
// nanoseconds; the C ABI takes seconds as a double and multiplies by 1e9
// again on the other side. The JS argument used to be passed straight
// through, so a script following the docs asked for 60'000'000'000
// seconds and got a single bar covering the whole tape. Convert once,
// here, and accept a BigInt or a Number (toInt64 takes either).
//
// A plain ns / 1e9 can come back one nanosecond short after the
// truncation on the other side, so step the double up until the round
// trip lands on the nanosecond count that was asked for.
static double intervalNsToSeconds(int64_t interval_ns)
{
  double seconds = static_cast<double>(interval_ns) / 1'000'000'000.0;
  for (int i = 0; i < 4 && interval_ns > 0 &&
                  static_cast<int64_t>(seconds * 1'000'000'000.0) < interval_ns;
       ++i)
  {
    seconds = std::nextafter(seconds, std::numeric_limits<double>::infinity());
  }
  return seconds;
}

static JSValue js_agg_time(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return doAgg(c, a, lrvx_aggregate_time_bars, intervalNsToSeconds(toInt64(c, a[4])));
}

static JSValue js_agg_tick(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  auto ts = jsArrayToInt64s(c, a[0]);
  auto px = jsArrayToDoubles(c, a[1]);
  auto qty = jsArrayToDoubles(c, a[2]);
  uint32_t n = static_cast<uint32_t>(ts.size());
  std::vector<uint8_t> side = aggSides(c, a[3], n);
  std::vector<LrvxBar> bars(n);
  uint32_t got = lrvx_aggregate_tick_bars(ts.data(), px.data(), qty.data(), side.data(),
                                          n, toUint32(c, a[4]), bars.data(), n);
  bars.resize(got);
  return barsToJsArray(c, bars);
}

static JSValue js_agg_volume(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return doAgg(c, a, lrvx_aggregate_volume_bars, toDouble(c, a[4]));
}

static JSValue js_agg_range(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return doAgg(c, a, lrvx_aggregate_range_bars, toDouble(c, a[4]));
}

static JSValue js_agg_renko(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return doAgg(c, a, lrvx_aggregate_renko_bars, toDouble(c, a[4]));
}

static JSValue js_agg_heikin(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  return doAgg(c, a, lrvx_aggregate_heikin_ashi_bars, intervalNsToSeconds(toInt64(c, a[4])));
}

// ============================================================
// Extended segment ops (returning JS objects)
// ============================================================

static JSValue js_seg_merge_dir(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  LRVX_JS_CSTRING_OR_THROW(in, c, a[0]);
  LRVX_JS_CSTRING_OR_THROW(out, c, a[1]);
  LrvxMergeResult r{};
  std::vector<char> path(4096);
  std::vector<char> errors(65536);
  lrvx_clear_last_error();
  lrvx_segment_merge_dir_report(in, out, &r, path.data(), path.size(), errors.data(),
                                errors.size());
  JSValue o = JS_NewObject(c);
  JS_SetPropertyStr(c, o, "success", JS_NewBool(c, r.success));
  JS_SetPropertyStr(c, o, "segmentsMerged", JS_NewInt64(c, static_cast<int64_t>(r.segments_merged)));
  JS_SetPropertyStr(c, o, "eventsWritten", JS_NewInt64(c, static_cast<int64_t>(r.events_written)));
  JS_SetPropertyStr(c, o, "bytesWritten", JS_NewInt64(c, static_cast<int64_t>(r.bytes_written)));
  JS_SetPropertyStr(c, o, "outputPath", JS_NewString(c, path.data()));
  // One entry per line of the report.
  JSValue list = JS_NewArray(c);
  uint32_t count = 0;
  const std::string_view all(errors.data());
  for (size_t start = 0; start < all.size();)
  {
    size_t end = all.find('\n', start);
    if (end == std::string_view::npos)
    {
      end = all.size();
    }
    JS_SetPropertyUint32(c, list, count++, JS_NewStringLen(c, all.data() + start, end - start));
    start = end + 1;
  }
  JS_SetPropertyStr(c, o, "errors", list);
  return o;
}

static JSValue js_seg_split(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  LRVX_JS_CSTRING_OR_THROW(in, c, a[0]);
  LRVX_JS_CSTRING_OR_THROW(dir, c, a[1]);
  uint8_t mode = argc > 2 ? static_cast<uint8_t>(toUint32(c, a[2])) : 0;
  int64_t tns = argc > 3 ? toInt64(c, a[3]) : 0;
  uint64_t epc = argc > 4 ? static_cast<uint64_t>(toInt64(c, a[4])) : 0;
  LrvxSplitResult r = lrvx_segment_split(in, dir, mode, tns, epc);
  JSValue o = JS_NewObject(c);
  JS_SetPropertyStr(c, o, "success", JS_NewBool(c, r.success));
  JS_SetPropertyStr(c, o, "segmentsCreated", JS_NewUint32(c, r.segments_created));
  JS_SetPropertyStr(c, o, "eventsWritten", JS_NewInt64(c, static_cast<int64_t>(r.events_written)));
  return o;
}

static JSValue js_seg_export(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  LRVX_JS_CSTRING_OR_THROW(in, c, a[0]);
  LRVX_JS_CSTRING_OR_THROW(out, c, a[1]);
  uint8_t fmt = argc > 2 ? static_cast<uint8_t>(toUint32(c, a[2])) : 0;
  int64_t from = argc > 3 ? toInt64(c, a[3]) : 0;
  int64_t to = argc > 4 ? toInt64(c, a[4]) : 0;
  std::vector<uint32_t> syms;
  if (argc > 5 && JS_IsArray(c, a[5]))
  {
    JSValue lv = JS_GetPropertyStr(c, a[5], "length");
    uint32_t n = 0;
    JS_ToUint32(c, &n, lv);
    JS_FreeValue(c, lv);
    for (uint32_t i = 0; i < n; i++)
    {
      JSValue e = JS_GetPropertyUint32(c, a[5], i);
      syms.push_back(toUint32(c, e));
      JS_FreeValue(c, e);
    }
  }
  LrvxExportResult r = lrvx_segment_export(in, out, fmt, from, to,
                                           syms.empty() ? nullptr : syms.data(),
                                           static_cast<uint32_t>(syms.size()));
  JSValue o = JS_NewObject(c);
  JS_SetPropertyStr(c, o, "success", JS_NewBool(c, r.success));
  JS_SetPropertyStr(c, o, "eventsExported", JS_NewInt64(c, static_cast<int64_t>(r.events_exported)));
  JS_SetPropertyStr(c, o, "bytesWritten", JS_NewInt64(c, static_cast<int64_t>(r.bytes_written)));
  return o;
}

static JSValue js_seg_validate_full(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  LRVX_JS_CSTRING_OR_THROW(path, c, a[0]);
  LrvxSegmentValidation r = lrvx_segment_validate_full(path,
                                                       argc > 1 ? static_cast<uint8_t>(JS_ToBool(c, a[1])) : 1,
                                                       argc > 2 ? static_cast<uint8_t>(JS_ToBool(c, a[2])) : 1);
  JSValue o = JS_NewObject(c);
  JS_SetPropertyStr(c, o, "valid", JS_NewBool(c, r.valid));
  JS_SetPropertyStr(c, o, "headerValid", JS_NewBool(c, r.header_valid));
  JS_SetPropertyStr(c, o, "reportedEventCount", JS_NewInt64(c, static_cast<int64_t>(r.reported_event_count)));
  JS_SetPropertyStr(c, o, "actualEventCount", JS_NewInt64(c, static_cast<int64_t>(r.actual_event_count)));
  JS_SetPropertyStr(c, o, "hasIndex", JS_NewBool(c, r.has_index));
  JS_SetPropertyStr(c, o, "indexValid", JS_NewBool(c, r.index_valid));
  JS_SetPropertyStr(c, o, "tradesFound", JS_NewInt64(c, static_cast<int64_t>(r.trades_found)));
  JS_SetPropertyStr(c, o, "bookUpdatesFound", JS_NewInt64(c, static_cast<int64_t>(r.book_updates_found)));
  JS_SetPropertyStr(c, o, "crcErrors", JS_NewUint32(c, r.crc_errors));
  JS_SetPropertyStr(c, o, "timestampAnomalies", JS_NewUint32(c, r.timestamp_anomalies));
  return o;
}

static JSValue js_dataset_validate(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  LRVX_JS_CSTRING_OR_THROW(dir, c, a[0]);
  lrvx_clear_last_error();
  LrvxDatasetValidation r = lrvx_dataset_validate(dir);
  JSValue o = JS_NewObject(c);
  JS_SetPropertyStr(c, o, "valid", JS_NewBool(c, r.valid));
  JS_SetPropertyStr(c, o, "totalSegments", JS_NewUint32(c, r.total_segments));
  JS_SetPropertyStr(c, o, "validSegments", JS_NewUint32(c, r.valid_segments));
  JS_SetPropertyStr(c, o, "corruptedSegments", JS_NewUint32(c, r.corrupted_segments));
  JS_SetPropertyStr(c, o, "totalEvents", JS_NewInt64(c, static_cast<int64_t>(r.total_events)));
  JS_SetPropertyStr(c, o, "totalBytes", JS_NewInt64(c, static_cast<int64_t>(r.total_bytes)));
  JS_SetPropertyStr(c, o, "firstTimestamp", JS_NewBigInt64(c, r.first_timestamp));
  JS_SetPropertyStr(c, o, "lastTimestamp", JS_NewBigInt64(c, r.last_timestamp));
  return o;
}

static JSValue js_seg_recompress(JSContext* c, JSValueConst, int argc, JSValueConst* a)
{
  LRVX_JS_CSTRING_OR_THROW(in, c, a[0]);
  LRVX_JS_CSTRING_OR_THROW(out, c, a[1]);
  uint8_t comp = argc > 2 ? static_cast<uint8_t>(toUint32(c, a[2])) : 1;
  lrvx_clear_last_error();
  JSValue ret = JS_NewBool(c, lrvx_segment_recompress(in, out, comp));
  return ret;
}

static JSValue js_seg_extract_symbols(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  LRVX_JS_CSTRING_OR_THROW(in, c, a[0]);
  LRVX_JS_CSTRING_OR_THROW(out, c, a[1]);
  std::vector<uint32_t> syms;
  if (JS_IsArray(c, a[2]))
  {
    JSValue lv = JS_GetPropertyStr(c, a[2], "length");
    uint32_t n = 0;
    JS_ToUint32(c, &n, lv);
    JS_FreeValue(c, lv);
    for (uint32_t i = 0; i < n; i++)
    {
      JSValue e = JS_GetPropertyUint32(c, a[2], i);
      syms.push_back(toUint32(c, e));
      JS_FreeValue(c, e);
    }
  }
  lrvx_clear_last_error();
  uint64_t written = lrvx_segment_extract_symbols(in, out, syms.data(), static_cast<uint32_t>(syms.size()));
  return JS_NewInt64(c, static_cast<int64_t>(written));
}

static JSValue js_seg_extract_time(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  LRVX_JS_CSTRING_OR_THROW(in, c, a[0]);
  LRVX_JS_CSTRING_OR_THROW(out, c, a[1]);
  lrvx_clear_last_error();
  uint64_t written = lrvx_segment_extract_time_range(in, out, toInt64(c, a[2]), toInt64(c, a[3]));
  return JS_NewInt64(c, static_cast<int64_t>(written));
}

// ============================================================
// loadCsv -- read OHLCV CSV and return array of bar objects
// ============================================================

static int64_t detectTimestampNs(int64_t ts)
{
  // Best-effort unit guess for a raw CSV column -- genuinely unit-less
  // input, valid only for that case. Never apply this to a value whose
  // unit the API already declares as nanoseconds; a copy of this same
  // heuristic did exactly that in python/strategy_bindings.h's run_bars
  // and silently corrupted legitimate small nanosecond values (a 60s-in
  // bar offset read as 60 seconds-since-epoch and rescaled again).
  //
  // Thresholds match Python normalizeTimestamp and Codon _parse_ts.
  // Modern unix-ms timestamps (~1.78e12 in 2026) exceed 1e12, so the
  // seconds/ms boundary must be at 1e12 for seconds, 1e15 for ms. Do not
  // retune these -- the unit ranges overlap on real timestamps, so any
  // cutoff is a bet on which dates show up.
  if (ts < 1'000'000'000'000LL)
  {
    return ts * 1'000'000'000LL;  // seconds → ns
  }
  if (ts < 1'000'000'000'000'000LL)
  {
    return ts * 1'000'000LL;  // ms → ns
  }
  if (ts < 1'000'000'000'000'000'000LL)
  {
    return ts * 1'000LL;  // us → ns
  }
  return ts;  // already ns
}

static JSValue js_load_csv(JSContext* c, JSValueConst, int, JSValueConst* a)
{
  LRVX_JS_CSTRING_OR_THROW(path, c, a[0]);
  std::ifstream f(path);
  if (!f.is_open())
  {
    return JS_ThrowTypeError(c, "loadCsv: file not found");
  }

  JSValue arr = JS_NewArray(c);
  uint32_t idx = 0;
  bool firstLine = true;
  std::string line;
  while (std::getline(f, line))
  {
    if (line.empty())
    {
      continue;
    }
    // Parse comma-separated: ts,open,high,low,close,volume
    std::vector<std::string> parts;
    std::istringstream ss(line);
    std::string tok;
    while (std::getline(ss, tok, ','))
    {
      parts.push_back(tok);
    }
    if (parts.size() < 6)
    {
      continue;
    }
    // Detect header: first field non-numeric
    if (firstLine)
    {
      firstLine = false;
      char* ep;
      strtoll(parts[0].c_str(), &ep, 10);
      if (*ep != '\0')
      {
        continue;  // header row
      }
    }
    try
    {
      // Store ts in nanoseconds, as a BigInt. Every other bar source here
      // (the aggregators, the live onBar path) reports a nanosecond
      // BigInt, and a millisecond Number both truncated everything below
      // the millisecond and threw a TypeError the moment a script
      // subtracted a CSV bar's ts from an aggregator bar's ts.
      int64_t ts_ns = detectTimestampNs(std::stoll(parts[0]));
      double o = std::stod(parts[1]);
      double h = std::stod(parts[2]);
      double l = std::stod(parts[3]);
      double cl = std::stod(parts[4]);
      double v = std::stod(parts[5]);
      JSValue o2 = JS_NewObject(c);
      JS_SetPropertyStr(c, o2, "ts", JS_NewBigInt64(c, ts_ns));
      JS_SetPropertyStr(c, o2, "open", JS_NewFloat64(c, o));
      JS_SetPropertyStr(c, o2, "high", JS_NewFloat64(c, h));
      JS_SetPropertyStr(c, o2, "low", JS_NewFloat64(c, l));
      JS_SetPropertyStr(c, o2, "close", JS_NewFloat64(c, cl));
      JS_SetPropertyStr(c, o2, "volume", JS_NewFloat64(c, v));
      JS_SetPropertyUint32(c, arr, idx++, o2);
    }
    catch (...)
    {
      continue;
    }
  }
  return arr;
}

static JSValue js_capi_abi_version(JSContext* c, JSValueConst, int, JSValueConst*)
{
  return JS_NewUint32(c, lrvx_capi_abi_version());
}

bool registerLrvxBindings(JSContext* ctx)
{
  // Before anything else: an engine that loaded against the wrong
  // library must not get a surface that reads wrong numbers. Registering
  // nothing leaves every __lrvx_* global undefined, so a script that
  // runs anyway fails on its first call rather than on a wrong price.
  {
    std::string abiMessage;
    if (!capi::checkAbiVersion(LRVX_CAPI_ABI_VERSION, &abiMessage))
    {
      JS_ThrowInternalError(ctx, "%s", abiMessage.c_str());
      return false;
    }
  }

  // Register handle class. The id is process-global and allocated once
  // (see ensureHandleClassId); registering it against this engine's
  // runtime class table happens every time, as before.
  ensureHandleClassId();
  JS_NewClass(JS_GetRuntime(ctx), jsHandleClassId, &jsHandleClassDef);
  ensureGraphClassIds();
  JS_NewClass(JS_GetRuntime(ctx), jsGraphClassId, &jsGraphClassDef);
  JS_NewClass(JS_GetRuntime(ctx), jsStreamingClassId, &jsStreamingClassDef);
  ensureFeedClockClassId();
  JS_NewClass(JS_GetRuntime(ctx), jsFeedClockClassId, &jsFeedClockClassDef);

  // console
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue console = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, console, "log", JS_NewCFunction(ctx, js_console_log, "log", 1));
  JS_SetPropertyStr(ctx, console, "warn", JS_NewCFunction(ctx, js_console_warn, "warn", 1));
  JS_SetPropertyStr(ctx, console, "error", JS_NewCFunction(ctx, js_console_error, "error", 1));
  JS_SetPropertyStr(ctx, global, "console", console);
  JS_FreeValue(ctx, global);

  // Signal emission
  addGlobalFunc(ctx, "__lrvx_emit_market_buy", js_emit_market_buy, 3);
  addGlobalFunc(ctx, "__lrvx_emit_market_sell", js_emit_market_sell, 3);
  addGlobalFunc(ctx, "__lrvx_emit_limit_buy_tif", js_emit_limit_buy_tif, 5);
  addGlobalFunc(ctx, "__lrvx_emit_limit_sell_tif", js_emit_limit_sell_tif, 5);
  addGlobalFunc(ctx, "__lrvx_emit_cancel", js_emit_cancel, 2);
  addGlobalFunc(ctx, "__lrvx_emit_cancel_all", js_emit_cancel_all, 2);
  addGlobalFunc(ctx, "__lrvx_emit_modify", js_emit_modify, 4);
  addGlobalFunc(ctx, "__lrvx_emit_stop_market", js_emit_stop_market, 5);
  addGlobalFunc(ctx, "__lrvx_emit_stop_limit", js_emit_stop_limit, 6);
  addGlobalFunc(ctx, "__lrvx_emit_take_profit_market", js_emit_take_profit_market, 5);
  addGlobalFunc(ctx, "__lrvx_emit_take_profit_limit", js_emit_take_profit_limit, 6);
  addGlobalFunc(ctx, "__lrvx_emit_trailing_stop", js_emit_trailing_stop, 5);
  addGlobalFunc(ctx, "__lrvx_emit_trailing_stop_pct", js_emit_trailing_stop_pct, 5);
  addGlobalFunc(ctx, "__lrvx_emit_close_position", js_emit_close_position, 2);

  // Context queries
  addGlobalFunc(ctx, "__lrvx_position", js_position, 2);
  addGlobalFunc(ctx, "__lrvx_last_trade_price", js_last_trade_price, 2);
  addGlobalFunc(ctx, "__lrvx_best_bid", js_best_bid, 2);
  addGlobalFunc(ctx, "__lrvx_best_ask", js_best_ask, 2);
  addGlobalFunc(ctx, "__lrvx_mid_price", js_mid_price, 2);
  addGlobalFunc(ctx, "__lrvx_get_order_status", js_get_order_status, 2);
  addGlobalFunc(ctx, "__lrvx_strategy_last_closed_bar", js_strategy_last_closed_bar, 4);
  addGlobalFunc(ctx, "__lrvx_strategy_last_n_closed_bars", js_strategy_last_n_closed_bars, 5);
  addGlobalFunc(ctx, "__lrvx_strategy_get_bar_ring_capacity",
                js_strategy_get_bar_ring_capacity, 1);
  addGlobalFunc(ctx, "__lrvx_strategy_set_bar_ring_capacity",
                js_strategy_set_bar_ring_capacity, 2);
  addGlobalFunc(ctx, "__lrvx_feed_clock_create", js_feed_clock_create, 6);
  addGlobalFunc(ctx, "__lrvx_feed_clock_destroy", js_feed_clock_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_feed_clock_symbol_count", js_feed_clock_symbol_count, 1);
  addGlobalFunc(ctx, "__lrvx_feed_clock_symbol_at", js_feed_clock_symbol_at, 2);
  addGlobalFunc(ctx, "__lrvx_feed_clock_tick", js_feed_clock_tick, 3);
  addGlobalFunc(ctx, "__lrvx_feed_clock_last_fired", js_feed_clock_last_fired, 1);
  addGlobalFunc(ctx, "__lrvx_feed_clock_last_triggered_by",
                js_feed_clock_last_triggered_by, 1);
  addGlobalFunc(ctx, "__lrvx_feed_clock_last_seen_at", js_feed_clock_last_seen_at, 2);
  addGlobalFunc(ctx, "__lrvx_feed_clock_staleness_at", js_feed_clock_staleness_at, 2);
  addGlobalFunc(ctx, "__lrvx_feed_clock_reset", js_feed_clock_reset, 1);

  // Batch indicators
  addGlobalFunc(ctx, "__lrvx_indicator_sma", js_indicator_sma, 2);
  addGlobalFunc(ctx, "__lrvx_indicator_ema", js_indicator_ema, 2);
  addGlobalFunc(ctx, "__lrvx_indicator_rsi", js_indicator_rsi, 2);
  addGlobalFunc(ctx, "__lrvx_indicator_atr", js_indicator_atr, 4);
  addGlobalFunc(ctx, "__lrvx_indicator_macd", js_indicator_macd, 4);
  addGlobalFunc(ctx, "__lrvx_indicator_bollinger", js_indicator_bollinger, 3);
  addGlobalFunc(ctx, "__lrvx_indicator_rma", js_indicator_rma, 2);
  addGlobalFunc(ctx, "__lrvx_indicator_dema", js_indicator_dema, 2);
  addGlobalFunc(ctx, "__lrvx_indicator_tema", js_indicator_tema, 2);
  addGlobalFunc(ctx, "__lrvx_indicator_kama", js_indicator_kama, 4);
  addGlobalFunc(ctx, "__lrvx_indicator_slope", js_indicator_slope, 2);
  addGlobalFunc(ctx, "__lrvx_indicator_adx", js_indicator_adx, 4);
  addGlobalFunc(ctx, "__lrvx_indicator_cci", js_indicator_cci, 4);
  addGlobalFunc(ctx, "__lrvx_indicator_stochastic", js_indicator_stochastic, 5);
  addGlobalFunc(ctx, "__lrvx_indicator_chop", js_indicator_chop, 4);
  addGlobalFunc(ctx, "__lrvx_indicator_obv", js_indicator_obv, 2);
  addGlobalFunc(ctx, "__lrvx_indicator_vwap", js_indicator_vwap, 3);
  addGlobalFunc(ctx, "__lrvx_indicator_cvd", js_indicator_cvd, 5);
  addGlobalFunc(ctx, "__lrvx_indicator_skewness", js_indicator_skewness, 2);
  addGlobalFunc(ctx, "__lrvx_indicator_kurtosis", js_indicator_kurtosis, 2);
  addGlobalFunc(ctx, "__lrvx_indicator_rolling_zscore", js_indicator_rolling_zscore, 2);
  addGlobalFunc(ctx, "__lrvx_indicator_shannon_entropy", js_indicator_shannon_entropy, 3);
  addGlobalFunc(ctx, "__lrvx_indicator_parkinson_vol", js_indicator_parkinson_vol, 3);
  addGlobalFunc(ctx, "__lrvx_indicator_rogers_satchell_vol", js_indicator_rogers_satchell_vol, 5);
  addGlobalFunc(ctx, "__lrvx_indicator_correlation", js_indicator_correlation, 3);
  addGlobalFunc(ctx, "__lrvx_indicator_adf", js_indicator_adf, 3);
  addGlobalFunc(ctx, "__lrvx_indicator_autocorrelation", js_indicator_autocorrelation, 3);

  // Streaming indicators (one C handle per indicator object)
  addGlobalFunc(ctx, "__lrvx_streaming_indicator_create", js_streaming_indicator_create, 2);
  addGlobalFunc(ctx, "__lrvx_streaming_indicator_destroy", js_streaming_indicator_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_streaming_indicator_update", js_streaming_indicator_update, 2);
  addGlobalFunc(ctx, "__lrvx_streaming_indicator_update_bar", js_streaming_indicator_update_bar,
                4);
  addGlobalFunc(ctx, "__lrvx_streaming_indicator_update_high_low",
                js_streaming_indicator_update_high_low, 3);
  addGlobalFunc(ctx, "__lrvx_streaming_indicator_update_ohlc", js_streaming_indicator_update_ohlc,
                5);
  addGlobalFunc(ctx, "__lrvx_streaming_indicator_update_pair", js_streaming_indicator_update_pair,
                3);
  addGlobalFunc(ctx, "__lrvx_streaming_indicator_value", js_streaming_indicator_value, 1);
  addGlobalFunc(ctx, "__lrvx_streaming_indicator_output", js_streaming_indicator_output, 2);
  addGlobalFunc(ctx, "__lrvx_streaming_indicator_ready", js_streaming_indicator_ready, 1);
  addGlobalFunc(ctx, "__lrvx_streaming_indicator_count", js_streaming_indicator_count, 1);
  addGlobalFunc(ctx, "__lrvx_streaming_indicator_reset", js_streaming_indicator_reset, 1);

  // Targets (forward-looking labels)
  addGlobalFunc(ctx, "__lrvx_target_future_return", js_target_future_return, 2);
  addGlobalFunc(ctx, "__lrvx_target_future_ctc_volatility", js_target_future_ctc_volatility, 2);
  addGlobalFunc(ctx, "__lrvx_target_future_linear_slope", js_target_future_linear_slope, 2);

  // IndicatorGraph (batch)
  addGlobalFunc(ctx, "__lrvx_graph_create", js_graph_create, 0);
  addGlobalFunc(ctx, "__lrvx_graph_destroy", js_graph_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_graph_set_bars", js_graph_set_bars, 6);
  addGlobalFunc(ctx, "__lrvx_graph_add_node", js_graph_add_node, 5);
  addGlobalFunc(ctx, "__lrvx_graph_require", js_graph_require, 3);
  addGlobalFunc(ctx, "__lrvx_graph_get", js_graph_get, 3);
  addGlobalFunc(ctx, "__lrvx_graph_close", js_graph_close, 2);
  addGlobalFunc(ctx, "__lrvx_graph_high", js_graph_high, 2);
  addGlobalFunc(ctx, "__lrvx_graph_low", js_graph_low, 2);
  addGlobalFunc(ctx, "__lrvx_graph_volume", js_graph_volume, 2);
  addGlobalFunc(ctx, "__lrvx_graph_invalidate", js_graph_invalidate, 2);
  addGlobalFunc(ctx, "__lrvx_graph_invalidate_all", js_graph_invalidate_all, 1);

  // StreamingIndicatorGraph
  addGlobalFunc(ctx, "__lrvx_streaming_create", js_streaming_create, 0);
  addGlobalFunc(ctx, "__lrvx_streaming_destroy", js_streaming_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_streaming_add_node", js_streaming_add_node, 5);
  addGlobalFunc(ctx, "__lrvx_streaming_step", js_streaming_step, 6);
  addGlobalFunc(ctx, "__lrvx_streaming_current", js_streaming_current, 3);
  addGlobalFunc(ctx, "__lrvx_streaming_bar_count", js_streaming_bar_count, 2);
  addGlobalFunc(ctx, "__lrvx_streaming_reset", js_streaming_reset, 2);
  addGlobalFunc(ctx, "__lrvx_streaming_reset_all", js_streaming_reset_all, 1);
  addGlobalFunc(ctx, "__lrvx_streaming_close", js_streaming_close, 2);
  addGlobalFunc(ctx, "__lrvx_streaming_high", js_streaming_high, 2);
  addGlobalFunc(ctx, "__lrvx_streaming_low", js_streaming_low, 2);
  addGlobalFunc(ctx, "__lrvx_streaming_volume", js_streaming_volume, 2);

  // Order book
  addGlobalFunc(ctx, "__lrvx_book_create", js_book_create, 1);
  addGlobalFunc(ctx, "__lrvx_book_destroy", js_book_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_book_apply_snapshot", js_book_apply_snapshot, 5);
  addGlobalFunc(ctx, "__lrvx_book_apply_delta", js_book_apply_delta, 5);
  addGlobalFunc(ctx, "__lrvx_book_best_bid", js_book_best_bid, 1);
  addGlobalFunc(ctx, "__lrvx_book_best_ask", js_book_best_ask, 1);
  addGlobalFunc(ctx, "__lrvx_book_mid", js_book_mid, 1);
  addGlobalFunc(ctx, "__lrvx_book_spread", js_book_spread, 1);
  addGlobalFunc(ctx, "__lrvx_book_clear", js_book_clear, 1);
  addGlobalFunc(ctx, "__lrvx_book_is_crossed", js_book_is_crossed, 1);

  // Executor
  addGlobalFunc(ctx, "__lrvx_simulated_executor_create", js_executor_create, 0);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_destroy", js_executor_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_submit", js_executor_submit, 7);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_submit_ex", js_executor_submit_ex, 10);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_submit_bracket",
                js_executor_submit_bracket, 13);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_cancel_bracket",
                js_executor_cancel_bracket, 2);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_bracket_state",
                js_executor_bracket_state, 2);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_set_bracket_child_arm_mode",
                js_executor_set_bracket_child_arm_mode, 2);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_submit_iceberg",
                js_executor_submit_iceberg, 7);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_set_iceberg_refresh_latency",
                js_executor_set_iceberg_refresh_latency, 2);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_iceberg_hidden_remaining_raw",
                js_executor_iceberg_hidden_remaining_raw, 2);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_set_iceberg_size_randomisation_pct",
                js_executor_set_iceberg_size_randomisation_pct, 2);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_set_iceberg_priority_mode",
                js_executor_set_iceberg_priority_mode, 2);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_set_iceberg_jitter_seed",
                js_executor_set_iceberg_jitter_seed, 2);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_on_bar", js_executor_on_bar, 3);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_on_bar_ohlc", js_executor_on_bar_ohlc, 6);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_begin_bar_callback_window",
                js_executor_begin_bar_callback_window, 1);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_end_bar_callback_window",
                js_executor_end_bar_callback_window, 1);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_reset", js_executor_reset, 1);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_on_trade", js_executor_on_trade, 4);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_advance_clock", js_executor_advance, 2);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_fill_count", js_executor_fill_count, 1);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_set_default_slippage",
                js_executor_set_default_slippage, 6);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_set_symbol_slippage",
                js_executor_set_symbol_slippage, 7);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_set_queue_model", js_executor_set_queue_model, 3);
  // js_strategy.cpp:159 exposes setQueueFifoTopN() and matching-modes.md
  // documents it, but the global was never registered -- every call was a
  // ReferenceError.
  addGlobalFunc(ctx, "__lrvx_simulated_executor_set_queue_fifo_top_n",
                js_executor_set_queue_fifo_top_n, 2);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_set_top_priority_share",
                js_executor_set_top_priority_share, 2);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_set_lmm_orders",
                js_executor_set_lmm_orders, 2);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_set_lmm_bonus_multiplier",
                js_executor_set_lmm_bonus_multiplier, 2);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_set_order_priority_multiplier",
                js_executor_set_order_priority_multiplier, 3);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_set_submit_ack", js_executor_set_submit_ack, 3);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_set_cancel_ack", js_executor_set_cancel_ack, 3);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_set_replace_ack", js_executor_set_replace_ack, 3);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_apply_latency_profile",
                js_executor_apply_latency_profile, 2);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_set_stp_mode",
                js_executor_set_stp_mode, 2);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_set_stp_group_membership",
                js_executor_set_stp_group_membership, 3);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_stp_group_for",
                js_executor_stp_group_for, 2);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_set_fok_mode",
                js_executor_set_fok_mode, 2);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_fok_mode",
                js_executor_fok_mode, 1);
  addGlobalFunc(ctx, "__lrvx_latency_distribution_create", js_latency_dist_create, 0);
  addGlobalFunc(ctx, "__lrvx_latency_distribution_destroy", js_latency_dist_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_latency_distribution_set_constant",
                js_latency_dist_set_constant, 2);
  addGlobalFunc(ctx, "__lrvx_latency_distribution_set_uniform",
                js_latency_dist_set_uniform, 3);
  addGlobalFunc(ctx, "__lrvx_latency_distribution_set_lognormal",
                js_latency_dist_set_lognormal, 3);
  addGlobalFunc(ctx, "__lrvx_latency_distribution_set_empirical",
                js_latency_dist_set_empirical, 2);
  addGlobalFunc(ctx, "__lrvx_latency_distribution_set_burst_correlation",
                js_latency_dist_set_burst, 2);
  addGlobalFunc(ctx, "__lrvx_latency_distribution_median_ns",
                js_latency_dist_median_ns, 1);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_set_submit_ack_latency_distribution",
                js_executor_set_submit_ack_dist, 2);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_set_cancel_ack_latency_distribution",
                js_executor_set_cancel_ack_dist, 2);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_set_replace_ack_latency_distribution",
                js_executor_set_replace_ack_dist, 2);
  addGlobalFunc(ctx, "__lrvx_rate_limit_policy_create", js_rate_limit_create, 0);
  addGlobalFunc(ctx, "__lrvx_rate_limit_policy_destroy", js_rate_limit_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_rate_limit_policy_add_bucket_family",
                js_rate_limit_add_bucket_family, 9);
  addGlobalFunc(ctx, "__lrvx_rate_limit_policy_add_bucket",
                js_rate_limit_add_bucket, 7);
  addGlobalFunc(ctx, "__lrvx_rate_limit_policy_set_ban", js_rate_limit_set_ban, 3);
  addGlobalFunc(ctx, "__lrvx_rate_limit_policy_load_profile",
                js_rate_limit_load_profile, 2);
  addGlobalFunc(ctx, "__lrvx_rate_limit_policy_ban_until_ns",
                js_rate_limit_ban_until_ns, 1);
  addGlobalFunc(ctx, "__lrvx_rate_limit_policy_consecutive_rejects",
                js_rate_limit_consecutive_rejects, 1);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_set_rate_limit_policy",
                js_executor_set_rate_limit_policy, 2);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_clear_rate_limit_policy",
                js_executor_clear_rate_limit_policy, 1);
  addGlobalFunc(ctx, "__lrvx_venue_availability_create",
                js_venue_availability_create, 0);
  addGlobalFunc(ctx, "__lrvx_venue_availability_destroy",
                js_venue_availability_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_venue_availability_schedule_outage_ex",
                js_venue_availability_schedule_outage_ex, 8);
  addGlobalFunc(ctx, "__lrvx_venue_availability_submits_allowed",
                js_venue_availability_submits_allowed, 2);
  addGlobalFunc(ctx, "__lrvx_venue_availability_cancels_allowed",
                js_venue_availability_cancels_allowed, 2);
  addGlobalFunc(ctx, "__lrvx_venue_availability_book_updates_allowed",
                js_venue_availability_book_updates_allowed, 2);
  addGlobalFunc(ctx, "__lrvx_venue_availability_trades_allowed",
                js_venue_availability_trades_allowed, 2);
  addGlobalFunc(ctx, "__lrvx_venue_availability_latency_multiplier",
                js_venue_availability_latency_multiplier, 2);
  addGlobalFunc(ctx, "__lrvx_venue_availability_consume_wrong_side_recovery_bps",
                js_venue_availability_consume_wrong_side_recovery_bps, 1);
  addGlobalFunc(ctx, "__lrvx_venue_availability_schedule_outage",
                js_venue_availability_schedule_outage, 5);
  addGlobalFunc(ctx, "__lrvx_venue_availability_auto_random_outages",
                js_venue_availability_auto_random_outages, 5);
  addGlobalFunc(ctx, "__lrvx_venue_availability_is_up",
                js_venue_availability_is_up, 2);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_set_venue_availability",
                js_executor_set_venue_availability, 2);
  addGlobalFunc(ctx, "__lrvx_fee_schedule_create", js_fee_schedule_create, 0);
  addGlobalFunc(ctx, "__lrvx_fee_schedule_destroy", js_fee_schedule_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_fee_schedule_add_tier", js_fee_schedule_add_tier, 4);
  addGlobalFunc(ctx, "__lrvx_fee_schedule_load_profile",
                js_fee_schedule_load_profile, 2);
  addGlobalFunc(ctx, "__lrvx_liquidation_engine_create",
                js_liquidation_engine_create, 0);
  addGlobalFunc(ctx, "__lrvx_liquidation_engine_destroy",
                js_liquidation_engine_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_liquidation_engine_add_tier",
                js_liquidation_engine_add_tier, 3);
  addGlobalFunc(ctx, "__lrvx_liquidation_engine_set_insurance_fund_capital",
                js_liquidation_engine_set_insurance_fund_capital, 2);
  addGlobalFunc(ctx, "__lrvx_liquidation_engine_insurance_fund_balance",
                js_liquidation_engine_insurance_fund_balance, 1);
  addGlobalFunc(ctx, "__lrvx_liquidation_engine_set_adl_enabled",
                js_liquidation_engine_set_adl_enabled, 2);
  addGlobalFunc(ctx, "__lrvx_liquidation_engine_set_adl_ranking",
                js_liquidation_engine_set_adl_ranking, 2);
  addGlobalFunc(ctx, "__lrvx_liquidation_engine_adl_ranking",
                js_liquidation_engine_adl_ranking, 1);
  addGlobalFunc(ctx, "__lrvx_liquidation_engine_set_liquidation_slippage_bps",
                js_liquidation_engine_set_liquidation_slippage_bps, 2);
  addGlobalFunc(ctx, "__lrvx_liquidation_engine_open_position",
                js_liquidation_engine_open_position, 6);
  addGlobalFunc(ctx, "__lrvx_liquidation_engine_close_position",
                js_liquidation_engine_close_position, 3);
  addGlobalFunc(ctx, "__lrvx_liquidation_engine_on_mark",
                js_liquidation_engine_on_mark, 3);
  addGlobalFunc(ctx, "__lrvx_liquidation_engine_liquidations_count",
                js_liquidation_engine_liquidations_count, 1);
  addGlobalFunc(ctx, "__lrvx_liquidation_engine_insurance_payments_count",
                js_liquidation_engine_insurance_payments_count, 1);
  addGlobalFunc(ctx, "__lrvx_liquidation_engine_adl_closeouts_count",
                js_liquidation_engine_adl_closeouts_count, 1);
  addGlobalFunc(ctx, "__lrvx_liquidation_engine_load_profile",
                js_liquidation_engine_load_profile, 2);
  addGlobalFunc(ctx, "__lrvx_liquidation_engine_set_executor",
                js_liquidation_engine_set_executor, 2);
  addGlobalFunc(ctx, "__lrvx_liquidation_engine_ticks_to_first_adl",
                js_liquidation_engine_ticks_to_first_adl, 1);
  addGlobalFunc(ctx, "__lrvx_liquidation_engine_reset_stats",
                js_liquidation_engine_reset_stats, 1);
  addGlobalFunc(ctx, "__lrvx_liquidation_engine_deficits_paid_by_fund",
                js_liquidation_engine_deficits_paid_by_fund, 1);
  addGlobalFunc(ctx, "__lrvx_liquidation_engine_deficits_paid_by_adl",
                js_liquidation_engine_deficits_paid_by_adl, 1);
  addGlobalFunc(ctx, "__lrvx_liquidation_engine_cascade_sizes",
                js_liquidation_engine_cascade_sizes, 1);
  addGlobalFunc(ctx, "__lrvx_liquidation_engine_fund_balance_history",
                js_liquidation_engine_fund_balance_history, 1);
  addGlobalFunc(ctx, "__lrvx_liquidation_engine_set_mark_impact_model",
                js_liquidation_engine_set_mark_impact_model, 3);
  addGlobalFunc(ctx, "__lrvx_liquidation_engine_mark_impact_model",
                js_liquidation_engine_mark_impact_model, 1);
  addGlobalFunc(ctx, "__lrvx_liquidation_engine_mark_impact_weight",
                js_liquidation_engine_mark_impact_weight, 1);
  addGlobalFunc(ctx, "__lrvx_liquidation_engine_set_max_cascade_depth",
                js_liquidation_engine_set_max_cascade_depth, 2);
  addGlobalFunc(ctx, "__lrvx_liquidation_engine_max_cascade_depth",
                js_liquidation_engine_max_cascade_depth, 1);
  // Account + cross-margin bindings.
  addGlobalFunc(ctx, "__lrvx_account_create", js_account_create, 2);
  addGlobalFunc(ctx, "__lrvx_account_destroy", js_account_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_account_id", js_account_id, 1);
  addGlobalFunc(ctx, "__lrvx_account_equity", js_account_equity, 1);
  addGlobalFunc(ctx, "__lrvx_account_set_equity", js_account_set_equity, 2);
  addGlobalFunc(ctx, "__lrvx_account_add_equity", js_account_add_equity, 2);
  addGlobalFunc(ctx, "__lrvx_account_margin_mode", js_account_margin_mode, 1);
  addGlobalFunc(ctx, "__lrvx_account_set_margin_mode",
                js_account_set_margin_mode, 2);
  addGlobalFunc(ctx, "__lrvx_account_open_position",
                js_account_open_position, 4);
  addGlobalFunc(ctx, "__lrvx_account_close_position",
                js_account_close_position, 2);
  addGlobalFunc(ctx, "__lrvx_account_position_count",
                js_account_position_count, 1);
  addGlobalFunc(ctx, "__lrvx_account_set_mark", js_account_set_mark, 4);
  addGlobalFunc(ctx, "__lrvx_account_mark_ts", js_account_mark_ts, 2);
  addGlobalFunc(ctx, "__lrvx_account_has_stale_marks",
                js_account_has_stale_marks, 3);
  addGlobalFunc(ctx, "__lrvx_liquidation_engine_on_marks",
                js_liquidation_engine_on_marks, 3);
  addGlobalFunc(ctx, "__lrvx_account_total_notional",
                js_account_total_notional, 1);
  addGlobalFunc(ctx, "__lrvx_account_total_unrealised_pnl",
                js_account_total_upnl, 1);
  addGlobalFunc(ctx, "__lrvx_account_record_fill", js_account_record_fill, 4);
  addGlobalFunc(ctx, "__lrvx_account_rolling_notional_by_symbol",
                js_account_rolling_notional_by_symbol, 1);
  addGlobalFunc(ctx, "__lrvx_account_rolling_notional_30d",
                js_account_rolling_notional_30d, 1);
  addGlobalFunc(ctx, "__lrvx_account_reset_rolling",
                js_account_reset_rolling, 1);
  addGlobalFunc(ctx, "__lrvx_liquidation_engine_attach_account",
                js_liquidation_engine_attach_account, 2);
  addGlobalFunc(ctx, "__lrvx_liquidation_engine_detach_account",
                js_liquidation_engine_detach_account, 2);
  addGlobalFunc(ctx, "__lrvx_fee_schedule_bind_account",
                js_fee_schedule_bind_account, 2);
  addGlobalFunc(ctx, "__lrvx_fee_schedule_clear_account_binding",
                js_fee_schedule_clear_account_binding, 1);
  // VenueStack.
  addGlobalFunc(ctx, "__lrvx_venue_stack_create", js_venue_stack_create, 3);
  addGlobalFunc(ctx, "__lrvx_venue_stack_destroy", js_venue_stack_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_venue_stack_executor", js_venue_stack_executor, 1);
  addGlobalFunc(ctx, "__lrvx_venue_stack_account", js_venue_stack_account, 1);
  addGlobalFunc(ctx, "__lrvx_venue_stack_liquidation",
                js_venue_stack_liquidation, 1);
  addGlobalFunc(ctx, "__lrvx_venue_stack_fees", js_venue_stack_fees, 1);
  addGlobalFunc(ctx, "__lrvx_venue_stack_funding", js_venue_stack_funding, 1);
  addGlobalFunc(ctx, "__lrvx_venue_stack_venue", js_venue_stack_venue, 1);
  addGlobalFunc(ctx, "__lrvx_venue_stack_venue_name",
                js_venue_stack_venue_name, 1);
  addGlobalFunc(ctx, "__lrvx_fee_schedule_record_fill", js_fee_schedule_record_fill, 3);
  addGlobalFunc(ctx, "__lrvx_fee_schedule_fee_for", js_fee_schedule_fee_for, 4);
  addGlobalFunc(ctx, "__lrvx_fee_schedule_current_tier",
                js_fee_schedule_current_tier, 1);
  addGlobalFunc(ctx, "__lrvx_fee_schedule_rolling_notional",
                js_fee_schedule_rolling_notional, 1);
  addGlobalFunc(ctx, "__lrvx_fee_schedule_reset_rolling", js_fee_schedule_reset, 1);
  addGlobalFunc(ctx, "__lrvx_funding_schedule_create", js_funding_schedule_create, 0);
  addGlobalFunc(ctx, "__lrvx_funding_schedule_destroy", js_funding_schedule_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_funding_schedule_set_constant",
                js_funding_schedule_set_constant, 3);
  addGlobalFunc(ctx, "__lrvx_funding_schedule_load_profile",
                js_funding_schedule_load_profile, 2);
  addGlobalFunc(ctx, "__lrvx_funding_schedule_load_tape",
                js_funding_schedule_load_tape, 2);
  addGlobalFunc(ctx, "__lrvx_funding_schedule_set_tape_by_symbol",
                js_funding_schedule_set_tape_by_symbol, 4);
  addGlobalFunc(ctx, "__lrvx_funding_schedule_set_constant_rate",
                js_funding_schedule_set_constant_rate, 2);
  addGlobalFunc(ctx, "__lrvx_funding_schedule_reset", js_funding_schedule_reset, 1);
  addGlobalFunc(ctx, "__lrvx_funding_schedule_tick", js_funding_schedule_tick, 5);
  addGlobalFunc(ctx, "__lrvx_last_error", js_last_error, 0);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_on_trade_qty", js_executor_on_trade_qty, 5);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_on_best_levels", js_executor_on_best_levels, 6);
  addGlobalFunc(ctx, "__lrvx_simulated_executor_on_top_of_book", js_executor_on_top_of_book, 8);

  // Backtest result
  addGlobalFunc(ctx, "__lrvx_backtest_result_create", js_backtest_result_create, 6);
  addGlobalFunc(ctx, "__lrvx_backtest_result_destroy", js_backtest_result_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_backtest_result_record_fill",
                js_backtest_result_record_fill, 7);
  addGlobalFunc(ctx, "__lrvx_backtest_result_ingest", js_backtest_result_ingest, 2);
  addGlobalFunc(ctx, "__lrvx_backtest_result_stats", js_backtest_result_stats, 1);
  addGlobalFunc(ctx, "__lrvx_backtest_result_equity_curve",
                js_backtest_result_equity_curve, 1);
  addGlobalFunc(ctx, "__lrvx_backtest_result_write_csv", js_backtest_result_write_csv, 2);

  // Position tracker
  addGlobalFunc(ctx, "__lrvx_pos_create", js_pos_create, 1);
  addGlobalFunc(ctx, "__lrvx_pos_destroy", js_pos_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_pos_on_fill", js_pos_on_fill, 5);
  addGlobalFunc(ctx, "__lrvx_pos_position", js_pos_position, 2);
  addGlobalFunc(ctx, "__lrvx_pos_avg_entry", js_pos_avg_entry, 2);
  addGlobalFunc(ctx, "__lrvx_pos_pnl", js_pos_pnl, 2);
  addGlobalFunc(ctx, "__lrvx_pos_total_pnl", js_pos_total_pnl, 1);

  // Delta book compression
  addGlobalFunc(ctx, "__lrvx_delta_book_encoder_create", js_delta_book_encoder_create, 1);
  addGlobalFunc(ctx, "__lrvx_delta_book_encoder_destroy", js_delta_book_encoder_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_delta_book_encoder_encode", js_delta_book_encoder_encode, 4);
  addGlobalFunc(ctx, "__lrvx_delta_book_replayer_create", js_delta_book_replayer_create, 0);
  addGlobalFunc(ctx, "__lrvx_delta_book_replayer_destroy", js_delta_book_replayer_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_delta_book_replayer_apply", js_delta_book_replayer_apply, 5);

  addGlobalFunc(ctx, "__lrvx_run_recorder_create", js_run_recorder_create, 1);
  addGlobalFunc(ctx, "__lrvx_run_recorder_destroy", js_run_recorder_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_run_recorder_add_tape_ref", js_run_recorder_add_tape_ref, 2);
  addGlobalFunc(ctx, "__lrvx_run_recorder_set_run_ended_ns", js_run_recorder_set_run_ended_ns, 2);
  addGlobalFunc(ctx, "__lrvx_run_recorder_write_signal", js_run_recorder_write_signal, 2);
  addGlobalFunc(ctx, "__lrvx_run_recorder_write_order_event", js_run_recorder_write_order_event, 2);
  addGlobalFunc(ctx, "__lrvx_run_recorder_write_fill", js_run_recorder_write_fill, 2);
  addGlobalFunc(ctx, "__lrvx_run_recorder_close", js_run_recorder_close, 1);
  addGlobalFunc(ctx, "__lrvx_run_reader_open", js_run_reader_open, 1);
  addGlobalFunc(ctx, "__lrvx_run_reader_close", js_run_reader_close, 1);
  addGlobalFunc(ctx, "__lrvx_run_reader_strategy_id", js_run_reader_strategy_id, 1);
  addGlobalFunc(ctx, "__lrvx_run_reader_run_started_ns", js_run_reader_run_started_ns, 1);
  addGlobalFunc(ctx, "__lrvx_run_reader_run_ended_ns", js_run_reader_run_ended_ns, 1);
  addGlobalFunc(ctx, "__lrvx_run_reader_signals", js_run_reader_signals, 1);
  addGlobalFunc(ctx, "__lrvx_run_reader_orders", js_run_reader_orders, 1);
  addGlobalFunc(ctx, "__lrvx_run_reader_fills", js_run_reader_fills, 1);

  // Order group (multi-leg state machine; QuickJS class wraps these in
  // quickjs/lrvx/order_group.js so all four bindings share the engine).
  addGlobalFunc(ctx, "__lrvx_order_group_create", js_order_group_create, 2);
  addGlobalFunc(ctx, "__lrvx_order_group_destroy", js_order_group_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_order_group_add_market_leg", js_order_group_add_market_leg, 4);
  addGlobalFunc(ctx, "__lrvx_order_group_add_limit_leg", js_order_group_add_limit_leg, 5);
  addGlobalFunc(ctx, "__lrvx_order_group_leg_count", js_order_group_leg_count, 1);
  addGlobalFunc(ctx, "__lrvx_order_group_leg_state", js_order_group_leg_state, 2);
  addGlobalFunc(ctx, "__lrvx_order_group_leg_filled", js_order_group_leg_filled, 2);
  addGlobalFunc(ctx, "__lrvx_order_group_leg_order_id", js_order_group_leg_order_id, 2);
  addGlobalFunc(ctx, "__lrvx_order_group_record_submit", js_order_group_record_submit, 3);
  addGlobalFunc(ctx, "__lrvx_order_group_record_fill", js_order_group_record_fill, 3);
  addGlobalFunc(ctx, "__lrvx_order_group_record_cancel", js_order_group_record_cancel, 2);
  addGlobalFunc(ctx, "__lrvx_order_group_record_failure", js_order_group_record_failure, 2);
  addGlobalFunc(ctx, "__lrvx_order_group_record_replace_accepted",
                js_order_group_record_replace_accepted, 3);
  addGlobalFunc(ctx, "__lrvx_order_group_record_replace_rejected",
                js_order_group_record_replace_rejected, 2);
  addGlobalFunc(ctx, "__lrvx_order_group_find_leg_by_order_id",
                js_order_group_find_leg_by_order_id, 2);
  addGlobalFunc(ctx, "__lrvx_order_group_state", js_order_group_state, 1);
  addGlobalFunc(ctx, "__lrvx_order_group_recommended_actions",
                js_order_group_recommended_actions, 1);
  addGlobalFunc(ctx, "__lrvx_order_group_mark_action_dispatched",
                js_order_group_mark_action_dispatched, 3);
  addGlobalFunc(ctx, "__lrvx_order_group_set_risk_limits", js_order_group_set_risk_limits, 4);
  addGlobalFunc(ctx, "__lrvx_order_group_precheck_submission",
                js_order_group_precheck_submission, 3);
  addGlobalFunc(ctx, "__lrvx_order_group_set_pair_latency_budget_ns",
                js_order_group_set_pair_latency_budget_ns, 2);
  addGlobalFunc(ctx, "__lrvx_order_group_pair_latency_decision",
                js_order_group_pair_latency_decision, 4);

  // Live queue position estimator
  addGlobalFunc(ctx, "__lrvx_live_queue_position_create",
                js_live_queue_position_create, 0);
  addGlobalFunc(ctx, "__lrvx_live_queue_position_destroy",
                js_live_queue_position_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_live_queue_position_set_confidence_half_life_ns",
                js_live_queue_position_set_half_life, 2);
  addGlobalFunc(ctx, "__lrvx_live_queue_position_set_shrink_factor",
                js_live_queue_position_set_shrink_factor, 2);
  addGlobalFunc(ctx, "__lrvx_live_queue_position_on_order_placed",
                js_live_queue_position_on_order_placed, 8);
  addGlobalFunc(ctx, "__lrvx_live_queue_position_on_order_cancelled",
                js_live_queue_position_on_order_cancelled, 3);
  addGlobalFunc(ctx, "__lrvx_live_queue_position_on_order_filled",
                js_live_queue_position_on_order_filled, 4);
  addGlobalFunc(ctx, "__lrvx_live_queue_position_on_trade",
                js_live_queue_position_on_trade, 5);
  addGlobalFunc(ctx, "__lrvx_live_queue_position_on_trade_with_flag",
                js_live_queue_position_on_trade_with_flag, 6);
  addGlobalFunc(ctx, "__lrvx_live_queue_position_set_hidden_order_policy",
                js_live_queue_position_set_hidden_order_policy, 2);
  addGlobalFunc(ctx, "__lrvx_live_queue_position_on_level_update",
                js_live_queue_position_on_level_update, 6);
  addGlobalFunc(ctx, "__lrvx_live_queue_position_snapshot",
                js_live_queue_position_snapshot, 3);
  addGlobalFunc(ctx, "__lrvx_live_queue_position_tracked_count",
                js_live_queue_position_tracked_count, 1);

  // Bar dispatch recorder (cross-binding parity test fixture)
  addGlobalFunc(ctx, "__lrvx_bar_dispatch_recorder_create", js_bar_dispatch_recorder_create, 0);
  addGlobalFunc(ctx, "__lrvx_bar_dispatch_recorder_destroy", js_bar_dispatch_recorder_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_bar_dispatch_recorder_add_time_seconds",
                js_bar_dispatch_recorder_add_time_seconds, 2);
  addGlobalFunc(ctx, "__lrvx_bar_dispatch_recorder_on_trade",
                js_bar_dispatch_recorder_on_trade, 5);
  addGlobalFunc(ctx, "__lrvx_bar_dispatch_recorder_finalize",
                js_bar_dispatch_recorder_finalize, 1);
  addGlobalFunc(ctx, "__lrvx_bar_dispatch_recorder_count", js_bar_dispatch_recorder_count, 1);
  addGlobalFunc(ctx, "__lrvx_bar_dispatch_recorder_type_at",
                js_bar_dispatch_recorder_type_at, 2);
  addGlobalFunc(ctx, "__lrvx_bar_dispatch_recorder_param_at",
                js_bar_dispatch_recorder_param_at, 2);

  // Tape diff
  addGlobalFunc(ctx, "__lrvx_tape_diff", js_tape_diff, 3);

  // Execution algos (TWAP / VWAP / Iceberg / POV)
  addGlobalFunc(ctx, "__lrvx_exec_twap_create", js_exec_twap_create, 8);
  addGlobalFunc(ctx, "__lrvx_exec_vwap_create", js_exec_vwap_create, 6);
  addGlobalFunc(ctx, "__lrvx_exec_iceberg_create", js_exec_iceberg_create, 6);
  addGlobalFunc(ctx, "__lrvx_exec_pov_create", js_exec_pov_create, 7);
  addGlobalFunc(ctx, "__lrvx_exec_destroy", js_exec_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_exec_step", js_exec_step, 2);
  addGlobalFunc(ctx, "__lrvx_exec_report_fill", js_exec_report_fill, 2);
  addGlobalFunc(ctx, "__lrvx_exec_observe_volume", js_exec_observe_volume, 2);
  addGlobalFunc(ctx, "__lrvx_exec_submitted_qty", js_exec_submitted_qty, 1);
  addGlobalFunc(ctx, "__lrvx_exec_filled_qty", js_exec_filled_qty, 1);
  addGlobalFunc(ctx, "__lrvx_exec_remaining_qty", js_exec_remaining_qty, 1);
  addGlobalFunc(ctx, "__lrvx_exec_is_done", js_exec_is_done, 1);

  // Portfolio risk aggregator
  addGlobalFunc(ctx, "__lrvx_portfolio_risk_create", js_portfolio_risk_create, 2);
  addGlobalFunc(ctx, "__lrvx_portfolio_risk_destroy", js_portfolio_risk_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_portfolio_risk_update", js_portfolio_risk_update, 3);
  addGlobalFunc(ctx, "__lrvx_portfolio_risk_remove", js_portfolio_risk_remove, 2);
  addGlobalFunc(ctx, "__lrvx_portfolio_risk_reset", js_portfolio_risk_reset_kill_switch, 1);
  addGlobalFunc(ctx, "__lrvx_portfolio_risk_check_order", js_portfolio_risk_check_order, 4);
  addGlobalFunc(ctx, "__lrvx_portfolio_risk_snapshot", js_portfolio_risk_snapshot, 1);

  // Latency models (Phase 1 sampling primitive)
  addGlobalFunc(ctx, "__lrvx_lat_constant_create", js_lat_constant_create, 3);
  addGlobalFunc(ctx, "__lrvx_lat_gaussian_create", js_lat_gaussian_create, 7);
  addGlobalFunc(ctx, "__lrvx_lat_exponential_create", js_lat_exponential_create, 4);
  addGlobalFunc(ctx, "__lrvx_lat_empirical_create", js_lat_empirical_create, 4);
  addGlobalFunc(ctx, "__lrvx_lat_destroy", js_lat_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_lat_feed_delay", js_lat_feed_delay, 1);
  addGlobalFunc(ctx, "__lrvx_lat_order_delay", js_lat_order_delay, 1);
  addGlobalFunc(ctx, "__lrvx_lat_fill_delay", js_lat_fill_delay, 1);
  addGlobalFunc(ctx, "__lrvx_lat_sample", js_lat_sample, 1);
  addGlobalFunc(ctx, "__lrvx_lat_reset", js_lat_reset, 2);

  // Volume profile
  addGlobalFunc(ctx, "__lrvx_vprofile_create", js_vprofile_create, 1);
  addGlobalFunc(ctx, "__lrvx_vprofile_destroy", js_vprofile_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_vprofile_add_trade", js_vprofile_add_trade, 4);
  addGlobalFunc(ctx, "__lrvx_vprofile_poc", js_vprofile_poc, 1);
  addGlobalFunc(ctx, "__lrvx_vprofile_vah", js_vprofile_vah, 1);
  addGlobalFunc(ctx, "__lrvx_vprofile_val", js_vprofile_val, 1);
  addGlobalFunc(ctx, "__lrvx_vprofile_total_volume", js_vprofile_total_vol, 1);
  addGlobalFunc(ctx, "__lrvx_vprofile_clear", js_vprofile_clear, 1);

  // Stats
  addGlobalFunc(ctx, "__lrvx_stat_correlation", js_stat_correlation, 2);

  // Stat extras
  // L3 book
  addGlobalFunc(ctx, "__lrvx_l3_create", [](JSContext* c, JSValueConst, int, JSValueConst*) -> JSValue
                { return createHandleObject(c, lrvx_l3_book_create(), lrvx_l3_book_destroy); }, 0);
  addGlobalFunc(ctx, "__lrvx_l3_destroy", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                { return js_generic_handle_destroy(c, a[0]); }, 1);
  addGlobalFunc(ctx, "__lrvx_l3_add_order", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                { return JS_NewInt32(
                      c, lrvx_l3_book_add_order(
                             static_cast<LrvxL3BookHandle>(getHandle(c, a[0])),
                             static_cast<uint64_t>(toInt64(c, a[1])), toDouble(c, a[2]),
                             toDouble(c, a[3]), static_cast<uint8_t>(toUint32(c, a[4])))); }, 5);
  addGlobalFunc(ctx, "__lrvx_l3_remove_order", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                { return JS_NewInt32(
                      c, lrvx_l3_book_remove_order(
                             static_cast<LrvxL3BookHandle>(getHandle(c, a[0])),
                             static_cast<uint64_t>(toInt64(c, a[1])))); }, 2);
  addGlobalFunc(ctx, "__lrvx_l3_modify_order", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                { return JS_NewInt32(
                      c, lrvx_l3_book_modify_order(
                             static_cast<LrvxL3BookHandle>(getHandle(c, a[0])),
                             static_cast<uint64_t>(toInt64(c, a[1])), toDouble(c, a[2]))); }, 3);
  addGlobalFunc(ctx, "__lrvx_l3_best_bid", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                {
                  double p = 0;
                  if (lrvx_l3_book_best_bid(
                          static_cast<LrvxL3BookHandle>(getHandle(c, a[0])), &p)){
                    return JS_NewFloat64(c, p);
}
                  return JS_NULL; }, 1);
  addGlobalFunc(ctx, "__lrvx_l3_best_ask", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                {
                  double p = 0;
                  if (lrvx_l3_book_best_ask(
                          static_cast<LrvxL3BookHandle>(getHandle(c, a[0])), &p)){
                    return JS_NewFloat64(c, p);
}
                  return JS_NULL; }, 1);

  // Footprint (native, separate from volume profile)
  addGlobalFunc(ctx, "__lrvx_fp_create", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                { return createHandleObject(c, lrvx_footprint_create(toDouble(c, a[0])), lrvx_footprint_destroy); }, 1);
  addGlobalFunc(ctx, "__lrvx_fp_destroy", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                { return js_generic_handle_destroy(c, a[0]); }, 1);
  addGlobalFunc(ctx, "__lrvx_fp_add_trade", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                {
                  lrvx_footprint_add_trade(
                      static_cast<LrvxFootprintHandle>(getHandle(c, a[0])), toDouble(c, a[1]),
                      toDouble(c, a[2]), static_cast<uint8_t>(toUint32(c, a[3])));
                  return JS_UNDEFINED; }, 4);
  addGlobalFunc(ctx, "__lrvx_fp_total_delta", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                { return JS_NewFloat64(
                      c, lrvx_footprint_total_delta(
                             static_cast<LrvxFootprintHandle>(getHandle(c, a[0])))); }, 1);
  addGlobalFunc(ctx, "__lrvx_fp_total_volume", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                { return JS_NewFloat64(
                      c, lrvx_footprint_total_volume(
                             static_cast<LrvxFootprintHandle>(getHandle(c, a[0])))); }, 1);
  addGlobalFunc(ctx, "__lrvx_fp_clear", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                {
                  lrvx_footprint_clear(static_cast<LrvxFootprintHandle>(getHandle(c, a[0])));
                  return JS_UNDEFINED; }, 1);

  // Book level access
  addGlobalFunc(ctx, "__lrvx_book_get_bids", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                {
                  double prices[100], qtys[100];
                  uint32_t n = lrvx_book_get_bids(
                      static_cast<LrvxBookHandle>(getHandle(c, a[0])), prices, qtys,
                      std::min(toUint32(c, a[1]), 100u));
                  JSValue arr = JS_NewArray(c);
                  for (uint32_t i = 0; i < n; i++)
                  {
                    JSValue level = JS_NewArray(c);
                    JS_SetPropertyUint32(c, level, 0, JS_NewFloat64(c, prices[i]));
                    JS_SetPropertyUint32(c, level, 1, JS_NewFloat64(c, qtys[i]));
                    JS_SetPropertyUint32(c, arr, i, level);
                  }
                  return arr; }, 2);
  addGlobalFunc(ctx, "__lrvx_book_get_asks", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                {
                  double prices[100], qtys[100];
                  uint32_t n = lrvx_book_get_asks(
                      static_cast<LrvxBookHandle>(getHandle(c, a[0])), prices, qtys,
                      std::min(toUint32(c, a[1]), 100u));
                  JSValue arr = JS_NewArray(c);
                  for (uint32_t i = 0; i < n; i++)
                  {
                    JSValue level = JS_NewArray(c);
                    JS_SetPropertyUint32(c, level, 0, JS_NewFloat64(c, prices[i]));
                    JS_SetPropertyUint32(c, level, 1, JS_NewFloat64(c, qtys[i]));
                    JS_SetPropertyUint32(c, arr, i, level);
                  }
                  return arr; }, 2);

  // Bootstrap CI
  addGlobalFunc(ctx, "__lrvx_stat_bootstrap_ci", [](JSContext* c, JSValueConst, int argc, JSValueConst* a) -> JSValue
                {
                  auto data = jsArrayToDoubles(c, a[0]);
                  double conf = (argc > 1) ? toDouble(c, a[1]) : 0.95;
                  uint32_t samples = (argc > 2) ? toUint32(c, a[2]) : 10000;
                  double lo, med, hi;
                  lrvx_stat_bootstrap_ci(data.data(), data.size(), conf, samples, &lo, &med, &hi);
                  JSValue r = JS_NewObject(c);
                  JS_SetPropertyStr(c, r, "lower", JS_NewFloat64(c, lo));
                  JS_SetPropertyStr(c, r, "median", JS_NewFloat64(c, med));
                  JS_SetPropertyStr(c, r, "upper", JS_NewFloat64(c, hi));
                  return r; }, 3);

  // Permutation test
  addGlobalFunc(ctx, "__lrvx_stat_permutation_test", [](JSContext* c, JSValueConst, int argc, JSValueConst* a) -> JSValue
                {
                  auto g1 = jsArrayToDoubles(c, a[0]);
                  auto g2 = jsArrayToDoubles(c, a[1]);
                  uint32_t n = (argc > 2) ? toUint32(c, a[2]) : 10000;
                  return JS_NewFloat64(
                      c, lrvx_stat_permutation_test(g1.data(), g1.size(), g2.data(), g2.size(), n)); }, 3);

  // Segment ops
  addGlobalFunc(ctx, "__lrvx_segment_validate", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                {
                  LRVX_JS_CSTRING_OR_THROW(path, c, a[0]);
                  lrvx_clear_last_error();
                  uint8_t r = lrvx_segment_validate(path);
                  return JS_NewBool(c, r); }, 1);
  addGlobalFunc(ctx, "__lrvx_segment_merge", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                {
                  LRVX_JS_CSTRING_OR_THROW(dir, c, a[0]);
                  LRVX_JS_CSTRING_OR_THROW(out, c, a[1]);
                  lrvx_clear_last_error();
                  uint8_t r = lrvx_segment_merge(dir, out);
                  return JS_NewBool(c, r); }, 2);

  addGlobalFunc(ctx, "__lrvx_stat_profit_factor", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                {
                  auto d = jsArrayToDoubles(c, a[0]);
                  return JS_NewFloat64(c, lrvx_stat_profit_factor(d.data(), d.size())); }, 1);
  addGlobalFunc(ctx, "__lrvx_stat_win_rate", [](JSContext* c, JSValueConst, int, JSValueConst* a) -> JSValue
                {
                  auto d = jsArrayToDoubles(c, a[0]);
                  return JS_NewFloat64(c, lrvx_stat_win_rate(d.data(), d.size())); }, 1);

  // MarketProfile
  addGlobalFunc(ctx, "__lrvx_mp_create", js_mp_create, 3);
  addGlobalFunc(ctx, "__lrvx_mp_destroy", js_mp_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_mp_add_trade", js_mp_add_trade, 5);
  addGlobalFunc(ctx, "__lrvx_mp_poc", js_mp_poc, 1);
  addGlobalFunc(ctx, "__lrvx_mp_vah", js_mp_vah, 1);
  addGlobalFunc(ctx, "__lrvx_mp_val", js_mp_val, 1);
  addGlobalFunc(ctx, "__lrvx_mp_ib_high", js_mp_ib_high, 1);
  addGlobalFunc(ctx, "__lrvx_mp_ib_low", js_mp_ib_low, 1);
  addGlobalFunc(ctx, "__lrvx_mp_is_poor_high", js_mp_is_poor_high, 1);
  addGlobalFunc(ctx, "__lrvx_mp_is_poor_low", js_mp_is_poor_low, 1);
  addGlobalFunc(ctx, "__lrvx_mp_clear", js_mp_clear, 1);

  // CompositeBook
  addGlobalFunc(ctx, "__lrvx_cb_create", js_cb_create, 0);
  addGlobalFunc(ctx, "__lrvx_cb_destroy", js_cb_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_cb_apply_snapshot", js_cb_apply_snapshot, 8);
  addGlobalFunc(ctx, "__lrvx_cb_apply_delta", js_cb_apply_delta, 8);
  addGlobalFunc(ctx, "__lrvx_cb_best_bid", js_cb_best_bid, 2);
  addGlobalFunc(ctx, "__lrvx_cb_best_ask", js_cb_best_ask, 2);
  addGlobalFunc(ctx, "__lrvx_cb_has_arb", js_cb_has_arb, 2);
  addGlobalFunc(ctx, "__lrvx_cb_mark_stale", js_cb_mark_stale, 3);
  addGlobalFunc(ctx, "__lrvx_cb_check_staleness", js_cb_check_staleness, 3);

  // OrderTracker
  addGlobalFunc(ctx, "__lrvx_ot_create", js_ot_create, 0);
  addGlobalFunc(ctx, "__lrvx_ot_destroy", js_ot_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_ot_submit", js_ot_submit, 6);
  addGlobalFunc(ctx, "__lrvx_ot_filled", js_ot_filled, 3);
  addGlobalFunc(ctx, "__lrvx_ot_canceled", js_ot_canceled, 2);
  addGlobalFunc(ctx, "__lrvx_ot_is_active", js_ot_is_active, 2);
  addGlobalFunc(ctx, "__lrvx_ot_active_count", js_ot_active_count, 1);
  addGlobalFunc(ctx, "__lrvx_ot_total_count", js_ot_total_count, 1);
  addGlobalFunc(ctx, "__lrvx_ot_prune", js_ot_prune, 1);

  // OrderJourneyTracer
  addGlobalFunc(ctx, "__lrvx_ojt_create", js_ojt_create, 4);
  addGlobalFunc(ctx, "__lrvx_ojt_destroy", js_ojt_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_ojt_order_count", js_ojt_order_count, 1);
  addGlobalFunc(ctx, "__lrvx_ojt_record_count", js_ojt_record_count, 1);
  addGlobalFunc(ctx, "__lrvx_ojt_median_ack", js_ojt_median_ack, 1);
  addGlobalFunc(ctx, "__lrvx_ojt_median_ttf", js_ojt_median_ttf, 1);
  addGlobalFunc(ctx, "__lrvx_ojt_maker_ratio", js_ojt_maker_ratio, 1);
  addGlobalFunc(ctx, "__lrvx_ojt_cancel_race", js_ojt_cancel_race, 1);
  addGlobalFunc(ctx, "__lrvx_ojt_result", js_ojt_result, 1);
  addGlobalFunc(ctx, "__lrvx_ojt_journey", js_ojt_journey, 2);
  addGlobalFunc(ctx, "__lrvx_ojt_clear", js_ojt_clear, 1);

  // PositionGroupTracker
  addGlobalFunc(ctx, "__lrvx_pg_create", js_pg_create, 0);
  addGlobalFunc(ctx, "__lrvx_pg_destroy", js_pg_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_pg_open", js_pg_open, 6);
  addGlobalFunc(ctx, "__lrvx_pg_close", js_pg_close, 3);
  addGlobalFunc(ctx, "__lrvx_pg_partial_close", js_pg_partial_close, 4);
  addGlobalFunc(ctx, "__lrvx_pg_net", js_pg_net, 2);
  addGlobalFunc(ctx, "__lrvx_pg_pnl", js_pg_pnl, 2);
  addGlobalFunc(ctx, "__lrvx_pg_total_pnl", js_pg_total_pnl, 1);
  addGlobalFunc(ctx, "__lrvx_pg_open_count", js_pg_open_count, 2);
  addGlobalFunc(ctx, "__lrvx_pg_prune", js_pg_prune, 1);

  // DataWriter
  addGlobalFunc(ctx, "__lrvx_dw_create", js_dw_create, 3);
  addGlobalFunc(ctx, "__lrvx_dw_destroy", js_dw_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_dw_write_trade", js_dw_write_trade, 8);
  addGlobalFunc(ctx, "__lrvx_dw_write_book", js_dw_write_book, 10);
  addGlobalFunc(ctx, "__lrvx_dw_flush", js_dw_flush, 1);
  addGlobalFunc(ctx, "__lrvx_dw_close", js_dw_close, 1);
  addGlobalFunc(ctx, "__lrvx_dw_stats", js_dw_stats, 1);

  // DataReader
  addGlobalFunc(ctx, "__lrvx_dr_create", js_dr_create, 1);
  addGlobalFunc(ctx, "__lrvx_dr_create_filtered", js_dr_create_filtered, 4);
  addGlobalFunc(ctx, "__lrvx_dr_create_ex", js_dr_create_ex, 8);
  addGlobalFunc(ctx, "__lrvx_dr_destroy", js_dr_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_dr_count", js_dr_count, 1);
  addGlobalFunc(ctx, "__lrvx_dr_summary", js_dr_summary, 1);
  addGlobalFunc(ctx, "__lrvx_dr_stats", js_dr_stats, 1);
  addGlobalFunc(ctx, "__lrvx_dr_read_trades", js_dr_read_trades, 2);
  addGlobalFunc(ctx, "__lrvx_dr_read_bbo", js_dr_read_bbo, 2);
  addGlobalFunc(ctx, "__lrvx_dr_read_book_updates", js_dr_read_book_updates, 1);
  addGlobalFunc(ctx, "__lrvx_dr_read_trades_from", js_dr_read_trades_from, 3);
  addGlobalFunc(ctx, "__lrvx_dr_read_bbo_from", js_dr_read_bbo_from, 3);
  addGlobalFunc(ctx, "__lrvx_dr_read_book_updates_from", js_dr_read_book_updates_from, 2);

  // MergedTapeReader
  addGlobalFunc(ctx, "__lrvx_mtr_create", js_mtr_create, 4);
  addGlobalFunc(ctx, "__lrvx_mtr_destroy", js_mtr_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_mtr_symbol_count", js_mtr_symbol_count, 1);
  addGlobalFunc(ctx, "__lrvx_mtr_get_symbols", js_mtr_get_symbols, 1);
  addGlobalFunc(ctx, "__lrvx_mtr_tape_count", js_mtr_tape_count, 1);
  addGlobalFunc(ctx, "__lrvx_mtr_get_tape_stats", js_mtr_get_tape_stats, 1);
  addGlobalFunc(ctx, "__lrvx_mtr_time_range", js_mtr_time_range, 1);
  addGlobalFunc(ctx, "__lrvx_mtr_count_trades", js_mtr_count_trades, 1);
  addGlobalFunc(ctx, "__lrvx_mtr_read_trades", js_mtr_read_trades, 2);
  addGlobalFunc(ctx, "__lrvx_mtr_count_books", js_mtr_count_books, 1);
  addGlobalFunc(ctx, "__lrvx_mtr_read_books", js_mtr_read_books, 1);

  // BinaryLogRecorderHook
  addGlobalFunc(ctx, "__lrvx_blrh_create", js_blrh_create, 6);
  addGlobalFunc(ctx, "__lrvx_blrh_destroy", js_blrh_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_blrh_as_recorder", js_blrh_as_recorder, 1);
  addGlobalFunc(ctx, "__lrvx_blrh_add_symbol", js_blrh_add_symbol, 7);
  addGlobalFunc(ctx, "__lrvx_blrh_flush", js_blrh_flush, 1);
  addGlobalFunc(ctx, "__lrvx_blrh_stats", js_blrh_stats, 1);

  // MarketDataRecorder handle drivers — push lifecycle + trade events
  // directly through the recorder callbacks, no Runner required.
  addGlobalFunc(ctx, "__lrvx_recorder_on_start", js_recorder_on_start, 1);
  addGlobalFunc(ctx, "__lrvx_recorder_on_stop", js_recorder_on_stop, 1);
  addGlobalFunc(ctx, "__lrvx_recorder_on_trade", js_recorder_on_trade, 6);

  // Partitioner
  addGlobalFunc(ctx, "__lrvx_part_create", js_part_create, 1);
  addGlobalFunc(ctx, "__lrvx_part_destroy", js_part_destroy, 1);
  addGlobalFunc(ctx, "__lrvx_part_by_time", js_part_by_time, 3);
  addGlobalFunc(ctx, "__lrvx_part_by_duration", js_part_by_duration, 3);
  addGlobalFunc(ctx, "__lrvx_part_by_calendar", js_part_by_calendar, 3);
  addGlobalFunc(ctx, "__lrvx_part_by_symbol", js_part_by_symbol, 2);
  addGlobalFunc(ctx, "__lrvx_part_per_symbol", js_part_per_symbol, 1);
  addGlobalFunc(ctx, "__lrvx_part_by_event_count", js_part_by_event_count, 2);

  // Aggregators
  addGlobalFunc(ctx, "__lrvx_agg_time", js_agg_time, 5);
  addGlobalFunc(ctx, "__lrvx_agg_tick", js_agg_tick, 5);
  addGlobalFunc(ctx, "__lrvx_agg_volume", js_agg_volume, 5);
  addGlobalFunc(ctx, "__lrvx_agg_range", js_agg_range, 5);
  addGlobalFunc(ctx, "__lrvx_agg_renko", js_agg_renko, 5);
  addGlobalFunc(ctx, "__lrvx_agg_heikin", js_agg_heikin, 5);

  // Extended segment ops
  addGlobalFunc(ctx, "__lrvx_seg_merge_dir", js_seg_merge_dir, 2);
  addGlobalFunc(ctx, "__lrvx_seg_split", js_seg_split, 5);
  addGlobalFunc(ctx, "__lrvx_seg_export", js_seg_export, 6);
  addGlobalFunc(ctx, "__lrvx_seg_validate_full", js_seg_validate_full, 3);
  addGlobalFunc(ctx, "__lrvx_dataset_validate", js_dataset_validate, 1);
  addGlobalFunc(ctx, "__lrvx_seg_recompress", js_seg_recompress, 3);
  addGlobalFunc(ctx, "__lrvx_seg_extract_symbols", js_seg_extract_symbols, 3);
  addGlobalFunc(ctx, "__lrvx_seg_extract_time", js_seg_extract_time, 4);

  // CSV loader
  addGlobalFunc(ctx, "__lrvx_load_csv", js_load_csv, 1);

  // The ABI pair the handshake above compared, so a script can report
  // what the engine it is running inside was built against.
  addGlobalFunc(ctx, "__lrvx_capi_abi_version", js_capi_abi_version, 0);
  {
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "__LRVX_CAPI_ABI_VERSION",
                      JS_NewUint32(ctx, LRVX_CAPI_ABI_VERSION));
    JS_FreeValue(ctx, g);
  }

  return true;
}

}  // namespace lrvx
