# Simulate venue downtime and reconnects

Real exchanges go down — scheduled maintenance, surprise outages,
matching-engine restarts. A strategy that ignores this is brittle
in production. lrvx models planned and random outages inside the
backtest, so you can measure the PnL cost and fix the recovery path
before going live.

## What an outage does

When a venue is "down" inside the simulator:

- Submit / cancel / replace requests **buffer** locally and flush in
  FIFO order at the recovery edge. They never reach the matching
  engine while the venue is down.
- Market-data callbacks (`onTrade`, `onBookUpdate`, `onBar`) are
  **silently dropped** so the strategy sees a feed gap. No fills,
  no level updates, no trades during the outage.
- The venue applies an **on-open-orders policy** at outage start:
  - `cancel_all` — every resting order is cancelled immediately
  - `hold` — resting orders stay put and wake up on recovery
  - `expire_gtc_after` — orders older than a TTL are dropped at outage
    start; the rest are held

Both planned (scheduled) and random (Poisson-arrival) outages are
supported. Random outages are deterministic given a seed.

## Configure

A worked example (Python):

```python
--8<-- "examples/python_venue_downtime.py"
```

=== "Python"

    ```python
    import lrvx

    va = lrvx.VenueAvailability()
    # 2-minute maintenance window, kill every resting order on disconnect
    va.schedule_outage(start_ns=t0 + 3600 * 10**9,
                       duration_ns=120 * 10**9,
                       on_open_orders="cancel_all")
    # plus 0.5 random outages per day, 30s mean duration, hold open orders
    va.auto_random_outages(per_day=0.5,
                           mean_duration_ns=30 * 10**9,
                           on_open_orders="hold",
                           seed=42)

    exec = lrvx.SimulatedExecutor()
    exec.set_venue_availability(va)
    ```

=== "TypeScript (Node)"

    ```typescript
    import { SimulatedExecutor, VenueAvailability } from "@lrvx/lrvx";

    const va = new VenueAvailability();
    va.scheduleOutage(t0 + 3600 * 1e9, 120 * 1e9, "cancel_all");
    va.autoRandomOutages(0.5, 30 * 1e9, "hold", 42);

    const exec = new SimulatedExecutor();
    exec.setVenueAvailability(va);
    ```

=== "TypeScript (QuickJS)"

    ```typescript
    const va = new VenueAvailability();
    va.scheduleOutage(t0 + 3600 * 1e9, 120 * 1e9, "cancel_all");
    va.autoRandomOutages(0.5, 30 * 1e9, "hold", 42);

    const exec = new SimulatedExecutor();
    exec.setVenueAvailability(va);
    ```

=== "Codon"

    ```python
    from lrvx.backtest import (SimulatedExecutor, VenueAvailability,
                               OUTAGE_CANCEL_ALL, OUTAGE_HOLD)

    va = VenueAvailability()
    va.schedule_outage(t0 + 3600 * 10**9, 120 * 10**9, OUTAGE_CANCEL_ALL)
    va.auto_random_outages(0.5, 30 * 10**9, OUTAGE_HOLD, 42)

    exec = SimulatedExecutor()
    exec.set_venue_availability(va)
    ```

=== "C"

    ```c
    LrvxVenueAvailabilityHandle va = lrvx_venue_availability_create();
    lrvx_venue_availability_schedule_outage(va, t0 + 3600e9, 120e9,
                                            /*policy=*/0, /*gtc_ttl_ns=*/0);
    lrvx_venue_availability_auto_random_outages(va, 0.5, 30e9,
                                                /*policy=*/1, /*seed=*/42);

    LrvxSimulatedExecutorHandle exec = lrvx_simulated_executor_create();
    lrvx_simulated_executor_set_venue_availability(exec, va);
    /* later: */
    lrvx_simulated_executor_set_venue_availability(exec, NULL);
    lrvx_venue_availability_destroy(va);
    ```

In QuickJS the policy is `"cancel_all"` (also what leaving it out means), `"hold"` or
`"expire_gtc_after"`. Any other string throws a `TypeError` listing the three; it used to be read
as `"cancel_all"`.

## What this catches

Stress-testing a backtest with a downtime profile surfaces failure
modes that a clean tape can't:

- Strategies that rely on cancel-on-fill atomicity (one leg fills,
  the other should auto-cancel) — under HOLD, the cancel buffers
  during the outage and the second leg may fill against an old
  price.
- Reconnect storms: 100 strategies hitting the venue at recovery
  edge all at once. Combined with `RateLimitPolicy`, you see
  realistic rate-limit rejects on flush.
- GTC orders that get dropped server-side after a long outage —
  `expire_gtc_after` models the typical venue TTL (e.g. 24h on
  Binance, 7 days on Bybit).

## Verifying with isUp

Strategy code can poll `va.is_up(now_ns)` to gate optional actions
during known outage windows (e.g., skip recomputing risk while the
feed is gapped). The same `VenueAvailability` instance is the source
of truth for both the simulator and the strategy, so there is no
desync.

## Outage pathology variants

`schedule_outage` produces a total outage — everything blocks. Real
incidents are messier. Use `schedule_outage_ex` to pick a pathology:

| outage_type           | What happens                                                                       |
|-----------------------|------------------------------------------------------------------------------------|
| `total` (default)     | Existing behaviour: submit/cancel/replace buffered, market data dropped.            |
| `submit_only_down`    | Cancels still work, submits buffered until recovery. Common during rolling restarts.|
| `cancel_only_down`    | Submits still work, cancels buffered.                                              |
| `slow_degradation`    | Every submit/cancel/replace ack latency multiplied by `degradation_latency_multiplier`. Market data still flows. |
| `stale_book`          | `onBookUpdate` is dropped during the window; trades continue. Orders match against the frozen book. |
| `wrong_side_recovery` | On recovery, accumulates `wrong_side_recovery_bps` for the next mark feed (consume via `consume_wrong_side_recovery_bps`). |

=== "Python"

    ```python
    va.schedule_outage_ex(
        start_ns=t0, duration_ns=120 * 10**9,
        outage_type="slow_degradation",
        degradation_latency_multiplier=50.0,
    )
    print(va.latency_multiplier(t0 + 10**9))  # 50.0
    ```

=== "Node.js"

    ```javascript
    va.scheduleOutageEx(
      startNs, durationNs,
      "stale_book",  // book updates dropped, trades flow
      "hold",
    );
    ```

## Notes

- Random outages realise lazily on the first `isUp` call after each
  time advance; sampling is reproducible given the seed.
- Random outages assume a Poisson process with constant rate
  `perDay`. Real venues cluster outages around upgrade windows; for
  fine-grained scheduling, layer scheduled outages on top.
- The buffer for in-flight requests during an outage is unbounded.
  If your strategy generates 10k orders per second and the outage
  lasts 10 minutes, the recovery-edge flush will be huge.
