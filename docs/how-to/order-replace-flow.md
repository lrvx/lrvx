# Model order replace acknowledgement

Like cancellation, an order replace request takes time to acknowledge
at the exchange. The simulator can model that delay so a backtest
can reproduce the late-replace-after-fill race: the original order
filling in the window between replaceOrder and the venue's ack.

## Events

Async replace produces a three-event sequence on success:

- REPLACE_SUBMITTED — fires immediately on replaceOrder()
- REPLACE_ACCEPTED — fires at the sampled ack deadline
- REPLACED — terminal "old gone, new alive"

If the original order fills before the ack arrives:

- FILLED — on the original order
- REPLACE_REJECTED — with reject_reason "late_replace_after_fill"

REPLACED still fires through the existing onOrderReplaced listener;
the new in-flight statuses dispatch through three new virtuals
onOrderReplaceSubmitted, onOrderReplaceAccepted, onOrderReplaceRejected.
On the strategy callback surface, the in-flight events flow through
the existing on_order_update / onOrderUpdate path.

## Configure

Two BacktestConfig knobs:

- replaceAckLatencyNs — base ack delay. Default 0 preserves the
  legacy synchronous replaceOrder().
- replaceAckJitterNs — uniform jitter band, clamped non-negative.

RNG sampling shares cancelAckSeed.

The bindings expose the pair as one SimulatedExecutor setter,
set_replace_ack_latency(latency_ns, jitter_ns=0). BacktestRunner's
constructor takes only (registry, fee_rate, initial_capital) and
owns its executor internally, so a BacktestRunner's replace latency
is configured through BacktestConfig in C++.

=== "Python"

    ```python
    import lrvx

    ex = lrvx.SimulatedExecutor()
    ex.set_replace_ack_latency(10_000_000, 2_000_000)
    ```

=== "Node.js"

    ```javascript
    const ex = new lrvx.SimulatedExecutor();
    ex.setReplaceAckLatency(10_000_000, 2_000_000);
    ```

=== "C++"

    ```cpp
    lrvx::BacktestConfig cfg;
    cfg.replaceAckLatencyNs = 10'000'000;
    cfg.replaceAckJitterNs  =  2'000'000;
    lrvx::BacktestRunner runner(cfg);
    ```

## Detect the race from a strategy

=== "Python"

    ```python
    import lrvx

    class ReplaceWatcher(lrvx.Strategy):
        def __init__(self, syms):
            super().__init__(syms)
            self.replaces_lost = 0

        def on_order_update(self, ctx, ev):
            if (ev.status == "REPLACE_REJECTED" and
                ev.reject_reason == "late_replace_after_fill"):
                self.replaces_lost += 1
    ```

=== "Node.js"

    ```javascript
    const strat = {
      onOrderUpdate(ctx, ev) {
        if (ev.status === "REPLACE_REJECTED" &&
            ev.rejectReason === "late_replace_after_fill") {
          replacesLost += 1;
        }
      },
    };
    ```

## Notes

- Neither the Python nor the Node SimulatedExecutor exposes a
  replace call. A backtest replace is emitted from the strategy
  (modify_order / emitModify); the runner turns that Modify signal
  into replaceOrder on its own executor.
- A Modify names an order id, a new price and a new quantity. The
  replacement is that order with the two fields changed: it keeps its
  id, symbol, side, type and flags, so a later cancel or modify by the
  same id still finds it. The new quantity is the quantity left to
  work; what the original had already filled is not carried over.
- The pending replace is dropped if the order is canceled by other
  means before the ack arrives. Replace ack finalization runs on
  the next simulator tick after the deadline passes.
- Conditional orders (stops, take-profits, trailing) use the same
  async path when replaceAckLatencyNs > 0.
- The clock source is the engine's simulated clock; live runs
  follow whichever clock the connector attached.
