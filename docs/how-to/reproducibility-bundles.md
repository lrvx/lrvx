# Reproducibility bundles with `lrvx bundle`

A bundle is a single tarball that encodes everything needed to reproduce a backtest result on another machine: strategy code, the captured tape it ran against, the engine version, and the expected output. `lrvx bundle pack` captures a run; `lrvx bundle validate` replays it and compares the result with the expected output field by field (trade count, every fill's symbol, side, price and quantity, total filled quantity), along with the strategy and tape hashes and the engine version.

**`lrvx bundle replay` and `lrvx bundle validate` run the strategy code shipped inside the bundle.** A bundle is designed to be shared, which is exactly why this matters: replaying one is the same as running any other code someone handed you. Only replay or validate a bundle from a source you trust. Nothing in the format authenticates who produced it.

## Why this exists

Backtest divergence is the most common bug in algorithmic trading. A strategy passes on your laptop, fails on someone else's machine, or behaves differently after an engine bump. A bundle pins all the inputs (strategy SHA, tape SHA, engine version, slippage config) and the recorded output, so divergence becomes a hard CI failure instead of a slow erosion of trust.

## Layout

```
bundle.tar
├── manifest.json          # versions + integrity hashes + config
├── strategy/
│   └── strategy.py        # the user's strategy module
├── config/
│   └── params.json        # runtime params (slippage, queue model, ...)
├── tape/                  # the tape this run drove against
│   ├── manifest.json
│   └── trades-*.bin
├── expected_output.json   # fill sequence, totals, trade count (legacy JSON summary)
└── expected.lrvxrun/      # per-run trace: signals, orders, fills
    ├── manifest.json
    ├── signals-*.bin
    ├── orders-*.bin
    └── fills-*.bin
```

Older bundles produced before the `.lrvxrun` ride-along landed contain only `expected_output.json`. The replay path falls back to JSON-only diff for those; new bundles ship both, so a reader can do richer signal / order / fill comparisons against the bundled trace.

## Pack

```bash
lrvx bundle pack \
    --strategy ./my_strategy.py \
    --tape ./tapes/bybit-btc-2026-05-07 \
    --output ./bundles/strat-2026-05-07.tar \
    --slippage-model fixed_bps --slippage-bps 5
```

Pack runs the strategy through the supplied tape with the given config, captures the fill sequence as `expected_output.json`, and writes the bundle. The tarball is reproducible-friendly: same inputs produce a manifest with the same SHAs.

### The tick

Pack learns the symbol's tick from `--tick-size` (`pack_bundle(tick_size=...)`) and nowhere else; it reads nothing about the tick from the tape. Without it the symbol is registered with no declared tick, so order and trigger prices are not checked against one -- the same as running the strategy on a symbol registered without `tick_size`. With it, prices off that tick are refused at pack and at every replay. The manifest records the choice as `tick_size_declared` (with `tick_size` beside it, 0.01 when none was declared).

Bundles packed before `tick_size_declared` existed have no such key. Pack then always declared its tick (0.01 unless `--tick-size` said otherwise), so replay of such a manifest declares `tick_size`, or 0.01 when that key is absent too: an old bundle replays exactly as it always has.

## Replay

```bash
lrvx bundle replay ./bundles/strat-2026-05-07.tar
```

Extracts the bundle, runs the strategy through the bundled tape with the bundled config, and prints actual versus expected counts. Replay does not assert; for the assertion use `validate`.

## Validate

```bash
lrvx bundle validate ./bundles/strat-2026-05-07.tar
```

Replay plus a strict comparison against the recorded `expected_output.json`. Exits `0` on match, `1` on divergence with a per-key diff printed to stderr. This is the form to wire into CI.

## What's compared

The validator checks `trade_count`, `fill_count`, the per-fill `(symbol, side, price, quantity)` tuples in order, and `total_filled_quantity`. Order IDs are intentionally excluded because the runner assigns them from a process-wide counter that is not stable between processes.

It also recomputes `strategy_sha256` and `tape_sha256` from what was actually extracted and re-hashes the running engine's version, then compares all three against the values the manifest recorded at pack time. This catches a bundle whose `strategy.py` or tape changed after packing, even if `expected_output.json` was re-recorded to match the new content. It is not a substitute for signing: the manifest lives unsigned in the same archive it describes, so a party who edits the strategy can recompute these hashes too. What it does catch is accidental drift and an engine-version mismatch, which is the common case this format exists for.

When the bundle ships an `expected.lrvxrun/` trace, the validator also compares it with a trace captured from the replay. What is compared depends on what the expected trace holds, and the result says which in `lrvxrun_compared` (the same value in `actual` and `expected`):

| Bundle | Expected trace holds | Compared | `lrvxrun_compared` |
|--------|----------------------|----------|--------------------|
| Manifest has `lrvxrun_signals_recorded: true` (every bundle packed now) | one signal and one submit order event per order the strategy sent, one fill per fill | `lrvxrun_signal_count`, `lrvxrun_order_count`, `lrvxrun_fill_count` | `signals, order events, fills` |
| No `lrvxrun_signals_recorded`, and the trace has at least one signal or order event | the same three record kinds | the same three counts | `signals, order events, fills` |
| No `lrvxrun_signals_recorded`, and the trace has no signal and no order event | fills only | `lrvxrun_fill_count` | `fills` |
| No `expected.lrvxrun/` at all | nothing | the JSON fields alone | key absent |

The third row is a bundle packed while pack lost its signals: the runner's order ids are `(subscriber id << 32) + n`, the trace writer took a 32-bit signal id, and the refused write was swallowed together with the order event that followed it. Such a trace has fills and nothing else, so it cannot say how many signals the run emitted; the replay's signal and order-event counts are left out of both sides instead of being compared with zero. A trace from that time with any signal or order event in it recorded all of them, because one run's ids are all in the same range.

Pack now writes the signal with its whole 64-bit id (`signal_id64` on the record is the order id, and the order event's and the fill's `order_id` equal it), and a signal or order event that cannot be written fails the pack or the replay with a `RuntimeError` naming the trace and the writer's error. Nothing about the trace is skipped quietly.

Each record of the trace pack writes carries the fields the [`.lrvxrun` spec](../spec/lrvxrun.md) gives it. `feed_ts_ns` is the tape event the record belongs to: for a signal and its order event the trade the strategy was handling, for a fill the simulator's time of the fill. `run_ts_ns` is the wall clock at the moment the record was produced, so a fill sorts between the order that caused it and whatever the strategy did next. `fill_id` numbers the run's fills 1, 2, 3, ... (the simulator issues no fill ids of its own), so two fills of one order are two ids. Before this, every fill was written after the run had ended, with one `run_ts_ns` for all of them, `feed_ts_ns` 0, and the order's id in `fill_id`.

A bundle packed before that validates as it did: the comparison is of the record counts in the table above, never of ids or timestamps.

A trace that is present but cannot be read fails validation with a `lrvxrun_read_error` diff line naming the side and the reader's error; it is never skipped.

If you need stricter comparison (per-fill timestamps, intermediate equity curve), open an issue. The current set is the minimum that catches engine-level regressions; extending it is cheap once a real divergence shows up.

## Strategy file rules

The strategy file is a normal Python module. Pack expects exactly one `lrvx.Strategy` subclass; multiple subclasses fail with a clear error. The class is instantiated with the symbol list pack registered; if your `__init__` does not accept that, pack falls back to a no-arg constructor.

```python
import lrvx


class MyStrategy(lrvx.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self._fired = False

    def on_trade(self, ctx, trade):
        if self._fired:
            return
        self._fired = True
        self.market_buy(0.01)
```

Strategy state should reset every run (no global side effects, no clock dependencies, no random-without-seed). If your strategy uses `random`, seed it from the trade timestamp or pin a seed in the strategy code.

## What's not handled

- `CANCEL` / `CANCEL_ALL` / `MODIFY` signals. Pack routes `market`, `limit`, `stop_market`, `stop_limit`, `tp_market`, `tp_limit`, and `trailing_stop` into `SimulatedExecutor.submit_order`; anything else is dropped, so a strategy that cancels or replaces will not reproduce those actions in the bundle.
- Multi-strategy ensembles. Pack supports one strategy per bundle.
- Network or exchange-stub bundles. The tape is the only input source.

## Versioning

`bundle_format_version` is recorded in the manifest. A reader from a newer lrvx release reads `v1` bundles; a `v1` reader rejects newer bundles with a clear error rather than parsing them wrong. Migration paths land alongside any future format bump.

## See also

* [Record and replay tapes](tape-record.md). Bundles consume the same `.lrvx` format.
* [Paper trading](paper-trading.md). The same `SimulatedExecutor` powers paper mode and bundle replay.
* [Backtest with realistic fills](backtest-realistic-fills.md). The slippage / queue knobs flow into bundles unchanged.
