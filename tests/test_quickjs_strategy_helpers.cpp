// QuickJS surface of the strategy helpers: the position facts on ctx
// (entryPrice, barsInPosition), the exit rules (AtrStop, ChandelierExit,
// BreakEvenTrail, PercentStop, TakeProfit, TimeStop), History with
// crossedAbove / crossedBelow, and the Highest / Lowest indicators.
//
// The exit scenarios are the ones tests/test_exits.cpp and
// python/tests/test_exits.py pin, so every binding is held to the same
// levels. The JS side records each failed check as a line in `fails`; the
// C++ side prints them.

#include "js_bindings.h"
#include "js_engine.h"
#include "js_strategy.h"

#include "lrvx/aggregator/events/bar_event.h"
#include "lrvx/backtest/backtest_config.h"
#include "lrvx/backtest/backtest_runner.h"
#include "lrvx/capi/bridge_strategy.h"
#include "lrvx/capi/lrvx_capi.h"
#include "lrvx/engine/symbol_registry.h"
#include "lrvx/replay/binary_format_v1.h"
#include "lrvx/replay/writers/binary_log_writer.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace lrvx;

namespace
{

class ScriptFile
{
 public:
  explicit ScriptFile(const std::string& content)
  {
    _path = std::filesystem::temp_directory_path() /
            ("lrvx_test_helpers_" + std::to_string(std::random_device{}()) + "_" +
             std::to_string(counter++) + ".js");
    std::ofstream f(_path);
    f << content;
  }
  ~ScriptFile() { std::filesystem::remove(_path); }
  std::string path() const { return _path.string(); }

 private:
  std::filesystem::path _path;
  static inline int counter = 0;
};

// The prelude every script here starts with: check() records a failure, and
// section() runs a block, recording a throw (for instance a ReferenceError
// for a class the binding does not define) as one failure.
const char* const kPrelude = R"(
var fails = [];
var checks = 0;
function check(cond, msg) { checks++; if (!cond) fails.push(msg); }
function section(name, fn) {
  try { fn(); } catch (e) { fails.push(name + ' threw: ' + e); }
}
function throws(fn) { try { fn(); return false; } catch (e) { return true; } }
function isNum(v) { return typeof v === 'number'; }
function absent(v) { return v === null || (typeof v === 'number' && isNaN(v)); }
function same(a, b) {
  if (absent(a) || absent(b)) return absent(a) && absent(b);
  return a === b;
}
function show(v) { return v === null ? 'null' : String(v); }
)";

std::string readString(LrvxJsStrategy& strat, const char* expr)
{
  JSContext* ctx = strat.engine().context();
  JSValue v = JS_Eval(ctx, expr, std::strlen(expr), "<read>", JS_EVAL_TYPE_GLOBAL);
  std::string out;
  if (JS_IsException(v))
  {
    JSValue exc = JS_GetException(ctx);
    const char* s = JS_ToCString(ctx, exc);
    out = std::string("<exception: ") + (s ? s : "?") + ">";
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, exc);
    return out;
  }
  const char* s = JS_ToCString(ctx, v);
  out = s ? s : "";
  JS_FreeCString(ctx, s);
  JS_FreeValue(ctx, v);
  return out;
}

int readInt(LrvxJsStrategy& strat, const char* name)
{
  JSValue v = strat.engine().getGlobalProperty(name);
  int32_t out = 0;
  JS_ToInt32(strat.engine().context(), &out, v);
  JS_FreeValue(strat.engine().context(), v);
  return out;
}

void expectNoFails(LrvxJsStrategy& strat, int minChecks)
{
  EXPECT_EQ(readString(strat, "fails.join('\\n')"), "");
  EXPECT_GE(readInt(strat, "checks"), minChecks) << "the checks did not all run";
}

void runUnitScript(const std::string& body, int minChecks)
{
  ScriptFile script(std::string(kPrelude) + body);
  SymbolRegistry registry;
  LrvxJsStrategy jsStrat(script.path(), registry);
  expectNoFails(jsStrat, minChecks);
}

}  // namespace

// ============================================================
// Exit rules
// ============================================================

TEST(JsExitRules, AtrStopIsFrozenAtEntryMinusMultipleOfAtr)
{
  runUnitScript(R"(
var LONG_BARS = [[103,99,2],[106,102,2],[110,105,2.5],[108,104,2.5],[105,101.5,2.5],[103,97,3]];
section('AtrStop', function () {
  var s = new AtrStop(2.0);
  check(s.armed === false, 'AtrStop: not armed before arm');
  check(s.update(101, 99, 2) === false, 'AtrStop: nothing is watched before arm');
  s.arm('long', 100, 2);
  check(s.armed === true, 'AtrStop: armed after arm');
  check(s.stop === 96, 'AtrStop long stop ' + show(s.stop) + ', want 96');
  var hit = false;
  for (var i = 0; i < LONG_BARS.length && !hit; i++) {
    var b = LONG_BARS[i];
    hit = s.update(b[0], b[1], b[2]);
    check(s.stop === 96, 'AtrStop: the stop does not move (' + show(s.stop) + ')');
  }
  check(hit === false, 'AtrStop: the lowest low 97 stays above 96');
  s.arm('short', 100, 2);
  check(s.stop === 104, 'AtrStop short stop ' + show(s.stop) + ', want 104');
  check(s.update(103, 99, 2) === false, 'AtrStop short: high 103 below 104');
  check(s.update(104, 101, 2) === true, 'AtrStop short: high == stop is a hit');
  check(s.update(104, 101, 2) === true, 'AtrStop: a hit is reported until disarm');
  s.disarm();
  check(s.armed === false, 'AtrStop: disarmed');
  check(absent(s.stop), 'AtrStop: no stop after disarm (' + show(s.stop) + ')');
  check(throws(function () { s.arm('buy', 100, 2); }), "AtrStop: side 'buy' is refused");
  if (typeof s.destroy === 'function') s.destroy();
});
)",
                14);
}

TEST(JsExitRules, ChandelierExitHangsBelowTheHighestHighAndOnlyRises)
{
  runUnitScript(R"(
section('ChandelierExit', function () {
  var r = new ChandelierExit(0, 3.0);
  r.arm('long', 100, 2);
  check(r.stop === 94, 'Chandelier: initial stop ' + show(r.stop) + ', want 94');
  var bars = [[103,99,2,97],[106,102,2,100],[110,105,2.5,102.5],[108,104,2.5,102.5]];
  for (var i = 0; i < bars.length; i++) {
    var b = bars[i];
    check(r.update(b[0], b[1], b[2]) === false, 'Chandelier bar ' + i + ': no hit');
    check(r.stop === b[3], 'Chandelier bar ' + i + ': stop ' + show(r.stop) + ', want ' + b[3]);
  }
  check(r.update(105, 101.5, 2.5) === true, 'Chandelier: low 101.5 reaches 102.5');
  check(r.stop === 102.5, 'Chandelier: stop stays 102.5');

  var lb = new ChandelierExit(2, 3.0);
  lb.arm('long', 100, 2);
  for (var j = 0; j < bars.length; j++) lb.update(bars[j][0], bars[j][1], bars[j][2]);
  check(lb.stop === 102.5, 'Chandelier lookback 2: stop ' + show(lb.stop));
  check(lb.update(105, 101.5, 2.5) === true, 'Chandelier lookback 2: hit');
  check(lb.stop === 102.5, 'Chandelier lookback 2: the stop never falls');

  var sh = new ChandelierExit(0, 3.0);
  sh.arm('short', 100, 2);
  check(sh.stop === 106, 'Chandelier short: initial 106, got ' + show(sh.stop));
  check(sh.update(101, 97, 2) === false && sh.stop === 103, 'Chandelier short: 103');
  check(sh.update(99, 94, 2) === false && sh.stop === 100, 'Chandelier short: 100');
  check(sh.update(100.5, 96, 2) === true, 'Chandelier short: high 100.5 >= 100');
  sh.disarm();
  check(sh.armed === false && absent(sh.stop), 'Chandelier: disarm clears the stop');
  [r, lb, sh].forEach(function (x) { if (typeof x.destroy === 'function') x.destroy(); });
});
)",
                18);
}

TEST(JsExitRules, BreakEvenTrailMovesToEntryOnTheTriggerThenTrails)
{
  runUnitScript(R"(
section('BreakEvenTrail', function () {
  var r = new BreakEvenTrail(2.0, 3.0, 1.5);
  r.arm('long', 100, 2);
  check(r.stop === 96 && r.atBreakEven === false, 'BreakEvenTrail: hard stop 96, got ' + show(r.stop));
  check(r.update(103, 99, 2) === false, 'BreakEvenTrail bar 0: no hit');
  check(r.stop === 96 && r.atBreakEven === false, 'BreakEvenTrail bar 0: still 96');
  check(r.update(106, 102, 2) === false, 'BreakEvenTrail bar 1: no hit');
  check(r.atBreakEven === true && r.stop === 103, 'BreakEvenTrail bar 1: break even, trail 103, got ' + show(r.stop));
  check(r.update(110, 105, 2.5) === false, 'BreakEvenTrail bar 2: no hit');
  check(r.stop === 106.25, 'BreakEvenTrail bar 2: 106.25, got ' + show(r.stop));
  check(r.update(108, 104, 2.5) === true, 'BreakEvenTrail bar 3: low 104 reaches 106.25');
  check(r.stop === 106.25, 'BreakEvenTrail bar 3: stop holds');
  r.disarm();
  check(r.armed === false && r.atBreakEven === false && absent(r.stop), 'BreakEvenTrail: disarm');
  if (typeof r.destroy === 'function') r.destroy();
});
)",
                10);
}

TEST(JsExitRules, PercentStopLevelsAndHits)
{
  runUnitScript(R"(
section('PercentStop', function () {
  check(throws(function () { new PercentStop(0); }), 'PercentStop(0) is refused');
  check(throws(function () { new PercentStop(-1); }), 'PercentStop(-1) is refused');
  check(throws(function () { new PercentStop(NaN); }), 'PercentStop(NaN) is refused');
  var s = new PercentStop(5);
  check(s.pct === 5, 'PercentStop.pct');
  check(s.armed === false, 'PercentStop: not armed before arm');
  check(s.update(300, 1) === false, 'PercentStop: update before arm is false');
  s.arm('long', 200);
  check(s.armed === true, 'PercentStop: armed');
  check(s.stop === 190, 'PercentStop long@200: stop ' + show(s.stop) + ', want 190');
  check(s.update(215, 195) === false, 'PercentStop long: low 195 above 190');
  check(s.update(212, 190) === true, 'PercentStop long: low == stop is a hit');
  check(s.update(212, 190) === true, 'PercentStop: a hit does not disarm');
  check(s.armed === true, 'PercentStop: still armed after the hit');
  s.arm('short', 200);
  check(s.stop === 210, 'PercentStop short@200: stop ' + show(s.stop) + ', want 210');
  check(s.update(209, 195) === false, 'PercentStop short: high 209 below 210');
  check(s.update(210, 201) === true, 'PercentStop short: high == stop is a hit');
  s.disarm();
  check(s.armed === false && absent(s.stop), 'PercentStop: disarm clears the stop');
  check(s.update(1000, 1) === false, 'PercentStop: disarmed watches nothing');
  // The level is entry * (1 -/+ pct / 100), bit for bit the same on every side.
  var f = new PercentStop(2.5);
  f.arm('short', 100);
  check(f.stop === 100 * (1 + 2.5 / 100), 'PercentStop(2.5) short@100: ' + show(f.stop));
  check(f.stop === 102.49999999999999, 'PercentStop(2.5) short@100 pinned');
  f.arm('long', 100);
  check(f.stop === 97.5, 'PercentStop(2.5) long@100: ' + show(f.stop));
  check(throws(function () { f.arm('buy', 100); }), "PercentStop: side 'buy' is refused");
  [s, f].forEach(function (x) { if (typeof x.destroy === 'function') x.destroy(); });
});
)",
                22);
}

TEST(JsExitRules, TakeProfitLevelsAndHits)
{
  runUnitScript(R"(
section('TakeProfit', function () {
  check(throws(function () { new TakeProfit(0); }), 'TakeProfit(0) is refused');
  check(throws(function () { new TakeProfit(-5); }), 'TakeProfit(-5) is refused');
  var t = new TakeProfit(25);
  check(t.pct === 25, 'TakeProfit.pct');
  check(t.update(1000, 1) === false, 'TakeProfit: update before arm is false');
  t.arm('long', 200);
  check(t.target === 250, 'TakeProfit long@200: target ' + show(t.target) + ', want 250');
  check(t.update(249, 190) === false, 'TakeProfit long: high 249 short of 250');
  check(t.update(250, 205) === true, 'TakeProfit long: high == target is a hit');
  check(t.update(250, 205) === true, 'TakeProfit: a hit does not disarm');
  t.arm('short', 200);
  check(t.target === 150, 'TakeProfit short@200: target ' + show(t.target) + ', want 150');
  check(t.update(195, 151) === false, 'TakeProfit short: low 151 above 150');
  check(t.update(190, 150) === true, 'TakeProfit short: low == target is a hit');
  t.disarm();
  check(t.armed === false && absent(t.target), 'TakeProfit: disarm clears the target');
  if (typeof t.destroy === 'function') t.destroy();
});
)",
                12);
}

TEST(JsExitRules, TimeStopFiresOnTheMaxBarsUpdate)
{
  runUnitScript(R"(
section('TimeStop', function () {
  check(throws(function () { new TimeStop(0); }), 'TimeStop(0) is refused');
  var t = new TimeStop(3);
  check(t.maxBars === 3, 'TimeStop.maxBars');
  check(t.armed === false && t.update() === false, 'TimeStop: update before arm is false');
  t.arm();
  check(t.armed === true && t.bars === 0, 'TimeStop: armed, no bars yet');
  var got = [t.update(), t.update(), t.update(), t.update()];
  check(JSON.stringify(got) === '[false,false,true,true]', 'TimeStop(3) updates ' + JSON.stringify(got));
  check(t.bars === 4, 'TimeStop: bars counts updates since arm (' + t.bars + ')');
  t.disarm();
  check(t.armed === false && t.bars === 0, 'TimeStop: disarm resets');
  t.arm();
  check(t.update() === false && t.bars === 1, 'TimeStop: re-arm restarts the count');
  var one = new TimeStop(1);
  one.arm();
  check(one.update() === true, 'TimeStop(1): the first update fires');
  [t, one].forEach(function (x) { if (typeof x.destroy === 'function') x.destroy(); });
});
)",
                9);
}

// ============================================================
// History and crossings
// ============================================================

TEST(JsHistory, NewestFirstRingWithBounds)
{
  runUnitScript(R"(
section('History', function () {
  check(throws(function () { new History(0); }), 'History(0) is refused');
  var h = new History(3);
  check(h.capacity === 3, 'History.capacity');
  check(h.length === 0 && h.full === false, 'History: empty');
  check(throws(function () { h.get(0); }), 'History: get(0) on empty throws');
  h.push(10);
  check(h.get(0) === 10 && h.length === 1, 'History: one value');
  h.push(20);
  check(h.get(0) === 20 && h.get(1) === 10, 'History: 0 is newest, 1 previous');
  check(h.full === false, 'History: not full at 2 of 3');
  h.push(30);
  check(h.full === true && h.length === 3, 'History: full at 3');
  h.push(40);
  check(h.get(0) === 40 && h.get(1) === 30 && h.get(2) === 20, 'History: oldest dropped');
  check(h.length === 3, 'History: length stays at capacity');
  check(throws(function () { h.get(3); }), 'History: get(length) throws');
  h.push(NaN);
  check(Number.isNaN(h.get(0)) && h.get(1) === 40, 'History: NaN stored as given');
  var one = new History(1);
  one.push(1); one.push(2);
  check(one.get(0) === 2 && one.length === 1 && one.full === true, 'History(1)');
  [h, one].forEach(function (x) { if (typeof x.destroy === 'function') x.destroy(); });
});
)",
                13);
}

TEST(JsHistory, CrossedAboveAndBelowEdges)
{
  runUnitScript(R"(
section('crossings', function () {
  // A fixed level is passed as both b values.
  check(crossedAbove(1, 3, 2, 2) === true, 'above: from below to above');
  check(crossedAbove(2, 3, 2, 2) === true, 'above: leaving from an equal start is a cross');
  check(crossedAbove(1, 2, 2, 2) === false, 'above: touching is not a cross');
  check(crossedAbove(3, 4, 2, 2) === false, 'above: already above');
  check(crossedAbove(2, 2, 2, 2) === false, 'above: equal throughout');
  check(crossedAbove(3, 1, 2, 2) === false, 'above: a fall is not a rise');
  check(crossedBelow(3, 1, 2, 2) === true, 'below: from above to below');
  check(crossedBelow(2, 1, 2, 2) === true, 'below: leaving from an equal start is a cross');
  check(crossedBelow(3, 2, 2, 2) === false, 'below: touching is not a cross');
  check(crossedBelow(1, 0, 2, 2) === false, 'below: already below');
  check(crossedBelow(2, 2, 2, 2) === false, 'below: equal throughout');
  // Two moving series.
  check(crossedAbove(1, 3, 2, 2.5) === true, 'above: two series');
  check(crossedAbove(1, 2.5, 0.5, 3) === false, 'above: a was above b before');
  check(crossedBelow(3, 1, 2, 1.5) === true, 'below: two series');
  check(crossedBelow(10, 5, 4, 6) === true, 'below: b rising through a');
  check(crossedAbove(5, 10, 6, 4) === true, 'above: b falling through a');
  var bad = [NaN, null, undefined];
  for (var k = 0; k < bad.length; k++) {
    var x = bad[k];
    check(crossedAbove(x, 3, 2, 2) === false, 'above: ' + x + ' a_prev');
    check(crossedAbove(1, x, 2, 2) === false, 'above: ' + x + ' a');
    check(crossedAbove(1, 3, x, 2) === false, 'above: ' + x + ' b_prev');
    check(crossedAbove(1, 3, 2, x) === false, 'above: ' + x + ' b');
    check(crossedBelow(x, 1, 2, 2) === false, 'below: ' + x + ' a_prev');
    check(crossedBelow(3, x, 2, 2) === false, 'below: ' + x + ' a');
    check(crossedBelow(3, 1, x, 2) === false, 'below: ' + x + ' b_prev');
    check(crossedBelow(3, 1, 2, x) === false, 'below: ' + x + ' b');
  }
});
)",
                40);
}

// ============================================================
// Highest / Lowest
// ============================================================

TEST(JsHighestLowest, StreamingWarmUpDropOutNaNBatchAndReset)
{
  runUnitScript(R"(
function feed(ind, xs) { return xs.map(function (x) { return ind.update(x); }); }
function sameArr(a, b) {
  if (a.length !== b.length) return false;
  for (var i = 0; i < a.length; i++) if (!same(a[i], b[i])) return false;
  return true;
}
function showArr(a) { return '[' + Array.prototype.map.call(a, show).join(',') + ']'; }

section('Highest', function () {
  var xs = [5, 3, 4, 9, 2, 2, 1, 7];
  var h = new Highest(3);
  check(h.count === 0 && h.ready === false && h.value === null, 'Highest: fresh');
  var got = feed(h, xs);
  var want = [null, null, 5, 9, 9, 9, 2, 7];
  check(sameArr(got, want), 'Highest(3) ' + showArr(got) + ', want ' + showArr(want));
  check(h.value === 7 && h.ready === true && h.count === 8, 'Highest: value/ready/count');
  check(sameArr(Highest.compute(xs, 3), want), 'Highest.compute equals streaming');
  h.reset();
  check(h.count === 0 && h.value === null && h.ready === false, 'Highest: reset');
  check(sameArr(feed(h, xs), want), 'Highest: same values after reset');
  var eq = new Highest(3);
  check(sameArr(feed(eq, [4, 4, 4, 4]), [null, null, 4, 4]), 'Highest: equal values');
  // A NaN poisons the window it is in, as SMA's does.
  var n = new Highest(3);
  var nx = [1, NaN, 2, 3, 4, 5];
  var ng = feed(n, nx);
  check(sameArr(ng, [null, null, null, null, 4, 5]), 'Highest NaN: ' + showArr(ng));
  var sma = new SMA(3);
  var sg = feed(sma, nx);
  for (var i = 0; i < nx.length; i++) {
    check((ng[i] === null) === (sg[i] === null), 'Highest NaN ready matches SMA at ' + i);
  }
  check(sameArr(Highest.compute(nx, 3), ng), 'Highest.compute equals streaming through NaN');
  [h, eq, n, sma].forEach(function (x) { if (typeof x.destroy === 'function') x.destroy(); });
});

section('Lowest', function () {
  var xs = [5, 3, 4, 9, 2, 2, 1, 7];
  var l = new Lowest(3);
  var got = feed(l, xs);
  var want = [null, null, 3, 3, 2, 2, 1, 1];
  check(sameArr(got, want), 'Lowest(3) ' + showArr(got) + ', want ' + showArr(want));
  check(sameArr(Lowest.compute(xs, 3), want), 'Lowest.compute equals streaming');
  var drop = new Lowest(2);
  check(sameArr(feed(drop, [1, 5, 6, 0]), [null, 1, 5, 0]), 'Lowest: the old minimum drops out');
  l.reset();
  check(l.count === 0 && l.value === null, 'Lowest: reset');
  var n = new Lowest(3);
  var ng = feed(n, [1, NaN, 2, 3, 4, 5]);
  check(sameArr(ng, [null, null, null, null, 2, 3]), 'Lowest NaN: ' + showArr(ng));
  check(throws(function () { new Highest(0); }), 'Highest(0) is refused');
  check(throws(function () { new Lowest(0); }), 'Lowest(0) is refused');
  [l, drop, n].forEach(function (x) { if (typeof x.destroy === 'function') x.destroy(); });
});
)",
                24);
}

// ============================================================
// Position facts on ctx: entryPrice and barsInPosition
// ============================================================

namespace
{

constexpr int64_t kMinuteNs = 60'000'000'000LL;

// Scenario A: open 100 + 10 i, high open + 3, low open - 2, close open + 1.
BarEvent scenarioBar(SymbolId sym, int i, double base)
{
  const double open = base + 10.0 * i;
  BarEvent ev{};
  ev.symbol = sym;
  ev.barType = BarType::Time;
  ev.barTypeParam = static_cast<uint64_t>(kMinuteNs);
  ev.bar.open = Price::fromDouble(open);
  ev.bar.high = Price::fromDouble(open + 3.0);
  ev.bar.low = Price::fromDouble(open - 2.0);
  ev.bar.close = Price::fromDouble(open + 1.0);
  ev.bar.volume = Volume::fromDouble(100.0);
  ev.bar.startTime = TimePoint{std::chrono::nanoseconds{kMinuteNs * i}};
  ev.bar.endTime = TimePoint{std::chrono::nanoseconds{kMinuteNs * (i + 1)}};
  return ev;
}

// Logs, per bar and per symbol, what ctx says; sends scenario A's orders on
// the first symbol and one buy on the second symbol (when it has one) on its
// bar 3.
const char* const kPositionScript = R"(
var log = [];
var n = {};
function fmt(ctx) {
  return (ctx.entryPrice === null ? 'null' : (typeof ctx.entryPrice === 'number' ? String(ctx.entryPrice) : 'missing:' + typeof ctx.entryPrice))
       + '/' + (typeof ctx.barsInPosition === 'number' ? ctx.barsInPosition : 'missing');
}
class PosStrat extends Strategy {
  constructor() { super({ exchange: "Test", symbols: SYMBOLS }); }
  onBar(ctx, bar) {
    var s = ctx.symbolId;
    var i = n[s] === undefined ? 0 : n[s];
    n[s] = i + 1;
    log.push(s + ':' + i + ':' + fmt(ctx));
    var name = SYMBOLS[s === FIRST ? 0 : 1];
    if (s === FIRST) {
      if (i === 1) this.marketBuy({ symbol: name, qty: 1.0 });
      else if (i === 3) this.marketBuy({ symbol: name, qty: 1.0 });
      else if (i === 5) this.marketSell({ symbol: name, qty: 1.0 });
      else if (i === 7) this.marketSell({ symbol: name, qty: 3.0 });
      else if (i === 9) this.marketBuy({ symbol: name, qty: 2.0 });
    } else if (i === 3) {
      this.marketBuy({ symbol: name, qty: 1.0 });
    }
  }
  onTrade(ctx, trade) {
    var s = ctx.symbolId;
    var i = n[s] === undefined ? 0 : n[s];
    n[s] = i + 1;
    log.push(s + ':' + i + ':' + fmt(ctx));
    if (i === 0) this.marketBuy({ symbol: SYMBOLS[0], qty: 1.0 });
  }
}
lrvx.register(new PosStrat());
)";

struct Harness
{
  std::unique_ptr<ScriptFile> script;
  SymbolRegistry registry;
  std::unique_ptr<LrvxJsStrategy> js;
  std::unique_ptr<BridgeStrategy> bridge;
  std::vector<uint32_t> ids;

  explicit Harness(bool twoSymbols)
  {
    std::string head = twoSymbols ? "var SYMBOLS = ['BTCUSDT', 'ETHUSDT'];\n"
                                  : "var SYMBOLS = ['BTCUSDT'];\n";
    script = std::make_unique<ScriptFile>(head + "var FIRST = 0;\n" + kPositionScript);
    js = std::make_unique<LrvxJsStrategy>(script->path(), registry);
    ids = js->symbolIds();
    // FIRST is the id the first symbol resolved to.
    js->engine().eval(("FIRST = " + std::to_string(ids.at(0)) + ";").c_str());
    bridge = std::make_unique<BridgeStrategy>(
        1, std::vector<SymbolId>(ids.begin(), ids.end()), registry, js->getCallbacks());
    js->injectHandle(static_cast<LrvxStrategyHandle>(bridge.get()));
  }

  void clearLog() { js->engine().eval("log = []; n = {};"); }
  std::string log() { return readString(*js, "log.join(' ')"); }
};

std::string expectedScenarioA(uint32_t sym)
{
  const char* want[] = {"null/0", "null/0", "120/1", "120/2", "130/3", "130/4",
                        "130/5", "130/6", "180/1", "180/2", "null/0", "null/0"};
  std::string out;
  for (int i = 0; i < 12; ++i)
  {
    out += (i ? " " : "") + std::to_string(sym) + ":" + std::to_string(i) + ":" + want[i];
  }
  return out;
}

}  // namespace

TEST(JsPositionFacts, ScenarioAOnRunBars)
{
  for (double fee : {0.0, 0.01})
  {
    SCOPED_TRACE("fee rate " + std::to_string(fee));
    Harness h(false);
    std::vector<BarEvent> bars;
    for (int i = 0; i < 12; ++i)
    {
      bars.push_back(scenarioBar(h.ids[0], i, 100.0));
    }
    BacktestConfig cfg;
    cfg.feeRate = fee;
    BacktestRunner runner(cfg);
    runner.setSymbolRegistry(&h.registry);
    runner.setStrategy(h.bridge.get());
    runner.runBars(bars);
    // Bar 4: (120 + 140) / 2. Bar 6: a partial close leaves 130 (FIFO would
    // say 140). Bar 8: the sell of 3 flips to -2 at 180 and the count
    // restarts.
    EXPECT_EQ(h.log(), expectedScenarioA(h.ids[0]));
  }
}

TEST(JsPositionFacts, ASecondRunOnTheSameRunnerStartsClean)
{
  Harness h(false);
  std::vector<BarEvent> bars;
  for (int i = 0; i < 12; ++i)
  {
    bars.push_back(scenarioBar(h.ids[0], i, 100.0));
  }
  BacktestRunner runner;
  runner.setSymbolRegistry(&h.registry);
  runner.setStrategy(h.bridge.get());
  // The first run stops while long (2 bars in).
  runner.runBars(std::vector<BarEvent>(bars.begin(), bars.begin() + 4));
  EXPECT_NE(h.log().find(":3:120/2"), std::string::npos) << h.log();
  h.clearLog();
  runner.runBars(bars);
  EXPECT_EQ(h.log(), expectedScenarioA(h.ids[0]));
}

TEST(JsPositionFacts, SymbolsAreIndependent)
{
  Harness h(true);
  ASSERT_EQ(h.ids.size(), 2u);
  std::vector<BarEvent> bars;
  for (int i = 0; i < 12; ++i)
  {
    bars.push_back(scenarioBar(h.ids[0], i, 100.0));
    bars.push_back(scenarioBar(h.ids[1], i, 1000.0));
  }
  BacktestRunner runner;
  runner.setSymbolRegistry(&h.registry);
  runner.setStrategy(h.bridge.get());
  runner.runBars(bars);
  const std::string log = h.log();
  // The first symbol follows scenario A untouched by the second.
  std::string first;
  std::string second;
  size_t pos = 0;
  while (pos < log.size())
  {
    size_t end = log.find(' ', pos);
    if (end == std::string::npos)
    {
      end = log.size();
    }
    const std::string item = log.substr(pos, end - pos);
    std::string& dst = item.rfind(std::to_string(h.ids[0]) + ":", 0) == 0 ? first : second;
    dst += (dst.empty() ? "" : " ") + item;
    pos = end + 1;
  }
  EXPECT_EQ(first, expectedScenarioA(h.ids[0]));
  // The second symbol buys 1 on its bar 3 and fills at bar 4's open, 1040.
  const std::string s = std::to_string(h.ids[1]) + ":";
  EXPECT_NE(second.find(s + "3:null/0"), std::string::npos) << second;
  EXPECT_NE(second.find(s + "4:1040/1"), std::string::npos) << second;
  EXPECT_NE(second.find(s + "11:1040/8"), std::string::npos) << second;
}

TEST(JsPositionFacts, TickOnlyRunHasAnEntryButNoBars)
{
  Harness h(false);
  const auto dir = std::filesystem::temp_directory_path() /
                   ("lrvx_qjs_helpers_tape_" + std::to_string(std::random_device{}()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  {
    replay::WriterConfig cfg;
    cfg.output_dir = dir;
    replay::BinaryLogWriter writer(cfg);
    const double px[] = {100.0, 101.5, 103.0, 104.5};
    for (int i = 0; i < 4; ++i)
    {
      replay::TradeRecord r{};
      r.exchange_ts_ns = 1'700'000'000'000'000'000LL + i * 1'000'000'000LL;
      r.recv_ts_ns = r.exchange_ts_ns;
      r.price_raw = Price::fromDouble(px[i]).raw();
      r.qty_raw = Quantity::fromDouble(1.0).raw();
      r.symbol_id = h.ids[0];
      r.side = 1;
      r.trade_id = static_cast<uint64_t>(i + 1);
      writer.writeTrade(r);
    }
  }
  BacktestRunner runner;
  runner.setSymbolRegistry(&h.registry);
  runner.setStrategy(h.bridge.get());
  runner.runTape(dir);
  std::filesystem::remove_all(dir);
  const std::string log = h.log();
  const std::string s = std::to_string(h.ids[0]) + ":";
  // The buy sent on the first trade (100) fills against that trade; from
  // the next trade on there is an entry price, and with no bar seen
  // barsInPosition stays 0.
  EXPECT_EQ(log, s + "0:null/0 " + s + "1:100/0 " + s + "2:100/0 " + s + "3:100/0");
}
