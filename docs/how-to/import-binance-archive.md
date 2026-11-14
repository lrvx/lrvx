# Import Binance public archives into a lrvx tape

`data.binance.vision` publishes one aggregate-trade zip per symbol and UTC day for spot, USDT-margined futures (`um-futures`) and coin-margined futures (`cm-futures`):

```text
https://data.binance.vision/data/spot/daily/aggTrades/<SYMBOL>/<SYMBOL>-aggTrades-<YYYY-MM-DD>.zip
https://data.binance.vision/data/futures/um/daily/aggTrades/<SYMBOL>/<SYMBOL>-aggTrades-<YYYY-MM-DD>.zip
https://data.binance.vision/data/futures/cm/daily/aggTrades/<SYMBOL>/<SYMBOL>-aggTrades-<YYYY-MM-DD>.zip
```

The per-row work is fiddly: two layouts, two timestamp units, side mapping, and dedup on re-runs.

`lrvx.archives.binance` wraps the converter in two function calls, plus a `lrvx archive binance` CLI subcommand. The output is a regular `.lrvx` tape, which means every aggregator, `MergedTapeReader`, the live recorder hook, and the engine itself read it without special-casing.

## Install

```bash
pip install lrvx
```

No extras are required. Downloads use `urllib` from the standard library.

## Convert a single day

The simplest form takes one already-downloaded zip and writes a tape directory. The example below is the same script CI runs on every push: it builds a synthetic Binance-style zip in memory, calls `aggtrades_to_lrvx`, then reads the produced tape back through `DataReader`.

```python
--8<-- "examples/python_binance_archive.py"
```

`csv_path` accepts either the zip published by Binance or the extracted CSV.

## Columns and units

The layout is recognised per file:

- futures files (`um-futures`, `cm-futures`) from mid 2022 on open with the header `agg_trade_id,price,quantity,first_trade_id,last_trade_id,transact_time,is_buyer_maker`; older futures files have the same seven columns and no header;
- spot files have no header and eight columns: the same seven plus `is_best_match`, with `True` / `False` flags.

A file whose first row is neither raises `lrvx.archives.UnknownLayoutError`, and a file with rows of which none parse raises `lrvx.archives.NothingParsedError`; nothing is written in either case. Rows that fail to parse in an otherwise good file are counted in `rows_invalid`.

`transact_time` is Unix milliseconds, except in spot files from 2025-01-01 on, where Binance writes microseconds (as its [public data README](https://github.com/binance/binance-public-data) states). The importer reads a value of 10^14 or more as microseconds, so both land on the tape as nanoseconds.

`quantity` is stored as published: the base asset for spot and um-futures, contracts for cm-futures (`BTCUSD_PERP` 100 USD each, other symbols 10 USD). `qty_multiplier` (`--qty-multiplier`) multiplies every quantity by a constant you supply, for example a contract size; the importer does not know contract sizes. `metadata.json` records the unit and the multiplier under `custom.qty_unit` and `custom.qty_multiplier`.

Side encoding follows the lrvx convention. `is_buyer_maker = true` becomes `Side::SELL` (the buyer was the resting maker, so the active flow is a seller hitting the bid). `is_buyer_maker = false` becomes `Side::BUY`.

## Convert a date range

For multi-day backfills, `range_to_lrvx` takes a range of UTC days. Binance cuts its files at midnight UTC, so `--from 2024-01-15 --to 2024-01-15` is 2024-01-15T00:00Z to 2024-01-16T00:00Z. Missing zips are downloaded from `data.binance.vision` in parallel into the shared archive cache and reused on follow-up calls (see [the cache](import-multi-exchange-archives.md#shared-download-cache)). The function signature:

```text
binance.range_to_lrvx(
    symbol: str,
    market: str,                # "spot" | "um-futures" | "cm-futures"
    date_from: str | date,      # YYYY-MM-DD, inclusive
    date_to:   str | date,      # YYYY-MM-DD, inclusive
    out_tape:  str | Path,
    *,
    mirror:    str | Path | None = None,   # default ~/.lrvx/archive-cache/binance
    parallel:  int = 4,              # downloads in flight
    symbol_id: int = 1,
    skip_missing: bool = False,
    workers:   int | None = None,    # processes parsing files; None = one per core
    ...
) -> ConvertStats
```

Downloads run `parallel` at a time and the files are parsed in `workers` processes; the tape is written in date order by the calling process, so it is the same for any worker count. See [Conversion in worker processes](import-multi-exchange-archives.md#conversion-in-worker-processes).

The symbol is checked against the form the archive uses for the market before anything is downloaded: `BTCUSDT` for spot, `BTCUSDT` or `BTCUSDT_240329` for um-futures, and for cm-futures the contract name, `BTCUSD_PERP` or `BTCUSD_240329`. A symbol of another shape (`BTCUSDT` on cm-futures, `btcusdt`, `BTC-USDT`) fails at once with an example of the expected form.

A day Binance has not published answers 404; it is asked once, not retried. Without `skip_missing` such a day stops the range with an error naming its URL; with `skip_missing=True` (`--skip-missing`) the range steps over it, which is common at the head of the archive (a day's file usually appears the following day). A range in which no day is published fails either way, with a message naming the symbol, market and dates and the first URL tried. Timeouts, 5xx answers and incomplete downloads are retried; see [Downloads](import-multi-exchange-archives.md#downloads).

## CLI

`lrvx archive binance` is the same surface from the shell. For a date range:

```bash
lrvx archive binance \
  --symbol BTCUSDT \
  --market um-futures \
  --from 2024-01-01 --to 2024-01-31 \
  --out ./tapes/binance-um-BTCUSDT \
  --parallel 4
```

The summary line ends with the first and last trade written, in UTC:

```text
lrvx archive binance: trades_written=... rows_read=... rows_skipped=0 rows_invalid=0 files=31 first_trade=2024-01-01T00:00:00.112Z last_trade=2024-01-31T23:59:59.870Z
```

`first_trade=- last_trade=-` means nothing new was written (every trade was on the tape already).

For one-off conversions of a file already on disk, pass `--csv` instead of the date range:

```bash
lrvx archive binance \
  --csv ./BTCUSDT-aggTrades-2024-01-15.zip \
  --out ./tapes/binance-um-BTCUSDT \
  --symbol BTCUSDT --market um-futures --symbol-id 1
```

## Append-safe and idempotent

The converter dedups on `agg_trade_id` against the trades already in the target tape for the same symbol within the incoming file's time range. Re-running the same day, or running an overlapping range, adds nothing: every previously-imported row is skipped, and the writer adds zero new records. The reported `rows_skipped` counter shows how many rows were elided; this empty result is not an error.

## Real-archive tests

`python/tests/test_binance_archive.py` builds its fixtures from the real headers and row shapes. Its `BinanceRealArchiveTests` download one day per market (including a spot day in microseconds) and compare decoded trades with the file; they run only with `LRVX_ARCHIVE_NETWORK_TESTS=1`.

## metadata.json

The tape's `metadata.json` is created (or merged into) on every successful conversion. `MergedTapeReader` keys symbols by `(metadata.exchange, name)`, so the Binance archive tapes line up against tapes captured live by the recorder hook. The symbol IDs picked here do not need to match what the live recorder picked; the reader rekeys both into a global ID space at read time.

## What is and is not in the archive

aggTrades only carries print events. There is no book information. The bookTicker archive (best bid / ask, um-futures and cm-futures, days from 2023-05 to 2024) has its own converters in `lrvx.archives.binance`:

| Function | Archive |
|---|---|
| `t1_to_lrvx(csv_path, out_tape, ...)` | `T1` / `bookTicker` — top of book |
| `range_book_to_lrvx(symbol, market, book_type, date_from, date_to, out_tape, ...)` | multi-day bookTicker range, downloads and converts in one call |

They write into the same tape directory, so a single lrvx can carry both trades and book updates. See [Import Binance book archives](import-binance-book-archive.md).

Other public archives ship as sibling subcommands under `lrvx archive`: `bybit`, `okx` and `bitget`. See [Import multi-exchange archives](import-multi-exchange-archives.md).
