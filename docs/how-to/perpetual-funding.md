# Model perpetual funding payments

Perpetuals settle funding on a fixed schedule — every 8 hours on
Binance UM / Bybit linear / OKX swap, every hour on some Bitget
markets. The payment per settlement is:

```
amount = -position_signed * mark_price * rate
```

Sign convention: a long (positive position) pays out when rate is
positive (amount negative). A short receives in the same regime.
Negative rate flips both signs.

`FundingSchedule` walks the schedule and produces a `FundingPayment`
record per (settlement timestamp, symbol). The strategy / backtest
runner is responsible for routing the amount into equity, since
lrvx's BacktestResult is fill-driven and does not own a
periodic-event clock of its own.

## Construct a schedule

=== "Python"

    ```python
    --8<-- "examples/python_funding_schedule.py"
    ```

=== "Node.js"

    ```javascript
    const lrvx = require('@lrvx/lrvx');
    const sched = new lrvx.FundingSchedule();
    sched.loadProfile('binance_um_futures');
    sched.setConstantRate(0.0001);
    const events = sched.tick(nowNs, [1, 2], [1.0, -2.0], [50000, 3000]);
    ```

=== "Codon"

    ```python
    from lrvx.funding_schedule import FundingSchedule

    s = FundingSchedule()
    s.load_profile("binance_um_futures")
    s.set_constant_rate(0.0001)
    events = s.tick(9 * 3600 * 1_000_000_000,
                    [1, 2], [1.0, -2.0], [50000.0, 3000.0])
    ```

=== "QuickJS"

    ```javascript
    const h = __lrvx_funding_schedule_create();
    __lrvx_funding_schedule_load_profile(h, "binance_um_futures");
    __lrvx_funding_schedule_set_constant_rate(h, 0.0001);
    const events = __lrvx_funding_schedule_tick(h, nowNs, [1, 2],
                                                 [1.0, -2.0],
                                                 [50000.0, 3000.0]);
    ```

=== "C++"

    ```cpp
    auto s = lrvx::FundingSchedule::binance_um_futures();
    s.setConstantRate(0.0001);
    auto events = s.tick(nowNs, {1, 2}, {1.0, -2.0}, {50000.0, 3000.0});
    ```

## Canned profiles

| Profile               | Interval | Default rate |
|-----------------------|----------|--------------|
| `binance_um_futures`  | 8h       | 0            |
| `bybit_linear`        | 8h       | 0            |
| `okx_swap`            | 8h       | 0            |
| `bitget_hourly`       | 1h       | 0            |

All profiles default to a zero rate — callers must supply a real
rate either via `set_constant_rate` or by attaching a recorded
tape via `tape(...)`.

## Tape mode

For research that wants the exchange's actually-published rates
(not a flat assumption), use `FundingSchedule.tape([(ts, rate),
...])`. Each tape event triggers one payment at its timestamp.

### Per-symbol tape

Real venues publish different rates per symbol per settlement. To
capture that, use a per-symbol tape: each row is
`(timestamp_ns, symbol, funding_rate)`. Symbols without an entry at
a given settlement timestamp fall back to the constant rate (which
defaults to 0; override with `set_constant_rate`).

Sample CSV:

```csv
timestamp_ns,symbol,funding_rate
1700000000000000000,1,0.0001
1700000000000000000,2,-0.0002
1700028800000000000,1,0.0003
```

=== "Python"

    ```python
    sched = FundingSchedule()
    sched.load_tape("funding.csv")
    sched.set_constant_rate(0.00005)  # fallback for missing rows
    payments = sched.tick(now_ns, [1, 2], [pos1, pos2], [mark1, mark2])
    ```

=== "Node.js"

    ```javascript
    const sched = new lrvx.FundingSchedule();
    sched.loadTape("funding.csv");
    sched.setConstantRate(0.00005);
    const payments = sched.tick(nowNs, [1, 2], [pos1, pos2], [mark1, mark2]);
    // payments[i].timestampNs is a BigInt; rate, markPrice, positionSigned, amount are Numbers
    ```

`load_tape` / `loadTape` returns `False` when the file cannot be opened and the schedule keeps
the tape it had. The reason is then in `lrvx.last_error()` (Python), `lrvx.lastError()` (Node,
QuickJS) or `lrvx.abi.last_error()` (Codon).

To build the same tape in memory instead of from CSV, pass
`FundingTapeEntry` rows (`timestamp_ns`, `symbol`, `rate`) to the
static `FundingSchedule.tape_by_symbol(entries)`
(`setTapeBySymbol(timestampsNs, symbols, rates)` in Node).

## Inspecting a loaded schedule

| Method | Returns |
|---|---|
| `interval_ns()` | Settlement cadence in ns (0 for a tape-driven schedule) |
| `constant_rate()` | Current fallback rate |
| `last_tick_ns()` | Internal cursor, advanced by `tick` |
| `settlement_timestamps()` | Every settlement timestamp in the loaded tape, deduplicated and sorted |
| `per_symbol_tape()` | The loaded per-symbol rows as `FundingTapeEntry` objects |

## Integration recipe

A typical backtest tick loop:

```python
last_ns = 0
for event in market_data:
    pos, mark = current_position(event.symbol), event.mark_price
    payments = sched.tick(event.timestamp_ns, [event.symbol], [pos], [mark])
    for p in payments:
        equity += p.amount
        ledger.append(p)
    last_ns = event.timestamp_ns
```

## Notes

- The schedule's internal cursor (`last_tick_ns`) advances on every
  tick. To restart a backtest, call `reset()`.
- The first `tick` seeds that cursor from the timestamp it is given, so a
  fixed-interval schedule opening on a real exchange timestamp settles at
  most the boundary that just passed -- not every boundary since 1970
  (61,362 of them at an 8h cadence on a 2026 date). The cadence from the
  second tick on is unchanged, and a schedule driven from a small base
  behaves exactly as before. `reset()` clears the seed as well as the
  cursor.
- Per-day settlement venues (BitMEX-style daily funding) need a
  custom interval. The interval is fixed at construction and has no
  setter: build the schedule with the static factory
  `FundingSchedule.constant(24 * 3600 * 1_000_000_000, rate)`
  (`setConstant(intervalNs, rate)` in Node).
- The signed position should reflect the actual exposure at the
  funding boundary, not the average over the period. Snap the
  position at the boundary timestamp.
- For multi-symbol portfolios pass parallel arrays — one tick call
  walks every symbol per boundary.
