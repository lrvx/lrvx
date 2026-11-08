#include "js_bindings.h"
#include "js_engine.h"
#include "js_executor.h"
#include "js_strategy.h"

#include "lrvx/backtest/backtest_result.h"
#include "lrvx/backtest/simulated_clock.h"
#include "lrvx/backtest/simulated_executor.h"
#include "lrvx/book/events/book_update_event.h"
#include "lrvx/capi/bridge_strategy.h"
#include "lrvx/capi/lrvx_capi.h"

#include <gtest/gtest.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory_resource>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace lrvx;

// A stand-in for the library's own lrvx_capi_abi_version, so a test can
// construct the header/library skew registerLrvxBindings refuses. One
// build tree has one version, and the refusal path is unreachable without
// this; the executable's definition satisfies the reference liblrvx_capi
// leaves undefined, including the one inside js_bindings.cpp.
namespace
{
uint32_t g_reportedAbiVersion = LRVX_CAPI_ABI_VERSION;

class ReportedAbiVersion
{
 public:
  explicit ReportedAbiVersion(uint32_t version) : _saved(g_reportedAbiVersion)
  {
    g_reportedAbiVersion = version;
  }
  ~ReportedAbiVersion() { g_reportedAbiVersion = _saved; }

  ReportedAbiVersion(const ReportedAbiVersion&) = delete;
  ReportedAbiVersion& operator=(const ReportedAbiVersion&) = delete;

 private:
  uint32_t _saved;
};
}  // namespace

extern "C" uint32_t lrvx_capi_abi_version(void)
{
  return g_reportedAbiVersion;
}

// ============================================================
// Unit tests — engine basics
// ============================================================

TEST(JsEngineTest, CreateAndEval)
{
  LrvxJsEngine engine;
  EXPECT_TRUE(engine.eval("var x = 1 + 2;"));
}

TEST(JsEngineTest, EvalError)
{
  LrvxJsEngine engine;
  EXPECT_FALSE(engine.eval("throw new Error('test error');"));
  auto msg = engine.getErrorMessage();
  EXPECT_TRUE(msg.find("test error") != std::string::npos);
}

TEST(JsEngineTest, GlobalProperty)
{
  LrvxJsEngine engine;
  EXPECT_TRUE(engine.eval("var testVal = 42;"));

  JSValue val = engine.getGlobalProperty("testVal");
  int32_t result = 0;
  JS_ToInt32(engine.context(), &result, val);
  EXPECT_EQ(result, 42);
  JS_FreeValue(engine.context(), val);
}

TEST(JsEngineTest, BindingsRegister)
{
  LrvxJsEngine engine;
  registerLrvxBindings(engine.context());

  JSValue fn = engine.getGlobalProperty("__lrvx_emit_market_buy");
  EXPECT_TRUE(JS_IsFunction(engine.context(), fn));
  JS_FreeValue(engine.context(), fn);

  // console.log should exist
  JSValue console = engine.getGlobalProperty("console");
  EXPECT_FALSE(JS_IsUndefined(console));
  JS_FreeValue(engine.context(), console);
}

TEST(JsEngineTest, SMA)
{
  LrvxJsEngine engine;
  EXPECT_TRUE(engine.eval(R"(
    class SMA {
      constructor(period) { this._period = period; this._count = 0; this._sum = 0; this._value = 0; this._buffer = []; this._index = 0; }
      update(v) { if (this._count < this._period) { this._buffer.push(v); this._sum += v; this._count++; } else { this._sum -= this._buffer[this._index]; this._buffer[this._index] = v; this._sum += v; this._index = (this._index + 1) % this._period; } this._value = this._sum / this._count; return this._value; }
      get value() { return this._value; }
      get ready() { return this._count >= this._period; }
    }
    var sma = new SMA(3);
    sma.update(10); sma.update(20); sma.update(30);
    var ready = sma.ready;
    var avg = sma.value;
  )"));

  JSValue avg = engine.getGlobalProperty("avg");
  double avgVal = 0;
  JS_ToFloat64(engine.context(), &avgVal, avg);
  EXPECT_DOUBLE_EQ(avgVal, 20.0);
  JS_FreeValue(engine.context(), avg);

  JSValue ready = engine.getGlobalProperty("ready");
  EXPECT_TRUE(JS_ToBool(engine.context(), ready));
  JS_FreeValue(engine.context(), ready);
}

// ============================================================
// Helper: write a temp JS file
// ============================================================

class TempJsFile
{
 public:
  TempJsFile(const std::string& content)
  {
    _path = std::filesystem::temp_directory_path() / ("lrvx_test_" + std::to_string(counter_++) + ".js");
    std::ofstream f(_path);
    f << content;
  }
  ~TempJsFile() { std::filesystem::remove(_path); }
  std::string path() const { return _path.string(); }
  std::filesystem::path _path;
  static int counter_;
};
int TempJsFile::counter_ = 0;

// ============================================================
// Integration tests — full strategy lifecycle
// ============================================================

TEST(JsIntegrationTest, TargetsBindings)
{
  TempJsFile script(R"(
    var fr = lrvx.targets.future_return([100.0, 101.0, 99.0, 105.0, 110.0], 2);
    var fr0 = fr[0];
    var fr2 = fr[2];
    var fr_tail_nan = isNaN(fr[3]) && isNaN(fr[4]);

    var constClose = [];
    for (var i = 0; i < 20; i++) constClose.push(100.0);
    var vol = lrvx.targets.future_ctc_volatility(constClose, 5);
    var vol0 = vol[0];

    var lin = [];
    for (var i = 0; i < 20; i++) lin.push(100.0 + 0.5 * i);
    var sl = lrvx.targets.future_linear_slope(lin, 4);
    var sl0 = sl[0];
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);

  auto getNum = [&](const char* name)
  {
    JSValue v = jsStrat.engine().getGlobalProperty(name);
    double d = 0;
    JS_ToFloat64(jsStrat.engine().context(), &d, v);
    JS_FreeValue(jsStrat.engine().context(), v);
    return d;
  };

  EXPECT_NEAR(getNum("fr0"), 99.0 / 100.0 - 1.0, 1e-12);
  EXPECT_NEAR(getNum("fr2"), 110.0 / 99.0 - 1.0, 1e-12);
  EXPECT_NEAR(getNum("vol0"), 0.0, 1e-12);
  EXPECT_NEAR(getNum("sl0"), 0.5, 1e-12);

  JSValue tail = jsStrat.engine().getGlobalProperty("fr_tail_nan");
  EXPECT_TRUE(JS_ToBool(jsStrat.engine().context(), tail));
  JS_FreeValue(jsStrat.engine().context(), tail);
}

TEST(JsEngineTest, AdfBindingExposed)
{
  LrvxJsEngine engine;
  registerLrvxBindings(engine.context());

  // Build a deterministic random walk and call __lrvx_indicator_adf.
  EXPECT_TRUE(engine.eval(R"(
    var n = 200;
    var seed = 42;
    function rand() { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return ((seed + 1) / 0x80000000); }
    function gauss() {
      var u1 = rand();
      var u2 = rand();
      return Math.sqrt(-2 * Math.log(u1)) * Math.cos(2 * Math.PI * u2);
    }
    var walk = [0];
    for (var i = 1; i < n; ++i) walk.push(walk[i-1] + gauss());
    var r = __lrvx_indicator_adf(walk, 4, "c");
    var test_stat = r.test_stat;
    var p_value = r.p_value;
    var used_lag = r.used_lag;
  )"));

  JSValue ts = engine.getGlobalProperty("test_stat");
  double tsVal = 0;
  JS_ToFloat64(engine.context(), &tsVal, ts);
  JS_FreeValue(engine.context(), ts);
  EXPECT_TRUE(std::isfinite(tsVal));

  JSValue ul = engine.getGlobalProperty("used_lag");
  uint32_t ulVal = 0;
  JS_ToUint32(engine.context(), &ulVal, ul);
  JS_FreeValue(engine.context(), ul);
  EXPECT_LE(ulVal, 4u);
}

TEST(JsIntegrationTest, AutoCorrelationBindings)
{
  TempJsFile script(R"(
    var linear = [];
    for (var i = 0; i < 50; ++i) linear.push(5.0 + 0.7 * i);

    // Batch: AutoCorrelation.compute / __lrvx_indicator_autocorrelation.
    var ac = AutoCorrelation.compute(linear, 10, 1);
    var batch_at_10 = ac[10];
    var warmup_nan = isNaN(ac[9]);

    // Streaming class.
    var stream = new AutoCorrelation(10, 1);
    var lastStream = NaN;
    for (var j = 0; j < linear.length; ++j) {
      lastStream = stream.update(linear[j]);
    }
    var stream_eq_batch = Math.abs(lastStream - ac[ac.length - 1]) < 1e-9;
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);

  auto* ctx = jsStrat.engine().context();

  JSValue v = jsStrat.engine().getGlobalProperty("batch_at_10");
  double d = 0;
  JS_ToFloat64(ctx, &d, v);
  JS_FreeValue(ctx, v);
  EXPECT_NEAR(d, 1.0, 1e-10);

  JSValue w = jsStrat.engine().getGlobalProperty("warmup_nan");
  EXPECT_TRUE(JS_ToBool(ctx, w));
  JS_FreeValue(ctx, w);

  JSValue eq = jsStrat.engine().getGlobalProperty("stream_eq_batch");
  EXPECT_TRUE(JS_ToBool(ctx, eq));
  JS_FreeValue(ctx, eq);
}

TEST(JsIntegrationTest, IndicatorGraphBindings)
{
  TempJsFile script(R"(
    var ramp = [];
    for (var i = 0; i < 50; ++i) ramp.push(i);

    var g = new lrvx.IndicatorGraph();
    g.setBars(0, ramp);

    g.addNode("ema5", [], function(graph, sym) {
      return SMA.compute(graph.close(sym), 5);
    });
    g.addNode("sma5", [], function(graph, sym) {
      return SMA.compute(graph.close(sym), 5);
    });
    g.addNode("diff", ["ema5", "sma5"], function(graph, sym) {
      var a = graph.get(sym, "ema5");
      var b = graph.get(sym, "sma5");
      var out = [];
      for (var i = 0; i < a.length; ++i) out.push(a[i] - b[i]);
      return out;
    });

    var ema5 = g.require(0, "ema5");
    var diff = g.require(0, "diff");
    var ema5_len = ema5.length;
    var diff_len = diff.length;
    var sma5_cached = g.get(0, "sma5");
    var sma5_not_null = sma5_cached !== null;

    var threw = false;
    try { g.require(0, "missing"); } catch (e) { threw = true; }
    var require_throws = threw;

    g.destroy();
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);

  auto* ctx = jsStrat.engine().context();

  auto getInt = [&](const char* name)
  {
    JSValue v = jsStrat.engine().getGlobalProperty(name);
    int32_t i = 0;
    JS_ToInt32(ctx, &i, v);
    JS_FreeValue(ctx, v);
    return i;
  };
  auto getBool = [&](const char* name)
  {
    JSValue v = jsStrat.engine().getGlobalProperty(name);
    bool b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
  };

  EXPECT_EQ(getInt("ema5_len"), 50);
  EXPECT_EQ(getInt("diff_len"), 50);
  EXPECT_TRUE(getBool("sma5_not_null"));
  EXPECT_TRUE(getBool("require_throws"));
}

TEST(JsIntegrationTest, StreamingIndicatorGraph)
{
  TempJsFile script(R"(
    var sg = new lrvx.StreamingIndicatorGraph();
    sg.addNode("double_close", [], function(graph, sym) {
      var c = graph.close(sym);
      var out = [];
      for (var i = 0; i < c.length; ++i) out.push(c[i] * 2.0);
      return out;
    });

    var closes = [10.0, 20.0, 30.0, 40.0, 50.0];
    var lastDouble = 0;
    var barCounts = [];
    for (var i = 0; i < closes.length; ++i) {
      sg.step(0, closes[i]);
      lastDouble = sg.current(0, "double_close");
      barCounts.push(sg.barCount(0));
    }

    // After 5 steps: current == last close * 2, bar counts 1..5.
    var ok_last = Math.abs(lastDouble - 100.0) < 1e-9;
    var ok_counts = barCounts[0] === 1 && barCounts[4] === 5;

    // Parity: batch on same data should match.
    var bg = new lrvx.IndicatorGraph();
    bg.setBars(0, new Float64Array(closes));
    bg.addNode("double_close", [], function(graph, sym) {
      var c = graph.close(sym);
      var out = [];
      for (var i = 0; i < c.length; ++i) out.push(c[i] * 2.0);
      return out;
    });
    var batchOut = bg.require(0, "double_close");
    var parity = Math.abs(batchOut[batchOut.length - 1] - lastDouble) < 1e-9;
    bg.destroy();

    // Reset and verify bar count resets.
    sg.reset(0);
    var after_reset_count = sg.barCount(0);
    var after_reset_nan = isNaN(sg.current(0, "double_close"));
    sg.destroy();
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  auto* ctx = jsStrat.engine().context();

  auto getBool = [&](const char* name)
  {
    JSValue v = jsStrat.engine().getGlobalProperty(name);
    bool b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
  };
  auto getInt = [&](const char* name)
  {
    JSValue v = jsStrat.engine().getGlobalProperty(name);
    int32_t i = 0;
    JS_ToInt32(ctx, &i, v);
    JS_FreeValue(ctx, v);
    return i;
  };

  EXPECT_TRUE(getBool("ok_last"));
  EXPECT_TRUE(getBool("ok_counts"));
  EXPECT_TRUE(getBool("parity"));
  EXPECT_EQ(getInt("after_reset_count"), 0);
  EXPECT_TRUE(getBool("after_reset_nan"));
}

TEST(JsIntegrationTest, LoadStrategyAndResolveSymbols)
{
  TempJsFile script(R"(
    class TestStrat extends Strategy {
      constructor() {
        super({ exchange: "Binance", symbols: ["BTCUSDT", "ETHUSDT"] });
      }
    }
    lrvx.register(new TestStrat());
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);

  EXPECT_EQ(jsStrat.symbolIds().size(), 2u);
  // Symbols should have been registered in the registry
  EXPECT_EQ(registry.size(), 2u);

  auto btcId = registry.getSymbolId("Binance", "BTCUSDT");
  ASSERT_TRUE(btcId.has_value());
  EXPECT_EQ(jsStrat.symbolIds()[0], btcId.value());

  auto ethId = registry.getSymbolId("Binance", "ETHUSDT");
  ASSERT_TRUE(ethId.has_value());
  EXPECT_EQ(jsStrat.symbolIds()[1], ethId.value());
}

TEST(JsIntegrationTest, OnStartOnStopCalled)
{
  TempJsFile script(R"(
    var startCalled = false;
    var stopCalled = false;
    class TestStrat extends Strategy {
      constructor() { super({ exchange: "Test", symbols: ["SYM1"] }); }
      onStart() { startCalled = true; }
      onStop()  { stopCalled = true; }
    }
    lrvx.register(new TestStrat());
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();

  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<LrvxStrategyHandle>(bridge.get()));

  bridge->start();
  bridge->stop();

  JSValue started = jsStrat.engine().getGlobalProperty("startCalled");
  EXPECT_TRUE(JS_ToBool(jsStrat.engine().context(), started));
  JS_FreeValue(jsStrat.engine().context(), started);

  JSValue stopped = jsStrat.engine().getGlobalProperty("stopCalled");
  EXPECT_TRUE(JS_ToBool(jsStrat.engine().context(), stopped));
  JS_FreeValue(jsStrat.engine().context(), stopped);
}

TEST(JsIntegrationTest, OnTradeReceivesCorrectData)
{
  TempJsFile script(R"(
    var lastSymbol = "";
    var lastPrice = 0;
    var lastSide = "";
    var tradeCount = 0;

    class TestStrat extends Strategy {
      constructor() { super({ exchange: "Test", symbols: ["BTCUSDT"] }); }
      onTrade(ctx, trade) {
        lastSymbol = trade.symbol;
        lastPrice = trade.price;
        lastSide = trade.side;
        tradeCount++;
      }
    }
    lrvx.register(new TestStrat());
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();

  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<LrvxStrategyHandle>(bridge.get()));

  // Simulate a trade event through the C callback
  LrvxSymbolContext fctx{};
  fctx.symbol_id = symIds[0];

  LrvxTradeData ftrade{};
  ftrade.symbol = symIds[0];
  ftrade.price_raw = lrvx_price_from_double(50123.45);
  ftrade.quantity_raw = lrvx_quantity_from_double(1.5);
  ftrade.is_buy = 1;
  ftrade.exchange_ts_ns = 1000000000;

  callbacks.on_trade(callbacks.user_data, &fctx, &ftrade);

  auto* ctx = jsStrat.engine().context();

  JSValue count = jsStrat.engine().getGlobalProperty("tradeCount");
  int32_t countVal = 0;
  JS_ToInt32(ctx, &countVal, count);
  EXPECT_EQ(countVal, 1);
  JS_FreeValue(ctx, count);

  JSValue sym = jsStrat.engine().getGlobalProperty("lastSymbol");
  const char* symStr = JS_ToCString(ctx, sym);
  EXPECT_STREQ(symStr, "BTCUSDT");
  JS_FreeCString(ctx, symStr);
  JS_FreeValue(ctx, sym);

  JSValue price = jsStrat.engine().getGlobalProperty("lastPrice");
  double priceVal = 0;
  JS_ToFloat64(ctx, &priceVal, price);
  EXPECT_NEAR(priceVal, 50123.45, 0.01);
  JS_FreeValue(ctx, price);

  JSValue side = jsStrat.engine().getGlobalProperty("lastSide");
  const char* sideStr = JS_ToCString(ctx, side);
  EXPECT_STREQ(sideStr, "buy");
  JS_FreeCString(ctx, sideStr);
  JS_FreeValue(ctx, side);
}

TEST(JsIntegrationTest, MultipleTrades)
{
  TempJsFile script(R"(
    var tradeCount = 0;

    class TestStrat extends Strategy {
      constructor() { super({ exchange: "Test", symbols: ["SYM1"] }); }
      onTrade(ctx, trade) { tradeCount++; }
    }
    lrvx.register(new TestStrat());
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();

  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<LrvxStrategyHandle>(bridge.get()));

  // Feed 100 trades
  for (int i = 0; i < 100; i++)
  {
    LrvxSymbolContext fctx{};
    fctx.symbol_id = symIds[0];
    LrvxTradeData ftrade{};
    ftrade.symbol = symIds[0];
    ftrade.price_raw = lrvx_price_from_double(100.0 + i);
    ftrade.quantity_raw = lrvx_quantity_from_double(1.0);
    ftrade.is_buy = (i % 2 == 0) ? 1 : 0;
    callbacks.on_trade(callbacks.user_data, &fctx, &ftrade);
  }

  JSValue count = jsStrat.engine().getGlobalProperty("tradeCount");
  int32_t countVal = 0;
  JS_ToInt32(jsStrat.engine().context(), &countVal, count);
  EXPECT_EQ(countVal, 100);
  JS_FreeValue(jsStrat.engine().context(), count);
}

TEST(JsIntegrationTest, QualifiedSymbolFormat)
{
  TempJsFile script(R"(
    class TestStrat extends Strategy {
      constructor() {
        super({ symbols: ["Binance:BTCUSDT", "Bybit:ETHUSDT"] });
      }
    }
    lrvx.register(new TestStrat());
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);

  EXPECT_EQ(jsStrat.symbolIds().size(), 2u);
  auto btcId = registry.getSymbolId("Binance", "BTCUSDT");
  ASSERT_TRUE(btcId.has_value());
  auto ethId = registry.getSymbolId("Bybit", "ETHUSDT");
  ASSERT_TRUE(ethId.has_value());
}

TEST(JsIntegrationTest, NoExchangeThrows)
{
  TempJsFile script(R"(
    class TestStrat extends Strategy {
      constructor() { super({ symbols: ["BTCUSDT"] }); }
    }
    lrvx.register(new TestStrat());
  )");

  SymbolRegistry registry;
  EXPECT_THROW(LrvxJsStrategy(script.path(), registry), std::runtime_error);
}

TEST(JsIntegrationTest, NoRegisterOk)
{
  // Plain scripts without lrvx.register() are valid (standalone script use case)
  TempJsFile script("var x = 1;");

  SymbolRegistry registry;
  EXPECT_NO_THROW(LrvxJsStrategy(script.path(), registry));
}

// ============================================================
// Composite-condition DSL
//
// `when(strategy, sym, barType, param).ema(50).gt(when(...).ema(200))`
// builds a tree out of indicator nodes + comparison/logical wrappers.
// Pure JS sugar over `lastNClosedBars`; no engine state.
// ============================================================

TEST(JsIntegrationTest, CompositeDslCrossover)
{
  TempJsFile script(R"(
    var crossUpReady = false;
    var crossUpValue = false;
    var rsiReady = false;
    var rsiValue = 0;

    class TestStrat extends Strategy {
      constructor() { super({ exchange: "Test", symbols: ["BTCUSDT"] }); }
      onBar(ctx, bar) {
        var TIME = 0;
        var M1 = 60 * 1000000000;
        var fast = when(this, "BTCUSDT", TIME, M1).ema(3);
        var slow = when(this, "BTCUSDT", TIME, M1).ema(6);
        var cross = fast.gt(slow);
        crossUpReady = cross.isReady();
        crossUpValue = cross.isReady() ? cross.value() : false;

        var rsi = when(this, "BTCUSDT", TIME, M1).rsi(3);
        rsiReady = rsi.isReady();
        rsiValue = rsi.isReady() ? rsi.value() : 0;
      }
    }
    lrvx.register(new TestStrat());
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();

  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<LrvxStrategyHandle>(bridge.get()));

  const uint64_t M1_NS = 60ull * 1'000'000'000ull;
  // Climb from 100 → 110 over 8 bars so the fast EMA(3) ends well above
  // the slow EMA(6).
  for (int i = 0; i < 8; ++i)
  {
    BarEvent ev{};
    ev.symbol = symIds[0];
    ev.barType = BarType::Time;
    ev.barTypeParam = M1_NS;
    double price = 100.0 + i * 1.5;
    ev.bar.open = Price::fromDouble(price);
    ev.bar.close = Price::fromDouble(price + 0.5);
    ev.bar.high = Price::fromDouble(price + 1.0);
    ev.bar.low = Price::fromDouble(price - 0.5);
    ev.bar.startTime = TimePoint{std::chrono::nanoseconds{static_cast<int64_t>(M1_NS) * i}};
    ev.bar.endTime = TimePoint{std::chrono::nanoseconds{static_cast<int64_t>(M1_NS) * (i + 1)}};
    bridge->onBar(ev);
  }

  auto* ctx = jsStrat.engine().context();

  JSValue ready = jsStrat.engine().getGlobalProperty("crossUpReady");
  EXPECT_TRUE(JS_ToBool(ctx, ready));
  JS_FreeValue(ctx, ready);

  JSValue val = jsStrat.engine().getGlobalProperty("crossUpValue");
  EXPECT_TRUE(JS_ToBool(ctx, val));
  JS_FreeValue(ctx, val);

  JSValue rready = jsStrat.engine().getGlobalProperty("rsiReady");
  EXPECT_TRUE(JS_ToBool(ctx, rready));
  JS_FreeValue(ctx, rready);

  JSValue rval = jsStrat.engine().getGlobalProperty("rsiValue");
  double rsiV = 0;
  JS_ToFloat64(ctx, &rsiV, rval);
  // Climbing series → RSI well above 50, ideally near 100.
  EXPECT_GT(rsiV, 80.0);
  JS_FreeValue(ctx, rval);
}

TEST(JsIntegrationTest, CompositeDslLogicalOps)
{
  TempJsFile script(R"(
    var andReady = false;
    var andValue = false;
    var orValue = false;
    var notValue = false;

    class TestStrat extends Strategy {
      constructor() { super({ exchange: "Test", symbols: ["BTCUSDT"] }); }
      onBar(ctx, bar) {
        var M1 = 60 * 1000000000;
        var fast = when(this, "BTCUSDT", 0, M1).ema(3);
        var slow = when(this, "BTCUSDT", 0, M1).ema(6);
        var crossUp = fast.gt(slow);
        var aboveHundred = fast.gt(99);

        var both = crossUp.and(aboveHundred);
        var either = crossUp.or(aboveHundred);
        var notCross = crossUp.not();

        andReady = both.isReady();
        andValue = both.isReady() ? both.value() : false;
        orValue = either.isReady() ? either.value() : false;
        notValue = notCross.isReady() ? notCross.value() : false;
      }
    }
    lrvx.register(new TestStrat());
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();

  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<LrvxStrategyHandle>(bridge.get()));

  const uint64_t M1_NS = 60ull * 1'000'000'000ull;
  for (int i = 0; i < 8; ++i)
  {
    BarEvent ev{};
    ev.symbol = symIds[0];
    ev.barType = BarType::Time;
    ev.barTypeParam = M1_NS;
    double price = 100.0 + i * 1.5;
    ev.bar.open = Price::fromDouble(price);
    ev.bar.close = Price::fromDouble(price + 0.5);
    ev.bar.high = Price::fromDouble(price + 1.0);
    ev.bar.low = Price::fromDouble(price - 0.5);
    ev.bar.startTime = TimePoint{std::chrono::nanoseconds{static_cast<int64_t>(M1_NS) * i}};
    ev.bar.endTime = TimePoint{std::chrono::nanoseconds{static_cast<int64_t>(M1_NS) * (i + 1)}};
    bridge->onBar(ev);
  }

  auto* ctx = jsStrat.engine().context();

  JSValue ar = jsStrat.engine().getGlobalProperty("andReady");
  EXPECT_TRUE(JS_ToBool(ctx, ar));
  JS_FreeValue(ctx, ar);

  JSValue av = jsStrat.engine().getGlobalProperty("andValue");
  EXPECT_TRUE(JS_ToBool(ctx, av));
  JS_FreeValue(ctx, av);

  JSValue ov = jsStrat.engine().getGlobalProperty("orValue");
  EXPECT_TRUE(JS_ToBool(ctx, ov));
  JS_FreeValue(ctx, ov);

  JSValue nv = jsStrat.engine().getGlobalProperty("notValue");
  EXPECT_FALSE(JS_ToBool(ctx, nv));
  JS_FreeValue(ctx, nv);
}

// ============================================================
// Multi-feed clock
// ============================================================

TEST(JsIntegrationTest, MultiFeedClockWaitForAll)
{
  TempJsFile script(R"(
    var fired1 = "";
    var fired2 = "";
    var fired3 = "";
    var staleAfter = 0;
    class TestStrat extends Strategy {
      constructor() { super({ exchange: "Test", symbols: ["BTCUSDT", "ETHUSDT"] }); }
      onStart() {
        var c = new MultiFeedClock({
          symbols: [1, 2],
          policy: FeedClockPolicy.WaitForAll,
          timeoutMs: 200,
        });
        var r1 = c.tick(1000000000, 1);
        fired1 = String(r1.fired);
        var r2 = c.tick(1100000000, 2);
        fired2 = String(r2.fired);
        var r3 = c.tick(1200000000, 1);
        fired3 = String(r3.fired);
        staleAfter = r2.stalenessNs[1];
      }
    }
    lrvx.register(new TestStrat());
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();

  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<LrvxStrategyHandle>(bridge.get()));
  bridge->start();

  auto* ctx = jsStrat.engine().context();

  auto getStr = [&](const char* name)
  {
    JSValue v = jsStrat.engine().getGlobalProperty(name);
    const char* s = JS_ToCString(ctx, v);
    std::string out = s ? s : "";
    if (s)
    {
      JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);
    return out;
  };

  EXPECT_EQ(getStr("fired1"), "false");
  EXPECT_EQ(getStr("fired2"), "true");
  EXPECT_EQ(getStr("fired3"), "false");

  JSValue stale = jsStrat.engine().getGlobalProperty("staleAfter");
  int64_t staleVal = 0;
  JS_ToBigInt64(ctx, &staleVal, stale);
  EXPECT_EQ(staleVal, 100000000);
  JS_FreeValue(ctx, stale);
}

// ============================================================
// Multi-leg order group
// ============================================================

TEST(JsIntegrationTest, OrderGroupRiskGateDeniesOversizedBasket)
{
  TempJsFile script(R"(
    var deniedBig = false;
    var ruleBig = "";
    var deniedSmall = true;
    class TestStrat extends Strategy {
      constructor() { super({ exchange: "Test", symbols: ["BTCUSDT", "ETHUSDT"] }); }
      onStart() {
        var g = new OrderGroup();
        g.addMarketLeg(1, 0, 0.1);
        g.addMarketLeg(2, 1, 2.0);
        g.setRiskLimits({ maxConcentrationPct: 0.05 });
        var b = g.precheckSubmission({ equity: 100000, marketRefPrices: [50000, 3000] });
        deniedBig = b.denied;
        ruleBig = b.rule;
        var g2 = new OrderGroup();
        g2.addMarketLeg(1, 0, 0.001);
        g2.setRiskLimits({ maxConcentrationPct: 0.05 });
        deniedSmall = g2.precheckSubmission({ equity: 100000, marketRefPrices: [50000] }).denied;
      }
    }
    lrvx.register(new TestStrat());
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();
  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<LrvxStrategyHandle>(bridge.get()));
  bridge->start();

  auto* ctx = jsStrat.engine().context();
  JSValue dBig = jsStrat.engine().getGlobalProperty("deniedBig");
  EXPECT_TRUE(JS_ToBool(ctx, dBig));
  JS_FreeValue(ctx, dBig);
  JSValue rule = jsStrat.engine().getGlobalProperty("ruleBig");
  const char* rs = JS_ToCString(ctx, rule);
  EXPECT_STREQ(rs, "maxConcentrationPct");
  JS_FreeCString(ctx, rs);
  JS_FreeValue(ctx, rule);
  JSValue dSmall = jsStrat.engine().getGlobalProperty("deniedSmall");
  EXPECT_FALSE(JS_ToBool(ctx, dSmall));
  JS_FreeValue(ctx, dSmall);
}

TEST(JsIntegrationTest, OrderGroupAutoDispatchFiresAndIsIdempotent)
{
  TempJsFile script(R"(
    var fired = -1;
    var fired2 = -1;
    var sells = [];
    var cancels = [];
    class TestStrat extends Strategy {
      constructor() { super({ exchange: "Test", symbols: ["BTCUSDT", "ETHUSDT"] }); }
      onStart() {
        var fakeStrat = {
          cancel: function(oid) { cancels.push(oid); },
          marketBuy: function(o) {  /* unused in this scenario */ },
          marketSell: function(o) { sells.push([o.symbol, o.qty]); },
        };
        var g = new OrderGroup({ parentSignalId: 9, policy: OrderGroupPolicy.AllOrNothing });
        g.addMarketLeg(1, 0, 0.1);
        g.addMarketLeg(2, 1, 2.0);
        g.recordSubmit(0, 100); g.recordSubmit(1, 101);
        g.recordFill(0, 0.1); g.recordFailure(1);
        fired = g.autoDispatch(fakeStrat);
        fired2 = g.autoDispatch(fakeStrat);
      }
    }
    lrvx.register(new TestStrat());
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();
  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<LrvxStrategyHandle>(bridge.get()));
  bridge->start();

  auto* ctx = jsStrat.engine().context();
  auto getInt = [&](const char* name)
  {
    JSValue v = jsStrat.engine().getGlobalProperty(name);
    int32_t out = 0;
    JS_ToInt32(ctx, &out, v);
    JS_FreeValue(ctx, v);
    return out;
  };
  EXPECT_EQ(getInt("fired"), 1) << "first autoDispatch fires the revert";
  EXPECT_EQ(getInt("fired2"), 0) << "second autoDispatch is a no-op";

  JSValue sells = jsStrat.engine().getGlobalProperty("sells");
  uint32_t len = 0;
  JS_ToUint32(ctx, &len, JS_GetPropertyStr(ctx, sells, "length"));
  EXPECT_EQ(len, 1u);
  JS_FreeValue(ctx, sells);
}

TEST(JsIntegrationTest, OrderGroupAllOrNothingReverts)
{
  TempJsFile script(R"(
    var groupState = "";
    var actions = [];
    class TestStrat extends Strategy {
      constructor() { super({ exchange: "Test", symbols: ["BTCUSDT", "ETHUSDT"] }); }
      onStart() {
        var g = new OrderGroup({ parentSignalId: 7, policy: OrderGroupPolicy.AllOrNothing });
        g.addMarketLeg(1, 0, 0.1);
        g.addMarketLeg(2, 1, 2.0);
        g.recordSubmit(0, 100);
        g.recordSubmit(1, 101);
        g.recordFill(0, 0.1);
        g.recordFailure(1);
        groupState = g.state();
        actions = g.recommendedActions();
      }
    }
    lrvx.register(new TestStrat());
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();

  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<LrvxStrategyHandle>(bridge.get()));
  bridge->start();

  auto* ctx = jsStrat.engine().context();
  JSValue st = jsStrat.engine().getGlobalProperty("groupState");
  const char* stStr = JS_ToCString(ctx, st);
  EXPECT_STREQ(stStr, "Reverting") << "Reverting state expected (AllOrNothing + leg failure)";
  JS_FreeCString(ctx, stStr);
  JS_FreeValue(ctx, st);

  // actions = [{kind:'revert', legIndex:0, symbol:1, side:1, qty:0.1}]
  JSValue acts = jsStrat.engine().getGlobalProperty("actions");
  uint32_t len = 0;
  JS_ToUint32(ctx, &len, JS_GetPropertyStr(ctx, acts, "length"));
  EXPECT_EQ(len, 1u);
  JS_FreeValue(ctx, acts);
}

// ============================================================
// Indicator-grid sugar
//
// `grid(strategy, [BTC, ETH], [H4, M5]).ema(50)` instantiates one
// indicator per (symbol, timeframe) cell. Lookup by
// `g.get(symbol, barType, param)`.
// ============================================================

TEST(JsIntegrationTest, IndicatorGridCrossProduct)
{
  TempJsFile script(R"(
    var gridSize = 0;
    var btcReady = false;
    var btcValue = 0;
    var ethReady = false;
    var keysShape = "";

    class TestStrat extends Strategy {
      constructor() { super({ exchange: "Test", symbols: ["BTCUSDT", "ETHUSDT"] }); }
      onBar(ctx, bar) {
        var BTC = this._symbolMap["BTCUSDT"];
        var ETH = this._symbolMap["ETHUSDT"];
        var M1 = 60 * 1000000000;
        var H1 = 3600 * 1000000000;
        var g = grid(this, [BTC, ETH], [M1, H1]).ema(3);
        gridSize = g.size();

        var btcEma = g.get(BTC, 0, M1);
        btcReady = btcEma.isReady();
        if (btcReady) btcValue = btcEma.value();

        var ethEma = g.get(ETH, 0, H1);
        ethReady = ethEma.isReady();

        var ks = g.keys();
        keysShape = ks.length + "/" + ks[0].symbol + ":" + ks[0].param;
      }
    }
    lrvx.register(new TestStrat());
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();

  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<LrvxStrategyHandle>(bridge.get()));

  // Drive 5 M1 bars on BTC so the M1 EMA(3) is ready; H1 cells stay cold.
  const uint64_t M1_NS = 60ull * 1'000'000'000ull;
  for (int i = 0; i < 5; ++i)
  {
    BarEvent ev{};
    ev.symbol = symIds[0];  // BTC
    ev.barType = BarType::Time;
    ev.barTypeParam = M1_NS;
    ev.bar.open = Price::fromDouble(100.0 + i);
    ev.bar.close = Price::fromDouble(101.0 + i);
    ev.bar.high = Price::fromDouble(102.0 + i);
    ev.bar.low = Price::fromDouble(99.0 + i);
    ev.bar.startTime = TimePoint{std::chrono::nanoseconds{static_cast<int64_t>(M1_NS) * i}};
    ev.bar.endTime = TimePoint{std::chrono::nanoseconds{static_cast<int64_t>(M1_NS) * (i + 1)}};
    bridge->onBar(ev);
  }

  auto* ctx = jsStrat.engine().context();

  JSValue size = jsStrat.engine().getGlobalProperty("gridSize");
  int32_t sizeVal = 0;
  JS_ToInt32(ctx, &sizeVal, size);
  EXPECT_EQ(sizeVal, 4) << "2 symbols x 2 timeframes";
  JS_FreeValue(ctx, size);

  JSValue btcReady = jsStrat.engine().getGlobalProperty("btcReady");
  EXPECT_TRUE(JS_ToBool(ctx, btcReady));
  JS_FreeValue(ctx, btcReady);

  JSValue ethReady = jsStrat.engine().getGlobalProperty("ethReady");
  EXPECT_FALSE(JS_ToBool(ctx, ethReady)) << "ETH/H1 cell stays cold";
  JS_FreeValue(ctx, ethReady);
}

// ============================================================
// Multi-TF alignment helpers
//
// Parity with the pybind11 + NAPI surface added earlier: a JS strategy
// can read its per-(symbol, type, param) bar ring via lastClosedBar and
// lastNClosedBars without bookkeeping by hand.
// ============================================================

TEST(JsIntegrationTest, MultiTfHelpersExposeBarRing)
{
  TempJsFile script(R"(
    var firstClose = -1;   // close of the M5 bar after the first onBar
    var thirdClose = -1;   // close of the latest M5 bar after the third onBar
    var thirdCount = 0;    // size of lastNClosedBars(3) after the third onBar
    var thirdAllCloses = [];
    var BAR_TYPE_TIME = 0;
    var M5_NS = 5 * 60 * 1000000000;

    class TestStrat extends Strategy {
      constructor() { super({ exchange: "Test", symbols: ["BTCUSDT"] }); }
      onBar(ctx, bar) {
        var last = this.lastClosedBar("BTCUSDT", BAR_TYPE_TIME, M5_NS);
        if (firstClose < 0) {
          firstClose = last ? last.close : -1;
        }
        thirdClose = last ? last.close : -1;
        var nbars = this.lastNClosedBars("BTCUSDT", BAR_TYPE_TIME, M5_NS, 5);
        thirdCount = nbars.length;
        thirdAllCloses = nbars.map(function(b){ return b.close; });
      }
    }
    lrvx.register(new TestStrat());
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();

  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<LrvxStrategyHandle>(bridge.get()));

  const uint64_t M5_NS = 5ull * 60 * 1'000'000'000ull;
  auto pushBar = [&](double open, double close, int64_t startNs)
  {
    BarEvent ev{};
    ev.symbol = symIds[0];
    ev.barType = BarType::Time;
    ev.barTypeParam = M5_NS;
    ev.bar.open = Price::fromDouble(open);
    ev.bar.close = Price::fromDouble(close);
    ev.bar.high = Price::fromDouble(std::max(open, close));
    ev.bar.low = Price::fromDouble(std::min(open, close));
    ev.bar.startTime = TimePoint{std::chrono::nanoseconds{startNs}};
    ev.bar.endTime = TimePoint{std::chrono::nanoseconds{startNs + (int64_t)M5_NS}};
    bridge->onBar(ev);
  };

  pushBar(100.0, 101.0, 0);
  pushBar(101.0, 102.5, (int64_t)M5_NS);
  pushBar(102.5, 103.0, 2 * (int64_t)M5_NS);

  auto* ctx = jsStrat.engine().context();

  JSValue first = jsStrat.engine().getGlobalProperty("firstClose");
  double firstVal = 0;
  JS_ToFloat64(ctx, &firstVal, first);
  EXPECT_NEAR(firstVal, 101.0, 1e-9);
  JS_FreeValue(ctx, first);

  JSValue third = jsStrat.engine().getGlobalProperty("thirdClose");
  double thirdVal = 0;
  JS_ToFloat64(ctx, &thirdVal, third);
  EXPECT_NEAR(thirdVal, 103.0, 1e-9);
  JS_FreeValue(ctx, third);

  JSValue count = jsStrat.engine().getGlobalProperty("thirdCount");
  int32_t countVal = 0;
  JS_ToInt32(ctx, &countVal, count);
  EXPECT_EQ(countVal, 3);
  JS_FreeValue(ctx, count);
}

TEST(JsIntegrationTest, MultiTfHelpersReturnNullBeforeAnyBar)
{
  TempJsFile script(R"(
    var beforeAnyBar = "unset";
    class TestStrat extends Strategy {
      constructor() { super({ exchange: "Test", symbols: ["BTCUSDT"] }); }
      onStart() {
        var bar = this.lastClosedBar("BTCUSDT", 0, 5*60*1000000000);
        beforeAnyBar = bar === null ? "null" : "non-null";
      }
    }
    lrvx.register(new TestStrat());
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();

  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<LrvxStrategyHandle>(bridge.get()));

  bridge->start();

  auto* ctx = jsStrat.engine().context();
  JSValue val = jsStrat.engine().getGlobalProperty("beforeAnyBar");
  const char* s = JS_ToCString(ctx, val);
  EXPECT_STREQ(s, "null");
  JS_FreeCString(ctx, s);
  JS_FreeValue(ctx, val);
}

TEST(JsIntegrationTest, MultiTfHelpersRingCapacityIsAdjustable)
{
  TempJsFile script(R"(
    var initialCap = 0;
    var afterSet = 0;
    var keptCount = 0;
    class TestStrat extends Strategy {
      constructor() { super({ exchange: "Test", symbols: ["BTCUSDT"] }); }
      onStart() {
        initialCap = this.barRingCapacity;
        this.setBarRingCapacity(3);
        afterSet = this.barRingCapacity;
      }
      onBar(ctx, bar) {
        var bars = this.lastNClosedBars("BTCUSDT", 0, 5*60*1000000000, 10);
        keptCount = bars.length;
      }
    }
    lrvx.register(new TestStrat());
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();

  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<LrvxStrategyHandle>(bridge.get()));

  bridge->start();

  const uint64_t M5_NS = 5ull * 60 * 1'000'000'000ull;
  for (int i = 0; i < 7; ++i)
  {
    BarEvent ev{};
    ev.symbol = symIds[0];
    ev.barType = BarType::Time;
    ev.barTypeParam = M5_NS;
    ev.bar.open = Price::fromDouble(100.0 + i);
    ev.bar.close = Price::fromDouble(100.5 + i);
    ev.bar.high = Price::fromDouble(101.0 + i);
    ev.bar.low = Price::fromDouble(99.5 + i);
    ev.bar.startTime = TimePoint{std::chrono::nanoseconds{static_cast<int64_t>(M5_NS) * i}};
    ev.bar.endTime = TimePoint{std::chrono::nanoseconds{static_cast<int64_t>(M5_NS) * (i + 1)}};
    bridge->onBar(ev);
  }

  auto* ctx = jsStrat.engine().context();
  JSValue cap = jsStrat.engine().getGlobalProperty("afterSet");
  int32_t capVal = 0;
  JS_ToInt32(ctx, &capVal, cap);
  EXPECT_EQ(capVal, 3);
  JS_FreeValue(ctx, cap);

  JSValue kept = jsStrat.engine().getGlobalProperty("keptCount");
  int32_t keptVal = 0;
  JS_ToInt32(ctx, &keptVal, kept);
  EXPECT_EQ(keptVal, 3);
  JS_FreeValue(ctx, kept);
}

TEST(JsIntegrationTest, JsExceptionInOnTradeDoesNotCrash)
{
  TempJsFile script(R"(
    var tradeCount = 0;
    class TestStrat extends Strategy {
      constructor() { super({ exchange: "T", symbols: ["S1"] }); }
      onTrade(ctx, trade) {
        tradeCount++;
        if (tradeCount === 2) throw new Error("intentional");
      }
    }
    lrvx.register(new TestStrat());
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();

  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<LrvxStrategyHandle>(bridge.get()));

  // Trade 1: OK, Trade 2: throws, Trade 3: should still work
  for (int i = 0; i < 3; i++)
  {
    LrvxSymbolContext fctx{};
    fctx.symbol_id = symIds[0];
    LrvxTradeData ftrade{};
    ftrade.symbol = symIds[0];
    ftrade.price_raw = lrvx_price_from_double(100.0);
    ftrade.quantity_raw = lrvx_quantity_from_double(1.0);
    ftrade.is_buy = 1;
    callbacks.on_trade(callbacks.user_data, &fctx, &ftrade);
  }

  JSValue count = jsStrat.engine().getGlobalProperty("tradeCount");
  int32_t countVal = 0;
  JS_ToInt32(jsStrat.engine().context(), &countVal, count);
  EXPECT_EQ(countVal, 3);  // All 3 trades processed
  JS_FreeValue(jsStrat.engine().context(), count);
}

// ============================================================
// One JSRuntime, one dedicated thread. The live engine subscribes a
// single strategy to three independently-threaded buses (trades, book
// updates, bars); before LrvxJsExecutor, that meant up to three threads
// entering the same non-reentrant QuickJS runtime concurrently. Reproduced
// pre-fix at 10 events; these run at 10,000+ across two producer threads.
// ============================================================

namespace
{
const char* const kCounterStrategyScript = R"(
    var tradeCount = 0;
    var bookCount = 0;
    class CounterStrat extends Strategy {
      constructor() { super({ exchange: "T", symbols: ["S1"] }); }
      onTrade(ctx, trade) { tradeCount++; }
      onBookUpdate(ctx, book) { bookCount++; }
    }
    lrvx.register(new CounterStrat());
  )";

int32_t readGlobalInt32(LrvxJsStrategy& strat, const char* name)
{
  JSValue v = strat.engine().getGlobalProperty(name);
  int32_t out = 0;
  JS_ToInt32(strat.engine().context(), &out, v);
  JS_FreeValue(strat.engine().context(), v);
  return out;
}
}  // namespace

// t13: two producer threads (one trades, one book updates) hammer a single
// LrvxJsExecutor through the callbacks lrvx_live_engine_add_strategy would
// use. Manual threads rather than the real bus, so this isolates the
// executor/queue design from the eventing subsystem's own behavior.
TEST(JsExecutorThreadingTest, ManualProducerThreadsDoNotCorruptRuntime)
{
  constexpr int kPerThread = 5000;  // 10,000 events total, matching the acceptance test
  constexpr int kRuns = 20;         // the acceptance bar: zero crashes in 20 runs
  for (int run = 0; run < kRuns; ++run)
  {
    TempJsFile script(kCounterStrategyScript);
    SymbolRegistry registry;
    LrvxJsExecutor executor(script.path(), registry);
    auto symIds = executor.symbolIds();
    ASSERT_FALSE(symIds.empty());
    auto callbacks = executor.getCallbacks();

    std::thread tradeThread(
        [&]
        {
          for (int i = 0; i < kPerThread; ++i)
          {
            LrvxSymbolContext fctx{};
            fctx.symbol_id = symIds[0];
            LrvxTradeData ftrade{};
            ftrade.symbol = symIds[0];
            ftrade.price_raw = lrvx_price_from_double(100.0 + i);
            ftrade.quantity_raw = lrvx_quantity_from_double(1.0);
            callbacks.on_trade(callbacks.user_data, &fctx, &ftrade);
          }
        });
    std::thread bookThread(
        [&]
        {
          for (int i = 0; i < kPerThread; ++i)
          {
            LrvxSymbolContext fctx{};
            fctx.symbol_id = symIds[0];
            LrvxBookData fbook{};
            fbook.symbol = symIds[0];
            callbacks.on_book(callbacks.user_data, &fctx, &fbook);
          }
        });
    tradeThread.join();
    bookThread.join();
    executor.waitIdle();

    // Read the counters back through the executor's own queue -- not by
    // touching engine() from this thread, which is exactly the kind of
    // foreign-thread access the owner-thread check exists to refuse.
    EXPECT_EQ(executor.getGlobalInt32("tradeCount"), kPerThread);
    EXPECT_EQ(executor.getGlobalInt32("bookCount"), kPerThread);
  }
}

// Same scenario, but through the actual public C ABI: lrvx_live_engine_*,
// exactly the path lrvx_live_engine_add_strategy documents. This is what
// broke in the audit's t13live reproduction (3/3 crashes, first crash at
// 10 events); the JSRuntime itself must now survive 10,000 events
// published from this thread while the live engine's three internal bus
// threads dispatch them.
//
// Driving a real lrvx_live_engine this hard exercises more than the JS
// runtime -- it also exercises lrvx::Strategy's per-symbol state, which
// every BridgeStrategy-based binding (Node, Python, Codon, QuickJS) reads
// and writes from whatever bus thread happens to deliver an event for
// that symbol. That state now has one owner: a symbol's context belongs
// to whoever holds that symbol's lock, and a dispatch keeps the lock until
// its hook returns. Before that rule existed, a trade and a book update
// for the *same* symbol arriving on two bus threads both wrote the
// context with nothing between them, and ThreadSanitizer reported it as
// races in SymbolStateMap::operator[], NLevelOrderBook::applyBookUpdate,
// Strategy::onBookUpdate and Decimal::raw().
//
// So this test runs under every configuration, ThreadSanitizer included,
// and asserts exact counts rather than bounds -- an event could be
// dropped only while the race was live.
TEST(JsExecutorThreadingTest, LiveEngineThreeBusesDoNotCorruptRuntime)
{
  constexpr int kEventsPerKind = 5000;  // 10,000 total, per the acceptance test

  TempJsFile script(kCounterStrategyScript);
  SymbolRegistry registry;
  LrvxJsExecutor executor(script.path(), registry);
  auto symIds = executor.symbolIds();
  ASSERT_FALSE(symIds.empty());

  LrvxRegistryHandle regHandle = static_cast<LrvxRegistryHandle>(&registry);
  LrvxStrategyHandle strat = lrvx_strategy_create(
      1, symIds.data(), static_cast<uint32_t>(symIds.size()), regHandle, executor.getCallbacks());
  ASSERT_NE(strat, nullptr);
  executor.injectHandle(strat);

  LrvxLiveEngineHandle engine = lrvx_live_engine_create(regHandle);
  ASSERT_NE(engine, nullptr);
  lrvx_live_engine_add_strategy(engine, strat, nullptr, nullptr);
  lrvx_live_engine_start(engine);

  for (int i = 0; i < kEventsPerKind; ++i)
  {
    lrvx_live_engine_publish_trade(engine, symIds[0], 100.0 + i, 1.0, 1, 1000000000LL + i);
    double bidP = 99.0, bidQ = 1.0, askP = 101.0, askQ = 1.0;
    lrvx_live_engine_publish_book_snapshot(engine, symIds[0], &bidP, &bidQ, 1, &askP, &askQ, 1,
                                           1000000000LL + i);
  }

  // Wait for the bus threads to finish delivering before stopping the
  // engine. The buses throw away whatever is still in the ring at stop()
  // unless drain-on-stop is switched on, so stopping first would count
  // that truncation as a lost event and there would be no way to tell it
  // apart from a real one.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
  int trades = 0;
  int books = 0;
  while (std::chrono::steady_clock::now() < deadline)
  {
    trades = executor.getGlobalInt32("tradeCount");
    books = executor.getGlobalInt32("bookCount");
    if (trades >= kEventsPerKind && books >= kEventsPerKind)
    {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  lrvx_live_engine_stop(engine);
  executor.waitIdle();

  // Reaching here at all -- 10,000 events on 3 live bus threads -- is the
  // point of this test: pre-fix this crashed 3/3 times, usually inside
  // the first ten events. Exact counts, not bounds: every published event
  // reaches the strategy exactly once.
  EXPECT_EQ(executor.getGlobalInt32("tradeCount"), kEventsPerKind);
  EXPECT_EQ(executor.getGlobalInt32("bookCount"), kEventsPerKind);

  lrvx_live_engine_destroy(engine);
  lrvx_strategy_destroy(strat);
}

// Control: the same 10x the event volume (400,000), single-threaded, to
// rule out "just a lot of events" as an alternative explanation for a
// pre-fix crash. Must stay clean both before and after the fix.
TEST(JsExecutorThreadingTest, SingleThreadedControlHandles400kEvents)
{
  TempJsFile script(kCounterStrategyScript);
  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();
  ASSERT_FALSE(symIds.empty());

  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<LrvxStrategyHandle>(bridge.get()));

  constexpr int kTotal = 200000;
  for (int i = 0; i < kTotal; ++i)
  {
    LrvxSymbolContext fctx{};
    fctx.symbol_id = symIds[0];
    LrvxTradeData ftrade{};
    ftrade.symbol = symIds[0];
    ftrade.price_raw = lrvx_price_from_double(100.0);
    ftrade.quantity_raw = lrvx_quantity_from_double(1.0);
    callbacks.on_trade(callbacks.user_data, &fctx, &ftrade);
    LrvxBookData fbook{};
    fbook.symbol = symIds[0];
    callbacks.on_book(callbacks.user_data, &fctx, &fbook);
  }

  EXPECT_EQ(readGlobalInt32(jsStrat, "tradeCount"), kTotal);
  EXPECT_EQ(readGlobalInt32(jsStrat, "bookCount"), kTotal);
}

// ============================================================
// The microtask queue is now drained after every dispatch
// (LrvxJsStrategy::invokeMethod calls LrvxJsEngine::pumpPendingJobs()).
// Before this fix, JS_ExecutePendingJob was never called anywhere in this
// engine: an `await` inside onTrade never resumed, the order was never
// placed, and the unresolved job leaked ~1.6 KB per event until the heap
// limit killed the process a few thousand events later.
// ============================================================

TEST(JsIntegrationTest, AsyncOnTradeResumesAfterAwait)
{
  TempJsFile script(R"(
    var resumed = false;
    var orderId = -1;
    class TestStrat extends Strategy {
      constructor() { super({ exchange: "T", symbols: ["S1"] }); }
      async onTrade(ctx, trade) {
        await Promise.resolve();
        resumed = true;
        orderId = this.limitBuy({ symbol: trade.symbol, price: 100.0, qty: 1.0 });
      }
    }
    lrvx.register(new TestStrat());
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();

  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<LrvxStrategyHandle>(bridge.get()));

  LrvxSymbolContext fctx{};
  fctx.symbol_id = symIds[0];
  LrvxTradeData ftrade{};
  ftrade.symbol = symIds[0];
  ftrade.price_raw = lrvx_price_from_double(100.0);
  ftrade.quantity_raw = lrvx_quantity_from_double(1.0);
  callbacks.on_trade(callbacks.user_data, &fctx, &ftrade);

  JSValue resumed = jsStrat.engine().getGlobalProperty("resumed");
  EXPECT_TRUE(JS_ToBool(jsStrat.engine().context(), resumed));
  JS_FreeValue(jsStrat.engine().context(), resumed);
  EXPECT_GT(readGlobalInt32(jsStrat, "orderId"), 0);
}

TEST(JsIntegrationTest, AsyncOnTradeManyEventsDoNotLeakQueuedJobs)
{
  // Pre-fix this grew the heap by ~1.6 KB/event and hit the 32 MB default
  // limit around event 20,400; 2,000 events stays well under that if the
  // drain is working and would already show heap-limit failures if it is
  // not (JS_NewFloat64/JS_Call would start throwing OOM inside onTrade).
  TempJsFile script(R"(
    var completed = 0;
    class TestStrat extends Strategy {
      constructor() { super({ exchange: "T", symbols: ["S1"] }); }
      async onTrade(ctx, trade) {
        await Promise.resolve();
        completed++;
      }
    }
    lrvx.register(new TestStrat());
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();

  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<LrvxStrategyHandle>(bridge.get()));

  constexpr int kEvents = 2000;
  for (int i = 0; i < kEvents; ++i)
  {
    LrvxSymbolContext fctx{};
    fctx.symbol_id = symIds[0];
    LrvxTradeData ftrade{};
    ftrade.symbol = symIds[0];
    ftrade.price_raw = lrvx_price_from_double(100.0);
    ftrade.quantity_raw = lrvx_quantity_from_double(1.0);
    callbacks.on_trade(callbacks.user_data, &fctx, &ftrade);
  }

  EXPECT_EQ(readGlobalInt32(jsStrat, "completed"), kEvents);
}

// ============================================================
// NaN, +-Infinity, and a missing quantity field used to reach the
// exchange as price 0 / INT64_MAX / quantity 0 with no error at all.
// ============================================================

TEST(JsIntegrationTest, OrderPriceAndQuantityRejectNonFiniteInput)
{
  TempJsFile script(R"(
    var results = [];
    function tryOrder(opts) {
      try {
        this.limitBuy(opts);
        results.push("ok");
      } catch (e) {
        results.push("threw:" + e.message);
      }
    }
    class TestStrat extends Strategy {
      constructor() { super({ exchange: "T", symbols: ["S1"] }); }
      onTrade(ctx, trade) {
        tryOrder.call(this, { symbol: trade.symbol, price: NaN, qty: 1.0 });
        tryOrder.call(this, { symbol: trade.symbol, price: Infinity, qty: 1.0 });
        tryOrder.call(this, { symbol: trade.symbol, price: -Infinity, qty: 1.0 });
        tryOrder.call(this, { symbol: trade.symbol, price: 100.0, qty: undefined });
        tryOrder.call(this, { symbol: trade.symbol, price: 100.0, qty: 0 });
        tryOrder.call(this, { symbol: trade.symbol, price: 100.0, qty: -1.0 });
        tryOrder.call(this, { symbol: trade.symbol, price: 100.5, qty: 2.0 });
      }
    }
    lrvx.register(new TestStrat());
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();

  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<LrvxStrategyHandle>(bridge.get()));

  LrvxSymbolContext fctx{};
  fctx.symbol_id = symIds[0];
  LrvxTradeData ftrade{};
  ftrade.symbol = symIds[0];
  ftrade.price_raw = lrvx_price_from_double(100.0);
  ftrade.quantity_raw = lrvx_quantity_from_double(1.0);
  callbacks.on_trade(callbacks.user_data, &fctx, &ftrade);

  auto* ctx = jsStrat.engine().context();
  JSValue results = jsStrat.engine().getGlobalProperty("results");
  for (int i = 0; i < 7; ++i)
  {
    JSValue elem = JS_GetPropertyUint32(ctx, results, i);
    const char* s = JS_ToCString(ctx, elem);
    std::string entry = s ? s : "";
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, elem);
    if (i < 6)
    {
      EXPECT_TRUE(entry.rfind("threw:", 0) == 0) << "case " << i << " got: " << entry;
    }
    else
    {
      EXPECT_EQ(entry, "ok") << "the one healthy order must still go through";
    }
  }
  JS_FreeValue(ctx, results);
}

// ============================================================
// A non-string (or throwing-toString) graph node name used to
// reach strlen(nullptr) and segfault. It must throw a catchable TypeError
// instead.
// ============================================================

TEST(JsIntegrationTest, GraphAddNodeWithSymbolNameThrowsInsteadOfCrashing)
{
  LrvxJsEngine engine;
  registerLrvxBindings(engine.context());
  EXPECT_TRUE(engine.eval(R"(
    var threw = false;
    var g = __lrvx_graph_create();
    try {
      __lrvx_graph_add_node(g, Symbol('ema'), [], function() { return []; }, {});
    } catch (e) {
      threw = true;
    }
    __lrvx_graph_destroy(g);
  )"));
  JSValue threw = engine.getGlobalProperty("threw");
  EXPECT_TRUE(JS_ToBool(engine.context(), threw));
  JS_FreeValue(engine.context(), threw);
}

// ============================================================
// High/low/close arrays of mismatched length used to segfault
// (empty arrays) or silently read past the shorter buffer (near-matching
// lengths). Every high/low/close indicator now validates the three
// lengths match before calling into the C ABI.
// ============================================================

TEST(JsIntegrationTest, AtrWithMismatchedArrayLengthsThrows)
{
  LrvxJsEngine engine;
  registerLrvxBindings(engine.context());
  EXPECT_TRUE(engine.eval(R"(
    var threw = false;
    try {
      __lrvx_indicator_atr([10, 11, 12, 13, 14, 15, 16, 17, 18, 19], [], [], 3);
    } catch (e) {
      threw = true;
    }
  )"));
  JSValue threw = engine.getGlobalProperty("threw");
  EXPECT_TRUE(JS_ToBool(engine.context(), threw));
  JS_FreeValue(engine.context(), threw);
}

// ============================================================
// The feed clock handle used to be a raw pointer round-tripped
// through a BigInt, so any integer a script cared to pass was dereferenced
// directly, and there was no destroy() at all on the JS facade. It is now
// a typed, finalized handle: a forged or already-destroyed handle throws
// or no-ops instead of touching arbitrary memory, and destroy() exists.
// ============================================================

TEST(JsIntegrationTest, FeedClockForgedHandleIsRejectedNotDereferenced)
{
  // destroy() on a bogus handle is a deliberate no-op (matching every
  // other handle-shaped destroy() in this file) rather than a throw --
  // the assertion here is that it does not touch the number 12345 as a
  // pointer. A read/write accessor on the same bogus handle is expected
  // to throw, since (unlike destroy) it has no sensible no-op result.
  LrvxJsEngine engine;
  registerLrvxBindings(engine.context());
  EXPECT_TRUE(engine.eval(R"(
    __lrvx_feed_clock_destroy(12345n);   // must not crash
    var threw = false;
    try {
      __lrvx_feed_clock_symbol_count(12345n);
    } catch (e) {
      threw = true;
    }
  )"));
  JSValue threw = engine.getGlobalProperty("threw");
  EXPECT_TRUE(JS_ToBool(engine.context(), threw));
  JS_FreeValue(engine.context(), threw);
}

TEST(JsIntegrationTest, FeedClockDoubleDestroyAndDestroyThenUseAreSafe)
{
  // Bare LrvxJsEngine only has the __lrvx_* globals (the class sugar in
  // quickjs/lrvx/feed_clock.js is part of loadStdlib(), which only
  // LrvxJsStrategy runs) -- exercise the same lifecycle through them.
  LrvxJsEngine engine;
  registerLrvxBindings(engine.context());
  EXPECT_TRUE(engine.eval(R"(
    var h = __lrvx_feed_clock_create([1], 1, 0, 1000, 1, 10);
    __lrvx_feed_clock_destroy(h);
    var threw = false;
    try {
      __lrvx_feed_clock_destroy(h);           // second destroy: no-op, not SIGABRT
      __lrvx_feed_clock_symbol_count(h);       // use-after-destroy: must throw
    } catch (e) {
      threw = true;
    }
  )"));
  JSValue threw = engine.getGlobalProperty("threw");
  EXPECT_TRUE(JS_ToBool(engine.context(), threw));
  JS_FreeValue(engine.context(), threw);
}

TEST(JsIntegrationTest, FeedClockCountMismatchedWithArrayIsRejected)
{
  // The other half of the unbounded-allocation bug below: `count` used
  // to size a std::vector directly with no relation to the array
  // actually passed -- the reproduction used count=4e9 against a
  // 1-element array. It must now be rejected instead of allocating.
  LrvxJsEngine engine;
  registerLrvxBindings(engine.context());
  EXPECT_TRUE(engine.eval(R"(
    var threw = false;
    try {
      __lrvx_feed_clock_create([1], 4000000000, 0, 1000, 1, 10);
    } catch (e) {
      threw = true;
    }
  )"));
  JSValue threw = engine.getGlobalProperty("threw");
  EXPECT_TRUE(JS_ToBool(engine.context(), threw));
  JS_FreeValue(engine.context(), threw);
}

// ============================================================
// A count taken straight from JS used to size a plain
// std::vector, bypassing the engine's own memory limit entirely (measured:
// 11 GB RSS from one line before this fix). lastNClosedBars has the same
// shape of bug (n straight into vector<LrvxBar>(n)) and is capped the
// same way.
// ============================================================

TEST(JsIntegrationTest, LastNClosedBarsRejectsAbsurdCount)
{
  TempJsFile script(R"(
    var threw = false;
    var returned = -1;
    class TestStrat extends Strategy {
      constructor() { super({ exchange: "T", symbols: ["S1"] }); }
      onStart() {
        try {
          returned = this.lastNClosedBars(this._symbolMap["S1"], 0, 60000000000, 4000000000).length;
        } catch (e) {
          threw = true;
        }
      }
    }
    lrvx.register(new TestStrat());
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();
  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<LrvxStrategyHandle>(bridge.get()));
  callbacks.on_start(callbacks.user_data);

  JSValue threw = jsStrat.engine().getGlobalProperty("threw");
  EXPECT_TRUE(JS_ToBool(jsStrat.engine().context(), threw));
  JS_FreeValue(jsStrat.engine().context(), threw);
}

// ============================================================
// IndicatorGraph had no finalizer at all: a graph dropped without
// .destroy() leaked its C++ state (and every JS function reference each
// node held) for the life of the process. It also did not null its opaque
// pointer on destroy, so a second destroy (or a call through the raw
// global on a saved handle) was a double-free / use-after-free.
// ============================================================

TEST(JsIntegrationTest, IndicatorGraphWithoutDestroyIsReclaimedByGc)
{
  // Bare LrvxJsEngine has no `lrvx` global (that sugar comes from
  // loadStdlib(), which only LrvxJsStrategy runs) -- drive the graph
  // through the same __lrvx_graph_* globals the class wrapper itself
  // calls.
  LrvxJsEngine engine;
  registerLrvxBindings(engine.context());
  EXPECT_TRUE(engine.eval(R"(
    for (var i = 0; i < 2000; i++) {
      var g = __lrvx_graph_create();
      __lrvx_graph_set_bars(g, 1, [1, 2, 3], null, null, null);
      // g is dropped here without destroy() -- the finalizer must free it,
      // where pre-fix it leaked ~2.6 KB per graph unconditionally.
    }
    var done = true;
  )"));
  // The quantitative side of this (does memory actually come back) is
  // covered by the ASan/LSan leak check in CI, which this path now feeds
  // correctly; here the assertion is that dropping 2000 undestroyed
  // graphs does not itself misbehave.
  JSValue done = engine.getGlobalProperty("done");
  EXPECT_TRUE(JS_ToBool(engine.context(), done));
  JS_FreeValue(engine.context(), done);
}

// ============================================================
// The generic handle class every other handle-returning binding in this
// file shares (OrderGroup, Book, SimulatedExecutor, Account, ...) had the
// exact same missing-finalizer defect as IndicatorGraph above, just not
// yet caught: an AddressSanitizer leak-detection run (CI only -- leak
// detection does not run locally on this project's development platform,
// see the note on lrvxJsHandleLiveCountForTesting()) found 912 bytes
// leaked through OrderGroup specifically, but the underlying class is
// shared by every handle type built on createHandleObject, and every one
// of them leaked identically whenever a script dropped the wrapper
// without calling .destroy(). All of them (42 owned handle types across
// the file, audited individually against their C++ implementation to
// confirm each is a real allocation and not a borrowed view into
// something else's memory) now get a destroy function threaded through
// at creation and a shared finalizer that calls it.
//
// This test does not depend on a sanitizer to catch a regression here:
// lrvxJsHandleLiveCountForTesting() is a live-object counter maintained
// by createHandleObject/the finalizer/destroy() themselves, so it works
// in a plain, unsanitized build -- which is the only kind of local build
// this leak class can be checked in at all, since LeakSanitizer is not
// available locally on this platform (ASan runs, but leak detection
// inside it is Linux-only; this repo's sanitizer CI job is where that
// half actually executes). If a future handle type is added and its
// create site forgets to pass a destroy function, this test's delta
// assertion catches it without needing that CI job to run first.
// ============================================================

TEST(JsIntegrationTest, HandlesDroppedWithoutDestroyAreReclaimedNotLeaked)
{
  size_t before = lrvxJsHandleLiveCountForTesting();
  {
    LrvxJsEngine engine;
    registerLrvxBindings(engine.context());
    // A handful of distinct handle types sharing the generic class,
    // spanning different files' worth of createHandleObject call sites:
    // OrderGroup (the type the leak report actually named), a book, and
    // a simulated executor. None of these call .destroy().
    EXPECT_TRUE(engine.eval(R"(
      for (var i = 0; i < 50; i++) {
        __lrvx_order_group_create();
        __lrvx_book_create(0.01);
        __lrvx_simulated_executor_create();
      }
      var done = true;
    )"));
    JSValue done = engine.getGlobalProperty("done");
    EXPECT_TRUE(JS_ToBool(engine.context(), done));
    JS_FreeValue(engine.context(), done);
    // engine's destructor runs at the end of this scope, freeing the
    // JSContext and JSRuntime. QuickJS finalizes every object still
    // alive in a runtime as part of freeing it, so this is what stands
    // in for "the script ended and nothing was explicitly destroyed" --
    // exactly the reported scenario, not a contrived one.
  }
  size_t after = lrvxJsHandleLiveCountForTesting();
  EXPECT_EQ(after, before) << "150 handles (50 each of order group / book / "
                              "simulated executor) were dropped without "
                              "destroy() and without the engine's teardown "
                              "reclaiming them -- the finalizer path is not "
                              "running for at least one of these types";
}

TEST(JsIntegrationTest, ExplicitDestroyAlsoReclaimsAndIsIdempotent)
{
  size_t before = lrvxJsHandleLiveCountForTesting();
  LrvxJsEngine engine;
  registerLrvxBindings(engine.context());
  EXPECT_TRUE(engine.eval(R"(
    var g = __lrvx_order_group_create();
    __lrvx_order_group_destroy(g);
    __lrvx_order_group_destroy(g);  // second destroy: no-op, not a double-free
  )"));
  // Explicit destroy() must release the live-count slot immediately, not
  // wait for the runtime to be torn down.
  EXPECT_EQ(lrvxJsHandleLiveCountForTesting(), before);
}

TEST(JsIntegrationTest, IndicatorGraphDoubleDestroyAndUseAfterDestroyAreSafe)
{
  LrvxJsEngine engine;
  registerLrvxBindings(engine.context());
  EXPECT_TRUE(engine.eval(R"(
    var g = __lrvx_graph_create();
    __lrvx_graph_destroy(g);
    __lrvx_graph_destroy(g);   // second destroy: no-op, not SIGABRT
    var threw = false;
    try {
      __lrvx_graph_set_bars(g, 1, [1], null, null, null);  // use-after-destroy
    } catch (e) {
      threw = true;
    }
  )"));
  JSValue threw = engine.getGlobalProperty("threw");
  EXPECT_TRUE(JS_ToBool(engine.context(), threw));
  JS_FreeValue(engine.context(), threw);
}

// ============================================================
// Dispatching into a script that never called lrvx.register()
// used to throw a TypeError from inside every single dispatcher and
// silently accumulate a stale pending exception across events. It must
// now no-op instead.
// ============================================================

TEST(JsIntegrationTest, DispatchWithoutRegisterIsANoOpNotAnAccumulatingException)
{
  TempJsFile script("var x = 1;");
  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  ASSERT_TRUE(jsStrat.symbolIds().empty());

  LrvxSymbolContext fctx{};
  LrvxTradeData ftrade{};
  for (int i = 0; i < 3; ++i)
  {
    callbacks.on_trade(callbacks.user_data, &fctx, &ftrade);
  }
  callbacks.on_start(callbacks.user_data);

  // No observable way to ask "is there a pending exception" from outside
  // the engine (by design -- that is exactly the owner-thread boundary);
  // the assertion is that dispatching repeatedly into an unregistered
  // strategy does not throw, crash, or hang.
  SUCCEED();
}

// ============================================================
// No interrupt handler and no stack-size limit used to mean a strategy
// stuck in an infinite loop could only be stopped with SIGKILL. The
// interrupt handler now unwinds any single callback invocation that runs
// longer than its budget.
// ============================================================

TEST(JsIntegrationTest, InfiniteLoopInCallbackIsInterruptedNotHung)
{
  TempJsFile script(R"(
    var reached = false;
    class TestStrat extends Strategy {
      constructor() { super({ exchange: "T", symbols: ["S1"] }); }
      onTrade(ctx, trade) {
        while (true) { }
        reached = true;  // never reached
      }
    }
    lrvx.register(new TestStrat());
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  jsStrat.engine().setInterruptBudget(std::chrono::milliseconds(200));
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();
  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<LrvxStrategyHandle>(bridge.get()));

  auto start = std::chrono::steady_clock::now();
  LrvxSymbolContext fctx{};
  fctx.symbol_id = symIds[0];
  LrvxTradeData ftrade{};
  ftrade.symbol = symIds[0];
  callbacks.on_trade(callbacks.user_data, &fctx, &ftrade);
  auto elapsed = std::chrono::steady_clock::now() - start;

  // Interrupted well under a naive "wait forever" -- generous margin above
  // the 200ms budget to stay stable under CI scheduling noise.
  EXPECT_LT(elapsed, std::chrono::seconds(5));

  // The runtime must still be usable afterwards: the interrupt unwinds the
  // one stuck call, it does not tear down the engine.
  callbacks.on_trade(callbacks.user_data, &fctx, &ftrade);
}

// ============================================================
// String conversion at the JS boundary
//
// JS_ToCString returns nullptr for anything it cannot turn into a string
// (a Symbol, an object whose toString throws, a runtime out of memory).
// Every binding that takes a string now goes through the owning holder in
// js_cstring.h, so the nullptr becomes a thrown TypeError at the boundary
// instead of a strlen(nullptr) inside a C function. The tests below cover
// the three behaviours the holder offers rather than all of the call
// sites: a required argument, an optional one, and a site that
// substitutes a fallback.
// ============================================================

namespace
{
bool jsThrowsOn(LrvxJsEngine& engine, const std::string& expr)
{
  std::string script = "threw = false; try { " + expr + "; } catch (e) { threw = true; }";
  if (!engine.eval(script))
  {
    return false;
  }
  JSValue threw = engine.getGlobalProperty("threw");
  const bool did = JS_ToBool(engine.context(), threw) == 1;
  JS_FreeValue(engine.context(), threw);
  return did;
}
}  // namespace

TEST(JsIntegrationTest, RequiredStringArgumentsRejectUnconvertibleValues)
{
  LrvxJsEngine engine;
  registerLrvxBindings(engine.context());
  ASSERT_TRUE(engine.eval("var threw = false;"));

  // A Symbol: JS_ToCString refuses it outright.
  EXPECT_TRUE(jsThrowsOn(engine, "__lrvx_segment_validate(Symbol('x'))"));
  EXPECT_TRUE(jsThrowsOn(engine, "__lrvx_dw_create(Symbol('x'))"));
  EXPECT_TRUE(jsThrowsOn(engine, "__lrvx_dr_create(Symbol('x'))"));
  EXPECT_TRUE(jsThrowsOn(engine, "__lrvx_part_create(Symbol('x'))"));

  // An object whose toString throws: the conversion propagates that
  // exception rather than handing back a pointer.
  const std::string throwing = "{ toString: function() { throw new Error('nope'); } }";
  EXPECT_TRUE(jsThrowsOn(engine, "__lrvx_dw_create(" + throwing + ")"));
  EXPECT_TRUE(jsThrowsOn(engine, "__lrvx_dr_create(" + throwing + ")"));
  EXPECT_TRUE(jsThrowsOn(engine, "__lrvx_tape_diff('/tmp/a', " + throwing + ")"));

  // The runtime survives all of it.
  EXPECT_TRUE(engine.eval("var alive = 1 + 1;"));
}

TEST(JsIntegrationTest, OptionalStringArgumentIsEitherAbsentOrConvertible)
{
  LrvxJsEngine engine;
  registerLrvxBindings(engine.context());
  ASSERT_TRUE(engine.eval("var threw = false;"));

  // Absent: the binding falls back to its documented default.
  EXPECT_TRUE(engine.eval(R"(
    var series = [];
    for (var i = 0; i < 40; i++) { series.push(Math.sin(i / 3) * 10 + i * 0.01); }
    var withDefault = __lrvx_indicator_adf(series, 4);
    var withExplicit = __lrvx_indicator_adf(series, 4, 'c');
  )"));
  JSValue a = engine.getGlobalProperty("withDefault");
  JSValue b = engine.getGlobalProperty("withExplicit");
  EXPECT_TRUE(JS_IsObject(a));
  EXPECT_TRUE(JS_IsObject(b));
  JS_FreeValue(engine.context(), a);
  JS_FreeValue(engine.context(), b);

  // Present but unconvertible: rejected, not silently defaulted.
  EXPECT_TRUE(jsThrowsOn(engine, "__lrvx_indicator_adf(series, 4, Symbol('c'))"));
}

TEST(JsIntegrationTest, FallbackStringSitesSurviveUnconvertibleValues)
{
  LrvxJsEngine engine;
  registerLrvxBindings(engine.context());
  // console.log formats whatever it is handed; a Symbol argument must not
  // reach a C string function as a null pointer.
  EXPECT_TRUE(engine.eval("console.log(Symbol('x'), 'and text');"));
  EXPECT_TRUE(engine.eval("var alive = 1 + 1;"));
}

// ============================================================
// Nanosecond timestamps across the JS boundary
//
// An exchange timestamp is around 1.76e18 nanoseconds, far past 2^53,
// where a double stops holding every integer. At that magnitude
// neighbouring doubles sit 256 ns apart, so two trades 100 ns apart used
// to arrive in JS carrying the same number. Nothing about that shows up
// until something orders events by timestamp: a merge that resolves ties
// toward the later feed then emits the two in the wrong order. The fields
// cross as BigInt now, so the two values stay distinct and the merge keeps
// the order the engine dispatched.
// ============================================================

TEST(JsIntegrationTest, NanosecondTimestampsKeepEventOrderAcrossFeeds)
{
  TempJsFile script(R"(
    var feedA = [];
    var feedB = [];
    var tsType = "";

    class TestStrat extends Strategy {
      constructor() { super({ exchange: "Test", symbols: ["AAAUSDT", "BBBUSDT"] }); }
      onTrade(ctx, trade) {
        tsType = typeof trade.timestampNs;
        var row = { id: trade.symbol, ts: trade.timestampNs };
        if (trade.symbol === "AAAUSDT") { feedA.push(row); } else { feedB.push(row); }
      }
    }
    lrvx.register(new TestStrat());

    // The merge a tape reader performs: take whichever head is not later
    // than the other. Ties go to feed B.
    function mergeOrder() {
      var out = [];
      var i = 0;
      var j = 0;
      while (i < feedA.length || j < feedB.length) {
        if (i >= feedA.length) { out.push(feedB[j++].id); continue; }
        if (j >= feedB.length) { out.push(feedA[i++].id); continue; }
        if (feedB[j].ts <= feedA[i].ts) { out.push(feedB[j++].id); }
        else { out.push(feedA[i++].id); }
      }
      return out.join(",");
    }
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();
  ASSERT_EQ(symIds.size(), 2u);

  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<LrvxStrategyHandle>(bridge.get()));

  // 100 ns apart, both inside one 256 ns double step.
  const int64_t tsA = 1757000000123456789LL;
  const int64_t tsB = tsA + 100;

  LrvxSymbolContext fctx{};
  LrvxTradeData ftrade{};
  ftrade.price_raw = lrvx_price_from_double(100.0);
  ftrade.quantity_raw = lrvx_quantity_from_double(1.0);
  ftrade.is_buy = 1;

  fctx.symbol_id = symIds[0];
  ftrade.symbol = symIds[0];
  ftrade.exchange_ts_ns = tsA;
  callbacks.on_trade(callbacks.user_data, &fctx, &ftrade);

  fctx.symbol_id = symIds[1];
  ftrade.symbol = symIds[1];
  ftrade.exchange_ts_ns = tsB;
  callbacks.on_trade(callbacks.user_data, &fctx, &ftrade);

  auto* ctx = jsStrat.engine().context();
  ASSERT_TRUE(jsStrat.engine().eval(
      "var order = mergeOrder();"
      "var tsAStr = String(feedA[0].ts);"
      "var tsBStr = String(feedB[0].ts);"));

  auto readString = [&](const char* name)
  {
    JSValue v = jsStrat.engine().getGlobalProperty(name);
    const char* s = JS_ToCString(ctx, v);
    std::string out = s != nullptr ? s : "";
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
    return out;
  };

  // The field is a BigInt, so neither value is rounded on the way in.
  EXPECT_EQ(readString("tsType"), "bigint");
  EXPECT_EQ(readString("tsAStr"), "1757000000123456789");
  EXPECT_EQ(readString("tsBStr"), "1757000000123456889");

  // AAAUSDT was dispatched first and carries the earlier timestamp, so the
  // merge has to emit it first. With both timestamps collapsed onto the
  // same double the tie rule put BBBUSDT in front.
  EXPECT_EQ(readString("order"), "AAAUSDT,BBBUSDT");
}

// An event object carries BigInt nanosecond fields, and JSON.stringify
// refuses a BigInt. The console shim falls back to the plain string
// conversion instead of printing an empty line, and never leaves the
// refusal pending as an exception for the next statement to trip on.
TEST(JsIntegrationTest, ConsoleLogHandlesBigIntFieldsAndUnprintableValues)
{
  LrvxJsEngine engine;
  registerLrvxBindings(engine.context());
  EXPECT_TRUE(engine.eval("console.log({ timestampNs: 1757000000123456789n, price: 1.5 });"));
  EXPECT_TRUE(engine.eval("console.log(Symbol('x'), 'and text');"));
  // A pending exception left behind by either line would surface here.
  EXPECT_TRUE(engine.eval("var afterLogging = 1 + 1;"));
  JSValue v = engine.getGlobalProperty("afterLogging");
  int32_t n = 0;
  JS_ToInt32(engine.context(), &n, v);
  EXPECT_EQ(n, 2);
  JS_FreeValue(engine.context(), v);
}

// ============================================================
// CompositeBook — cross-exchange composite order book
// ============================================================
//
// Node, Codon, and pybind11 could already feed a CompositeBookMatrix
// (create/destroy/query came first; applySnapshot/applyDelta followed once
// the C ABI grew the two ingestion entry points). QuickJS had the query
// side only -- a matrix built from a script stayed empty forever, since
// nothing could ever write to it. These mirror node/test/test_composite_book.js.

// Mirrors "Bid-only delta does not wipe the ask side" in
// node/test/test_composite_book.js: BOOK-12 was a composite-book delta
// zeroing the side it did not touch. This exercises the fix through the
// QuickJS ingestion path specifically, not just the shared C++ core.
TEST(JsIntegrationTest, CompositeBookDeltaOnOneSideLeavesTheOtherUntouched)
{
  TempJsFile script(R"(
    var book = new CompositeBook();
    book.applySnapshot(
      1, 7,
      [100.0], [1.0],
      [100.05], [2.0],
      0n,
    );
    book.applyDelta(
      1, 7,
      [100.01], [3.0],
      [], [],
      0n,
    );
    var bid = book.bestBid(7);
    var ask = book.bestAsk(7);
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  auto* ctx = jsStrat.engine().context();

  auto priceOf = [&](const char* name)
  {
    JSValue quote = jsStrat.engine().getGlobalProperty(name);
    JSValue priceVal = JS_GetPropertyStr(ctx, quote, "price");
    double price = 0;
    JS_ToFloat64(ctx, &price, priceVal);
    JS_FreeValue(ctx, priceVal);
    JS_FreeValue(ctx, quote);
    return price;
  };

  EXPECT_NEAR(priceOf("bid"), 100.01, 1e-9) << "bid moved to the delta price";
  EXPECT_NEAR(priceOf("ask"), 100.05, 1e-9) << "ask side untouched by a bid-only delta";
}

// Mirrors "Arbitrage across two exchanges" in node/test/test_composite_book.js.
TEST(JsIntegrationTest, CompositeBookDetectsArbitrageAcrossExchanges)
{
  TempJsFile script(R"(
    var book = new CompositeBook();
    book.applySnapshot(1, 3, [101.0], [1.0], [101.5], [1.0], 0n);
    book.applySnapshot(2, 3, [102.0], [1.0], [103.0], [1.0], 0n);
    var arb = book.hasArbitrage(3);
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  auto* ctx = jsStrat.engine().context();

  JSValue arb = jsStrat.engine().getGlobalProperty("arb");
  EXPECT_TRUE(JS_ToBool(ctx, arb)) << "a higher bid on exchange 2 than the ask on exchange 1 is arbitrage";
  JS_FreeValue(ctx, arb);
}

// ============================================================
// symbol.bestBid() / bestAsk() / midPrice() answer null, not 0
// ============================================================
//
// The three strategy-side accessors used to come back through
// lrvx_best_bid_raw and its two siblings, which spend the value 0 as their
// "no quote" answer, and the bindings turned that into the JS number 0. A
// strategy reading an empty book got a price of zero and sized against it.
// They now go through the _opt trio and return null when there is no quote,
// which has to stay distinguishable from a real quote at exactly 0.0 -- a
// price a market walking through zero reaches.
namespace
{

double readGlobalDouble(LrvxJsStrategy& strat, const char* name)
{
  JSValue v = strat.engine().getGlobalProperty(name);
  double out = 0;
  JS_ToFloat64(strat.engine().context(), &out, v);
  JS_FreeValue(strat.engine().context(), v);
  return out;
}

// One book update, applied to the strategy's own context the way the bus
// applies one, so the JS hook reads the book the engine holds.
void pushBook(BridgeStrategy& bridge, SymbolId symbol,
              const std::vector<std::pair<double, double>>& bids,
              const std::vector<std::pair<double, double>>& asks)
{
  std::byte buf[4096];
  std::pmr::monotonic_buffer_resource res(buf, sizeof(buf));
  BookUpdateEvent ev(&res);
  ev.update.type = BookUpdateType::SNAPSHOT;
  ev.update.symbol = symbol;
  for (const auto& [price, qty] : bids)
  {
    ev.update.bids.push_back({Price::fromDouble(price), Quantity::fromDouble(qty)});
  }
  for (const auto& [price, qty] : asks)
  {
    ev.update.asks.push_back({Price::fromDouble(price), Quantity::fromDouble(qty)});
  }
  bridge.onBookUpdate(ev);
}

}  // namespace

TEST(JsIntegrationTest, BestQuoteAccessorsAnswerNullForNoQuoteAndZeroForAPriceOfZero)
{
  TempJsFile script(R"(
    var phase = 0;
    var emptyBidNull = false, emptyAskNull = false, emptyMidNull = false;
    var bidZeroIsNumber = false, bidZeroValue = -1;
    var askNullOnBidOnlyBook = false, midNullOnBidOnlyBook = false;
    var askZeroIsNumber = false, askZeroValue = -1;
    var bidNullOnAskOnlyBook = false, midNullOnAskOnlyBook = false;
    var midZeroIsNumber = false, midZeroValue = -1;
    var bidBelowZero = 0, askAboveZero = 0;

    class TestStrat extends Strategy {
      constructor() { super({ exchange: "T", symbols: ["ZERO"] }); }
      onBookUpdate(ctx, book) {
        phase++;
        var bid = this.bestBid("ZERO");
        var ask = this.bestAsk("ZERO");
        var mid = this.midPrice("ZERO");
        if (phase === 1) {
          emptyBidNull = (bid === null);
          emptyAskNull = (ask === null);
          emptyMidNull = (mid === null);
        } else if (phase === 2) {
          bidZeroIsNumber = (typeof bid === 'number');
          bidZeroValue = (typeof bid === 'number') ? bid : -1;
          askNullOnBidOnlyBook = (ask === null);
          midNullOnBidOnlyBook = (mid === null);
        } else if (phase === 3) {
          askZeroIsNumber = (typeof ask === 'number');
          askZeroValue = (typeof ask === 'number') ? ask : -1;
          bidNullOnAskOnlyBook = (bid === null);
          midNullOnAskOnlyBook = (mid === null);
        } else if (phase === 4) {
          midZeroIsNumber = (typeof mid === 'number');
          midZeroValue = (typeof mid === 'number') ? mid : -1;
          bidBelowZero = bid;
          askAboveZero = ask;
        }
      }
    }
    lrvx.register(new TestStrat());
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();
  ASSERT_FALSE(symIds.empty());

  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<LrvxStrategyHandle>(bridge.get()));

  const SymbolId sym = symIds[0];
  // 1: an update carrying no levels at all -- the venue saying the book is
  // gone. Nothing to quote on either side.
  pushBook(*bridge, sym, {}, {});
  // 2: a bid at exactly 0.0 and no ask.
  pushBook(*bridge, sym, {{0.0, 1.0}}, {});
  // 3: an ask at exactly 0.0 and no bid.
  pushBook(*bridge, sym, {}, {{0.0, 1.0}});
  // 4: a two-sided book straddling zero, so the mid is exactly 0.0 while
  // neither quote is. The symbol's tick size is 0.01, so both prices are on
  // an exact tick.
  pushBook(*bridge, sym, {{-0.01, 1.0}}, {{0.01, 1.0}});

  ASSERT_EQ(readGlobalInt32(jsStrat, "phase"), 4) << "the JS hook did not see every update";

  EXPECT_TRUE(readGlobalInt32(jsStrat, "emptyBidNull"))
      << "bestBid() on an empty book is not null -- a strategy reads it as a price of 0";
  EXPECT_TRUE(readGlobalInt32(jsStrat, "emptyAskNull"))
      << "bestAsk() on an empty book is not null";
  EXPECT_TRUE(readGlobalInt32(jsStrat, "emptyMidNull"))
      << "midPrice() on an empty book is not null";

  EXPECT_TRUE(readGlobalInt32(jsStrat, "bidZeroIsNumber"))
      << "a bid at exactly 0.0 comes back as null instead of the number 0";
  EXPECT_DOUBLE_EQ(readGlobalDouble(jsStrat, "bidZeroValue"), 0.0);
  EXPECT_TRUE(readGlobalInt32(jsStrat, "askNullOnBidOnlyBook"))
      << "bestAsk() on a book with no ask is not null";
  EXPECT_TRUE(readGlobalInt32(jsStrat, "midNullOnBidOnlyBook"))
      << "midPrice() on a one-sided book is not null";

  EXPECT_TRUE(readGlobalInt32(jsStrat, "askZeroIsNumber"))
      << "an ask at exactly 0.0 comes back as null instead of the number 0";
  EXPECT_DOUBLE_EQ(readGlobalDouble(jsStrat, "askZeroValue"), 0.0);
  EXPECT_TRUE(readGlobalInt32(jsStrat, "bidNullOnAskOnlyBook"))
      << "bestBid() on a book with no bid is not null";
  EXPECT_TRUE(readGlobalInt32(jsStrat, "midNullOnAskOnlyBook"))
      << "midPrice() on a one-sided book is not null";

  EXPECT_TRUE(readGlobalInt32(jsStrat, "midZeroIsNumber"))
      << "a mid of exactly 0.0 comes back as null instead of the number 0";
  EXPECT_DOUBLE_EQ(readGlobalDouble(jsStrat, "midZeroValue"), 0.0);
  EXPECT_NEAR(readGlobalDouble(jsStrat, "bidBelowZero"), -0.01, 1e-9);
  EXPECT_NEAR(readGlobalDouble(jsStrat, "askAboveZero"), 0.01, 1e-9);
}

// The same four books, read through the objects a callback receives rather
// than the accessors: ctx.book on every callback and book.snapshot on
// onBookUpdate. Both used to write 0 for an empty side, the number a bid at
// exactly 0.0 also writes, so a strategy reading ctx.book.bidPrice could not
// tell the two apart. null is "no quote"; a number, 0 included, is a price.
TEST(JsIntegrationTest, ContextBookAndSnapshotAnswerNullForNoQuoteAndZeroForAPriceOfZero)
{
  TempJsFile script(R"(
    var phase = 0;
    var emptyAllNull = false;
    var bidZeroCtxIsNumber = false, bidZeroCtxValue = -1, bidZeroSnapIsNumber = false;
    var askNullBidOnly = false, midNullBidOnly = false, spreadNullBidOnly = false;
    var askZeroCtxIsNumber = false, bidNullAskOnly = false, midNullAskOnly = false;
    var midZeroIsNumber = false, midZeroValue = -1, spreadValue = -1;
    var snapAgreesWithCtx = true;

    class TestStrat extends Strategy {
      constructor() { super({ exchange: "T", symbols: ["ZERO"] }); }
      onBookUpdate(ctx, book) {
        phase++;
        var b = ctx.book, s = book.snapshot;
        snapAgreesWithCtx = snapAgreesWithCtx &&
          (b.bidPrice === s.bidPrice) && (b.askPrice === s.askPrice) &&
          (b.midPrice === s.midPrice) && (b.spread === s.spread);
        if (phase === 1) {
          emptyAllNull = (b.bidPrice === null && b.askPrice === null &&
                          b.midPrice === null && b.spread === null);
        } else if (phase === 2) {
          bidZeroCtxIsNumber = (typeof b.bidPrice === 'number');
          bidZeroCtxValue = (typeof b.bidPrice === 'number') ? b.bidPrice : -1;
          bidZeroSnapIsNumber = (typeof s.bidPrice === 'number');
          askNullBidOnly = (b.askPrice === null);
          midNullBidOnly = (b.midPrice === null);
          spreadNullBidOnly = (b.spread === null);
        } else if (phase === 3) {
          askZeroCtxIsNumber = (typeof b.askPrice === 'number');
          bidNullAskOnly = (b.bidPrice === null);
          midNullAskOnly = (b.midPrice === null);
        } else if (phase === 4) {
          midZeroIsNumber = (typeof b.midPrice === 'number');
          midZeroValue = (typeof b.midPrice === 'number') ? b.midPrice : -1;
          spreadValue = (typeof b.spread === 'number') ? b.spread : -1;
        }
      }
    }
    lrvx.register(new TestStrat());
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();
  ASSERT_FALSE(symIds.empty());

  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<LrvxStrategyHandle>(bridge.get()));

  const SymbolId sym = symIds[0];
  pushBook(*bridge, sym, {}, {});
  pushBook(*bridge, sym, {{0.0, 1.0}}, {});
  pushBook(*bridge, sym, {}, {{0.0, 1.0}});
  pushBook(*bridge, sym, {{-0.01, 1.0}}, {{0.01, 1.0}});

  ASSERT_EQ(readGlobalInt32(jsStrat, "phase"), 4) << "the JS hook did not see every update";
  EXPECT_TRUE(readGlobalInt32(jsStrat, "snapAgreesWithCtx"))
      << "book.snapshot and ctx.book disagree on the same update";

  EXPECT_TRUE(readGlobalInt32(jsStrat, "emptyAllNull"))
      << "an empty book does not read as null on every field of ctx.book";

  EXPECT_TRUE(readGlobalInt32(jsStrat, "bidZeroCtxIsNumber"))
      << "a bid at exactly 0.0 reads as null on ctx.book instead of the number 0";
  EXPECT_DOUBLE_EQ(readGlobalDouble(jsStrat, "bidZeroCtxValue"), 0.0);
  EXPECT_TRUE(readGlobalInt32(jsStrat, "bidZeroSnapIsNumber"))
      << "a bid at exactly 0.0 reads as null on book.snapshot";
  EXPECT_TRUE(readGlobalInt32(jsStrat, "askNullBidOnly"));
  EXPECT_TRUE(readGlobalInt32(jsStrat, "midNullBidOnly"))
      << "ctx.book.midPrice on a one-sided book is not null";
  EXPECT_TRUE(readGlobalInt32(jsStrat, "spreadNullBidOnly"))
      << "ctx.book.spread on a one-sided book is not null";

  EXPECT_TRUE(readGlobalInt32(jsStrat, "askZeroCtxIsNumber"))
      << "an ask at exactly 0.0 reads as null on ctx.book";
  EXPECT_TRUE(readGlobalInt32(jsStrat, "bidNullAskOnly"));
  EXPECT_TRUE(readGlobalInt32(jsStrat, "midNullAskOnly"));

  EXPECT_TRUE(readGlobalInt32(jsStrat, "midZeroIsNumber"))
      << "a mid of exactly 0.0 reads as null instead of the number 0";
  EXPECT_DOUBLE_EQ(readGlobalDouble(jsStrat, "midZeroValue"), 0.0);
  EXPECT_NEAR(readGlobalDouble(jsStrat, "spreadValue"), 0.02, 1e-9);
}

// ============================================================
// Bar timestamps — one unit and one type on every path
// ============================================================
//
// A bar's `ts` used to mean two different things inside the same binding.
// `Engine.loadCsv` divided the parsed timestamp down to milliseconds and
// handed it over as a Number, while every aggregator emitted
// `start_time_ns` as a BigInt. A script that read one bar from the CSV and
// one from `lrvx.timeBars` was off by a factor of 1e6 in the best case and
// threw a TypeError the moment it subtracted one from the other -- JS
// refuses to mix BigInt and Number in arithmetic. Milliseconds also drop
// everything below the millisecond, which is the whole point of carrying a
// nanosecond timestamp.
//
// The convention these tests pin: every bar timestamp the QuickJS binding
// produces is a BigInt of nanoseconds, whatever produced the bar.

namespace
{

// A CSV file that lives for the duration of one test, mirroring TempJsFile.
class TempCsvFile
{
 public:
  explicit TempCsvFile(const std::string& content)
  {
    _path = std::filesystem::temp_directory_path() /
            ("lrvx_test_bars_" + std::to_string(counter_++) + ".csv");
    std::ofstream f(_path);
    f << content;
  }
  ~TempCsvFile() { std::filesystem::remove(_path); }
  std::string path() const { return _path.string(); }

 private:
  std::filesystem::path _path;
  static int counter_;
};
int TempCsvFile::counter_ = 0;

// Nanosecond timestamps whose sub-millisecond digits are non-zero and
// which sit far above 2^53, so neither a millisecond truncation nor a
// float64 round-trip can reproduce them.
constexpr const char* kCsvTsNs0 = "1776606960123456789";
constexpr const char* kCsvTsNs1 = "1776607020123456789";

std::string barCsvContent()
{
  return std::string("timestamp,open,high,low,close,volume\n") + kCsvTsNs0 +
         ",100.0,101.0,99.0,100.5,3.0\n" + kCsvTsNs1 + ",100.5,102.0,100.0,101.5,4.0\n";
}

// Prepends the CSV path as a global, so the script bodies below stay
// plain JS instead of C++ string concatenation.
std::string withCsvPath(const std::string& path, const std::string& body)
{
  return "var CSV_PATH = \"" + path + "\";\n" + body;
}

// Reads a global back as a string. A BigInt cannot be read through
// JS_ToFloat64 without losing exactly the digits under test.
std::string globalAsString(LrvxJsStrategy& strat, const char* name)
{
  auto* ctx = strat.engine().context();
  JSValue v = strat.engine().getGlobalProperty(name);
  const char* s = JS_ToCString(ctx, v);
  std::string out = s != nullptr ? s : "";
  JS_FreeCString(ctx, s);
  JS_FreeValue(ctx, v);
  return out;
}

}  // namespace

// Engine.loadCsv must hand the script the CSV timestamp in nanoseconds,
// exactly, as a BigInt -- not a millisecond Number.
TEST(JsBarTimestampUnits, LoadCsvBarTimestampIsBigIntNanoseconds)
{
  TempCsvFile csv(barCsvContent());
  TempJsFile script(withCsvPath(csv.path(), R"(
    var bars = lrvx.loadCsv(CSV_PATH);
    var barCount = bars.length;
    var tsType = typeof bars[0].ts;
    var tsStr = String(bars[0].ts);
    var gapStr = (tsType === "bigint") ? String(bars[1].ts - bars[0].ts) : "";
  )"));

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);

  EXPECT_EQ(globalAsString(jsStrat, "barCount"), "2");
  EXPECT_EQ(globalAsString(jsStrat, "tsType"), "bigint")
      << "a bar timestamp is a BigInt on every path";
  EXPECT_EQ(globalAsString(jsStrat, "tsStr"), kCsvTsNs0)
      << "the CSV timestamp reaches the script in nanoseconds, undivided and unrounded";
  EXPECT_EQ(globalAsString(jsStrat, "gapStr"), "60000000000")
      << "one minute between the two rows, measured in nanoseconds";
}

// Control: the aggregators already emit BigInt nanoseconds. That is the
// side of the boundary loadCsv has to meet, so it must stay this way.
TEST(JsBarTimestampUnits, AggregatorBarTimestampIsBigIntNanoseconds)
{
  TempJsFile script(R"(
    var ts  = [1000000000, 61000000000, 121000000000, 181000000000];
    var px  = [100.0, 101.0, 102.0, 103.0];
    var qty = [1.0, 1.0, 1.0, 1.0];
    var side = [0, 1, 0, 1];
    // Tick bars: the bar boundary is a trade count, so the interval
    // argument carries no unit of its own and cannot confuse the reading.
    var aggBars = lrvx.tickBars(ts, px, qty, side, 2);
    var aggCount = aggBars.length;
    var aggTsType = aggCount > 0 ? typeof aggBars[0].ts : "";
    var aggTsStr = aggCount > 0 ? String(aggBars[0].ts) : "";
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);

  ASSERT_NE(globalAsString(jsStrat, "aggCount"), "0") << "the tape closes at least one tick bar";
  EXPECT_EQ(globalAsString(jsStrat, "aggTsType"), "bigint");
  EXPECT_EQ(globalAsString(jsStrat, "aggTsStr"), "1000000000")
      << "the aggregator reports the bar start in nanoseconds";
}

// The failure a user actually hits: one bar from the CSV, one from an
// aggregator, and a subtraction between them. Today the operands are a
// Number and a BigInt and the subtraction throws.
TEST(JsBarTimestampUnits, CsvAndAggregatorBarsMixWithoutTypeError)
{
  TempCsvFile csv(barCsvContent());
  TempJsFile script(withCsvPath(csv.path(), R"(
    var csvBars = lrvx.loadCsv(CSV_PATH);
    var aggBars = lrvx.tickBars([1000000000, 61000000000, 121000000000, 181000000000],
                                [100.0, 101.0, 102.0, 103.0],
                                [1.0, 1.0, 1.0, 1.0],
                                [0, 1, 0, 1],
                                2);
    var mixError = "";
    var deltaType = "";
    var deltaStr = "";
    try {
      var delta = csvBars[0].ts - aggBars[0].ts;
      deltaType = typeof delta;
      deltaStr = String(delta);
    } catch (e) {
      mixError = String(e);
    }
  )"));

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);

  EXPECT_EQ(globalAsString(jsStrat, "mixError"), "")
      << "mixing a CSV bar with an aggregator bar must not throw";
  EXPECT_EQ(globalAsString(jsStrat, "deltaType"), "bigint");
  EXPECT_EQ(globalAsString(jsStrat, "deltaStr"), "1776606959123456789")
      << "the difference is exact nanoseconds, not a rounded double";
}

// Control: the live onBar path already delivers BigInt nanoseconds and
// keeps every digit of a timestamp far above 2^53.
TEST(JsBarTimestampUnits, OnBarBarTimestampIsBigIntNanoseconds)
{
  TempJsFile script(R"(
    var barTsType = "";
    var barTsStr = "";
    class TestStrat extends Strategy {
      constructor() { super({ exchange: "Test", symbols: ["BTCUSDT"] }); }
      onBar(ctx, bar) {
        barTsType = typeof bar.startTimeNs;
        barTsStr = String(bar.startTimeNs);
      }
    }
    lrvx.register(new TestStrat());
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();

  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<LrvxStrategyHandle>(bridge.get()));

  constexpr int64_t kStartNs = 1776606960123456789LL;
  const uint64_t M1_NS = 60ull * 1'000'000'000ull;

  BarEvent ev{};
  ev.symbol = symIds[0];
  ev.barType = BarType::Time;
  ev.barTypeParam = M1_NS;
  ev.bar.open = Price::fromDouble(100.0);
  ev.bar.high = Price::fromDouble(101.0);
  ev.bar.low = Price::fromDouble(99.0);
  ev.bar.close = Price::fromDouble(100.5);
  ev.bar.startTime = TimePoint{std::chrono::nanoseconds{kStartNs}};
  ev.bar.endTime = TimePoint{std::chrono::nanoseconds{kStartNs + static_cast<int64_t>(M1_NS)}};
  bridge->onBar(ev);

  EXPECT_EQ(globalAsString(jsStrat, "barTsType"), "bigint");
  EXPECT_EQ(globalAsString(jsStrat, "barTsStr"), "1776606960123456789");
}

// Control: the signal-list Engine reads the same `ts` field, so a change of
// unit and type there has to be carried through Engine.run's merged
// timeline, its clock and SignalBuilder. The script never names a unit --
// it timestamps its signals from the bars themselves -- so this stays true
// whichever representation the bar carries, and fails if only half of the
// Engine is converted.
TEST(JsBarTimestampUnits, EngineRunAcceptsSignalsTimestampedFromBars)
{
  TempCsvFile csv(barCsvContent());
  TempJsFile script(withCsvPath(csv.path(), R"(
    var engine = new Engine(10000.0, 0.0004);
    engine.loadCsv(CSV_PATH);
    var barCount = engine.barCount;
    var bars = engine._symbols["__default__"];
    var runError = "";
    var totalTrades = -1;
    var finalCapital = 0;
    try {
      var signals = new SignalBuilder();
      signals.buy(bars[0].ts, 0.01);
      signals.sell(bars[1].ts, 0.01);
      var stats = engine.run(signals);
      totalTrades = stats.totalTrades;
      finalCapital = stats.finalCapital;
    } catch (e) {
      runError = String(e);
    }
  )"));

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);

  EXPECT_EQ(globalAsString(jsStrat, "runError"), "");
  EXPECT_EQ(globalAsString(jsStrat, "barCount"), "2");
  EXPECT_EQ(globalAsString(jsStrat, "totalTrades"), "1")
      << "buy on the first bar, sell on the second -- one closed round trip";
  EXPECT_NE(globalAsString(jsStrat, "finalCapital"), "0");
}

// `lrvx.timeBars(ts, px, qty, sides, intervalNs)` documents its interval in
// nanoseconds, but handed the argument straight to
// lrvx_aggregate_time_bars(..., double interval_seconds), so a script
// following the docs asked for 60'000'000'000 seconds -- about 1900 years --
// and got back an empty array, no error said why. lrvx.heikinBars took the
// same argument through the same C entry point and was off by the same 1e9.
// The other aggregators take a trade count, a volume or a price distance,
// so they carry no unit to slip.
TEST(JsBarAggregatorUnits, TimeBarsIntervalIsNanoseconds)
{
  // One trade every 20 seconds across two full minutes, plus one that
  // opens a third bar. Only a closed bar is reported, so a minute interval
  // leaves exactly two.
  TempJsFile script(R"(
    var ts  = [0, 20000000000, 40000000000, 60000000000, 80000000000, 100000000000, 120000000000];
    var px  = [100.0, 101.0, 102.0, 103.0, 104.0, 105.0, 106.0];
    var qty = [1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0];
    var side = [0, 0, 0, 0, 0, 0, 0];
    var MINUTE_NS = 60000000000;

    var bars = lrvx.timeBars(ts, px, qty, side, MINUTE_NS);
    var barCount = bars.length;
    var firstTs = barCount > 0 ? String(bars[0].ts) : "";
    var secondTs = barCount > 1 ? String(bars[1].ts) : "";
    var firstOpen = barCount > 0 ? bars[0].open : 0;
    var firstClose = barCount > 0 ? bars[0].close : 0;
    var secondOpen = barCount > 1 ? bars[1].open : 0;
    var secondClose = barCount > 1 ? bars[1].close : 0;
    var firstTrades = barCount > 0 ? bars[0].trades : 0;

    // The same interval spelled as a BigInt, the way a script that took it
    // off a bar or an event would have it.
    var bigIntBars = lrvx.timeBars(ts, px, qty, side, 60000000000n);
    var bigIntCount = bigIntBars.length;
    var bigIntFirstTs = bigIntCount > 0 ? String(bigIntBars[0].ts) : "";

    var haBars = lrvx.heikinBars(ts, px, qty, side, MINUTE_NS);
    var haCount = haBars.length;
    var haFirstTs = haCount > 0 ? String(haBars[0].ts) : "";
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);

  EXPECT_EQ(globalAsString(jsStrat, "barCount"), "2")
      << "two closed minute bars; an interval read as seconds closes none";
  EXPECT_EQ(globalAsString(jsStrat, "firstTs"), "0");
  EXPECT_EQ(globalAsString(jsStrat, "secondTs"), "60000000000")
      << "the second bar starts one minute -- one interval -- after the first";
  EXPECT_EQ(globalAsString(jsStrat, "firstTrades"), "3")
      << "three trades fall inside the first minute";
  EXPECT_EQ(globalAsString(jsStrat, "firstOpen"), "100");
  EXPECT_EQ(globalAsString(jsStrat, "firstClose"), "102");
  EXPECT_EQ(globalAsString(jsStrat, "secondOpen"), "103");
  EXPECT_EQ(globalAsString(jsStrat, "secondClose"), "105");

  EXPECT_EQ(globalAsString(jsStrat, "bigIntCount"), "2")
      << "a BigInt interval means the same thing as the Number";
  EXPECT_EQ(globalAsString(jsStrat, "bigIntFirstTs"), "0");

  EXPECT_EQ(globalAsString(jsStrat, "haCount"), "2")
      << "heikinBars buckets on the same nanosecond interval";
  EXPECT_EQ(globalAsString(jsStrat, "haFirstTs"), "0");
}

// ============================================================
// Engine.run — the signal timeline, at nanosecond scale
// ============================================================
//
// EngineRunAcceptsSignalsTimestampedFromBars above proves the scale change
// did not break the Engine, but not that the timeline is right: with two
// bars and two signals, Engine.run's unconditional trailing flush (the
// `while (sigIdx < sorted.length)` after the merged-bar loop) submits
// whatever the main loop failed to reach, and the round trip closes anyway.
// A merged timeline divided down to milliseconds, a clock fed
// milliseconds, or a `<` where the boundary needs `<=` all hid behind it.
//
// The fixture below makes the timeline observable. Five bars, each with a
// different close, and a market order fills at the close of the bar it is
// submitted against -- so the bar a signal lands on is readable straight
// off the PnL, and stats.startTimeNs / endTimeNs report the executor clock
// at the first and last fill, which is the bar timestamp in nanoseconds.

namespace
{

// Five one-minute bars from a nanosecond-precision timestamp far above
// 2^53. The closes rise by 10, 20, 30, 40, so a signal landing one bar
// early or one bar late gives a different PnL in either direction.
constexpr const char* kTimelineTs0 = "1776606960123456789";
constexpr const char* kTimelineTs1 = "1776607020123456789";
constexpr const char* kTimelineTs2 = "1776607080123456789";
constexpr const char* kTimelineTs3 = "1776607140123456789";
constexpr const char* kTimelineTs4 = "1776607200123456789";

std::string timelineCsvContent()
{
  return std::string("timestamp,open,high,low,close,volume\n") +
         kTimelineTs0 + ",100.0,100.0,100.0,100.0,1.0\n" +
         kTimelineTs1 + ",110.0,110.0,110.0,110.0,1.0\n" +
         kTimelineTs2 + ",130.0,130.0,130.0,130.0,1.0\n" +
         kTimelineTs3 + ",160.0,160.0,160.0,160.0,1.0\n" +
         kTimelineTs4 + ",200.0,200.0,200.0,200.0,1.0\n";
}

}  // namespace

// A signal timestamped exactly on a bar is applied on that bar -- the
// `<=` boundary -- and the executor clock it fills under is that bar's
// nanosecond timestamp.
TEST(JsBarTimestampUnits, EngineRunAppliesASignalOnTheBarItIsTimestampedOn)
{
  TempCsvFile csv(timelineCsvContent());
  TempJsFile script(withCsvPath(csv.path(), R"(
    var engine = new Engine(10000.0, 0.0);
    engine.loadCsv(CSV_PATH);
    var bars = engine._symbols["__default__"];
    var signals = new SignalBuilder();
    signals.buy(bars[1].ts, 1.0);
    signals.sell(bars[3].ts, 1.0);
    var stats = engine.run(signals);
    var totalTrades = stats.totalTrades;
    var netPnl = stats.netPnl.toFixed(4);
    var finalCapital = stats.finalCapital.toFixed(4);
    var startTimeNs = String(stats.startTimeNs);
    var endTimeNs = String(stats.endTimeNs);
  )"));

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);

  EXPECT_EQ(globalAsString(jsStrat, "totalTrades"), "1");
  // Bought at bar 1's close of 110, sold at bar 3's close of 160. One bar
  // early would be 100 -> 130 (30); one bar late, 130 -> 200 (70).
  EXPECT_EQ(globalAsString(jsStrat, "netPnl"), "50.0000")
      << "the fill prices are the closes of bars 1 and 3, not of their neighbours";
  EXPECT_EQ(globalAsString(jsStrat, "finalCapital"), "10050.0000");
  EXPECT_EQ(globalAsString(jsStrat, "startTimeNs"), kTimelineTs1)
      << "the executor clock at the first fill is bar 1's nanosecond timestamp";
  EXPECT_EQ(globalAsString(jsStrat, "endTimeNs"), kTimelineTs3)
      << "and at the last fill, bar 3's -- both in nanoseconds, undivided";
}

// The other side of the same boundary: one nanosecond past a bar is not
// that bar. The signal waits for the next one, which a millisecond-scaled
// timeline could never distinguish.
TEST(JsBarTimestampUnits, EngineRunHoldsASignalOneNanosecondPastABarUntilTheNextBar)
{
  TempCsvFile csv(timelineCsvContent());
  TempJsFile script(withCsvPath(csv.path(), R"(
    var engine = new Engine(10000.0, 0.0);
    engine.loadCsv(CSV_PATH);
    var bars = engine._symbols["__default__"];
    var signals = new SignalBuilder();
    signals.buy(bars[1].ts + 1n, 1.0);
    signals.sell(bars[3].ts, 1.0);
    var stats = engine.run(signals);
    var totalTrades = stats.totalTrades;
    var netPnl = stats.netPnl.toFixed(4);
    var startTimeNs = String(stats.startTimeNs);
    var endTimeNs = String(stats.endTimeNs);
  )"));

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);

  EXPECT_EQ(globalAsString(jsStrat, "totalTrades"), "1");
  // The buy slid to bar 2 and filled at 130; the sell still lands on bar 3
  // at 160.
  EXPECT_EQ(globalAsString(jsStrat, "netPnl"), "30.0000");
  EXPECT_EQ(globalAsString(jsStrat, "startTimeNs"), kTimelineTs2)
      << "one nanosecond after bar 1 is not bar 1";
  EXPECT_EQ(globalAsString(jsStrat, "endTimeNs"), kTimelineTs3);
}

// SignalBuilder is the one place a script hands the Engine a timestamp it
// did not get from a bar, so it is the one place a Number can still enter
// the timeline. It normalises whatever it is given to a nanosecond BigInt;
// without that, a Number and a BigInt sit side by side in the same sorted
// list and the comparisons downstream are between mixed types.
TEST(JsBarTimestampUnits, SignalBuilderNormalisesEveryTimestampToNanosecondBigInt)
{
  TempJsFile script(R"(
    var signals = new SignalBuilder();
    signals.buy(2000000000, 1.0);          // a plain Number
    signals.sell(1000000000n, 1.0);        // a BigInt
    signals.limitBuy(1500000000, 99.0, 1.0);
    var sorted = signals.sorted();
    var count = sorted.length;
    var types = sorted.map(function(s) { return typeof s.tsNs; }).join(",");
    var order = sorted.map(function(s) { return String(s.tsNs); }).join(",");
    var sides = sorted.map(function(s) { return String(s.side); }).join(",");
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);

  EXPECT_EQ(globalAsString(jsStrat, "count"), "3");
  EXPECT_EQ(globalAsString(jsStrat, "types"), "bigint,bigint,bigint")
      << "a Number handed to buy/sell/limitBuy becomes a nanosecond BigInt";
  EXPECT_EQ(globalAsString(jsStrat, "order"), "1000000000,1500000000,2000000000");
  EXPECT_EQ(globalAsString(jsStrat, "sides"), "1,0,0")
      << "the BigInt-timestamped sell sorts ahead of both Number-timestamped entries";
}

// loadCsv's column-unit detection: the same instant written in seconds,
// milliseconds, microseconds and nanoseconds must all arrive as the same
// nanosecond BigInt. Every other CSV in this file is already nanoseconds,
// which is the one branch of the ladder that multiplies by nothing.
TEST(JsBarTimestampUnits, LoadCsvScalesSecondsMillisecondsAndMicrosecondsToNanoseconds)
{
  auto csvWith = [](const char* ts)
  {
    return std::string("timestamp,open,high,low,close,volume\n") + ts +
           ",100.0,101.0,99.0,100.5,3.0\n";
  };
  TempCsvFile seconds(csvWith("1776606960"));
  TempCsvFile millis(csvWith("1776606960000"));
  TempCsvFile micros(csvWith("1776606960000000"));
  TempCsvFile nanos(csvWith("1776606960000000000"));

  std::string prologue;
  auto bind = [&prologue](const char* name, const TempCsvFile& file)
  { prologue += "var " + std::string(name) + " = \"" + file.path() + "\";\n"; };
  bind("S", seconds);
  bind("MS", millis);
  bind("US", micros);
  bind("NS", nanos);

  TempJsFile script(prologue + R"(
    function firstTs(path) { return String(lrvx.loadCsv(path)[0].ts); }
    var fromSeconds = firstTs(S);
    var fromMillis = firstTs(MS);
    var fromMicros = firstTs(US);
    var fromNanos = firstTs(NS);
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);

  const std::string expected = "1776606960000000000";
  EXPECT_EQ(globalAsString(jsStrat, "fromSeconds"), expected)
      << "a seconds column is multiplied by 1e9";
  EXPECT_EQ(globalAsString(jsStrat, "fromMillis"), expected)
      << "a milliseconds column is multiplied by 1e6";
  EXPECT_EQ(globalAsString(jsStrat, "fromMicros"), expected)
      << "a microseconds column is multiplied by 1e3";
  EXPECT_EQ(globalAsString(jsStrat, "fromNanos"), expected)
      << "a nanoseconds column is passed through";
}

// tickBars closes on a trade count, not on an interval, so nothing about
// its last argument is a nanosecond and nothing may convert it. Counting
// the bars and the trades inside them is what says the count arrived
// intact -- a converted count truncates to zero and closes a bar per
// trade.
TEST(JsBarAggregatorUnits, TickBarsCloseOnTheTradeCountTheyAreGiven)
{
  TempJsFile script(R"(
    var ts  = [1000000000, 2000000000, 3000000000, 4000000000,
               5000000000, 6000000000, 7000000000];
    var px  = [10.0, 11.0, 12.0, 13.0, 14.0, 15.0, 16.0];
    var qty = [1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0];
    var side = [0, 1, 0, 1, 0, 1, 0];
    var bars = lrvx.tickBars(ts, px, qty, side, 3);
    var count = bars.length;
    var trades = bars.map(function(b) { return String(b.trades); }).join(",");
    var starts = bars.map(function(b) { return String(b.ts); }).join(",");
    var opens = bars.map(function(b) { return String(b.open); }).join(",");
    var closes = bars.map(function(b) { return String(b.close); }).join(",");
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);

  EXPECT_EQ(globalAsString(jsStrat, "count"), "2")
      << "seven trades, three to a bar: two bars close and the seventh opens a third";
  EXPECT_EQ(globalAsString(jsStrat, "trades"), "3,3")
      << "each closed bar holds exactly the requested trade count";
  EXPECT_EQ(globalAsString(jsStrat, "starts"), "1000000000,4000000000");
  EXPECT_EQ(globalAsString(jsStrat, "opens"), "10,13");
  EXPECT_EQ(globalAsString(jsStrat, "closes"), "12,15");
}

// The nanosecond interval crosses the C ABI as a double of seconds, and
// the division does not always round back up: 1'000'000'007 ns / 1e9,
// multiplied out again and truncated, lands on 1'000'000'006. An interval
// one nanosecond short moves every bucket boundary, so trades placed on
// either side of the first boundary end up in different bars. An interval
// of a round minute, which is what every other test here uses, divides
// exactly and can never show this.
TEST(JsBarAggregatorUnits, TimeBarsIntervalSurvivesADivisionThatDoesNotRoundBack)
{
  TempJsFile script(R"(
    var I = 1000000007;
    // One trade before the first boundary, one exactly on it, one on the
    // second. With the interval one nanosecond short, the trade at I - 1
    // moves into the second bucket and the bars come back 1,2 instead of
    // 2,1.
    var ts  = [0, I - 1, I, 2 * I];
    var px  = [10.0, 11.0, 12.0, 13.0];
    var qty = [1.0, 1.0, 1.0, 1.0];
    var side = [0, 0, 0, 0];
    var bars = lrvx.timeBars(ts, px, qty, side, I);
    var count = bars.length;
    var starts = bars.map(function(b) { return String(b.ts); }).join(",");
    var trades = bars.map(function(b) { return String(b.trades); }).join(",");
    var closes = bars.map(function(b) { return String(b.close); }).join(",");
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);

  EXPECT_EQ(globalAsString(jsStrat, "count"), "2");
  EXPECT_EQ(globalAsString(jsStrat, "starts"), "0,1000000007")
      << "the second bucket opens at exactly one interval, not one nanosecond early";
  EXPECT_EQ(globalAsString(jsStrat, "trades"), "2,1")
      << "the trade at I - 1 belongs to the first bucket";
  EXPECT_EQ(globalAsString(jsStrat, "closes"), "11,12");
}

// ============================================================
// The shipped example, checked on its numbers
// ============================================================
//
// CI runs every quickjs/examples/*.js through lrvx_js_runner and looks at
// the exit code. An example that scales a bar timestamp the old way still
// exits 0 -- it just backtests against the wrong clock, silently, which is
// precisely the mistake a reader copies out of an example. This runs the
// example's own source and reads the numbers it arrived at.
TEST(JsBarTimestampUnits, BacktestSmaExampleComputesItsPublishedNumbers)
{
  const std::filesystem::path repoRoot{LRVX_REPO_ROOT};
  const std::filesystem::path examplePath = repoRoot / "quickjs" / "examples" / "backtest_sma.js";
  ASSERT_TRUE(std::filesystem::exists(examplePath)) << examplePath;

  std::ifstream in(examplePath);
  std::stringstream body;
  body << in.rdbuf();

  // The example prints its report with console.log; silence it for the
  // duration and read the globals it leaves behind instead.
  TempJsFile script("var __log = console.log; console.log = function() {};\n" + body.str() +
                    R"(
    console.log = __log;
    var exBars = String(n);
    var exSignals = String(signals.length);
    var exTrades = String(stats.totalTrades);
    var exNetPnl = stats.netPnl.toFixed(4);
    var exFinalCapital = stats.finalCapital.toFixed(2);
    var exStartTimeNs = String(stats.startTimeNs);
    var exEndTimeNs = String(stats.endTimeNs);
    var exFirstBarTs = String(bars[0].ts);
  )");

  // The example resolves its CSV relative to the repo root.
  struct CwdGuard
  {
    std::filesystem::path previous;
    explicit CwdGuard(const std::filesystem::path& next) : previous(std::filesystem::current_path())
    {
      std::filesystem::current_path(next);
    }
    ~CwdGuard() { std::filesystem::current_path(previous); }
  } cwd{repoRoot};

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);

  EXPECT_EQ(globalAsString(jsStrat, "exBars"), "3000");
  EXPECT_EQ(globalAsString(jsStrat, "exFirstBarTs"), "1776606960000000000")
      << "the example reads the CSV's millisecond column as nanoseconds";
  EXPECT_EQ(globalAsString(jsStrat, "exSignals"), "117");
  EXPECT_EQ(globalAsString(jsStrat, "exTrades"), "116")
      << "a rescaled signal timestamp submits every signal against the first bar instead";
  EXPECT_EQ(globalAsString(jsStrat, "exNetPnl"), "-95.4962");
  EXPECT_EQ(globalAsString(jsStrat, "exFinalCapital"), "9904.50");
  EXPECT_EQ(globalAsString(jsStrat, "exStartTimeNs"), "1776608700000000000")
      << "the first fill happens on the bar the first crossover names, in nanoseconds";
  EXPECT_EQ(globalAsString(jsStrat, "exEndTimeNs"), "1776786540000000000");
}

// ============================================================
// Integration tests — the executor bar path
// ============================================================

// The QuickJS projection of the bar path -- onBarOhlc, the bar-callback
// window, reset, and closeReason on an aggregated bar -- was registered and
// declared but never executed by a test: the whole surface could be rewired
// or stubbed and every QuickJS case stayed green.
//
// The control is the C++ executor driven over the same tape in this same
// process, so the numbers asserted against the script are the engine's own
// answers rather than literals copied from another file. tests/
// test_capi_executor_bar_ohlc.cpp pins that control against
// BacktestRunner::runBars.
TEST(JsIntegrationTest, SimulatedExecutorDrivesTheBarPath)
{
  constexpr int64_t kMinuteNs = 60'000'000'000LL;
  constexpr SymbolId kSym = 1;

  struct Row
  {
    double open, high, low, close;
  };
  // Bar 1 opens at 106, a price bar 0 never shows, and runs its high to 109.5
  // over the resting sell at 109.
  const std::vector<Row> tape = {
      {100.0, 110.0, 90.0, 105.0}, {106.0, 109.5, 104.0, 107.0}, {103.0, 104.0, 102.0, 103.5}};
  // Flat bar to arm the bracket, then a bar straddling both of its children.
  const std::vector<Row> bracketTape = {{100.0, 100.0, 100.0, 100.0},
                                        {100.0, 108.0, 92.0, 100.0}};

  // ── the control, in C++ ──────────────────────────────────────────────
  SimulatedClock clock;
  SimulatedExecutor control(clock);
  {
    bool sent = false;
    for (size_t i = 0; i < tape.size(); ++i)
    {
      clock.advanceTo(UnixNanos::fromRaw(static_cast<int64_t>(i + 1) * kMinuteNs));
      control.onBar(kSym, Price::fromDouble(tape[i].open), Price::fromDouble(tape[i].high),
                    Price::fromDouble(tape[i].low), Price::fromDouble(tape[i].close));
      SimulatedExecutor::BarCallbackScope window(control);
      if (!sent)
      {
        sent = true;
        Order buy{};
        buy.id = 1;
        buy.symbol = kSym;
        buy.side = Side::BUY;
        buy.type = OrderType::MARKET;
        buy.quantity = Quantity::fromDouble(1.0);
        control.submitOrder(buy);

        Order sell{};
        sell.id = 2;
        sell.symbol = kSym;
        sell.side = Side::SELL;
        sell.type = OrderType::LIMIT;
        sell.price = Price::fromDouble(109.0);
        sell.quantity = Quantity::fromDouble(1.0);
        control.submitOrder(sell);
      }
    }
  }
  ASSERT_EQ(control.fills().size(), 2u);
  const double controlBuy = control.fills()[0].price.toDouble();
  const double controlSell = control.fills()[1].price.toDouble();
  const double controlPnl = controlSell - controlBuy;

  SimulatedClock bracketClock;
  SimulatedExecutor bracketControl(bracketClock);
  {
    bool sent = false;
    for (size_t i = 0; i < bracketTape.size(); ++i)
    {
      bracketClock.advanceTo(UnixNanos::fromRaw(static_cast<int64_t>(i + 1) * kMinuteNs));
      bracketControl.onBar(kSym, Price::fromDouble(bracketTape[i].open),
                           Price::fromDouble(bracketTape[i].high),
                           Price::fromDouble(bracketTape[i].low),
                           Price::fromDouble(bracketTape[i].close));
      SimulatedExecutor::BarCallbackScope window(bracketControl);
      if (!sent)
      {
        sent = true;
        BracketOrder b{};
        b.bracketId = 7;
        b.symbol = kSym;
        b.entry.side = Side::BUY;
        b.entry.type = OrderType::MARKET;
        b.entry.quantity = Quantity::fromDouble(1.0);
        b.takeProfit.side = Side::SELL;
        b.takeProfit.type = OrderType::LIMIT;
        b.takeProfit.price = Price::fromDouble(106.0);
        b.takeProfit.quantity = Quantity::fromDouble(1.0);
        b.stop.side = Side::SELL;
        b.stop.type = OrderType::STOP_MARKET;
        b.stop.triggerPrice = Price::fromDouble(94.0);
        b.stop.quantity = Quantity::fromDouble(1.0);
        bracketControl.submitBracket(b);
      }
    }
  }
  const auto controlState = bracketControl.bracketStatus(7).state;
  ASSERT_EQ(controlState, BracketState::STOP_FILLED)
      << "the control walked the high before the low";

  // ── the same drive, in JS ────────────────────────────────────────────
  TempJsFile script(R"(
    var MIN = 60000000000;
    var TAPE = [[100,110,90,105],[106,109.5,104,107],[103,104,102,103.5]];
    var BRACKET_TAPE = [[100,100,100,100],[100,108,92,100]];

    function driveOn(ex, useWindow) {
      var sent = false;
      for (var i = 0; i < TAPE.length; i++) {
        var b = TAPE[i];
        ex.advanceClock((i + 1) * MIN);
        ex.onBarOhlc(1, b[0], b[1], b[2], b[3]);
        if (useWindow) { ex.beginBarCallbackWindow(); }
        if (!sent) {
          sent = true;
          ex.submitOrder(1, "buy", 0.0, 1.0, 0, 1);     // 0 = market
          ex.submitOrder(2, "sell", 109.0, 1.0, 1, 1);  // 1 = limit
        }
        if (useWindow) { ex.endBarCallbackWindow(); }
      }
    }

    function pnlOf(ex) {
      var r = new BacktestResult(100000.0, 0.0);
      r.ingestExecutor(ex);
      var s = r.stats();
      r.destroy();
      return s.netPnl;
    }

    var held = new SimulatedExecutor();
    driveOn(held, true);
    var heldFills = held.fillCount;
    var heldPnl = pnlOf(held);

    // The same tape with no window open: nothing is held, so the market buy
    // matches inside the bar it was submitted from, at that bar's close.
    var loose = new SimulatedExecutor();
    driveOn(loose, false);
    var loosePnl = pnlOf(loose);

    // reset() drops the run, keeps the configuration: a repeat is the run,
    // not the sum of both.
    held.reset();
    var fillsAfterReset = held.fillCount;
    driveOn(held, true);
    var repeatFills = held.fillCount;
    var repeatPnl = pnlOf(held);

    var bracketEx = new SimulatedExecutor();
    var armed = false;
    for (var i = 0; i < BRACKET_TAPE.length; i++) {
      var b = BRACKET_TAPE[i];
      bracketEx.advanceClock((i + 1) * MIN);
      bracketEx.onBarOhlc(1, b[0], b[1], b[2], b[3]);
      bracketEx.beginBarCallbackWindow();
      if (!armed) {
        armed = true;
        bracketEx.submitBracket({
          bracketId: 7, symbol: 1,
          entrySide: "buy", entryType: "market", entryPrice: 0.0, quantity: 1.0,
          tpSide: "sell", tpType: "limit", tpPrice: 106.0,
          stopSide: "sell", stopType: "stop_market", stopTriggerPrice: 94.0,
        });
      }
      bracketEx.endBarCallbackWindow();
    }
    var bracketState = bracketEx.bracketState(7);
    var bracketFills = bracketEx.fillCount;

    var bars = lrvx.timeBars([0, 30000000000, 61000000000, 91000000000, 121000000000],
                             [100, 101, 102, 103, 104], [1, 1, 1, 1, 1], [1, 1, 1, 1, 1], 60);
    var barCount = bars.length;
    var hasCloseReason = barCount > 0 && ("closeReason" in bars[0]);
    var allThreshold = hasCloseReason && bars.every(function (b) { return b.closeReason === 0; });

    held.destroy();
    loose.destroy();
    bracketEx.destroy();
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  auto* ctx = jsStrat.engine().context();

  auto num = [&](const char* name)
  {
    JSValue v = jsStrat.engine().getGlobalProperty(name);
    double d = 0;
    JS_ToFloat64(ctx, &d, v);
    JS_FreeValue(ctx, v);
    return d;
  };
  auto flag = [&](const char* name)
  {
    JSValue v = jsStrat.engine().getGlobalProperty(name);
    const bool b = JS_ToBool(ctx, v) == 1;
    JS_FreeValue(ctx, v);
    return b;
  };
  auto str = [&](const char* name)
  {
    JSValue v = jsStrat.engine().getGlobalProperty(name);
    const char* s = JS_ToCString(ctx, v);
    std::string out = s ? s : "";
    if (s)
    {
      JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);
    return out;
  };

  EXPECT_EQ(num("heldFills"), 2.0) << "the hand-driven JS run did not fill twice";
  EXPECT_NEAR(num("heldPnl"), controlPnl, 1e-9)
      << "the JS bar path realised " << num("heldPnl") << " where the C++ control, on the "
      << "same tape, realised " << controlPnl << " (buy " << controlBuy << ", sell "
      << controlSell << ")";
  EXPECT_NEAR(num("loosePnl"), controlSell - tape[0].close, 1e-9)
      << "with no window open the buy has to match at the close of the bar it was "
         "submitted from";
  EXPECT_NE(num("loosePnl"), num("heldPnl"))
      << "beginBarCallbackWindow changed nothing: the order was never held";

  EXPECT_EQ(num("fillsAfterReset"), 0.0) << "reset left the previous run's fills behind";
  EXPECT_EQ(num("repeatFills"), 2.0);
  EXPECT_NEAR(num("repeatPnl"), controlPnl, 1e-9) << "the repeat run did not repeat";

  EXPECT_EQ(str("bracketState"), "stop_filled")
      << "the walk reached the high (108) before the low (92), so the take-profit filled "
         "instead of the stop";
  EXPECT_EQ(num("bracketFills"), 2.0);

  EXPECT_GE(num("barCount"), 1.0);
  EXPECT_TRUE(flag("hasCloseReason"))
      << "an aggregated bar reaches JS with no closeReason; the Python and Node "
         "aggregator bindings both carry it";
  EXPECT_TRUE(flag("allThreshold"))
      << "an aggregated bar reports a close reason the aggregator never set";
}

// ============================================================
// C ABI version handshake
// ============================================================

// lrvx_capi.h asks every caller to "compare LRVX_CAPI_ABI_VERSION against
// lrvx_capi_abi_version() once at startup and refuse the mismatch". The
// embedded engine registers its bindings once, at registerLrvxBindings,
// which is where that comparison belongs; what a script can observe is
// the pair of numbers it compared. The refusal itself is pinned in
// tests/test_capi_abi_check.cpp, which can force a wrong number.
TEST(JsEngineTest, AbiVersionPairIsExposedAndAgrees)
{
  LrvxJsEngine engine;
  registerLrvxBindings(engine.context());

  JSValue compiled = engine.getGlobalProperty("__LRVX_CAPI_ABI_VERSION");
  EXPECT_TRUE(JS_IsNumber(compiled))
      << "the version the binding was compiled against is not exposed";
  JS_FreeValue(engine.context(), compiled);

  JSValue fn = engine.getGlobalProperty("__lrvx_capi_abi_version");
  EXPECT_TRUE(JS_IsFunction(engine.context(), fn))
      << "the version the library reports is not exposed";
  JS_FreeValue(engine.context(), fn);

  ASSERT_TRUE(engine.eval(R"(
    if (typeof __LRVX_CAPI_ABI_VERSION !== 'number') {
      throw new Error('__LRVX_CAPI_ABI_VERSION missing');
    }
    if (typeof __lrvx_capi_abi_version !== 'function') {
      throw new Error('__lrvx_capi_abi_version missing');
    }
    if (__lrvx_capi_abi_version() !== __LRVX_CAPI_ABI_VERSION) {
      throw new Error('ABI skew: compiled against ' + __LRVX_CAPI_ABI_VERSION +
                      ', library reports ' + __lrvx_capi_abi_version());
    }
  )")) << engine.getErrorMessage();
}

// An engine that loaded the wrong library gets no surface at all: the
// refusal is the registration not happening, not a flag a script could
// read past. Pinning the returned false alone would leave a binding free
// to compute the refusal and register everything anyway.
TEST(JsEngineTest, AbiMismatchRegistersNothing)
{
  ReportedAbiVersion reported(LRVX_CAPI_ABI_VERSION + 7);

  LrvxJsEngine engine;
  JSContext* ctx = engine.context();
  EXPECT_FALSE(registerLrvxBindings(ctx)) << "registration accepted a skewed library";

  for (const char* name : {"__lrvx_emit_market_buy", "__lrvx_capi_abi_version", "console"})
  {
    JSValue value = engine.getGlobalProperty(name);
    EXPECT_TRUE(JS_IsUndefined(value)) << name << " was registered despite the refusal";
    JS_FreeValue(ctx, value);
  }

  JSValue pending = JS_GetException(ctx);
  EXPECT_FALSE(JS_IsNull(pending)) << "the refusal left no exception for the host";
  const char* text = JS_ToCString(ctx, pending);
  const std::string message = text != nullptr ? text : "";
  if (text != nullptr)
  {
    JS_FreeCString(ctx, text);
  }
  JS_FreeValue(ctx, pending);

  EXPECT_NE(message.find("built against version " + std::to_string(LRVX_CAPI_ABI_VERSION)),
            std::string::npos)
      << message;
  EXPECT_NE(message.find("reports version " + std::to_string(LRVX_CAPI_ABI_VERSION + 7)),
            std::string::npos)
      << message;
}

// The matched case still has to come up, or the test above would pass on
// a binding that refuses everything.
TEST(JsEngineTest, MatchedAbiRegistersTheSurface)
{
  LrvxJsEngine engine;
  EXPECT_TRUE(registerLrvxBindings(engine.context()));

  JSValue fn = engine.getGlobalProperty("__lrvx_emit_market_buy");
  EXPECT_TRUE(JS_IsFunction(engine.context(), fn));
  JS_FreeValue(engine.context(), fn);
}

// The QuickJS executor takes a touch-only frame: the touch it carries is
// what a market order matches against, and a null side has nothing to
// match.
TEST(JsIntegrationTest, SimulatedExecutorTakesTopOfBook)
{
  TempJsFile script(R"(
    var ex = new SimulatedExecutor();
    ex.onTopOfBook(1, { price: 99.5, qty: 5.0 }, { price: 100.5, qty: 2.0 });
    ex.submitOrder(1, "buy", 0.0, 1.0, 0, 1);
    var fillsAfterTop = ex.fillCount;
    ex.onTopOfBook(1, null, { price: 100.5, qty: 2.0 });
    ex.submitOrder(2, "sell", 0.0, 1.0, 0, 1);
    var fillsAfterNullBid = ex.fillCount;
    var badSideThrew = false;
    try { ex.onTopOfBook(1, 99.5, null); } catch (e) { badSideThrew = e instanceof TypeError; }
    ex.destroy();
  )");

  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  auto* ctx = jsStrat.engine().context();

  auto num = [&](const char* name)
  {
    JSValue v = jsStrat.engine().getGlobalProperty(name);
    double d = 0;
    JS_ToFloat64(ctx, &d, v);
    JS_FreeValue(ctx, v);
    return d;
  };

  EXPECT_EQ(num("fillsAfterTop"), 1.0) << "a market buy matches the fed ask touch";
  EXPECT_EQ(num("fillsAfterNullBid"), 1.0)
      << "no bid side, nothing for a market sell to match";
  EXPECT_EQ(num("badSideThrew"), 1.0)
      << "a bare number where a side object belongs must throw";
}

namespace
{
const char* const kTouchStrategyScript = R"(
    var touchCount = 0;
    var touchSymbol = "";
    var touchBid = 0;
    var touchAsk = 0;
    class TouchStrat extends Strategy {
      constructor() { super({ exchange: "Test", symbols: ["BTCUSDT"] }); }
      onTouch(ctx, book) {
        touchCount++;
        touchSymbol = book.symbol;
        touchBid = book.snapshot.bidPrice;
        touchAsk = book.snapshot.askPrice === null ? -1 : book.snapshot.askPrice;
      }
    }
    lrvx.register(new TouchStrat());
  )";

LrvxBookData touchFrame(uint32_t sym, double bid)
{
  LrvxBookData fbook{};
  fbook.symbol = sym;
  fbook.snapshot.has_bid = 1;
  fbook.snapshot.bid_price_raw = lrvx_price_from_double(bid);
  return fbook;
}
}  // namespace

TEST(JsIntegrationTest, OnTouchReceivesTheTouch)
{
  TempJsFile script(kTouchStrategyScript);
  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();
  ASSERT_NE(callbacks.on_touch, nullptr);

  LrvxSymbolContext fctx{};
  fctx.symbol_id = symIds[0];
  LrvxBookData fbook = touchFrame(symIds[0], 100.02);
  callbacks.on_touch(callbacks.user_data, &fctx, &fbook);

  auto* ctx = jsStrat.engine().context();
  EXPECT_EQ(readGlobalInt32(jsStrat, "touchCount"), 1);
  JSValue sym = jsStrat.engine().getGlobalProperty("touchSymbol");
  const char* symStr = JS_ToCString(ctx, sym);
  EXPECT_STREQ(symStr, "BTCUSDT");
  JS_FreeCString(ctx, symStr);
  JS_FreeValue(ctx, sym);
  JSValue bid = jsStrat.engine().getGlobalProperty("touchBid");
  double bidVal = 0;
  JS_ToFloat64(ctx, &bidVal, bid);
  EXPECT_NEAR(bidVal, 100.02, 1e-9);
  JS_FreeValue(ctx, bid);
  EXPECT_EQ(readGlobalInt32(jsStrat, "touchAsk"), -1) << "an empty ask side is null";
}

TEST(JsExecutorThreadingTest, ExecutorForwardsTouch)
{
  TempJsFile script(kTouchStrategyScript);
  SymbolRegistry registry;
  LrvxJsExecutor executor(script.path(), registry);
  auto symIds = executor.symbolIds();
  ASSERT_FALSE(symIds.empty());
  auto callbacks = executor.getCallbacks();
  ASSERT_NE(callbacks.on_touch, nullptr);

  LrvxSymbolContext fctx{};
  fctx.symbol_id = symIds[0];
  for (int i = 0; i < 3; ++i)
  {
    LrvxBookData fbook = touchFrame(symIds[0], 100.0 + i);
    callbacks.on_touch(callbacks.user_data, &fctx, &fbook);
  }
  executor.waitIdle();
  EXPECT_EQ(executor.getGlobalInt32("touchCount"), 3);
}

TEST(JsIntegrationTest, NoOnTouchMeansNoTouchSlot)
{
  TempJsFile script(R"(
    class Plain extends Strategy {
      constructor() { super({ exchange: "Test", symbols: ["BTCUSDT"] }); }
      onTrade(ctx, trade) {}
    }
    lrvx.register(new Plain());
  )");
  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  EXPECT_EQ(jsStrat.getCallbacks().on_touch, nullptr)
      << "a strategy without onTouch pays nothing for a touch move";

  LrvxJsExecutor executor(script.path(), registry);
  EXPECT_EQ(executor.getCallbacks().on_touch, nullptr);
}
