# Per-lifecycle-stage order timestamps

Every order event carries eight optional timestamps, one per
lifecycle stage. They make it possible to compute latencies (ack,
time-to-fill, cancel processing) directly from a single event,
without subscribing to every prior status and recording the clock.

## What's exposed

For every fill or order-update event, the payload has these fields
(Python / Codon names; Node uses camelCase: `submittedAtNs`, etc.):

- `submitted_at_ns` — when SUBMITTED fired
- `accepted_at_ns` — when ACCEPTED fired
- `first_fill_at_ns` — first PARTIALLY_FILLED or FILLED
- `last_fill_at_ns` — most recent fill (refreshes on every fragment)
- `canceled_at_ns` — when CANCELED fired
- `rejected_at_ns` — when REJECTED fired
- `triggered_at_ns` — when a conditional order transitioned from
  PENDING_TRIGGER to TRIGGERED
- `expired_at_ns` — when EXPIRED fired

In Node the timestamps are `bigint` (a JS `number` cannot hold a
nanosecond clock reading exactly), so a difference of two is a `bigint`
too; wrap it in `Number(...)` to do float arithmetic with it.

A timestamp reads zero if the order has not reached that stage. The
slot for the status of the current event is always the freshest;
earlier stages carry their historical timestamps.

## Compute latencies

=== "Python"

    ```python
    import lrvx

    class LatencyWatcher(lrvx.Strategy):
        def on_order_update(self, ctx, ev):
            if ev.status == "ACCEPTED":
                ack_ns = ev.accepted_at_ns - ev.submitted_at_ns
                print(f"order {ev.order_id} ack latency: {ack_ns} ns")

        def on_fill(self, ctx, ev):
            if ev.status == "FILLED":
                ttf = ev.first_fill_at_ns - ev.submitted_at_ns
                print(f"order {ev.order_id} time to first fill: {ttf} ns")
    ```

=== "Node.js"

    ```javascript
    const strat = {
      onOrderUpdate(ctx, ev, emit) {
        if (ev.status === "ACCEPTED") {
          const ackNs = Number(ev.acceptedAtNs - ev.submittedAtNs);
          console.log(`ack latency: ${ackNs} ns`);
        }
      },
    };
    ```

=== "QuickJS"

    ```javascript
    class LatencyWatcher extends Strategy {
      onOrderUpdate(ctx, ev) {
        if (ev.status === "ACCEPTED") {
          const ackNs = ev.acceptedAtNs - ev.submittedAtNs;
          this.log(`ack latency: ${ackNs} ns`);
        }
      }
    }
    ```

=== "Codon"

    ```python
    from lrvx.strategy import Strategy

    class LatencyWatcher(Strategy):
        def on_order_update(self, ctx, ev):
            if ev.status == 4:   # ACCEPTED
                ack_ns = ev.accepted_at_ns - ev.submitted_at_ns
                print("ack latency:", ack_ns, "ns")
    ```

## Notes

- The clock source is engine time. On backtests this is the
  simulator clock; on live this is whichever clock the connector
  attached to the engine.
- Latency in backtest resolves at tape-event granularity. Gaps
  smaller than one event tick read as zero.
- The fields persist for the lifetime of the order, including
  through partial fills. After the order reaches a terminal state
  the tracking entry is dropped from the simulator.
