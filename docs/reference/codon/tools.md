# Tools

Utility classes for order books, position tracking, profiles, data I/O, and statistics.

```codon
from lrvx.tools import (
    OrderBook, L3Book, CompositeBook,
    PositionTracker, PositionGroupTracker, OrderTracker,
    VolumeProfile, MarketProfile, FootprintBar,
    DataWriter, DataReader, BinaryLogRecorderHook, Partitioner,
    correlation, profit_factor, win_rate,
    permutation_test, bootstrap_ci,
    validate_segment, merge_segments, merge_dir,
    split_segment, export_segment, validate_segment_full, validate_dataset,
    recompress_segment, extract_symbols, extract_time_range,
)
```

---

## Order books

### OrderBook

L2 order book.

```codon
book = OrderBook(tick_size=0.01)
book.apply_snapshot(bid_prices, bid_qtys, ask_prices, ask_qtys)
bid = book.best_bid()
```

| Method | Returns | Description |
|--------|---------|-------------|
| `apply_snapshot(bp, bq, ap, aq)` | `None` | Full snapshot |
| `apply_delta(bp, bq, ap, aq)` | `None` | Incremental update |
| `best_bid()` | `Optional[float]` | Best bid price |
| `best_ask()` | `Optional[float]` | Best ask price |
| `mid()` | `Optional[float]` | Mid price |
| `spread()` | `Optional[float]` | Bid-ask spread |
| `is_crossed()` | `bool` | True if book is crossed |
| `clear()` | `None` | Clear all levels |

`tick_size` must be positive; the constructor raises `ValueError` otherwise. The
smallest representable tick is `1e-8`, since prices are fixed-point with a scale
of 1e8. The book covers 8192 consecutive ticks centred on the market and
re-anchors that window as a delta feed walks the price away from the last
snapshot. See
[The order book's tick window](../../explanation/order-book-tick-window.md).

### L3Book

Order-level book with individual order tracking.

```codon
book = L3Book()
book.add_order(order_id, price, qty, "buy")
```

| Method | Returns | Description |
|--------|---------|-------------|
| `add_order(order_id, price, qty, side)` | `int` | 0 on success |
| `remove_order(order_id)` | `int` | 0 on success |
| `modify_order(order_id, new_qty)` | `int` | 0 on success |
| `best_bid()` | `Optional[float]` | Best bid price |
| `best_ask()` | `Optional[float]` | Best ask price |
| `bid_at_price(price)` | `float` | Total bid quantity at price |
| `ask_at_price(price)` | `float` | Total ask quantity at price |

### CompositeBook

Aggregates books across multiple exchanges per symbol.

```codon
cb = CompositeBook()
result = cb.best_bid(symbol_id)  # Optional[Tuple[float, float]]
```

| Method | Returns | Description |
|--------|---------|-------------|
| `best_bid(symbol)` | `Optional[Tuple[float, float]]` | `(price, qty)` or `None` |
| `best_ask(symbol)` | `Optional[Tuple[float, float]]` | `(price, qty)` or `None` |
| `has_arbitrage(symbol)` | `bool` | True if arbitrage opportunity exists |
| `mark_stale(exchange, symbol)` | `None` | Mark exchange data as stale |
| `check_staleness(now_ns, threshold_ns)` | `None` | Evict stale data |

---

## Position tracking

### PositionTracker

FIFO/average-cost position tracking.

```codon
tracker = PositionTracker()          # FIFO (default)
tracker = PositionTracker(1)         # average cost

tracker.on_fill(symbol, "buy", price, qty)
pos = tracker.position(symbol)
```

| Method | Returns | Description |
|--------|---------|-------------|
| `on_fill(symbol, side, price, qty)` | `None` | Record a fill |
| `position(symbol)` | `float` | Current net position |
| `avg_entry_price(symbol)` | `float` | Average entry price |
| `realized_pnl(symbol)` | `float` | Realized PnL for symbol |
| `total_realized_pnl()` | `float` | Total realized PnL |

### PositionGroupTracker

Tracks individual named positions (open/partial-close/close).

```codon
tracker = PositionGroupTracker()
pos_id = tracker.open_position(order_id, symbol, "buy", price, qty)
tracker.close_position(pos_id, exit_price)
```

| Method | Returns | Description |
|--------|---------|-------------|
| `open_position(order_id, symbol, side, price, qty)` | `int` (position ID) | Open a position |
| `close_position(position_id, exit_price)` | `None` | Close fully |
| `partial_close(position_id, qty, exit_price)` | `None` | Partial close |
| `net_position(symbol)` | `float` | Net position for symbol |
| `realized_pnl(symbol)` | `float` | Realized PnL for symbol |
| `total_realized_pnl()` | `float` | Total realized PnL |
| `open_position_count(symbol)` | `int` | Number of open positions |
| `prune()` | `None` | Free closed position memory |

### OrderTracker

Tracks submitted/filled/canceled orders.

```codon
tracker = OrderTracker()
tracker.on_submitted(order_id, symbol, "buy", price, qty)
tracker.on_filled(order_id, fill_qty)
```

| Method / Property | Returns | Description |
|-------------------|---------|-------------|
| `on_submitted(order_id, symbol, side, price, qty)` | `bool` | Register a submitted order |
| `on_filled(order_id, fill_qty)` | `bool` | Record a fill |
| `on_canceled(order_id)` | `bool` | Record a cancellation |
| `is_active(order_id)` | `bool` | True if order is still active |
| `active_count` | `int` | Number of active orders |
| `total_count` | `int` | Total orders seen |
| `prune()` | `None` | Free completed order memory |

---

## Profiles

### VolumeProfile

```codon
vp = VolumeProfile(tick_size=0.01)
vp.add_trade(price, qty, is_buy)
poc = vp.poc()
```

| Method | Returns | Description |
|--------|---------|-------------|
| `add_trade(price, qty, is_buy)` | `None` | Feed a trade |
| `poc()` | `float` | Point of control |
| `value_area_high()` | `float` | Value area high |
| `value_area_low()` | `float` | Value area low |
| `total_volume()` | `float` | Total volume |
| `clear()` | `None` | Reset |

### MarketProfile

```codon
mp = MarketProfile(tick_size=0.01, period_minutes=30, session_start_ns=0)
mp.add_trade(timestamp_ns, price, qty, is_buy)
```

| Method | Returns | Description |
|--------|---------|-------------|
| `add_trade(ts_ns, price, qty, is_buy)` | `None` | Feed a trade |
| `poc()` | `float` | Point of control |
| `value_area_high()` | `float` | Value area high |
| `value_area_low()` | `float` | Value area low |
| `initial_balance_high()` | `float` | Initial balance high |
| `initial_balance_low()` | `float` | Initial balance low |
| `is_poor_high()` | `bool` | Poor high |
| `is_poor_low()` | `bool` | Poor low |
| `clear()` | `None` | Reset |

### FootprintBar

Buy/sell delta per price level.

```codon
fp = FootprintBar(tick_size=0.01)
fp.add_trade(price, qty, is_buy)
delta = fp.total_delta()
```

| Method / Property | Returns | Description |
|-------------------|---------|-------------|
| `add_trade(price, qty, is_buy)` | `None` | Feed a trade |
| `total_delta()` | `float` | Net buy minus sell volume |
| `total_volume()` | `float` | Total volume |
| `num_levels` | `int` | Number of price levels |
| `clear()` | `None` | Reset |

---

## Statistics

```codon
from lrvx.tools import correlation, profit_factor, win_rate, permutation_test, bootstrap_ci

r = correlation(x, y)
pf = profit_factor(pnl_list)
wr = win_rate(pnl_list)
p = permutation_test(group1, group2, num_permutations=10000)
lo, med, hi = bootstrap_ci(data, confidence=0.95, num_samples=10000)
```

| Function | Returns | Description |
|----------|---------|-------------|
| `correlation(x, y)` | `float` | Pearson correlation |
| `profit_factor(pnl)` | `float` | Gross profit / gross loss |
| `win_rate(pnl)` | `float` | Fraction of positive values |
| `permutation_test(g1, g2, n=10000)` | `float` | Two-sample permutation p-value |
| `bootstrap_ci(data, confidence=0.95, n=10000)` | `Tuple[float, float, float]` | `(lower, median, upper)` |

---

## Bar aggregation

Turn a raw trade array into bars in one call — no live engine required.
See [Bar aggregation](../../how-to/bar-aggregation.md) for the same helpers
in Python and Node.js.

```codon
from lrvx.tools import aggregate_renko_bars

bars = aggregate_renko_bars(timestamps, prices, quantities, is_buy,
                            brick_size=2.0)
for b in bars:
    print(b.start_time_ns, b.open(), b.high(), b.low(), b.close())
```

`AggBar` mirrors the C `LrvxBar` struct: `*_raw` fields are fixed-point
(divide by `1e8`), plus float accessors `open()`, `high()`, `low()`,
`close()`, `volume()`, `buy_volume()`, a plain `trade_count`, and
`close_reason` -- why the bar closed (`0` threshold, `2` forced, `3` warmup;
every bar this batch path returns closed on its own threshold, so `0` here).

| Function | Threshold argument | Description |
|----------|--------------------|--------------|
| `aggregate_time_bars(ts, px, qty, is_buy, interval_seconds)` | seconds per bar | Fixed-duration bars |
| `aggregate_tick_bars(ts, px, qty, is_buy, tick_count)` | trades per bar | Fixed trade-count bars |
| `aggregate_volume_bars(ts, px, qty, is_buy, volume_threshold)` | traded volume | Closes once volume crosses the threshold |
| `aggregate_range_bars(ts, px, qty, is_buy, range_size)` | price range | Closes once price has moved `range_size` from the open |
| `aggregate_renko_bars(ts, px, qty, is_buy, brick_size)` | brick size | Fixed-size Renko bricks |
| `aggregate_heikin_ashi_bars(ts, px, qty, is_buy, interval_seconds)` | seconds per bar | Heikin-Ashi smoothed bars |

---

## Data I/O

### DataWriter

```codon
writer = DataWriter("./data", max_segment_mb=256, exchange_id=0)
writer.write_trade(exchange_ts_ns, recv_ts_ns, price, qty, trade_id, symbol_id, side)
writer.flush()
writer.close()
```

| Method | Returns | Description |
|--------|---------|-------------|
| `write_trade(exchange_ts_ns, recv_ts_ns, price, qty, trade_id, symbol_id, side)` | `bool` | Write a trade record |
| `flush()` | `None` | Flush to disk |
| `close()` | `None` | Close and finalize |
| `stats()` | `WriterStats` | Write statistics |

`WriterStats` fields: `bytes_written`, `events_written`, `segments_created`, `trades_written`.

### DataReader

```codon
reader = DataReader("./data")
reader = DataReader.create_filtered("./data", from_ns=0, to_ns=0, symbols=[])
trades = reader.read_trades()
bbos = reader.read_bbo()
events = reader.read_book_updates()
```

| Method / Property | Returns | Description |
|-------------------|---------|-------------|
| `count()` | `int` | Total event count |
| `summary()` | `DatasetSummary` | Dataset summary |
| `reader_stats()` | `ReaderStats` | Read statistics |
| `read_trades(max_trades=0)` | `List[TradeRecord]` | Read trades (all if `max_trades=0`) |
| `read_trades_from(start_ts_ns, max_trades=0)` | `List[TradeRecord]` | Same as `read_trades` starting from `start_ts_ns` |
| `read_bbo(max_events=0)` | `List[BBO]` | Top of book per book update event |
| `read_bbo_from(start_ts_ns, max_events=0)` | `List[BBO]` | Same as `read_bbo` starting from `start_ts_ns` |
| `read_book_updates()` | `List[BookUpdate]` | Full depth per book update event |
| `read_book_updates_from(start_ts_ns)` | `List[BookUpdate]` | Same as `read_book_updates` starting from `start_ts_ns` |

`DatasetSummary` fields: `first_event_ns`, `last_event_ns`, `total_events`, `segment_count`, `total_bytes`, `duration_seconds`.

`TradeRecord` fields: `exchange_ts_ns`, `recv_ts_ns`, `price_raw`, `qty_raw`, `trade_id`, `symbol_id`, `side`. Methods: `price()`, `qty()`, `is_buy()`.

`BBO` fields: `exchange_ts_ns`, `recv_ts_ns`, `seq`, `symbol_id`, `event_type` (0=snapshot, 1=delta, 2=top-of-book — the record byte, not the frame type), `bid_price_raw`, `bid_qty_raw`, `ask_price_raw`, `ask_qty_raw`. Methods: `bid_price()`, `ask_price()`, `bid_qty()`, `ask_qty()`.

`BookLevel` fields: `price_raw`, `qty_raw`, `side` (0=bid, 1=ask). Methods: `price()`, `qty()`.

`BookUpdate` fields: `exchange_ts_ns`, `recv_ts_ns`, `seq`, `symbol_id`, `event_type`, `bids: List[BookLevel]`, `asks: List[BookLevel]`.

### BinaryLogRecorderHook

```codon
hook = BinaryLogRecorderHook("./data", max_segment_mb=256,
                              exchange_id=0, compression="none")
hook.add_symbol(symbol_id, "BTCUSDT", base="BTC", quote="USDT")
# attach via the runner's market-data-recorder slot;
# lifecycle is driven by the engine.
```

| Method | Description |
|--------|-------------|
| `add_symbol(symbol_id, name, base="", quote="", price_precision=8, qty_precision=8)` | Register a symbol in the recording metadata. |
| `flush()` | Flush buffered bytes to disk. |
| `stats()` | Returns trade / book / byte counters. |
| `close()` | Stop the underlying writer (idempotent). |
| `_as_recorder_handle()` | Borrowed `LrvxMarketDataRecorderHandle` for runner attach. |

---

## Partitioner

Splits a dataset into partitions for parallel backtesting.

```codon
p = Partitioner("./data")
partitions = p.by_time(num_partitions=4, warmup_ns=0)
for part in partitions:
    # part.from_ns, part.to_ns, part.warmup_from_ns, ...
    pass
p.close()
```

`Partition` fields: `partition_id`, `from_ns`, `to_ns`, `warmup_from_ns`, `estimated_events`, `estimated_bytes`.

| Method | Description |
|--------|-------------|
| `by_time(num_partitions, warmup_ns=0)` | Split into N equal time slices |
| `by_duration(duration_ns, warmup_ns=0)` | Split by fixed duration |
| `by_calendar(unit=CALENDAR_MONTHLY, warmup_ns=0)` | Split by calendar unit |
| `by_symbol(num_partitions)` | Split by symbol group |
| `per_symbol()` | One partition per symbol |
| `by_event_count(num_partitions)` | Split by event count |
| `close()` | Free resources |

Calendar unit constants: `Partitioner.CALENDAR_DAILY = 0`, `CALENDAR_WEEKLY = 1`, `CALENDAR_MONTHLY = 2`.

---

## Segment operations

```codon
from lrvx.tools import validate_segment, merge_segments, merge_dir, split_segment, export_segment

validate_segment("path/to/seg")            # bool
merge_segments("input_dir", "output.bin")  # bool
```

| Function | Returns | Description |
|----------|---------|-------------|
| `validate_segment(path)` | `bool` | Quick validation |
| `validate_segment_full(path, verify_crc=True, verify_timestamps=True)` | `SegmentValidation` | Full validation |
| `validate_dataset(data_dir)` | `DatasetValidation` | Validate all segments in directory |
| `merge_segments(input_dir, output_path)` | `bool` | Merge directory into one file |
| `merge_dir(input_dir, output_dir)` | `MergeResult` | Merge with result metadata |
| `split_segment(input_path, output_dir, mode=0, time_interval_ns=0, events_per_file=0)` | `SplitResult` | Split segment |
| `export_segment(input_path, output_path, format=0, from_ns=0, to_ns=0, symbols=[])` | `ExportResult` | Export to CSV or other format |
| `recompress_segment(input_path, output_path, compression=1)` | `bool` | Recompress |
| `extract_symbols(input_path, output_path, symbols)` | `int` | Extract specific symbols |
| `extract_time_range(input_path, output_path, from_ns, to_ns)` | `int` | Extract time slice |
