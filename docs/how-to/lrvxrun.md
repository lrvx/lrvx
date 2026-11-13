# Record a strategy run as `.lrvxrun`

A `.lrvx` tape holds what the exchange sent. A `.lrvxrun` directory holds what the strategy did with it: the signals, order events, and fills it produced. The two formats live next to each other so a viewer or a diff tool can line them up on one timeline.

This page covers the write-then-read loop. Format details live in the [lrvxrun spec](../spec/lrvxrun.md).

## When to write a `.lrvxrun`

- During a backtest, so the viewer can scrub through signals and orders alongside the input tape.
- During paper trading, so a post-run diff confirms the strategy did what its expected output says it should.
- During live trading, when you want a structured trace separate from the engine log.
- Inside a `lrvx bundle pack` artifact as the canonical "expected output". The bundle CLI integration is a follow-up task; today the recorder takes any path.

## Quick start

=== "Python"

    ```python
    from lrvx._lrvx import (
        FillLiquidity, OrderEventKind, TapeRef, TraceReader, TraceRecorder,
    )

    rec = TraceRecorder(
        path="run.lrvxrun",
        strategy_id="ratio-cross",
        strategy_hash="sha256:abc",
        run_started_ns=1_700_000_000_000_000_000,
        tape_refs=[TapeRef("BTCUSDT.lrvx", "sha256:def", 0, 0)],
    )
    rec.write_signal(
        run_ts_ns=1_700_000_000_100_000_000,
        signal_id=42,
        name="ratio-cross",
        symbol_ids=[1, 2],
        payload=b'{"src":"ETH","dst":"BTC"}',
    )
    rec.write_order_event(
        run_ts_ns=1_700_000_000_200_000_000,
        order_id=7,
        parent_signal_id=42,
        symbol_id=1,
        event_kind=OrderEventKind.SUBMIT,
        side=0,
        order_type=1,
        price_raw=5_000_000_000_000,
        qty_raw=100_000_000,
    )
    rec.write_fill(
        run_ts_ns=1_700_000_000_300_000_000,
        order_id=7,
        fill_id=12345,
        price_raw=5_000_000_000_000,
        qty_raw=100_000_000,
        liquidity=FillLiquidity.MAKER,
    )
    rec.close()

    reader = TraceReader("run.lrvxrun")
    print(reader.strategy_id, len(reader.read_all_signals()))
    ```

=== "Node.js"

    ```javascript
    const { TraceRecorder, TraceReader } = require('@lrvx/lrvx');

    const rec = new TraceRecorder({
      path: 'run.lrvxrun',
      strategyId: 'ratio-cross',
      runStartedNs: 1_700_000_000_000_000_000,
    });
    rec.addTapeRef({ path: 'BTCUSDT.lrvx', contentHash: 'sha256:def' });
    rec.writeSignal({
      runTsNs: 1_700_000_000_100_000_000,
      signalId: 42,
      name: 'ratio-cross',
      symbolIds: [1, 2],
      payload: Buffer.from('{"src":"ETH","dst":"BTC"}'),
    });
    rec.writeOrderEvent({
      runTsNs: 1_700_000_000_200_000_000,
      orderId: 7,
      parentSignalId: 42,
      symbolId: 1,
      eventKind: 1,
      side: 0,
      orderType: 1,
      priceRaw: 5_000_000_000_000,
      qtyRaw: 100_000_000,
    });
    rec.writeFill({
      runTsNs: 1_700_000_000_300_000_000,
      orderId: 7,
      fillId: 12345,
      priceRaw: 5_000_000_000_000,
      qtyRaw: 100_000_000,
      liquidity: 1,
    });
    rec.close();

    const reader = new TraceReader('run.lrvxrun');
    console.log(reader.strategyId(), reader.readAllSignals().length);
    ```

=== "Codon"

    ```python
    from lrvx.run_trace import TraceRecorder, TraceReader, ORDER_KIND_SUBMIT, LIQUIDITY_MAKER

    rec = TraceRecorder("run.lrvxrun", strategy_id="ratio-cross",
                        run_started_ns=1700000000000000000)
    rec.add_tape_ref("BTCUSDT.lrvx", "sha256:def")
    rec.write_signal(1700000000100000000, name="ratio-cross",
                     symbol_ids=[1, 2], signal_id=42,
                     payload='{"src":"ETH","dst":"BTC"}')
    rec.write_order_event(1700000000200000000, order_id=7,
                          parent_signal_id=42, symbol_id=1,
                          event_kind=ORDER_KIND_SUBMIT,
                          price_raw=5000000000000, qty_raw=100000000)
    rec.write_fill(1700000000300000000, order_id=7, fill_id=12345,
                   price_raw=5000000000000, qty_raw=100000000,
                   liquidity=LIQUIDITY_MAKER)
    rec.close()

    reader = TraceReader("run.lrvxrun")
    print(reader.counts().signals)
    for s in reader.read_all_signals():
        print(s.run_ts_ns, s.name, s.signal_id64)
    ```

=== "QuickJS"

    ```javascript
    var rec = new lrvx.TraceRecorder({
      path: 'run.lrvxrun',
      strategyId: 'ratio-cross',
      runStartedNs: 1700000000000000000n,
    });
    rec.addTapeRef({ path: 'BTCUSDT.lrvx' });
    rec.writeSignal({
      runTsNs: 1700000000100000000n,
      signalId: 42,
      name: 'ratio-cross',
      symbolIds: [1, 2],
      payload: '{"src":"ETH","dst":"BTC"}',
    });
    rec.writeOrderEvent({
      runTsNs: 1700000000200000000n,
      orderId: 7,
      parentSignalId: 42,
      symbolId: 1,
      eventKind: 1,
      priceRaw: 5000000000000,
      qtyRaw: 100000000,
    });
    rec.writeFill({
      runTsNs: 1700000000300000000n,
      orderId: 7,
      fillId: 12345,
      priceRaw: 5000000000000,
      qtyRaw: 100000000,
      liquidity: 1,
    });
    rec.close();

    var reader = new lrvx.TraceReader('run.lrvxrun');
    console.log(reader.readAllSignals().length);
    console.log(reader.runStartedNs() === 1700000000000000000n);  // true
    ```

    Timestamps are nanoseconds since the epoch and are past 2^53, where a Number no longer
    holds every integer: write them as BigInt literals (`1700000000100000000n`). The reader
    returns them, and every 64-bit id, as BigInt.

## Multi-symbol decisions

A signal can carry several symbol ids. A pair-trade signal that goes long ETH and short BTC lists both:

```python
rec.write_signal(
    run_ts_ns=now_ns,
    name="pair-rotate",
    symbol_ids=[btc_id, eth_id],
    flags=lrvx._lrvx.SIGNAL_FLAG_REBALANCE,
    payload=b'{"target_ratio": 0.5}',
)
```

This is why `.lrvxrun` does not live inside `.lrvx`. A tape captures one feed for one symbol; writing a cross-symbol decision into one of those tapes would lie about its scope. The trace lists the symbols a signal touches; each tape stays scoped to its own feed.

## Aligning trace events with tape events

Every record carries two timestamps:

- `run_ts_ns` is wall-clock when the strategy emitted the event.
- `feed_ts_ns` is the tape event timestamp that triggered the decision.

To line up a trace against the tape that produced it, merge on `feed_ts_ns`. To inspect strategy behavior on its own wall clock (latency between feed event and decision, for instance), order by `run_ts_ns`. A diff tool comparing two runs against the same tape uses `feed_ts_ns` as the alignment key.

If a record was not triggered by a specific tape event (a periodic rebalance, a manual order from a control plane), set `feed_ts_ns = 0`.

## Reading back

The reader returns parsed records in chronological order. If a run never wrote a given record kind, its accessor returns an empty list; the format does not require every segment file to exist.

```python
reader = TraceReader("run.lrvxrun")
print("strategy:", reader.strategy_id)
print("tapes:", [t["path"] for t in reader.tape_refs])
for s in reader.read_all_signals():
    print(s["run_ts_ns"], s["name"], s["symbol_ids"])
for e in reader.read_all_order_events():
    print(e["run_ts_ns"], e["order_id"], e["event_kind"])
for f in reader.read_all_fills():
    print(f["run_ts_ns"], f["order_id"], f["price_raw"], f["qty_raw"])
```

A signal has two id fields. `signal_id` is the low 32 bits, as it has always read; `signal_id64`
(`signalId64` in Node and QuickJS, `signal_id64` on a Codon `TraceSignal` and
`TraceReader.signal_id64(index)`, `lrvx_run_reader_signal_id64` in C) is the whole id. They are
equal for an id below 2^32. A signal a runner captured carries the id of the order it asks for,
`(subscriber id << 32) + n`: join it to that order's events on `signal_id64 == order_id`, and to
the events the C++ capture listener writes on `parent_signal_id` as well.

In Node and QuickJS the reader returns every 64-bit id as a BigInt: `signalId64` on a signal,
`orderId` and `parentSignalId` on an order event, `orderId` and `fillId` on a fill. An order id is
`(subscriber id << 32) + n`, so with a subscriber id of 2^21 or more it is past 2^53 and a Number
no longer holds it; they used to be Numbers, with the low bits of such an id lost. Compare with
`===` against a BigInt (`e.orderId === 7n`), or convert with `Number(e.orderId)` when the id is
known to be small. The Node writers (`writeOrderEvent`, `writeFill`, and
`Runner.traceOrderEvent` / `traceFill`) take `orderId`, `parentSignalId` and `fillId` as a
non-negative integer Number or a BigInt and `priceRaw` / `qtyRaw` / `feeRaw` as an integer Number or
a BigInt; anything else (NaN, a fraction, a negative id, a string) is a `TypeError` naming the
field, where it used to be written as whatever the conversion made of it.

`write_signal` takes the whole 64-bit id in every binding, so an order id can be passed as it
is: Python `signal_id=`, Node and QuickJS `signalId` (a Number or a BigInt), Codon `signal_id=`,
and `lrvx_run_recorder_write_signal64` in C (`lrvx_run_recorder_write_signal` keeps its 32-bit
parameter). The id is stored the way a captured one is, and an id below 2^32 writes the same
bytes as before. Python used to raise `TypeError` for an id of 2^32 or more; Node, QuickJS and
Codon wrote its low 32 bits.

The QuickJS writers check what they are given at the native boundary, as the Node ones do. An id
(`signalId`, `orderId`, `parentSignalId`, `fillId`) is a non-negative integer Number, exact up to
2^53, or a BigInt below 2^64; a raw value or a clock reading (`priceRaw`, `qtyRaw`, `feeRaw`,
`strengthRaw`, `runTsNs`, `feedTsNs`) is an integer Number within 2^53 or a BigInt that fits 64
signed bits; `symbolId`, `flags`, `side`, `eventKind`, `orderType` and `liquidity` are integers
from 0 to 2^32 - 1. Anything else throws a `TypeError` naming the member (`writeFill: orderId must
be ...`) and no record is written. `NaN` and a string used to be written as 0, a fraction
truncated, a negative id as an id near 2^64 and a BigInt past 64 bits wrapped.

The Codon reader returns the same records: `read_all_signals()`, `read_all_order_events()` and
`read_all_fills()` give lists of `TraceSignal`, `TraceOrderEvent` and `TraceFill` with the fields
above as attributes, next to `strategy_id()`, `strategy_hash()`, `tape_ref_paths()` and `counts()`.
A signal's `payload` is a `str`.

## What is in a `.lrvxrun` directory

```
run.lrvxrun/
├── manifest.json         strategy id, hash, tape refs, segment index
├── signals-000000.bin    SignalRecord frames
├── orders-000000.bin     OrderEventRecord frames
└── fills-000000.bin      FillRecord frames
```

`manifest.json` is JSON; the segment files are little-endian binary with the same `FrameHeader` and CRC layout as `.lrvx`. Segment files only exist for kinds the recorder actually wrote; an empty run produces only `manifest.json`.

### Reading a bundle you did not write

A `.lrvxrun` is meant to travel, so the reader treats one as untrusted input.
It raises `std::runtime_error` (`RuntimeError` in Python) rather than reading
past the end of a segment or outside the bundle directory:

- A record whose declared name, symbol-list or payload length runs past its own
  frame is rejected. Those lengths live inside the bytes the frame CRC covers,
  so a checksum that matches proves nothing about them.
- A segment name in the manifest has to be a relative path inside the bundle.
  An absolute path, or one that climbs out with `..`, is refused.

A bundle written by the recorder never hits either case.

## See also

- [lrvxrun spec v1.0](../spec/lrvxrun.md). Wire layout, manifest schema, frame types.
- [lrvx spec v1.0](../spec/lrvx.md). The companion market-data format.
- [Record and replay tapes](tape-record.md). The CLI surface for the input side.
