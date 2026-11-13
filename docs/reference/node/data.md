# Data I/O

---

## DataWriter

Writes trades to binary log segments.

```javascript
const writer = new lrvx.DataWriter(outputDir, maxSegmentMb?, exchangeId?);
writer.writeTrade(exchangeTsNs, recvTsNs, price, qty, tradeId, symbolId, side);
writer.flush();
writer.close();
```

| Method | Returns | Description |
|--------|---------|-------------|
| `writeTrade(exchangeTsNs, recvTsNs, price, qty, tradeId, symbolId, side)` | `boolean` | Write a trade record |
| `writeBook(exchangeTsNs, recvTsNs, seq, symbolId, isSnapshot, bids, asks, updateType?)` | `boolean` | Write a book update |
| `flush()` | `void` | Flush to disk |
| `close()` | `void` | Close and finalize |
| `stats()` | `{ bytesWritten, eventsWritten, segmentsCreated, tradesWritten }` | Write statistics |

`writeBook`'s `exchangeTsNs`, `recvTsNs` and `seq` are `bigint`. `bids` and
`asks` are flat `BigInt64Array`s of interleaved raw int64
`[price, qty, price, qty, ...]` at the 1e8 price/quantity scale.
`updateType` is the record byte a book-update record carries in `eventType`:
`0` snapshot, `1` delta, `2` top-of-book. When given it decides the type;
`isSnapshot` is the older two-way form, and `true` with an `updateType` other
than `0` throws a `RangeError`.

---

## DataReader

Reads binary log segments.

```javascript
const reader = new lrvx.DataReader(dataDir, fromNs?, toNs?);
const trades = reader.readTrades();
const bbos = reader.readBBO();
const events = reader.readBookUpdates();
```

| Method / Property | Returns | Description |
|-------------------|---------|-------------|
| `count` | `number` | Total event count (property) |
| `summary()` | `{ firstEventNs, lastEventNs, totalEvents, segmentCount, totalBytes, durationSeconds }` | Dataset summary. `firstEventNs` and `lastEventNs` are `bigint`; the rest are `number` |
| `stats()` | `{ filesRead, eventsRead, tradesRead, bookUpdatesRead, bytesRead, crcErrors }` | Read statistics |
| `readTrades(max?)` | trade record array | Read up to `max` trades (all if omitted) |
| `readTradesFrom(startTsNs, max?)` | trade record array | Same as `readTrades` starting from `startTsNs` |
| `readBBO(max?)` | BBO record array | Top of book per book update event |
| `readBBOFrom(startTsNs, max?)` | BBO record array | Same as `readBBO` starting from `startTsNs` |
| `readBookUpdates()` | book update array | Full depth: each event includes `bids` and `asks` arrays |
| `readBookUpdatesFrom(startTsNs)` | book update array | Same as `readBookUpdates` starting from `startTsNs` |
| `run(aggregators, options?)` | `boolean` | Streaming aggregator dispatch over the tape. See below. |

`run`'s second argument is either an `nThreads` integer (back-compat) or a
`DataReaderRunOptions` object:

| Field | Type | Description |
|-------|------|-------------|
| `nThreads` | `number` | `0` (default) auto-picks `min(blocks_per_segment/2, hardware_concurrency)`; `1` forces single-thread; `>1` sets the worker count (capped to the effective block count). |
| `onProgress` | `(pct, tsNs) => boolean \| void` | Called from inside the run loop with the bytes-based pct (0..1) and the cursor timestamp in nanoseconds (a `bigint`). Return `false` to cancel. Only fires on the single-thread path, so it requires `nThreads: 1`. |
| `progressIntervalMs` | `number` | Minimum interval between `onProgress` calls. Defaults to `1000`; `0` is treated as `1000`. |

Parallel mode partitions at the compressed-block level and merges per-worker
panels back into the caller's array. See the
[aggregate-tape-events how-to](../../how-to/aggregate-tape-events.md).

Record shapes:

- **Trade**: `{ exchangeTsNs, recvTsNs, price, qty, tradeId, symbolId, side }`
- **BBO**: `{ exchangeTsNs, recvTsNs, seq, symbolId, eventType, bidPrice, bidQty, askPrice, askQty }`
- **Book update**: `{ exchangeTsNs, recvTsNs, seq, symbolId, eventType, bids: [{price, qty}, ...], asks: [{price, qty}, ...] }`

`eventType` is the record byte: `0` snapshot, `1` delta, `2` top-of-book.

`exchangeTsNs` and `recvTsNs` are `bigint` on every record, from `DataReader`
and `MergedTapeReader` alike. They are clock readings, and a JS `number` is a
double that steps 256 ns at a time at today's values. Comparisons against a
`number` work; arithmetic needs both sides `bigint`, so take a difference
first and convert it: `Number(b.exchangeTsNs - a.exchangeTsNs)`.

`startTsNs`, `fromNs`, `toNs` accept either a `number` or a `bigint`. Pass
`bigint` for real nanosecond timestamps — JS `number` is float64 and silently
rounds values past 2^53 (e.g. `1765615835519000000` becomes
`1765615835519000064`), which can shift seek boundaries off by one event.

---

## BinaryLogRecorderHook

Built-in `.lrvx` recorder. Plug into a `Runner` via
`runner.setMarketDataRecorder(hook)` and the engine drives `start` /
`stop` for you.

```javascript
const hook = new lrvx.BinaryLogRecorderHook(
  outputDir, maxSegmentMb /*=256*/, exchangeId /*=0*/, compression /*"none"|"lz4"*/,
  exchangeName /*=""*/, instrumentType /*=""*/);
hook.addSymbol(symbolId, name, base, quote, pricePrecision, qtyPrecision);

runner.setMarketDataRecorder(hook);
runner.start();
// ... events flow ...
runner.stop();
console.log(hook.stats());
```

| Method | Description |
|--------|-------------|
| `addSymbol(symbolId, name, base, quote, pricePrecision, qtyPrecision)` | Register a symbol in the recording metadata. |
| `flush()` | Flush buffered bytes to disk. |
| `stats()` | Returns `{ tradesWritten, bookUpdatesWritten, bytesWritten, segmentsCreated, errors }` as BigInts. |

`exchangeName` is stamped into the tape manifest as `metadata.exchange` and is
required for `MergedTapeReader` keying — tapes recorded without it refuse to
merge. `instrumentType` is stamped as `metadata.instrument_type` (`"spot"`,
`"perpetual"`, `"futures"`, `"option"`).

---

## Partitioner

Splits a dataset into partitions for parallel backtesting.

```javascript
const partitioner = new lrvx.Partitioner(dataDir);
const partitions = partitioner.byTime(numPartitions, warmupNs);
```

All methods return an array of partition objects `{ partitionId, fromNs, toNs, warmupFromNs, estimatedEvents, estimatedBytes }`. `fromNs`, `toNs` and `warmupFromNs` are `bigint` and go straight back into `new DataReader(dataDir, fromNs, toNs)`. Pass `0` as `warmupNs` if no warmup period is needed.

| Method | Description |
|--------|-------------|
| `byTime(numPartitions, warmupNs)` | Split into N equal time slices |
| `byDuration(durationNs, warmupNs)` | Split by fixed duration |
| `byCalendar(unit, warmupNs)` | Split by calendar unit: `"hour"`, `"day"`, `"week"`, `"month"` |
| `bySymbol(numPartitions)` | Split by symbol group |
| `perSymbol()` | One partition per symbol |
| `byEventCount(numPartitions)` | Split by event count |

## Segment operations that return a flag or a count

`recompress(inputPath, outputPath, codec?)` returns `false`, and
`extractSymbols(inputPath, outputPath, symbols)` and
`extractTimeRange(inputPath, outputPath, fromNs, toNs)` return `0`, when the input cannot be read
or the output cannot be written. `lrvx.lastError()` then returns the reason: a string that opens
with the C function's name and names the file. Each of the three clears it on entry, so `''` means
the call did not fail; an extract that matched nothing returns `0` with `''`.
`FundingSchedule.loadTape(path)` reports a file it could not open the same way.

`validate(path, verifyCrc?, verifyTimestamps?)` checks CRCs and timestamp order unless told not to:
a flag left out (or `undefined`) is on, and `false` turns that check off. Both used to be ignored.

`mergeDir(dir, outputPath)` returns `{ success, segmentsMerged, eventsWritten, bytesWritten,
outputPath, errors }`. `outputPath` is the segment that was written (`""` when none was) and
`errors` has one string for everything that went wrong, `"Cannot open: <path>"` for each input that
was skipped while the others were merged; `success` is `false` whenever `errors` is not empty.
`validateDataset(dir)` clears `lastError()` on entry and leaves the reason there when the directory
is missing or cannot be listed.

`validateSegment(path)`, `validate(path)` and `mergeSegments(inputDir, outputPath)` clear it on
entry as well. `validateSegment` returns `false`, and `validate` returns `valid: false`, both for a
segment that fails its checks and for a file that could not be read; only the unreadable file
leaves a reason, so `''` means the segment was read and is not valid. `mergeSegments` returns
`false` when no segment was merged, and `lastError()` says why.

```javascript
if (!lrvx.recompress('in.lrvx', 'out.lrvx', 'lz4')) {
  console.error(lrvx.lastError());
}
```
