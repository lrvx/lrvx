# Replay

Read, write, and record market data in lrvx binary format. Supports LZ4 compression, time-range filtering, and symbol filtering.

## inspect()

Quick summary of a data directory without loading all data.

```python
info = lrvx.inspect(data_dir="./data")
print(f"Events: {info['total_events']}, Duration: {info['duration_seconds']:.0f}s")
```

**Returns dict:**

| Key | Type | Description |
|-----|------|-------------|
| `first_event_ns` | `int` | First event timestamp |
| `last_event_ns` | `int` | Last event timestamp |
| `total_events` | `int` | Total event count |
| `segment_count` | `int` | Number of segment files |
| `total_bytes` | `int` | Total data size |
| `duration_seconds` | `float` | Time span in seconds |
| `fully_indexed` | `bool` | Whether all segments have indexes |

---

## DataReader

Read trades and book updates from binary log files.

```python
reader = lrvx.DataReader(
    data_dir="./data",
    from_ns=None,             # start timestamp filter
    to_ns=None,               # end timestamp filter
    symbols=None,             # list of symbol IDs to filter
    reorder_window_ns=None,   # cross-block reorder window (default 10s)
)
```

`reorder_window_ns` controls the bounded reorder buffer used by `run()` and `streamForEach` on segments without the `Sorted` flag. Events arriving more than `reorder_window_ns` past the watermark raise `LrvxError(code="E_DATA_002")`. See [Aggregate tape events](../../how-to/aggregate-tape-events.md#out-of-order-tapes-and-the-reorder-buffer) for details.

### Methods

#### `summary() -> dict`

Dataset summary (same keys as `inspect()` plus `data_dir` and `symbols`).

#### `count() -> int`

Total event count.

#### `symbols() -> set[int]`

Set of available symbol IDs.

#### `time_range() -> tuple[int, int] | None`

`(start_ns, end_ns)` tuple, or `None` if empty.

#### `read_trades() -> ndarray`

Read all trades as a numpy structured array with `PyTrade` dtype.

```python
trades = reader.read_trades()
prices = trades['price_raw'] / 1e8
quantities = trades['qty_raw'] / 1e8
```

#### `read_trades_from(start_ts_ns) -> ndarray`

Read trades starting from a given timestamp.

```python
trades = reader.read_trades_from(start_ts_ns=1704067200_000_000_000)
```

#### `read_option_quotes_from(start_ts_ns) -> ndarray`

Read option quotes (mark price, implied vol, index price, open interest) from a
given timestamp as a numpy structured array with `PyOptionQuote` dtype.

```python
quotes = reader.read_option_quotes_from(start_ts_ns=1704067200_000_000_000)
marks = quotes['mark_price_raw'] / 1e8
ivs = quotes['iv_raw'] / 1e8
```

The walk is bounded by the reader's `to_ns`: quotes stamped at or past `to_ns`
are dropped, and the walk itself aborts once the event cursor passes
`to_ns` plus a 10-minute reorder slack. Without that bound a huge segment can
exhaust memory before the window ends.

#### `read_bbo() -> ndarray`

Read top-of-book (best bid/ask) from every book update event as a numpy structured array with `PyBBO` dtype.

```python
bbos = reader.read_bbo()
mids = (bbos['bid_price_raw'] + bbos['ask_price_raw']) / 2 / 1e8
```

#### `read_bbo_from(start_ts_ns) -> ndarray`

Same as `read_bbo()` but starts iterating from the first event whose `exchange_ts_ns >= start_ts_ns`.

```python
bbos = reader.read_bbo_from(start_ts_ns=1704067200_000_000_000)
```

#### `read_book_updates() -> tuple[ndarray, ndarray]`

Read every book update event with full depth. Returns `(headers, levels)`:

- `headers` — structured array of `PyBookUpdateHeader`. Each row carries `level_offset`, `bid_count`, `ask_count` for slicing the levels array.
- `levels` — single flat structured array of `PyLevel` shared by all events; bids first, then asks, per event.

```python
headers, levels = reader.read_book_updates()
for h in headers:
    off, nb, na = h['level_offset'], h['bid_count'], h['ask_count']
    bids = levels[off : off + nb]
    asks = levels[off + nb : off + nb + na]
```

#### `read_book_updates_from(start_ts_ns) -> tuple[ndarray, ndarray]`

Same as `read_book_updates()` but starts iterating from the first event whose `exchange_ts_ns >= start_ts_ns`.

```python
headers, levels = reader.read_book_updates_from(start_ts_ns=1704067200_000_000_000)
```

#### `stats() -> dict`

Reader statistics.

| Key | Type | Description |
|-----|------|-------------|
| `files_read` | `int` | Segment files read |
| `events_read` | `int` | Total events processed |
| `trades_read` | `int` | Trade events |
| `book_updates_read` | `int` | Book update events |
| `bytes_read` | `int` | Bytes read |
| `crc_errors` | `int` | CRC checksum failures |

#### `run(aggregators, n_threads=0, progress_callback=None, progress_interval_ms=1000) -> bool`

Single-pass streaming aggregator dispatch. Walks the tape once and forwards each event to every aggregator's `onEvent`, then calls `finalize()` once on each. The GIL is released for the whole walk; aggregators must be self-contained. An empty list is a no-op with no decompression.

```python
reader.run([stats, peaks, quantiles])           # n_threads=0 → auto
reader.run([stats, peaks, quantiles], 4)        # 4 workers
reader.run([stats, peaks, quantiles], 1)        # explicit single-thread

def on_progress(pct, cursor_ts_ns):
    print(f"{pct:.1%} @ {cursor_ts_ns}")
    return True                                  # return False to cancel

reader.run([stats], n_threads=1, progress_callback=on_progress,
           progress_interval_ms=500)
```

`progress_callback` fires inside the run loop at most once per
`progress_interval_ms` with `(pct: float in [0, 1], cursor_ts_ns: int)`.
Returning `False` cancels the run; raising stops it and re-raises after
`finalize()`. Progress is reported only on the single-thread path — multi-thread
runs ignore the callback, because per-event GIL re-acquisition would defeat the
parallelism.

`n_threads` policy:

- `0` (default): auto, resolved to `min(blocks_per_segment / 2, hardware_concurrency())`.
- `1`: explicit single-thread.
- `>1`: explicit worker count, capped to the effective block count per segment.

Parallel mode partitions the segment at the compressed-block level. Each worker holds a panel cloned via `cloneEmpty()` and walks its assigned block range; results merge via `merge()` before `finalize()`. See [Aggregate tape events](../../how-to/aggregate-tape-events.md#parallel-execution) for boundary semantics on sliding-window aggregators.

#### `segment_files() -> list[str]`

List of segment file paths.

#### `segments() -> list[dict]`

Segment metadata.

| Key | Type | Description |
|-----|------|-------------|
| `path` | `str` | File path |
| `first_event_ns` | `int` | First event timestamp |
| `last_event_ns` | `int` | Last event timestamp |
| `event_count` | `int` | Events in segment |
| `has_index` | `bool` | Whether segment has an index |

### PyTrade Dtype

| Field | Type | Description |
|-------|------|-------------|
| `exchange_ts_ns` | `int64` | Exchange timestamp (ns) |
| `recv_ts_ns` | `int64` | Local receive timestamp (ns) |
| `price_raw` | `int64` | Price * 10^8 |
| `qty_raw` | `int64` | Quantity * 10^8 |
| `trade_id` | `uint64` | Exchange trade ID |
| `symbol_id` | `uint32` | Symbol ID |
| `side` | `uint8` | 0 = buy, 1 = sell |

### PyBBO Dtype

| Field | Type | Description |
|-------|------|-------------|
| `exchange_ts_ns` | `int64` | Exchange timestamp (ns) |
| `recv_ts_ns` | `int64` | Local receive timestamp (ns) |
| `seq` | `int64` | Exchange sequence number |
| `symbol_id` | `uint32` | Symbol ID |
| `event_type` | `uint8` | 0 = snapshot, 1 = delta, 2 = top-of-book (the record byte, not the frame type) |
| `bid_price_raw` | `int64` | Best bid price * 10^8 (0 if absent) |
| `bid_qty_raw` | `int64` | Best bid quantity * 10^8 |
| `ask_price_raw` | `int64` | Best ask price * 10^8 (0 if absent) |
| `ask_qty_raw` | `int64` | Best ask quantity * 10^8 |

### PyBookUpdateHeader Dtype

| Field | Type | Description |
|-------|------|-------------|
| `exchange_ts_ns` | `int64` | Exchange timestamp (ns) |
| `recv_ts_ns` | `int64` | Local receive timestamp (ns) |
| `seq` | `int64` | Exchange sequence number |
| `symbol_id` | `uint32` | Symbol ID |
| `bid_count` | `uint16` | Number of bid levels for this event |
| `ask_count` | `uint16` | Number of ask levels for this event |
| `level_offset` | `uint32` | Index of this event's first level in the levels array |
| `event_type` | `uint8` | 0 = snapshot, 1 = delta, 2 = top-of-book (the record byte, not the frame type) |

### PyLevel Dtype

| Field | Type | Description |
|-------|------|-------------|
| `price_raw` | `int64` | Price * 10^8 |
| `qty_raw` | `int64` | Quantity * 10^8 |
| `side` | `uint8` | 0 = bid, 1 = ask |

### PyOptionQuote Dtype

Mixed scales: `mark_price_raw`, `index_price_raw`, `underlying_price_raw`,
`bid_price_raw` and `ask_price_raw` use `PRICE_SCALE` (10^8); `iv_raw`,
`bid_iv_raw` and `ask_iv_raw` use 10^8; `bid_size_raw`, `ask_size_raw` and
`open_interest_raw` use `QUANTITY_SCALE` (10^8).

| Field | Type | Description |
|-------|------|-------------|
| `exchange_ts_ns` | `int64` | Exchange timestamp (ns) |
| `recv_ts_ns` | `int64` | Local receive timestamp (ns) |
| `mark_price_raw` | `int64` | Mark price * 10^8 |
| `index_price_raw` | `int64` | Spot index price * 10^8 |
| `underlying_price_raw` | `int64` | Per-expiry forward used for pricing / moneyness * 10^8 |
| `iv_raw` | `int64` | Mark implied vol * 10^8 |
| `bid_price_raw` | `int64` | Best bid * 10^8 |
| `ask_price_raw` | `int64` | Best ask * 10^8 |
| `bid_size_raw` | `int64` | Best bid size * 10^8 |
| `ask_size_raw` | `int64` | Best ask size * 10^8 |
| `bid_iv_raw` | `int64` | Implied vol at the bid * 10^8 |
| `ask_iv_raw` | `int64` | Implied vol at the ask * 10^8 |
| `open_interest_raw` | `int64` | Open interest * 10^8 |
| `symbol_id` | `uint32` | Symbol ID |
| `instrument` | `uint8` | Instrument kind |

---

## DataWriter

Write trade data to binary log files.

```python
writer = lrvx.DataWriter(
    output_dir="./output",
    max_segment_mb=256,
    exchange_id=0,
    compression="none",   # "none" or "lz4"
)
```

### Methods

#### `write_trade(exchange_ts_ns, recv_ts_ns, price, qty, trade_id, symbol_id, side) -> bool`

Write a single trade. Returns `True` on success.

```python
writer.write_trade(
    exchange_ts_ns=1704067200_000_000_000,
    recv_ts_ns=1704067200_001_000_000,
    price=50000.0,
    qty=1.5,
    trade_id=12345,
    symbol_id=1,
    side=0,
)
```

#### `write_trades(exchange_ts_ns, recv_ts_ns, prices, quantities, trade_ids, symbol_ids, sides) -> int`

Vectorized write from numpy arrays. Returns number of trades written.

```python
count = writer.write_trades(
    exchange_ts_ns=ts_array,
    recv_ts_ns=recv_array,
    prices=price_array,
    quantities=qty_array,
    trade_ids=id_array,
    symbol_ids=sym_array,
    sides=side_array,
)
```

#### `write_book(exchange_ts_ns, recv_ts_ns, seq, symbol_id, is_snapshot, bids, asks) -> bool`

Write a single book update. `bids` and `asks` are `PyLevel` structured arrays.

```python
writer.write_book(
    exchange_ts_ns=1704067200_000_000_000,
    recv_ts_ns=1704067200_001_000_000,
    seq=42,
    symbol_id=1,
    is_snapshot=True,
    bids=bid_levels,
    asks=ask_levels,
)
```

#### `write_books(headers, levels) -> int`

Batched book writer. Takes the same `(headers, levels)` pair that
`DataReader.read_book_updates()` returns, so a read/write round-trip is
lossless.

#### `write_option_quotes(exchange_ts_ns, recv_ts_ns, mark_prices, index_prices, ivs, open_interest, symbol_ids, bid_prices=None, ask_prices=None, underlying_prices=None, bid_sizes=None, ask_sizes=None, bid_ivs=None, ask_ivs=None) -> int`

Bulk-write option quotes. `mark_prices` / `index_prices` are doubles at
`PRICE_SCALE`, `ivs` are doubles (e.g. `0.65`), `open_interest` is a double at
`QUANTITY_SCALE`. The trailing arguments are optional and default to zero:
`bid_prices` / `ask_prices` and `bid_sizes` / `ask_sizes` give the touch the
fill model crosses; `underlying_prices` is the per-expiry forward used for
pricing and moneyness, as opposed to `index_prices` (spot index); `bid_ivs` /
`ask_ivs` are the vol at each touch. Returns the count written.

#### `flush()`

Flush buffered data to disk.

#### `close()`

Close the writer and finalize all segments.

#### `stats() -> dict`

Writer statistics.

| Key | Type | Description |
|-----|------|-------------|
| `bytes_written` | `int` | Total bytes |
| `events_written` | `int` | Total events |
| `segments_created` | `int` | Segment files created |
| `trades_written` | `int` | Trade events |
| `book_updates_written` | `int` | Book update events |
| `blocks_written` | `int` | Data blocks |
| `uncompressed_bytes` | `int` | Uncompressed size |
| `compressed_bytes` | `int` | Compressed size |

#### `current_segment_path() -> str`

Path of the current segment being written.

---

## BinaryLogRecorderHook

Built-in `.lrvx` recorder. Plug into a `Runner` via
`runner.set_market_data_recorder(hook)`. Lifecycle is driven by the
engine; both trades and book updates are captured.

```python
hook = lrvx.BinaryLogRecorderHook(
    "./recordings",
    max_segment_mb=256,
    exchange_id=0,
    compression="none",       # or "lz4"
    exchange_name="binance",
    instrument_type="perpetual",
)
hook.add_symbol(1, "BTCUSDT", base="BTC", quote="USDT",
                price_precision=2, qty_precision=6)
runner.set_market_data_recorder(hook)
```

`exchange_name` is stamped into the recording's `metadata.json` as
`metadata.exchange`, and `instrument_type` as `metadata.instrument_type`
(`"spot"`, `"perpetual"`, `"futures"`, `"option"`). `MergedTapeReader` keys
symbols by `(exchange, name)` — tapes recorded without an exchange name refuse
to merge.

### Methods

#### `add_symbol(symbol_id, name, base="", quote="", price_precision=8, qty_precision=8)`

Register a symbol in the recording metadata.

#### `flush()`

Flush buffered bytes to disk.

#### `close()`

Stop the underlying writer (idempotent — also called by the engine's
on-stop lifecycle).

#### `current_segment_path() -> str`

Path of the segment currently being written. Empty before `start()`.

#### `stats() -> dict`

| Key | Type | Description |
|-----|------|-------------|
| `trades_written` | `int` | Trades recorded |
| `book_updates_written` | `int` | Book updates recorded |
| `bytes_written` | `int` | Bytes written |
| `segments_created` | `int` | Segments written |
| `errors` | `int` | Writer rejections |
