# Reproducibility bundles with `lrvx bundle`

A bundle is a single tarball that encodes everything needed to reproduce a backtest result on another machine: strategy code, the captured tape it ran against, the engine version, and the expected output. `lrvx bundle pack` captures a run; `lrvx bundle validate` replays it and compares the result with the expected output field by field (trade count, every fill's symbol, side, price and quantity, total filled quantity), along with the strategy and tape hashes, the engine version and the run's trace, record by record for a bundle packed now.

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

The validator checks `trade_count`, `fill_count`, the per-fill `(symbol, side, price, quantity)` tuples in order, and `total_filled_quantity`, plus `cancel_count` and `modify_count` for a bundle that replays cancels and modifies (see [Cancels and modifies](#cancels-and-modifies)). The JSON summary carries no order ids; for a bundle packed now they are compared in the trace (below).

It also recomputes `strategy_sha256` and `tape_sha256` from what was actually extracted and re-hashes the running engine's version, then compares all three against the values the manifest recorded at pack time. This catches a bundle whose `strategy.py` or tape changed after packing, even if `expected_output.json` was re-recorded to match the new content. It is not a substitute for signing: the manifest lives unsigned in the same archive it describes, so a party who edits the strategy can recompute these hashes too. What it does catch is accidental drift and an engine-version mismatch, which is the common case this format exists for.

When the bundle ships an `expected.lrvxrun/` trace, the validator also compares it with a trace captured from the replay. What is compared depends on what the expected trace holds, and the result says which in `lrvxrun_compared` (the same value in `actual` and `expected`):

| Bundle | Expected trace holds | Compared | `lrvxrun_compared` |
|--------|----------------------|----------|--------------------|
| Manifest has `lrvxrun_records_comparable: true` (every bundle packed now) | one signal and one submit order event per order the strategy sent, the cancel and modify records of a bundle with `cancels_and_modifies_routed` (see [Cancels and modifies](#cancels-and-modifies)), one fill per fill | the three counts below, and every record in order, field by field, except `run_ts_ns`: `lrvxrun_signals`, `lrvxrun_order_events`, `lrvxrun_fills` | `signal, order event and fill records` |
| Manifest has `lrvxrun_signals_recorded: true` but not `lrvxrun_records_comparable` | the same three record kinds | `lrvxrun_signal_count`, `lrvxrun_order_count`, `lrvxrun_fill_count` | `signals, order events, fills` |
| No `lrvxrun_signals_recorded`, and the trace has at least one signal or order event | the same three record kinds | the same three counts | `signals, order events, fills` |
| No `lrvxrun_signals_recorded`, and the trace has no signal and no order event | fills only | `lrvxrun_fill_count` | `fills` |
| No `expected.lrvxrun/` at all | nothing | the JSON fields alone | key absent |

The third row is a bundle packed while pack lost its signals: the runner's order ids are `(subscriber id << 32) + n`, the trace writer took a 32-bit signal id, and the refused write was swallowed together with the order event that followed it. Such a trace has fills and nothing else, so it cannot say how many signals the run emitted; the replay's signal and order-event counts are left out of both sides instead of being compared with zero. A trace from that time with any signal or order event in it recorded all of them, because one run's ids are all in the same range.

Pack now writes the signal with its whole 64-bit id (`signal_id64` on the record is the order id, and the order event's and the fill's `order_id` equal it), and a signal or order event that cannot be written fails the pack or the replay with a `RuntimeError` naming the trace and the writer's error. Nothing about the trace is skipped quietly.

Each record of the trace pack writes carries the fields the [`.lrvxrun` spec](../spec/lrvxrun.md) gives it. `feed_ts_ns` is the tape event the record belongs to: for a signal and its order event the trade the strategy was handling, for a fill the simulator's time of the fill. `run_ts_ns` is the wall clock at the moment the record was produced, so a fill sorts between the order that caused it and whatever the strategy did next. `fill_id` numbers the run's fills 1, 2, 3, ... (the simulator issues no fill ids of its own), so two fills of one order are two ids. Before this, every fill was written after the run had ended, with one `run_ts_ns` for all of them, `feed_ts_ns` 0, and the order's id in `fill_id`.

### Record by record

A bundle packed now records `lrvxrun_records_comparable: true` in its manifest, and validate reads every signal, order-event and fill record from both traces and compares them in order, field by field. A record that differs is one diff line naming the record and the fields, for example `lrvxrun_fills[1]: price_raw actual=10100000000 expected=10100000001`; a record that one side has and the other does not is named as missing. Before this the comparison was of the counts alone, so a fill at another price, an order on the other side or a signal under another name passed as long as the number of records agreed.

One field is left out: `run_ts_ns`, on every record kind. It is the wall clock at the moment the record was produced, so no two runs agree on it, in one process or in two. Everything else is the same at each replay of one bundle, including in another process: `feed_ts_ns` comes from the tape and the simulator's clock; the order ids (`signal_id`, `signal_id64`, `order_id`, `parent_signal_id`) are `(subscriber id << 32) + n` from the one strategy pack attaches, numbered by that strategy alone; `fill_id` numbers the run's fills; prices, quantities, fees, sides, kinds, flags, names, symbol ids and payloads follow from the tape and the strategy. The trace's manifest (`run_started_ns`, `run_ended_ns`) is not compared, since it is wall clock too.

A bundle packed before that marker validates as it did: the comparison is of the record counts in the table above, never of ids or timestamps.

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

## Cancels and modifies

A strategy's `cancel_order`, `cancel_all_orders` and `modify_order` reach the simulator in a bundle the same way they do in `BacktestRunner`: a cancel goes to `SimulatedExecutor.cancel_order`, a cancel-all to `cancel_all` for the bundle's symbol, and a modify to `modify_order`. For a modify, the new price becomes the trigger of a `stop_market`, `tp_market` or `trailing_stop` order and the limit of any other order, and the new quantity is what is left to work. An off-tick modify, or one naming an order that is no longer working, is refused and leaves the order as it was. A cancelled order no longer fills, and a modified one fills at its new price and size, at pack and at every replay.

Each one is recorded in the trace beside the run's submits:

- a signal named `cancel`, `cancel_all` or `modify`, whose `signal_id64` is the order's id (0 for a cancel-all);
- an order event with the order's id for each order the signal acted on: `CANCEL` for each order the simulator still held when a cancel or cancel-all came, with the order's side, type, price and quantity as last sent; `MODIFY` with the new price and quantity when the simulator took the replacement. A cancel of an order that has already filled or been cancelled, and a refused modify, write the signal and no order event.

`expected_output.json` counts them as `cancel_count` (orders cancelled, by a cancel or a cancel-all) and `modify_count` (replacements the simulator took), and validate compares both, along with the records.

The manifest of a bundle packed this way has `cancels_and_modifies_routed: true`. A bundle packed before that has no such key: it was packed with its cancels and modifies dropped, so its fills and trace show none of them. Its replay drops them again, so it validates exactly as it did, on the comparison its other markers give it.

## What's not handled

- OCO signals and the DEX liquidity signals (`provide_liquidity`, `withdraw_liquidity`). Pack sends the order types `market`, `limit`, `stop_market`, `stop_limit`, `tp_market`, `tp_limit` and `trailing_stop`, and cancels, cancel-alls and modifies (see [Cancels and modifies](#cancels-and-modifies)), to the simulator; any other signal is dropped.
- Multi-strategy ensembles. Pack supports one strategy per bundle.
- Network or exchange-stub bundles. The tape is the only input source.

## Versioning

`bundle_format_version` is recorded in the manifest. A reader from a newer lrvx release reads `v1` bundles; a `v1` reader rejects newer bundles with a clear error rather than parsing them wrong. Migration paths land alongside any future format bump.

## See also

* [Record and replay tapes](tape-record.md). Bundles consume the same `.lrvx` format.
* [Paper trading](paper-trading.md). The same `SimulatedExecutor` powers paper mode and bundle replay.
* [Backtest with realistic fills](backtest-realistic-fills.md). The slippage / queue knobs flow into bundles unchanged.
