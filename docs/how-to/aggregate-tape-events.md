# Aggregate tape events in a single pass

`DataReader.run([...])` walks a `.lrvx` once and forwards every event to a panel of streaming aggregators. The aggregators run entirely in C++; the dispatch loop never crosses into Python or JS during the walk.

The seven native aggregators:

| Aggregator | Per-cell value | Typical use |
|---|---|---|
| `EventTypeStatsAggregator` | Counter of trades, book snapshots, book deltas, book tops | Quick overview of a tape's contents |
| `BinCountAggregator` | Event count per time bucket | Trades per minute, per second, etc. |
| `VolumeBinAggregator` | Sum of trade `qty_raw` per time bucket | Volume profile over time |
| `OHLCBinAggregator` | Open / high / low / close of trade `price_raw` per bucket | Price-time series for returns and candle reconstruction |
| `BookSnapshotBinAggregator` | Order-book ladder (up to N levels/side) at each bucket boundary | Per-bucket book shape: spread, depth, imbalance |
| `PeakAggregator` | Top-N busiest fixed-width windows per scale | Finding bursts at chosen scales |
| `QuantileAggregator` | Window-count distribution to quantiles | Activity baseline at a given scale |

The filter parameters work the same way on all seven:

- `event_filter`: `Trades`, `BooksOnly`, or `Both`. Aggregators that only make sense for trades (volume, OHLC, peaks, quantiles) default to `Trades`; `BookSnapshotBin` defaults to `BooksOnly`.
- `symbol_filter`: a list of symbol ids. The default (empty) admits every symbol.

`BookSnapshotBinAggregator` requires `n_threads=1` — see [Parallel execution](#parallel-execution).

`run([])` is a no-op. It does not decompress anything; useful as a sanity check that the framework is wired up.

## Python

```python
import lrvx

reader = lrvx.DataReader("./tape")

stats = lrvx.EventTypeStatsAggregator()
counts = lrvx.BinCountAggregator(
    bucket_ns=60_000_000_000,  # 1-minute buckets
    by_side=True,
)
volume = lrvx.VolumeBinAggregator(
    bucket_ns=60_000_000_000,
    by_side=True,
)
ohlc = lrvx.OHLCBinAggregator(
    bucket_ns=60_000_000_000,
)
book = lrvx.BookSnapshotBinAggregator(
    bucket_ns=60_000_000_000,
    levels=20,                 # rows per side per bucket; default 20
)
peaks = lrvx.PeakAggregator(
    window_ns_list=[1_000_000, 10_000_000, 100_000_000, 1_000_000_000],
    top_n=10,
)
quantiles = lrvx.QuantileAggregator(
    window_ns_list=[1_000_000, 100_000_000],
    quantiles=[0.5, 0.95, 0.99],
)

# n_threads=1 is mandatory once the panel contains BookSnapshotBinAggregator.
reader.run([stats, counts, volume, ohlc, book, peaks, quantiles], n_threads=1)

# .result() is populated once run() returns.
print(stats.result())       # structured numpy array
print(counts.result())      # structured numpy array
print(volume.result())      # structured numpy array
print(ohlc.result())        # structured numpy array
print(book.result())        # structured numpy array
print(peaks.result())       # dict[window_ns, list[(count, start_ns)]]
print(quantiles.result())   # dict[window_ns, dict[quantile, count]]
```

The tabular aggregators (stats, counts, volume, ohlc, book) return structured numpy arrays. Field names match the row struct, so `arr["count"]`, `arr["bucket_ts_ns"]`, `arr["close_raw"]` work directly.

`side` encoding in `BinCount` and `VolumeBin` rows: `0` means aggregate (no side split), `1` is BUY, `2` is SELL. `symbol_id = 0` means aggregate; set `by_symbol=True` to get one row per (bucket, symbol_id) cell.

## Node.js

```js
import { DataReader, EventTypeStatsAggregator, BinCountAggregator,
         VolumeBinAggregator, OHLCBinAggregator, BookSnapshotBinAggregator,
         PeakAggregator, QuantileAggregator, AggregatorEventFilter }
    from '@lrvx/lrvx';

const reader = new DataReader('./tape');

const F = AggregatorEventFilter;
const stats = new EventTypeStatsAggregator(F.Both, []);
const counts = new BinCountAggregator(60_000_000_000n, /*bySide=*/true);
const volume = new VolumeBinAggregator(60_000_000_000n, /*bySide=*/true);
const ohlc = new OHLCBinAggregator(60_000_000_000n);
const book = new BookSnapshotBinAggregator(60_000_000_000n, /*levels=*/20);
const peaks = new PeakAggregator(
    [1_000_000n, 10_000_000n, 100_000_000n, 1_000_000_000n],
    /*topN=*/10
);
const quantiles = new QuantileAggregator(
    [1_000_000n, 100_000_000n],
    [0.5, 0.95, 0.99]
);

reader.run([stats, counts, volume, ohlc, book, peaks, quantiles], 1);  // n_threads=1

console.log(stats.result());
console.log(counts.result());
console.log(volume.result());
console.log(ohlc.result());
console.log(book.result());     // Array<BookSnapshotBinRow>
console.log(peaks.result());
console.log(quantiles.result());
```

Node returns BigInt for ns timestamps and 64-bit counters, so values past `2^53` keep precision. The `Peak` result is `Array<{windowNs, peaks: Array<{count, startNs}>}>` rather than a Map; iterate with `for (const entry of result)`.

## Merged tapes

`MergedTapeReader.run([...])` takes the same aggregator panel and walks the merged stream from N input tapes in a single pass. Aggregators see events with global-rewritten symbol ids; the originating tape index is not surfaced. Use `streamEvents()` directly when the tape index matters.

```python
merged = lrvx.MergedTapeReader(["./tape-bybit", "./tape-bitget"])
peaks = lrvx.PeakAggregator(window_ns_list=[1_000_000_000], top_n=20)
merged.run([peaks])
```

## Aggregator semantics

### EventTypeStats

Result is sorted by `symbol_id` ascending. Symbols with zero matching events don't appear in the output. Pass an explicit `symbol_filter` for a stable shape across runs.

### BinCount and VolumeBin

The bucket for an event at time `t` is `floor(t / bucket_ns) * bucket_ns`. Empty buckets produce no rows. `by_side=True` only splits trades (book events stay in `side=0`); `by_symbol=True` produces one row per (bucket, symbol_id) cell.

`VolumeBin` ignores book events. Pass `event_filter=BooksOnly` and you get an empty result by design.

### OHLCBin

Same bucketing as `BinCount` / `VolumeBin`. For every (bucket, optional symbol) cell, four values come out of the trades that fell into it:

- `open_raw`: `price_raw` of the trade with the smallest `exchange_ts_ns`.
- `close_raw`: `price_raw` of the trade with the largest `exchange_ts_ns`.
- `high_raw` / `low_raw`: max and min of `price_raw` across the cell.

Books are ignored — no scalar price per book event. Empty buckets produce no row; forward-fill on the caller side if a dense series is needed.

There is no `by_side` parameter. An "open price for buys" is not a generally useful primitive; pair with `VolumeBinAggregator(by_side=True)` when per-side flow has to sit next to the price series.

`*_raw` fields are fixed-point int64. Divide by `Price::SCALE` (= 1e8) to get floats. Parallel runs preserve open/close ordering by comparing the per-cell first/last timestamps on merge.

### BookSnapshotBin

Maintains the full ladder per symbol from `BookSnapshot` / `BookDelta` events and, at each bucket boundary, emits the latest state observed inside the closed bucket as up to `levels` rows per side. One row pairs the bid and the ask at the same depth (`level=0` is top of book); the shorter side is zero-padded, so a row can carry a bid and a zero ask. Buckets with no book events for a symbol produce no rows; the state at the end of the walk is emitted as the trailing bucket.

Row fields: `bucket_ts_ns`, `symbol_id`, `level`, `flags`, `bid_price_raw`, `bid_qty_raw`, `ask_price_raw`, `ask_qty_raw`. Rows are sorted by `(bucket_ts_ns, symbol_id, level)` ascending. `flags` bit 0 (`kFlagCrossed`, value 1) marks a bucket where the best bid was `>=` the best ask. A zero price and qty on one side means no level at that depth.

`event_filter` is accepted for API parity, but only book events ever contribute — a `Trades` filter yields an empty result. `bucket_ns` and `levels` must both be `> 0`.

This is the C++ fast path for per-bucket book shape. The pure-Python equivalent is [`OrderBookIterator`](iterate-orderbook.md), which has the same bucket semantics but is meant for point queries and hours-scale windows; use this aggregator for month-scale multi-symbol feature extraction.

### Peak

For each `window_ns`, finds the top-N intervals of width `window_ns` that contain the most events. After the walk, `finalize()` removes peaks that fall within `3 × window_ns` of a stronger one already kept; this keeps the top-N from being filled with adjacent samples of one burst.

`oversample_factor` (default 100) sets the in-flight candidate heap size to `top_n × oversample_factor`. Raise it when tapes have many distinct bursts clustered within `3 × window_ns` of an earlier candidate.

`start_ns` in the result is `t - window_ns`, where `t` is the arrival time of the last event in the window. The window starts `window_ns` earlier and ends at the event.

### Quantile

For each `window_ns`, the aggregator records the sliding-window event count at every event arrival and accumulates those observations into a histogram. `finalize()` then resolves each requested quantile `q` to the smallest count value `C` for which `fraction(observed ≤ C) ≥ q`.

Quantiles must be in `(0.0, 1.0]`. The result row count is `len(window_ns_list) × len(quantiles)`. When `event_filter` admits no events, rows still appear with `count=0` so the shape stays stable.

## Parallel execution

`reader.run(panel)` takes an optional `n_threads` argument:

```python
reader.run(panel, n_threads=4)   # explicit 4 workers
reader.run(panel)                # default n_threads=0 → auto
```

`n_threads` policy:

- `0` (default): auto, resolved to `min(blocks_per_segment / 2, hardware_concurrency())`. Small captures stay single-threaded; multi-block segments use all available cores. The caller does not need to know the tape layout or core count.
- `1`: explicit single-thread. Useful for benchmarking the base path, or for environments where threading is undesirable.
- `>1`: explicit worker count, capped to the effective block count per segment. `n_threads=100` on a 4-block segment allocates 4 panel clones.

Partitioning is intra-segment at the compressed-block level. Each worker holds its own panel, cloned from the caller's via `IAggregator::cloneEmpty()`, and walks its assigned block range; results merge into the caller's originals via `IAggregator::merge()` before `finalize()`. Captures with one large active segment (a long-running recorder that appends to a single file) benefit from this layout; segment-level partitioning would leave workers idle when one segment dominates.

Every thread count delivers events in the order `n_threads=1` does. `Sorted` segments whose time ranges overlap (two captures of one window, an archive import that overlaps a recording) are merged event by event on the calling thread and fed to the caller's aggregators directly; only the remaining segments are partitioned. A tape recorded in order has no such segments and is partitioned in full. See [BinaryLogReader, Ordering guarantee](../reference/api/replay/binary_log_reader.md#ordering-guarantee).

Aggregators with associative state (`EventTypeStats`, `BinCount`, `VolumeBin`, `Quantile`) produce identical results between single-thread and parallel runs.

`BookSnapshotBinAggregator` cannot run partitioned at all. Book reconstruction is order-dependent across the whole tape: a worker that starts mid-stream would apply deltas to a ladder whose snapshot it never saw. `cloneEmpty()` and `merge()` therefore throw, and a parallel run fails with "book reconstruction is order-dependent across the whole tape". Pass `n_threads=1`.

The failure is layout-dependent: the reader only partitions when a segment has enough compressed blocks, so on small or uncompressed tapes `n_threads>1` silently runs on the master panel and succeeds. Do not rely on that — pass `n_threads=1` explicitly.

`PeakAggregator` and `QuantileAggregator` are sliding-window aggregators. With `N` workers there are up to `N-1` partition seams per segment, each at most `max(window_ns)` wide; windows that span a seam are not seen by any single worker. The trade-off is documented on `IAggregator::merge`. Results match the single-thread reference when `window_ns` is much wider than the seam. At sub-millisecond scales on a tape with 16 workers, expect a small fraction of cross-seam windows to be miscounted.

`MergedTapeReader.run()` is single-threaded (per-tape symbol rekey is instance-local, and parallel workers would not share a global symbol-id space).

## Out-of-order tapes and the reorder buffer

For segments without the `Sorted` flag (tapes produced by external writers or third-party recorders), the reader applies two ordering stages:

1. Intra-block sort runs on decompress, removing the ~1–2 ms exchange-WS jitter that accounts for most exchange-feed reordering. Cost: one linear scan of the block buffer per Sorted=false block.
2. Cross-block reorder buffer is a bounded min-heap of in-flight events keyed by `exchange_ts_ns`. As events arrive (each block already intra-sorted), the heap drains anything whose timestamp falls outside `watermark - reorder_window_ns`. Memory: `O(reorder_window_ns × peak_event_rate)`.

`reorder_window_ns` is a constructor argument on `DataReader`:

```python
reader = lrvx.DataReader(
    "./tape",
    reorder_window_ns=30_000_000_000,  # 30s
)
```

The 10s default covers about 99% of cross-block inversions seen on live exchange captures written without sorting (1–25s tail, p99 around 5s).

### Events that arrive too late

An event with `exchange_ts_ns < watermark - reorder_window_ns` has no slot left in sorted order: the reader has already emitted past it. It is dropped, counted in `ReaderStats::late_dropped` (`stats()["late_dropped"]` in Python), and reported once per segment in a warning that names the symbol, the event type, the file and the offset.

```python
reader.run([agg], n_threads=1)
dropped = reader.stats()["late_dropped"]
if dropped:
    print(f"{dropped} events arrived past the reorder window and were discarded")
```

The symbol filter runs before that check, so an event belonging to a symbol you did not ask to replay neither moves the watermark nor counts as late. The watermark also carries across segment boundaries within one walk, so an inversion straddling two segments is caught instead of disappearing when the watermark restarts.

Set `strict_ordering=True` to raise `LrvxError(code="E_DATA_002")` on the first late event instead. Byte-for-byte reproducibility gates want that, because there a dropped frame is a silent difference between two runs. Everywhere else dropping is the better trade. On a five-day bybit capture of 12.9 million frames, fifteen events sit deeper than ten seconds; raising on them ended the walk at the halfway mark with aggregators already fed half a dataset.

```python
reader = lrvx.DataReader("./tape", strict_ordering=True)
```

The other two options are unchanged: raise `reorder_window_ns`, or pre-sort the tape through `BinaryLogWriter`, which sets the `Sorted` flag and bypasses the reorder buffer.

### Aggregators that cannot be partitioned

`BookSnapshotBinAggregator` rebuilds the book by applying deltas in order from the start of the tape, so it cannot run on partitioned workers. It reports that through `IAggregator::supportsParallel()`, and `run()` checks the panel before it partitions anything: `n_threads=0` resolves to a single thread, and an explicit `n_threads > 1` is refused up front with `E_DATA_003` rather than throwing from inside a worker halfway through the walk.

## Performance notes

`onEvent` runs once per event for each aggregator in the panel. Adding aggregators raises the per-event cost; it does not change how many times the tape is walked.

Aggregators don't do I/O and don't call into Python. The only hot-path allocation is the bounded sliding deque used by Peak and Quantile.

For multi-tape captures use `MergedTapeReader.run()`. The C++ N-way heap merge keeps peak memory at `O(N_tapes)` regardless of total event count.

The Quantile histogram is keyed by `count_value` in an `unordered_map`. There are at most a few dozen distinct count values (bounded by the peak burst size), so the histogram stays small even when the tape contains billions of observations.

## Long-running tapes: progress callback

`DataReader.run()` on a multi-day capture can sit silently for minutes. `progress_callback` lets the caller report status without changing the aggregator surface.

```python
def progress(pct: float, ts_ns: int) -> bool:
    print(f"  {pct * 100:5.1f}%  ts={ts_ns}", flush=True)
    return True  # return False to cancel

reader.run([agg],
           n_threads=1,
           progress_callback=progress,
           progress_interval_ms=1000)
```

The callback fires from inside the run loop at most once per `progress_interval_ms` (default 1 s) with the bytes-of-events pct in `[0.0, 1.0]` and the exchange timestamp of the most recent dispatched event. Returning `False` cancels the run: `run()` returns `False`, aggregators are still finalised with the partial state they had accumulated, and the caller can call `.result()` on them. Returning `True` (or `None`) lets the run continue. Raising inside the callback also cancels, and the exception re-raises after `run()` finalises so the caller sees it instead of a silent partial.

Progress is reported only on the single-thread path (`n_threads=1`). Multi-thread runs ignore the callback because per-event GIL reentry would defeat the parallelism gained from cloning aggregators across workers; for those, run a smaller probe slice first to estimate throughput, then let the full run complete without progress.

```js
// Node.js: { onProgress, progressIntervalMs, nThreads }
reader.run([agg], {
  nThreads: 1,
  progressIntervalMs: 1000,
  onProgress: (pct, tsNs) => { console.log(`${(pct*100).toFixed(1)}%`); },
});
```

## See also

- [Record and replay market data with `lrvx tape`](tape-record.md) for capturing the `.lrvx`.
- [Merged tape consumption](multi-tape.md) for `MergedTapeReader` reference when streaming `run()` is not enough.
- [Iterate the order book](iterate-orderbook.md) for the event-by-event Python reconstruction `BookSnapshotBinAggregator` replaces on the hot path.
