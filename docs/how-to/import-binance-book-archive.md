# Import Binance public book archives into a lrvx tape

The aggTrades importer covered in [Import Binance public archives](import-binance-archive.md) handles the trade stream. The book importer reads one product from `data.binance.vision`:

- **bookTicker** (book type `t1`): the best bid and ask after every book update, one row per update. Published for **um-futures** (USDT-margined) and **cm-futures** (coin-margined).

The files sit at

```text
https://data.binance.vision/data/futures/um/daily/bookTicker/<SYMBOL>/<SYMBOL>-bookTicker-<YYYY-MM-DD>.zip
https://data.binance.vision/data/futures/cm/daily/bookTicker/<SYMBOL>/<SYMBOL>-bookTicker-<YYYY-MM-DD>.zip
```

one per UTC day, with the header

```text
update_id,best_bid_price,best_bid_qty,best_ask_price,best_ask_qty,transaction_time,event_time
```

`transaction_time` (Unix milliseconds) is the record's timestamp on the tape; prices and quantities are as published (cm-futures quantities in contracts). A file whose first row is not that header raises `lrvx.archives.UnknownLayoutError`, and one with rows of which none parse raises `lrvx.archives.NothingParsedError`.

Binance publishes bookTicker only for a bounded date range per symbol, and the symbols checked have no day after 2024: `BTCUSDT` um-futures runs from 2023-05-16 to 2024-03-30, `BTCUSD_PERP` cm-futures from 2023-05-18 to 2024-10-14. The bucket listing shows a symbol's days, for example `https://s3-ap-northeast-1.amazonaws.com/data.binance.vision?delimiter=/&prefix=data/futures/um/daily/bookTicker/BTCUSDT/`. A day outside the range does not download (pass `--skip-missing` to step over such days in a range).

The spot archive has no bookTicker product, so a book import for `spot` fails with an error naming the supported markets. bookTicker is the only book product imported: any other book type fails with an error as well.

The importer lives alongside `aggtrades_to_lrvx` in `lrvx.archives.binance` and emits book events into the same `.lrvx` tape directory, so a single tape can carry trades + book updates the engine and `MergedTapeReader` read interleaved.

## Functions

```text
binance.t1_to_lrvx(csv_path, out_tape, *, symbol_id=1, symbol_name="",
                      market="um-futures", append=True, ...)
                      -> BookConvertStats

binance.range_book_to_lrvx(
    symbol, market, book_type, date_from, date_to, out_tape, *,
    mirror=None, parallel=2, ...,
) -> BookConvertStats
```

`book_type` is `"t1"`; `market` is `"um-futures"` or `"cm-futures"` (for coin-margined contracts the symbol is the contract name, e.g. `BTCUSD_PERP`). Anything else raises `ValueError` before any download. The range form downloads missing zips from `data.binance.vision`, caches them under `mirror` when set, and runs the converter on the local files serially so the writer stays append-safe.

## Example

The script below is the same one CI runs on every push. It builds a synthetic bookTicker zip, converts it to a `.lrvx`, and reads the book events back through `DataReader.read_book_updates`:

```python
--8<-- "examples/python_binance_book_archive.py"
```

## Delta encoding

bookTicker rows are best-bid / best-ask snapshots, one per update. The first row of each imported file becomes a book snapshot; every subsequent row emits a delta with the changed top level (qty=0 marks a removed price). Unchanged ticks are skipped so the tape does not accumulate dead data.

The converter keeps the running top-of-book state in memory while writing, so the engine's existing book replay (snapshot replaces book, delta applies on top) gives the same `(bids, asks)` sequence the original CSV described.

Each file is encoded on its own: the ladder starts empty, the file's first record is a snapshot, and its deltas are taken against the record before them in the same file, never against what the tape already holds. A multi-day tape therefore carries one snapshot at the start of every imported day, and a replay that starts at a day boundary needs nothing from the days before it.

## CLI

`lrvx archive binance --book t1` imports bookTicker on either form; `--book` takes no other value:

```bash
# Single day from a local CSV
lrvx archive binance --book t1 \
  --csv ./BTCUSDT-bookTicker-2024-01-15.zip \
  --out ./tapes/binance-um-BTCUSDT \
  --symbol BTCUSDT --market um-futures

# Multi-day range with download
lrvx archive binance --book t1 \
  --symbol BTCUSDT --market um-futures \
  --from 2024-01-01 --to 2024-01-07 \
  --out ./tapes/binance-um-BTCUSDT \
  --mirror ./.cache/binance \
  --parallel 2
```

`--market spot` with `--book` exits with status 2 and an error naming the supported markets, as does any `--book` value other than `t1`. An unrecognised header or a file with no parsable row exits with status 1 and a one-line error.

## Append-safe and idempotent

A record is left out when a book record with the same `update_id` is already on the tape for the same symbol within the incoming file's time range. Binance's update ids are unique per symbol and a record keeps its exchange timestamp, so only that range can hold a copy, and the dedup read covers that range instead of the whole tape: an import costs about one day of tape, whatever the tape's length.

Because a file's records never depend on the tape, the outcome does not depend on the order days are imported in. Re-running a day adds nothing, an overlapping range keeps each record once, a day earlier than the ones already on the tape or one that fills a gap lands in full, and a day cut short by an interrupted import is completed by importing it again. In each case the tape holds the same book records a single in-order import of the same days writes, so a replay ordered by timestamp (`orderbook.book_at`, `OrderBookIterator`) reconstructs the same ladders; and because every day opens with a snapshot, so does a consumer that streams segments in file-name order, where a day imported later sits after the days imported before it. The reported `rows_skipped` counter counts rows not written: unchanged ticks, malformed rows and rows already on the tape; `rows_invalid` counts the malformed ones alone. `--no-append` turns the check off.

`python/tests/test_binance_book_archive.py` also downloads one real um-futures day and checks the replayed top of book against the file's rows when `LRVX_ARCHIVE_NETWORK_TESTS=1` is set.

## Co-existence with aggTrades

Trades and book events sit in one `.lrvx` directory side by side. The engine and `MergedTapeReader` interleave them on `exchange_ts_ns` at read time, so the same tape feeds both trade aggregators and book-level analyses (depth-aware execution, vacuum / queue analysis, top-of-book microstructure) in a single pass.

`metadata.json` carries the union counters: `total_trades`, `total_book_updates`, `has_trades`, `has_book_snapshots`, `has_book_deltas`, and the maximum observed `book_depth`. The MergedTapeReader keys by `(metadata.exchange, name)` exactly as for the trade-only case.

## See also

- [Import Binance public archives](import-binance-archive.md) for the aggTrades counterpart.
- [Merge multiple tapes on read](multi-tape.md) for combining the imported book + trade tape with tapes from other exchanges.
