# Merge multiple tapes on read

Each tape is per-exchange. `exchange_id` lives in the manifest,
`recv_ts` comes from one writer process, and failure isolation is
per-venue. To let a strategy see N venues at once, lrvx merges them
*on read*, not on write. `.lrvx` format does not change.

Three consumption paths share the same `MergedTapeReader` primitive:

- [`lrvx.tape.replay_tapes`](#python-replay) is a callback API for
  ad-hoc cross-venue scripts.
- [`lrvx.MergedTapeReader`](#python-direct) has the DataReader
  shape and is the right fit for bulk analytics.
- [`lrvx tape view path1 path2 …`](#cli-view) renders N tapes
  side-by-side in the replay viewer.

## Symbol identity

The merge keys symbols by `(metadata.exchange, name)`. Two tapes that
recorded the same `(bybit, BTCUSDT)` collapse into one global symbol
id (their trades go into a single time-sorted stream). Two tapes
that recorded `(bybit, BTCUSDT)` and `(binance, BTCUSDT)` get
distinct global ids; the merge keeps them as separate venues.

Same `(exchange, name)` with different `price_precision` /
`qty_precision` is a data-quality error
([E_INPUT_003](../errors/E_INPUT_003.md)). Tapes from older capture
stacks that drifted on precision either get excluded or re-recorded
under a distinct exchange string (`"bybit-legacy"` vs `"bybit"`).

When you record with `BinaryLogRecorderHook`, pass `exchange_name`
explicitly so the merge has something to key on:

=== "Python"
    ```python
    hook = lrvx.BinaryLogRecorderHook(
        "tapes/bybit-btc",
        exchange_name="bybit",
        instrument_type="perpetual",
    )
    ```

=== "Node.js"
    ```javascript
    const hook = new lrvx.BinaryLogRecorderHook(
        "tapes/bybit-btc", 256, 0, "none", "bybit", "perpetual");
    ```

If `exchange_name` is empty the merge still runs; it just won't
discriminate venues from each other.

## Tie-breaking

For two events with identical `exchange_ts_ns` the merge breaks ties
by tape order in the `paths` list, then by per-tape internal
sequence. Reproducible, but only if you pass the paths in the same
order across runs. Reorder the list and the tie-broken sub-sequence
flips.

## Trade vs book overlap

The merge handles trades and books differently when two tapes claim
the same `(exchange, name)` and their time ranges intersect:

- Trades are emitted in time order, no dedup. Each writer's view is
  preserved faithfully. If the same trade appeared in both tapes it
  shows up twice; upstream non-overlap or downstream dedup is on the
  caller.
- Books raise [`OverlappingBookStreamError`](../errors/E_INPUT_003.md)
  at `MergedTapeReader` construction. Overlapping book state has no
  defined semantics (whose reset wins?). Either time-slice the
  inputs or pick one.

## <a name="python-replay"></a>Python: `replay_tapes` callback API

```python
from lrvx import tape

def on_trade(ts_ns, global_id, price, qty, side):
    print(f"  trade {ts_ns} sym={global_id} {price:.2f}@{qty}")

def on_book(ts_ns, global_id, is_snapshot, bids, asks):
    typ = "snap" if is_snapshot else "delta"
    print(f"  book  {ts_ns} sym={global_id} {typ} {len(bids)}b/{len(asks)}a")

def on_book_top(ts_ns, global_id, bid, ask):
    print(f"  top   {ts_ns} sym={global_id} {bid} / {ask}")

stats = tape.replay_tapes(
    ["tapes/bybit-btc", "tapes/binance-btc"],
    on_trade=on_trade,
    on_book=on_book,
    on_book_top=on_book_top,
)

print(f"merged {stats.trades} trades, {stats.books} books across "
      f"{len(stats.tapes)} tapes")
for entry in stats.tapes:
    print(f"  {entry['path']}: {entry['trades']} trades / {entry['books']} books")
```

`on_book` receives snapshots and deltas. A top-of-book frame goes to
`on_book_top`, with `bid` / `ask` a `(price, qty)` tuple or `None` for an
empty side; leave it out and tops are skipped. `on_book` sees the same
snapshots and deltas either way.

`stats.tapes` gives a per-tape breakdown. Useful when one of the
inputs is empty (typo in path, recording crashed before any events).

## <a name="python-direct"></a>Python: `MergedTapeReader` direct API

```python
import lrvx

reader = lrvx.MergedTapeReader(
    ["tapes/bybit-btc", "tapes/binance-btc"],
    from_ns=1_700_000_000_000_000_000,   # optional time filter
    to_ns=None,
    symbols=None,                         # optional global-id filter
)

# Symbol table: rekeyed (global_id, exchange, name, precisions).
for s in reader.symbol_table():
    print(s)

# Merged numpy structured arrays. Same dtype as DataReader.
trades = reader.read_trades()
headers, levels = reader.read_books()

# Trades sorted by exchange_ts_ns; symbol_id is the global rekey.
bybit_trades = trades[trades["symbol_id"] == 1]
binance_trades = trades[trades["symbol_id"] == 2]
```

`read_trades` and `read_books` materialise the merged arrays, and so
does `tape.replay_tapes` — it calls both before invoking your
callbacks. The memory-bounded path is `stream_events`, an N-way heap
merge whose peak memory is O(number of tapes):

```python
def on_trade(exchange_ts_ns, recv_ts_ns, price_raw, qty_raw,
             symbol_id, tape_index, side):
    ...      # return False to abort the walk

reader.stream_events(on_trade=on_trade)
```

`on_book(exchange_ts_ns, recv_ts_ns, symbol_id, tape_index,
is_snapshot, bids, asks)` receives `bids` / `asks` as lists of
`(price_raw, qty_raw)` tuples, for snapshots and deltas.
`on_book_top(exchange_ts_ns, recv_ts_ns, symbol_id, tape_index, bid, ask)`
receives a top-of-book frame, `bid` / `ask` a `(price_raw, qty_raw)` tuple
or `None`; without it tops are skipped. Returning `False` from any of the
three aborts the walk. Note that `stream_events` reports raw
fixed-point values and a `tape_index` for provenance, which
`replay_tapes` does not.

Two more readers round out the surface:

- `run(aggregators)` walks the merged stream through a panel of
  streaming aggregators in one decompression pass, same semantics as
  `DataReader.run`. Aggregators see global-rewritten symbol ids and
  no per-tape provenance.
- `summary()` returns `first_event_ns`, `last_event_ns`,
  `total_events` (populated after `read_trades` / `read_books`),
  `tape_count`, and `symbol_count`.

## Cross-exchange basis backtest example

A two-line strategy that watches the spread between bybit and binance
BTC and prints when it exceeds 5 bps:

```python
import lrvx
from lrvx import tape

last = {1: None, 2: None}      # global_id → last price
def on_trade(ts_ns, sym, price, qty, side):
    last[sym] = price
    a, b = last.get(1), last.get(2)
    if a is not None and b is not None:
        bps = (a - b) / b * 10_000
        if abs(bps) > 5:
            print(f"{ts_ns}: bybit-binance = {bps:+.1f} bps")

tape.replay_tapes(
    ["tapes/bybit-btc", "tapes/binance-btc"],
    on_trade=on_trade,
)
```

For a real backtest, hand the merged stream to a `BacktestRunner`
via `bt.run_tapes([t1, t2])`. Mirrors `bt.run_tape(t)`: single-tape
input round-trips identically.

## <a name="cli-view"></a>CLI: `lrvx tape view`

```bash
lrvx tape view tapes/bybit-btc tapes/binance-btc
```

Opens the replay viewer pre-loaded with both tapes merged into one
synthetic stream. Each global symbol gets its own colour on the
price chart.

Under the hood the CLI materialises the merge into a fresh
synthetic `.lrvx` inside the per-session temp dir, rekeyed to
global symbol ids, and points the existing single-tape viewer at it.
This is the v1 path. The follow-up plan is a streaming `/merge`
HTTP endpoint that the SPA reads directly, with a multi-venue legend
and per-venue book chart; the materialisation route is what works
today without SPA-side changes.

Limitations:

- Books for the same `(exchange, name)` must not overlap in time.
- Legend labels show the rekeyed global ids; reading the
  metadata.json the CLI writes alongside the synthetic tape gives
  you the `exchange/name` mapping until the SPA learns to render it.

## See also

- [Spec: lrvx format](../spec/lrvx.md) for the per-tape on-disk
  shape.
- [Record and replay tapes](tape-record.md) for capturing the input
  tapes.
- [`BinaryLogRecorderHook`](../reference/api/replay/market_data_recorder.md):
  the producer side. `exchange_name=` matters for merging.
- Errors: [E_INPUT_002](../errors/E_INPUT_002.md) (missing metadata),
  [E_INPUT_003](../errors/E_INPUT_003.md) (precision mismatch).
