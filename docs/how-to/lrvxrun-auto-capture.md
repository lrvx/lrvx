# Capture a strategy run automatically with `.lrvxrun`

The `.lrvxrun` recorder API ships standalone. By default the strategy author calls `rec.write_signal()` / `write_order_event()` / `write_fill()` by hand. This page shows how to attach the recorder once and have every event captured automatically.

## Adapter classes

`lrvx::run::TraceSignalHandler` wraps an existing `ISignalHandler`. Every emitted signal flows through it; the wrapper records it and then delegates to the inner handler. `lrvx::run::TraceExecutionListener` does the same for order lifecycle events on `IOrderExecutionListener`. A fill it records carries its own `fill_id`, the fill's number in the run (1, 2, 3, ...), and the price it filled at when the listener callback names one (`onOrderPartiallyFilled(order, qty, price)` and the three-argument `onOrderFilled`); it used to write the order id as the fill id and the order's limit price as the fill price.

```cpp
#include "lrvx/run/trace_handlers.h"
#include "lrvx/run/trace_recorder.h"

lrvx::run::TraceRecorderOptions opts;
opts.strategy_id = "my-strategy";
lrvx::run::TraceRecorder rec("./run.lrvxrun", std::move(opts));

lrvx::run::TraceSignalHandler trace_signals(real_signal_handler, &rec);
lrvx::run::TraceExecutionListener trace_exec(2, real_listener, &rec);

strategy.setSignalHandler(&trace_signals);
order_exec_bus.subscribe(&trace_exec);

// Update the feed timestamp once per dispatched market-data event so
// each captured signal / order / fill carries `feed_ts_ns`:
trace_signals.setFeedTsNs(trade.exchangeTsNs);
trace_exec.setFeedTsNs(trade.exchangeTsNs);
```

The `feed_ts_ns` setter is the only piece the engine wiring still has to drive — call it once per dispatched tape event so each captured record points back at its trigger.

## What the recorder writes

| Engine event | `.lrvxrun` record |
|--------------|------------------|
| `Signal::market` / `limit` / `stop*` etc. | `SignalRecord` (kind 10) |
| `IOrderExecutionListener::onOrderSubmitted` | `OrderEventRecord` kind=submit |
| `onOrderAccepted` | `OrderEventRecord` kind=ack |
| `onOrderCanceled` | `OrderEventRecord` kind=cancel |
| `onOrderRejected` | `OrderEventRecord` kind=reject (carries reason) |
| `onOrderExpired` | `OrderEventRecord` kind=expire |
| `onOrderFilled` / `onOrderPartiallyFilled` | `FillRecord` (kind 12) |

A captured signal's id is the id of the order it asks for, all 64 bits of it (`(subscriber id << 32) + n`; the record used to keep the low 32), and every order event the listener writes carries that order id in `parent_signal_id`. A signal and the events of its order therefore join on `signalId64() == parent_signal_id == order_id`, and a later cancel or modify signal for the same order carries the same id. See [the format](../spec/lrvxrun.md).

A `nullptr` recorder pointer disables capture without removing the inner-handler delegation, so attaching the wrapper unconditionally during setup is safe.

## Phase status

The C++ adapter classes ship in `include/lrvx/run/trace_handlers.h` and are exercised by `tests/test_trace_handlers.cpp`. The one-call binding helper (`Runner.attach_trace_recorder`) and the executor-mirroring helpers (`Runner.trace_order_event`, `Runner.trace_fill`, plus `Runner.set_trace_feed_ts_ns`) have shipped — see the sections below. A binding `Runner` does not subscribe anything to an executor's listener bus: the mirroring calls are made by the caller.

## One-call attach

The `Runner` exposes `attach_trace_recorder(recorder)`. What it records by itself is signals, and only signals:

- Every signal a strategy emits is written as a `SignalRecord`, order signals and `cancel` / `cancel_all` / `modify` alike. Its id is the order id the signal names, its name is the signal type (`market`, `limit`, `cancel`, ...), its symbol list is the signal's symbol, and its flags are `SIGNAL_FLAG_ENTER` for an order, `SIGNAL_FLAG_EXIT` for a cancel and 0 for the rest.
- The record is written before the kill switch, the order validator and the risk manager see the signal, so a signal one of them drops is in the trace although it reached neither `on_signal` nor the executor.
- No order event and no fill is written, with or without an executor set on the runner (`set_executor`). A trace captured this way has an empty order-event list and an empty fill list until the caller mirrors them with `trace_order_event` / `trace_fill` (below), or, in C++, subscribes a `TraceExecutionListener` to the executor's listener bus.

`python/tests/test_runner_auto_capture.py` holds each of these statements as a test.

```python
# pybind11
import lrvx
rec = lrvx.TraceRecorder(path="./run.lrvxrun", strategy_id="trend",
                             strategy_hash="sha256:abc",
                             run_started_ns=time.time_ns())
runner = lrvx.Runner(registry, on_signal=lambda sig: None)
runner.attach_trace_recorder(rec)
runner.set_trace_feed_ts_ns(trade.exchange_ts_ns)  # call once per tape event
```

```javascript
// node
const rec = new lrvx.TraceRecorder({
  path: "./run.lrvxrun",
  strategyId: "trend",
  strategyHash: "sha256:abc",
  runStartedNs: Date.now() * 1000000,
});
const runner = new lrvx.Runner(registry, sig => {});
runner.attachTraceRecorder(rec);
runner.setTraceFeedTsNs(trade.exchangeTsNs);
```

Codon reaches the same C ABI symbols (`lrvx_runner_attach_trace_recorder`, `lrvx_runner_set_trace_feed_ts_ns`) directly. Pass `null` / `None` to detach.

## Mirroring order events and fills

The signal capture hook above is one half of the trace. To get a complete `.lrvxrun` your executor wrapper has to mirror its own callbacks into the recorder. The runner exposes two methods for this:

```python
# pybind11 — inside your executor's on_filled callback
runner.trace_fill(order_id=fill.order_id, fill_id=fill.fill_id,
                   price=fill.price, qty=fill.qty, fee=fill.fee,
                   symbol_id=fill.symbol_id, side=fill.side, liquidity=2)
```

```javascript
// node — after the executor's on_canceled fires
runner.traceOrderEvent({
  orderId, parentSignalId: orderId, symbolId,
  eventKind: 2 /* Cancel */, side, orderType: 0,
  price: 0, qty: 0,
});
```

Pass the order's id as `parentSignalId` / `parent_signal_id`: it is the id of the signal the runner captured for that order (`signalId64` / `signal_id64` on the signal record), so the two join.

Both methods are no-ops when no recorder is attached, so wiring them unconditionally during executor setup is safe. `eventKind` matches `OrderEventKind`: 1=Submit, 2=Cancel, 3=Modify, 4=Ack, 5=Reject, 6=Expire. `liquidity`: 0=Unknown, 1=Maker, 2=Taker.

## See also

- [Record a strategy run as `.lrvxrun`](lrvxrun.md). The recorder API the adapters write into.
- [`lrvxrun` strategy-trace format spec](../spec/lrvxrun.md). The on-disk layout.
