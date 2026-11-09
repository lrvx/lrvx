# Order books, positions, profiles, statistics

---

## Order book

```javascript
const book = new OrderBook(0.01);  // tick size
book.applySnapshot([50000, 49999], [1.5, 2.0], [50001, 50002], [0.5, 1.0]);
book.bestBid();    // 50000
book.bestAsk();    // 50001
book.mid();
book.spread();
book.getBids(5);   // [[price, qty], ...]
book.getAsks(5);

// L3 (order-level)
const l3 = new L3Book();
l3.addOrder(1, 50000, 1.5, 'buy');
l3.removeOrder(1);
l3.bestBid();
```

The tick size defaults to 0.01 when the argument is omitted. An explicit value
must be positive; `new OrderBook(0)` throws a `RangeError`. The smallest
representable tick is `1e-8`, since prices are fixed-point with a scale of 1e8.
The book covers 8192 consecutive ticks centred on the market and re-anchors
that window as a delta feed walks the price away from the last snapshot. See
[The order book's tick window](../../explanation/order-book-tick-window.md).

---

## Composite book

Aggregates books across multiple exchanges per symbol.

```javascript
const matrix = new CompositeBook();
matrix.applySnapshot(exchangeId, symbol, bidPrices, bidQtys, askPrices, askQtys, recvNs);
matrix.applyDelta(exchangeId, symbol, bidPrices, bidQtys, askPrices, askQtys, recvNs);
matrix.bestBid(symbol);      // { price, qty } | null
matrix.bestAsk(symbol);      // { price, qty } | null
matrix.hasArbitrage(symbol);
matrix.markStale(exchangeId, symbol);
matrix.checkStaleness(nowNs, thresholdNs);
```

`applySnapshot` replaces both sides of that exchange's top-of-book wholesale,
including clearing a side that arrives as an empty array. `applyDelta` only
touches the side(s) actually present in the call -- a side passed as an empty
array is left exactly as it was, not zeroed. `recvNs` feeds `checkStaleness`'s
staleness clock; pass the actual receive timestamp if you use staleness
eviction.

---

## Position tracking

```javascript
const tracker = new PositionTracker();
tracker.onFill(symbolId, 'buy', 50000, 1.0);
tracker.onFill(symbolId, 'sell', 50100, 1.0);
tracker.position(symbolId);        // 0
tracker.realizedPnl(symbolId);     // 100
tracker.totalRealizedPnl();

// Group tracking
const groups = new PositionGroupTracker();
const pid = groups.openPosition(symbolId, groupId, 'buy', 50000, 1.0);
groups.closePosition(pid, 50500);
groups.totalRealizedPnl();
```

---

## Profiles

```javascript
// Volume profile
const vp = new VolumeProfile(0.01);
vp.addTrade(50000, 1.0, true);
vp.poc();
vp.valueAreaHigh();
vp.valueAreaLow();

// Market profile
const mp = new MarketProfile(0.01, 30, 0);
mp.addTrade(Date.now() * 1e6, 50000, 1.0, true);
mp.poc();
mp.initialBalanceHigh();
mp.isPoorHigh();

// Footprint
const fp = new FootprintBar(0.01);
fp.addTrade(50000, 1.0, true);
fp.totalDelta();
fp.totalVolume();
```

---

## Statistics

```javascript
lrvx.correlation([1, 2, 3], [1, 2, 3]);
lrvx.profitFactor([100, -50, 200, -30]);
lrvx.winRate([100, -50, 200, -30]);
lrvx.bootstrapCI([1, 2, 3, 4, 5], 0.95, 10000);  // { lower, median, upper }
lrvx.permutationTest([1, 2, 3], [4, 5, 6], 10000); // p-value
```

---

## Segment operations

Segment directories use the `.lrvx` extension.

```javascript
lrvx.validateSegment('/path/to/segment.lrvx');

// Second argument is an OUTPUT FILE PATH, not a directory.
lrvx.mergeSegments('/path/to/input_dir', '/path/to/merged.lrvx');
```

Extended operations:

| Call | Description |
|------|-------------|
| `lrvx.mergeDir(inputDir, outputDir)` | Merge a directory into an output directory |
| `lrvx.splitSegment(inputPath, outputDir, mode, timeIntervalNs, eventsPerFile)` | Split one segment into many |
| `lrvx.exportSegment(inputPath, outputPath, format, fromNs, toNs, symbols)` | Export a range to another format |
| `lrvx.validateSegmentFull(path, verifyCrc, verifyTimestamps)` | Full validation, optional CRC and timestamp checks |
| `lrvx.validateDataset(dataDir)` | Validate every segment in a dataset directory |
| `lrvx.recompressSegment(inputPath, outputPath, level)` | Recompress at a different level |
| `lrvx.extractSymbols(inputPath, outputDir, symbols)` | Extract selected symbols |
| `lrvx.extractTimeRange(inputPath, outputPath, fromNs, toNs)` | Extract a time window |

---

## Bar aggregation

Each helper takes `(timestamps, prices, quantities, sides, param)`. `lrvx.renkoBars` can return
more bars than there were input trades: a trade that gaps past more than one brick width closes
the brick that was forming and synthesizes the bricks in between (see [bar types](../../explanation/bar-types.md#renko-bars)).
Every other helper here closes at most one bar per input trade.

| Call | `param` |
|------|---------|
| `lrvx.timeBars(ts, px, qty, sides, intervalNs)` | Interval in nanoseconds — `60000000000` is one minute |
| `lrvx.tickBars(ts, px, qty, sides, ticksPerBar)` | Trades per bar |
| `lrvx.volumeBars(ts, px, qty, sides, volumePerBar)` | Volume per bar |
| `lrvx.rangeBars(ts, px, qty, sides, rangeSize)` | High-low range |
| `lrvx.renkoBars(ts, px, qty, sides, brickSize)` | Brick size |
| `lrvx.heikinBars(ts, px, qty, sides, intervalNs)` | Interval in nanoseconds |

A nanosecond interval may be a `Number` or a `BigInt`. Each bar comes back
with `ts` (the bar start, a nanosecond `BigInt`), `open`, `high`, `low`,
`close`, `volume`, `buyVolume` and `trades`. Only closed bars are returned:
a bar still forming when the tape ends is not one of them.

---

## Data reader / writer / recorder

Read trades and book updates from binary log segments, or record live data.

```javascript
const reader = new DataReader('./data');
// Filtered reader:
//   new DataReader({ dir: './data', fromNs, toNs, symbols })

reader.count;            // total events
reader.summary();        // { firstEventNs, lastEventNs, totalEvents, segmentCount, totalBytes, durationSeconds }
reader.stats();          // { filesRead, eventsRead, tradesRead, bookUpdatesRead, bytesRead, crcErrors }

const trades = reader.readTrades(maxTrades = 0);   // 0 = all
const bbos = reader.readBBO(maxEvents = 0);
const events = reader.readBookUpdates();

// Mid-stream seek: start from a given timestamp
const tradesFrom = reader.readTradesFrom(startTsNs, maxTrades = 0);
const bbosFrom = reader.readBBOFrom(startTsNs, maxEvents = 0);
const eventsFrom = reader.readBookUpdatesFrom(startTsNs);

reader.destroy();
```

Record shapes:

- **Trade**: `{ exchangeTsNs, recvTsNs, price, qty, tradeId, symbolId, side }`
- **BBO**: `{ exchangeTsNs, recvTsNs, seq, symbolId, eventType, bidPrice, bidQty, askPrice, askQty }`
- **Book update**: `{ exchangeTsNs, recvTsNs, seq, symbolId, eventType, bids: [{price, qty}, ...], asks: [{price, qty}, ...] }`

`eventType` is the record byte: `0` snapshot, `1` delta, `2` top-of-book.

```javascript
const writer = new DataWriter('./out', maxSegmentMb, exchangeId);
writer.writeTrade(exchangeTsNs, recvTsNs, price, qty, tradeId, symbolId, side);
// Raw int64 book levels — flat BigInt64Array as [price_raw, qty_raw, ...]
writer.writeBook(exchangeTsNs, recvTsNs, seqNs, symbolId, isSnapshot, bidsBuf, asksBuf);
// updateType, optional, is the record byte above (0, 1, 2) and decides when given
writer.writeBook(exchangeTsNs, recvTsNs, seqNs, symbolId, false, bidsBuf, asksBuf, 2);
writer.flush();
writer.close();
writer.stats();          // { bytesWritten, eventsWritten, segmentsCreated, tradesWritten }

const hook = new BinaryLogRecorderHook('./out', maxSegmentMb, exchangeId, 'none');
hook.addSymbol(symbolId, name, base, quote, pricePrecision, qtyPrecision);
// attach via the runner's market-data-recorder slot; lifecycle is engine-driven.
hook.destroy();
```
