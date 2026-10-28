# The JavaScript value boundary

Values move between C++ and the embedded QuickJS runtime on every
callback. Two of those crossings used to lose information without saying
so. This page covers what each does now, and what a strategy written
against the old behaviour has to change.

## Nanosecond timestamps cross as BigInt

**This is a breaking change.** Fields carrying an absolute nanosecond
clock reading used to arrive in JavaScript as a `Number`. They arrive as a
`BigInt`.

A JavaScript `Number` is a double. It holds every integer exactly up to
2^53, roughly 9.0e15. A nanosecond timestamp today is around 1.76e18,
about 200 times past that point, and at that magnitude the gap between one
representable double and the next is 256 ns. Two events 100 ns apart were
handed to the strategy carrying the same number.

A script that only reads a timestamp never notices. A script that orders
by one does. Merging two feeds, keying a map by timestamp, deduplicating a
replayed tape all compare timestamps, and two events the engine dispatched
in a definite order arrive indistinguishable. A merge that resolves ties
toward one feed then emits the pair backwards.

Measured on a real dispatch, before and after:

| | before | after |
|---|---|---|
| `String(trade.timestampNs)`, input `1757000000123456789` | `1757000000123456800` | `1757000000123456789` |
| `String(trade.timestampNs)`, input `1757000000123456889` | `1757000000123456800` | `1757000000123456889` |
| merge order of the two | second, first | first, second |

### What changed

Read side, now `BigInt`:

- `ctx.lastUpdateNs`
- `ctx.lastTouchNs`
- `trade.timestampNs`, `book.timestampNs`
- `bar.startTimeNs`, `bar.endTimeNs`
- on an order event: `exchangeTsNs`, `submittedAtNs`, `acceptedAtNs`,
  `firstFillAtNs`, `lastFillAtNs`, `canceledAtNs`, `rejectedAtNs`,
  `triggeredAtNs`, `expiredAtNs`
- on a tape record: `exchangeTsNs`, `recvTsNs`, `tsNs`
- backtest stats `startTimeNs` / `endTimeNs`, equity-point `timestampNs`
- trace reader run bounds, partition `fromNs` / `toNs` / `warmupFromNs`,
  merged-tape `firstEventNs` / `lastEventNs`
- `bar.ts` on every bar object: the aggregators (`lrvx.timeBars` and the
  rest) and `lrvx.loadCsv` / `Engine.loadCsv`

`bar.ts` from `loadCsv` changed unit as well as type. It used to be a
millisecond `Number`, while every other bar source reported nanoseconds,
so a script that read one bar from a CSV and one from an aggregator was
out by 1e6 and threw a `TypeError` the moment it subtracted one from the
other. A CSV column in seconds, milliseconds or microseconds is still
detected and scaled, but what reaches the script is always nanoseconds.
`SignalBuilder` and `Engine.run` follow: they timestamp, order and match
signals against bars in nanoseconds, and take a `BigInt` or a `Number`.

Write side: every binding that takes a nanosecond argument accepts a
`BigInt` or a `Number`, so `advanceClock`, `writeTrade`, `writeBook`,
`addTrade` and the rest keep working with the literals already in your
scripts.

### What did not change

Durations stay `Number`. A latency, a bar interval or a trade duration is
a difference, not a clock reading, and a duration under 104 days is under
2^53 nanoseconds and so exact in a double. `medianAckLatencyNs()`,
`avgTradeDurationNs`, `feedNs` / `orderNs` / `fillNs` and
`bar.barTypeParam` are all still numbers.

Order ids stay `Number` too. The id comes from a counter that starts at 1
and steps once per order, so reaching 2^53 takes more orders than a
session places. A nanosecond clock reading is past that range on the first
event.

### Migrating a strategy

BigInt and Number do not mix in arithmetic, though they compare fine.

```javascript
// Comparisons need no change: <, >, <=, >=, == all work across the two.
if (trade.timestampNs > lastSeenNs) { /* ... */ }

// Arithmetic does. Mixing the two throws a TypeError.
var ageNs = trade.timestampNs - bar.startTimeNs;   // BigInt - BigInt, fine
var ageMs = Number(ageNs) / 1e6;                   // convert once, at the edge

// Anything that wants a Number wants an explicit conversion.
var when = new Date(Number(trade.timestampNs / 1000000n));

// Strict equality against a literal needs the n suffix.
if (bar.endTimeNs === 1757000000000000000n) { /* ... */ }

// JSON.stringify refuses a BigInt outright. Convert, or use String().
console.log('ts=' + trade.timestampNs);
```

`console.log` on a whole event object still works: the console shim falls
back to a plain string conversion when `JSON.stringify` refuses the
object.

## A pre-trade gate that fails denies the order

The three pre-trade gates — `RiskManager.allow`, `KillSwitch.check`,
`OrderValidator.validate` — are C function pointers the engine calls
inline while a signal is in flight. Two failure modes used to be
unhandled: the gate threw, and the thrown value unwound out of a bridge
declared to C and surfaced at whatever JavaScript frame was on the stack;
or the gate returned something that was not a boolean, which one binding
folded into a deny and another into an allow.

The policy, one for every binding: a host-language throw and a
non-boolean return both DENY the order, and both are reported. Neither
ever lets the order through, and neither escapes through the C boundary.
A gate that plainly returns false is a decision, not a failure, and is
not reported.

In Node, reported means `runner.hookErrors()`: an array of
`{ hook, method, message }` records accumulated on the runner, oldest
first, readable synchronously after the call that failed. The
process-wide log callback is asynchronous and shared, so it cannot say
which runner denied what.

```javascript
runner.setKillSwitch({ check() { throw new Error('feed is down'); } });
runner.onTrade(sym, 100, 1, true, 1000n);   // the order is denied

runner.hookErrors();
// [ { hook: 'killSwitch', method: 'check', message: 'feed is down' } ]
```

A throw out of `Executor.capabilities()` crosses the same boundary and is
handled the same way: the executor is read as supporting nothing, and the
throw is recorded.

## String arguments reject values they cannot convert

A binding taking a string used to hand whatever QuickJS returned straight
to the C API. QuickJS returns a null pointer for any value it cannot turn
into a string, and several qualify: a `Symbol`, an object whose `toString`
throws, a `Proxy` with a throwing trap, a runtime that hit its memory
limit mid-conversion. The C API then called `strlen` on that null pointer
and the process died.

```javascript
// Each of these used to be a segmentation fault. Each now throws.
new lrvx.DataWriter(Symbol('out'));
new lrvx.DataReader({ toString: function () { throw new Error('no'); } });
graph.addNode(Symbol('ema'), [], function () { return []; });
```

The exception you get is the one QuickJS raised while trying to convert,
so a throwing `toString` surfaces its own error rather than a generic
message.

Optional string arguments keep their defaults when absent, and are checked
the same way when present: passing a `Symbol` where a string is optional
is an error, not a silent fall back to the default.

Where an empty string is a sensible reading of "no value", the binding
still substitutes one rather than throwing. Logging is the clearest case:
a value `console.log` cannot render prints as `[unprintable]` instead of
taking the process down or vanishing from the line.

There is one entry point for all of this, `JsCString` in
`src/quickjs/js_cstring.h`, which owns the converted string and frees it on
every exit path. `scripts/check_quickjs_tocstring.py` fails the build if a
binding calls the raw QuickJS conversion directly, or declares a holder
whose pointer reaches a consumer untested.
