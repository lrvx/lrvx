# Model submit-side ack latency

The simulator can defer the `ACCEPTED` event until a sampled
deadline after `submitOrder()`. This mirrors the cancel and replace
ack models and lets backtests reproduce the race where market data
moves between submission and the venue accepting the order.

## Behavior

- `SUBMITTED` fires immediately
- The order is held aside, not yet placed in the book
- At the sampled ack deadline: `ACCEPTED` fires + the order runs
  through the existing post-accept logic (POST_ONLY check, queue
  tracker registration, try-fill)
- If POST_ONLY would cross the book at acceptance time, the order
  is `REJECTED` with `reject_reason = "late_post_only_crossed"`

## Configure

Two `BacktestConfig` knobs:

- `submitAckLatencyNs` — base ack delay. Default `0` preserves the
  legacy synchronous flow.
- `submitAckJitterNs` — uniform jitter band, clamped non-negative.

RNG sampling shares `cancelAckSeed`.

`BacktestRunner` takes the pair as constructor arguments
(`submit_ack_latency_ns`, `submit_ack_jitter_ns`; Node
`submitAckLatencyNs`, `submitAckJitterNs`) and applies them to its built-in
executor. A `SimulatedExecutor` you drive yourself -- a standalone one, or
`PaperBroker.simulated_executor` -- takes them through
`set_submit_ack_latency(latency_ns, jitter_ns=0)`.

=== "Python"

    ```python
    import lrvx

    bt = lrvx.BacktestRunner(reg, fee_rate=0.0004, initial_capital=10_000.0,
                             submit_ack_latency_ns=5_000_000,   # 5 ms ± 2 ms
                             submit_ack_jitter_ns=2_000_000)

    ex = lrvx.SimulatedExecutor()
    ex.set_submit_ack_latency(5_000_000, 2_000_000)
    ```

=== "Node.js"

    ```javascript
    const bt = new lrvx.BacktestRunner(reg, 0.0004, 10_000,
        {submitAckLatencyNs: 5_000_000, submitAckJitterNs: 2_000_000});

    const ex = new lrvx.SimulatedExecutor();
    ex.setSubmitAckLatency(5_000_000, 2_000_000);
    ```

=== "Codon"

    ```python
    bt = BacktestRunner(reg, 0.0004, 10_000.0,
                        submit_ack_latency_ns=5_000_000, submit_ack_jitter_ns=2_000_000)
    ```

=== "C++"

    ```cpp
    lrvx::BacktestConfig cfg;
    cfg.submitAckLatencyNs = 5'000'000;
    cfg.submitAckJitterNs  = 2'000'000;
    lrvx::BacktestRunner runner(cfg);
    ```

## Detect the race from a strategy

```python
--8<-- "examples/python_submit_latency_watcher.py"
```

## Notes

- The order does not participate in queue matching or fills before
  `ACCEPTED`. Trades arriving during the ack window do not consume
  it.
- Conditional orders (stop / TP / trailing) follow the same async
  path when `submitAckLatencyNs > 0`. They reach
  `PENDING_TRIGGER` at the ack deadline, not at submit time.
- The clock source is the engine's simulated clock; live runs
  follow whichever clock the connector attached.
- On bar replay the clock reads the bar's end, so any positive ack
  latency moves a bar-callback order's fill to a later bar's open. See
  [Latency on bar replay](backtest-with-latency.md#latency-on-bar-replay).
