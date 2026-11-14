# Import multi-exchange public archives

`lrvx.archives` collects the per-exchange importers under one namespace. Each submodule exposes the same two entry points and the matching CLI subcommand:

| Exchange | Module | Trade entry point | CLI |
|---|---|---|---|
| Binance | `lrvx.archives.binance` | `aggtrades_to_lrvx` | `lrvx archive binance ...` |
| Bybit   | `lrvx.archives.bybit`   | `trades_to_lrvx`    | `lrvx archive bybit ...`   |
| OKX     | `lrvx.archives.okx`     | `trades_to_lrvx`    | `lrvx archive okx ...`     |
| Bitget  | `lrvx.archives.bitget`  | `trades_to_lrvx`    | `lrvx archive bitget ...`  |

Each importer reads the files the exchange publishes today, at the URLs below. Deribit is not in the list: it publishes no public trade file archive.

Adding a new venue is a self-contained module under `lrvx/archives/` that implements `trades_to_lrvx` + `range_to_lrvx` matching the `ArchiveReader` Protocol.

## What every importer checks

The CSV layout is recognised from the file's header row (or, for Binance files without one, from the column count). A file whose first row matches no layout the exchange publishes raises `lrvx.archives.UnknownLayoutError`, naming the row it found and the layouts it expected; nothing is written. A file with data rows of which none parse raises `lrvx.archives.NothingParsedError`. Rows that fail to parse in a file where others parse are counted in `rows_invalid`. An import that writes nothing because every trade is on the tape already is not an error: it returns `trades_written=0` with the trades counted in `rows_skipped`. The CLI reports either error as one line on stderr and exits with status 1.

## Days and time zones

`--from` and `--to` (`date_from` / `date_to`) name the exchange's own daily files, both included, and the exchanges do not cut their days at the same hour:

| Exchange | A day is | `--from 2024-01-15 --to 2024-01-15` covers |
|---|---|---|
| Binance | a UTC day | 2024-01-15T00:00Z to 2024-01-16T00:00Z |
| Bybit | a UTC day | 2024-01-15T00:00Z to 2024-01-16T00:00Z |
| OKX | a UTC+8 day | 2024-01-14T16:00Z to 2024-01-15T16:00Z |
| Bitget | a UTC+8 day | 2024-01-14T16:00Z to 2024-01-15T16:00Z |

Asking OKX or Bitget for one UTC day therefore yields a day shifted eight hours earlier; to cover the UTC day 2024-01-15 in full, import 2024-01-15 and 2024-01-16 from them. The tape stores every trade at its exchange timestamp, so the shift is in which trades land, not in their times. The CLI summary line names the first and last trade actually written, in UTC, and for an OKX or Bitget range also the UTC span the requested days cover:

```text
lrvx archive okx: trades_written=... rows_read=... rows_skipped=0 rows_invalid=0 rows_other_instruments=0 files=1 first_trade=2024-01-14T16:00:00.071Z last_trade=2024-01-15T15:59:59.934Z (OKX days are UTC+8 days: 2024-01-15 to 2024-01-15 covers 2024-01-14T16:00:00Z to 2024-01-15T16:00:00Z)
```

The same two timestamps are in the returned stats as `first_ts_ns` and `last_ts_ns` (nanoseconds, 0 when nothing was written). Each module states its boundary as `DAY_UTC_OFFSET_HOURS` (0 or 8).

## Symbols

Each range form checks the symbol against the form its exchange and market use before making any request, and fails with an example of the expected form:

| Exchange | Market | Symbol |
|---|---|---|
| Binance | spot | `BTCUSDT` |
| Binance | um-futures | `BTCUSDT`, `BTCUSDT_240329` |
| Binance | cm-futures | `BTCUSD_PERP`, `BTCUSD_240329` |
| Bybit | spot | `BTCUSDT` |
| Bybit | linear | `BTCUSDT`, `BTCPERP`, `BTC-27DEC24` |
| Bybit | inverse | `BTCUSD`, `BTCUSDZ24` |
| OKX | spot / swap | `BTC-USDT` / `BTC-USDT-SWAP` |
| OKX | futures / option | `BTC-USD-240329` / `BTC-USD-240329-50000-C` |
| Bitget | spot, umcbl / cmcbl / dmcbl | `BTCUSDT` / `BTCPERP` / `BTCUSD` (no `_UMCBL`-style suffix) |

The check is on the shape only; whether the exchange lists the symbol shows when its files are looked up.

## Downloads

The range forms share one download policy:

- A file the archive does not publish (HTTP 404 or 410, or the 403 a CDN answers for a missing object) is asked for once and not retried.
- A timeout, a dropped connection, a 5xx answer, a body shorter than its `Content-Length`, and a body that does not open as the zip or gzip it should be are retried, three attempts in all with a growing pause; if all fail the import stops with the URL and the last error.
- Without `--skip-missing`, a missing day stops the range with an error naming its URL. With `--skip-missing` the range steps over days the archive does not publish; a download that keeps failing for another reason still stops it.
- A range in which no day is published fails, with or without `--skip-missing`, naming the symbol, market and dates and the first URL tried. The CLI exits with status 1.

## Conversion in worker processes

Parsing the CSVs is the slow part of an import, so the range forms parse the files of a range in worker processes: `workers` (`--workers N`) of them, by default one per CPU core and at most one per file (a Bitget part counts as one file). `--workers 1` parses in the importing process. `parallel` (`--parallel`) is the number of downloads in flight and does not change how many processes convert.

The tape is written by the importing process alone, a file at a time in date order, with the dedup described below, so a range imported with any worker count gives the same tape: the same records in the same order, one segment per file, the same `metadata.json` and the same counters. A worker parses a file into a temporary file in the system temp directory (about 33 bytes per trade) and the workers run at most two files per worker ahead of the writer, so the temporary space stays at a few days' worth however long the range.

An import that fails leaves the tape as it found it, whatever the worker count: the error is the one the file raised (`UnknownLayoutError`, `NothingParsedError`, `CorruptArchiveError`, ...) and names the file, the segments written for earlier files of the range are removed and `metadata.json` keeps its old content. A cached file that does not open is removed from the cache, so the next run downloads it again.

The workers are started with the `spawn` method, on every platform. A script that calls a range importer from its top level needs the usual `if __name__ == "__main__":` guard; without it, pass `workers=1`.

```bash
lrvx archive bybit --symbol BTCUSDT --market linear \
  --from 2026-08-01 --to 2026-10-31 --out ./tapes/bybit-linear-BTCUSDT \
  --workers 8
```

On a 14-core Mac, the 92 Bybit BTCUSDT linear days from 2026-08-01 to 2026-10-31 (146 million trades, from a local mirror) import in 40 s with the default worker count and in 5 min 9 s with `--workers 1`, into the same tape.

## Append and dedup

All four trade importers append by default and dedup the same way: a row is skipped when its trade id is already on the tape for the same symbol within the incoming file's time range. A trade keeps its exchange timestamp, so only that range can hold a copy of it, and the dedup read covers that range instead of the whole tape. Re-importing a day adds nothing, a new day lands in full, an overlapping range keeps each trade once, and days may be imported in any order (a later day first, or a day that fills a gap). `--no-append` turns the check off.

Each import writes new segments named by the time of the import, so a tape filled out of order lists its segments out of time order. Readers and `run_tape` order segments by their first event, so the tape still replays forward in time.

Within a file, the Bybit, OKX and Bitget importers write trades in timestamp order (rows with the same timestamp keep their file order), whatever order the file lists them in; the Binance importer writes them in file order. Memory does not grow with the size of the day: a file is parsed in batches of 16,384 rows into a temporary file (about 33 bytes per row, in the system temp directory, removed when the import ends), files not in time order are merged back from sorted runs, and the dedup read pulls the tape's ids for the file's time range in short time windows. Only the ids already on the tape for that range stay in memory for the whole import, 8 bytes per trade. A two-million-trade Bybit day imports in about 60 MB of resident memory, and re-importing it takes about 90 MB.

The Binance bookTicker importer (`--book t1`) follows the same rule on the book record's update id, and encodes every file on its own, starting from a snapshot, so book days may be imported in any order too. See [Import Binance book archives](import-binance-book-archive.md#append-safe-and-idempotent).

## Quantity units

The tape stores each trade's quantity in the unit the archive publishes it in, which is also the unit the live path reports for that venue (the native Bybit and Bitget connectors pass the venue's size through, and the ccxt bridge reports contract markets in contracts). `metadata.json` records it under `custom.qty_unit`:

| Exchange | Market | Quantity |
|---|---|---|
| Binance | spot, um-futures | base asset |
| Binance | cm-futures | contracts (`BTCUSD_PERP` 100 USD each, other symbols 10 USD) |
| Bybit | spot, linear | base coin |
| Bybit | inverse | USD contracts (1 USD each) |
| OKX | spot | base currency |
| OKX | swap, futures, option | contracts (`BTC-USDT-SWAP` 0.01 BTC, `BTC-USD-SWAP` and BTC-USD futures 100 USD) |
| Bitget | every market | base coin |

The importers do not know contract sizes. Binance aggTrades and OKX take `qty_multiplier` (`--qty-multiplier` on the CLI), a constant every quantity is multiplied by, recorded under `custom.qty_multiplier`; pass the instrument's contract size from the exchange's instrument metadata (OKX `ctVal` from `/api/v5/public/instruments`) to store a linear contract in the base coin. For an inverse contract the multiplier gives the USD notional, not coins.

## Network tests

The tests under `python/tests/` build their CSV fixtures from the real headers and row shapes. Each importer also has tests that download one small real day per market and compare decoded trades with the downloaded file; they are skipped unless `LRVX_ARCHIVE_NETWORK_TESTS=1` is set:

```bash
LRVX_ARCHIVE_NETWORK_TESTS=1 PYTHONPATH=build/python python3 -m pytest \
  python/tests/test_binance_archive.py python/tests/test_binance_book_archive.py \
  python/tests/test_bybit_archive.py python/tests/test_okx_archive.py \
  python/tests/test_bitget_archive.py -k Real
```

## Bybit

Bybit publishes one gzipped CSV per symbol and UTC day at `https://public.bybit.com/`, which serves a directory listing of every symbol and file.

### URL layout

- spot: `public.bybit.com/spot/<SYMBOL>/<SYMBOL>_<YYYY-MM-DD>.csv.gz` (BTCUSDT from 2022-11-10)
- linear: `public.bybit.com/trading/<SYMBOL>/<SYMBOL><YYYY-MM-DD>.csv.gz` (BTCUSDT from 2020-03-25)
- inverse: `public.bybit.com/trading/<SYMBOL>/<SYMBOL><YYYY-MM-DD>.csv.gz`

Linear and inverse differ only in symbol naming (`BTCUSDT`, `BTCUSD`). The converter accepts the symbol verbatim; cross-exchange symbol normalisation is a connectors concern, not an importer one. Each tape keys its own `(metadata.exchange, name)` so `MergedTapeReader` treats `binance:BTCUSDT` and `bybit:BTCUSDT` as distinct global symbols.

### Columns

Linear and inverse files:

```text
timestamp,symbol,side,size,price,tickDirection,trdMatchID,grossValue,homeNotional,foreignNotional
```

Recent files add a trailing `RPI` column. `timestamp` is Unix seconds with a decimal fraction. `side` is the taker side, `Buy` / `Sell`, mapped to lrvx's `Side::BUY` / `Side::SELL`. `trdMatchID` is a UUID string that the importer folds into the 64-bit lrvx `trade_id` (the first 8 bytes of its MD5 digest, read big-endian); the folded ids carry no order, which the membership dedup above does not need. `size` is in the base coin for linear and in USD contracts for inverse. Older files list trades newest first.

Spot files:

```text
id,timestamp,price,volume,side
```

Recent files add a trailing `rpi` column. `timestamp` is Unix milliseconds, `side` is `buy` / `sell`, `volume` is in the base coin, and `id` counts the day's trades from 1; it is the lrvx `trade_id`.

### Example

The script below builds a Bybit-style gzipped CSV in memory, runs `trades_to_lrvx`, then reads the produced tape back through `DataReader.read_trades`:

```python
--8<-- "examples/python_bybit_archive.py"
```

### Range form

For multi-day backfills, `range_to_lrvx` downloads missing files from the public archive and reuses anything already in the local mirror cache:

```text
lrvx.archives.bybit.range_to_lrvx(
    symbol="BTCUSDT",
    market="linear",
    date_from="2024-01-01",
    date_to="2024-12-31",
    out_tape="/path/lrvx/BTCUSDT_bybit",
    mirror=None,        # default = ~/.lrvx/archive-cache/bybit
    parallel=4,         # downloads in flight
    skip_missing=False,
    workers=None,       # processes parsing files; None = one per core
)
```

CLI form:

```bash
lrvx archive bybit \
  --symbol BTCUSDT --market linear \
  --from 2024-01-01 --to 2024-12-31 \
  --out ./tapes/bybit-linear-BTCUSDT \
  --parallel 4
```

## OKX

OKX publishes daily trade files on its CDN and lists them through the public endpoint `GET https://www.okx.com/api/v5/public/market-data-history` (module `1`, tick-by-tick trades). A day is a UTC+8 calendar day: the file for 2024-01-15 holds trades from 2024-01-14T16:00Z to 2024-01-15T16:00Z. Files usually appear two days after the day ends.

### URL layout

All markets share one prefix, `https://static.okx.com/cdn/okex/traderecords/trades/daily/<YYYYMMDD>/`:

- spot:    `<INSTRUMENT>-trades-<YYYY-MM-DD>.zip` (`BTC-USDT`)
- swap:    `<INSTRUMENT>-trades-<YYYY-MM-DD>.zip` (`BTC-USDT-SWAP`)
- futures: `<FAMILY>-futureschain-trades-<YYYY-MM-DD>.zip`, every expiry of the family in one file (`BTC-USD` for `BTC-USD-240329`)
- option:  `<FAMILY>-optionchain-trades-<YYYY-MM-DD>.zip`, every option of the family in one file (`BTC-USD` for `BTC-USD-240329-50000-C`)

For BTC, spot and futures files start in September 2021, swap in October 2021 and options in 2023.

### Columns

The layout is read from the header. OKX publishes:

```text
instrument_name,trade_id,side,price,size,created_time
instrument_name,trade_id,side,price,size,created_time,source
trade_id,side,size,price,created_time
```

The last is a legacy per-instrument layout whose header cells carry a Chinese translation after a `/` (`trade_id/...`); OKX states these headers are being replaced. `created_time` is Unix milliseconds, `side` is the taker side (`buy` / `sell`, upper case in some older files), and `trade_id` is an integer that increases per instrument; it is the lrvx `trade_id`.

One tape holds one instrument. A futures or option import reads the family's chain file and keeps the rows of the requested instrument; the rest are counted in `rows_other_instruments`. A chain file holding no trade of the instrument raises `InstrumentNotInFileError` on a single-file import; in a range, such a day adds nothing, and the range raises only when no day holds a trade of it. A single-file import of a chain file needs `symbol_name` (`--symbol`).

### Example

The script below builds an OKX-style zipped CSV in memory and round-trips it through the converter:

```python
--8<-- "examples/python_okx_archive.py"
```

### CLI

```bash
# Single day from a local zip
lrvx archive okx \
  --csv ./BTC-USDT-SWAP-trades-2024-01-15.zip \
  --out ./tapes/okx-swap-BTC-USDT-SWAP \
  --symbol BTC-USDT-SWAP --market swap

# Multi-day range with download, contracts stored in BTC
lrvx archive okx \
  --symbol BTC-USDT-SWAP --market swap \
  --from 2024-01-01 --to 2024-12-31 \
  --out ./tapes/okx-swap-BTC-USDT-SWAP \
  --qty-multiplier 0.01 --parallel 4

# One dated future, from the BTC-USD futures chain files
lrvx archive okx \
  --symbol BTC-USD-240329 --market futures \
  --from 2024-01-01 --to 2024-03-29 \
  --out ./tapes/okx-futures-BTC-USD-240329
```

## Bitget

Bitget publishes daily trade files through its History data download page (`https://www.bitget.com/data-download`, "Transaction history", daily). The files sit on its CDN at

```text
https://img.bitgetimg.com/online/trades/<CODE>/<SYMBOL>/<PART>.zip
```

with `<CODE>` by market:

| `--market` | Code | Contracts | Symbol |
|---|---|---|---|
| `spot`  | `SPBL`  | spot | `BTCUSDT` |
| `umcbl` | `UMCBL` | USDT-margined perpetuals | `BTCUSDT` |
| `cmcbl` | `CMCBL` | USDC-margined perpetuals | `BTCPERP` |
| `dmcbl` | `DMCBL` | coin-margined perpetuals | `BTCUSD` |

A day is split into numbered parts of at most 100,000 trades. The part name has changed over time, and a day uses exactly one of:

```text
<SYMBOL>_<CODE>_<YYYYMMDD>_<NNN>     older days (2021 to 2024 in the files checked)
<YYYYMMDD>_<NNN>                     later days (end of 2024 to mid 2026)
<SYMBOL>_<YYYYMMDD>_<NNN>            recent days (November 2026)
```

The range form tries them in that order and downloads parts `_001`, `_002`, ... until the next one is missing (the CDN answers 403 for a missing file); the part list of a finished day is kept in the mirror, so a later run does not probe again. These names are what the download page hands out, not a documented API; a renaming shows up as a day the importer cannot find. A day is a UTC+8 calendar day, as the download page states. USDT-margined BTCUSDT files start in 2021; coverage of other symbols and markets varies, so long ranges usually want `--skip-missing`.

### Columns

```text
trade_id,timestamp,price,side,volume(quote),size(base)
```

`timestamp` is Unix milliseconds (recent files carry whole seconds). `side` is the taker side, `buy` / `sell`. `trade_id` is a 19-digit integer and the lrvx `trade_id`. `size(base)` is the quantity in the base coin for every market, coin-margined included, and is what the tape stores; `volume(quote)` is the quote notional and is not stored.

### Example

```python
--8<-- "examples/python_bitget_archive.py"
```

### CLI

```bash
# One part from a local file
lrvx archive bitget \
  --csv ./BTCUSDT_UMCBL_20240115_001.zip \
  --out ./tapes/bitget-umcbl-BTCUSDT \
  --symbol BTCUSDT --market umcbl

# Multi-day range with download, every part of each day
lrvx archive bitget \
  --symbol BTCUSDT --market umcbl \
  --from 2024-01-01 --to 2024-12-31 \
  --out ./tapes/bitget-umcbl-BTCUSDT \
  --parallel 4
```

## Shared download cache

Every range form, for every exchange and product (Binance aggTrades and bookTicker included), keeps the files it downloads in one on-disk cache, so a second run over the same days downloads nothing. The layout is the same for all of them:

```text
~/.lrvx/archive-cache/<exchange>/<market>/<symbol>/<file name as published>
```

`<symbol>` is the symbol in the file's URL: the instrument family (`BTC-USD`) for OKX futures and options chain files. Bitget also keeps a `<symbol>-<YYYY-MM-DD>.parts` list of a finished day's parts there.

- `LRVX_ARCHIVE_CACHE=/some/path` moves the root (`/some/path/<exchange>/...`).
- `--mirror DIR` (`mirror=DIR`) uses `DIR` in place of `~/.lrvx/archive-cache/<exchange>` for that import, with the same layout below it.

A download is written to a temporary name (`.<file>.<random>.part`) in the destination directory and renamed onto the file's real name only when it has arrived in full and opens as an archive, so an interrupted run never leaves a partial file under the real name. If a cached file nevertheless fails to open during an import, it is deleted from the cache and the import stops with an error; running it again downloads the file afresh. A file passed with `--csv` is never deleted.

The cache is not evicted automatically and grows with what you import. To clear it, delete the directory (`rm -rf ~/.lrvx/archive-cache`, or one exchange's subdirectory, or single files); the next import downloads what is missing.

## Cross-exchange research

Tapes from different exchanges sit side by side in `MergedTapeReader`. The reader assigns one global symbol id per `(exchange, name)` pair, so a strategy or analysis that consumes both `binance:BTCUSDT` and `bybit:BTCUSDT` sees two distinct streams with the right exchange tag on every event.

```text
import lrvx
reader = lrvx.MergedTapeReader([
    "./tapes/binance-um-BTCUSDT",
    "./tapes/bybit-linear-BTCUSDT",
])
trades = reader.read_trades()
sym_table = reader.symbol_table()   # [{global_id, exchange, name, ...}, ...]
```

The `(exchange, name)` keying is the contract: every cross-exchange analysis that wants the right interpretation of an event must hold the exchange tag alongside the symbol id.

## Adding a new exchange

A new exchange importer is one module under `lrvx/archives/<exchange>.py` exposing:

- `trades_to_lrvx(csv_path, out_tape, *, symbol_id, symbol_name, market, ...)` — parse one day's CSV, write trades via `DataWriter`, append-safe by the venue's trade id. `lrvx.archives._layout.CsvFile` recognises the layouts the exchange publishes and counts unparsable rows; `lrvx.archives._dedup.TradeFile` and `write_trade_file` do the bounded parse and the dedup described above.
- `range_to_lrvx(symbol, market, date_from, date_to, out_tape, *, mirror, parallel, ...)` — download a date range, hand each day to the single-day function, merge `metadata.json` with the union counters. `lrvx.archives._download.fetch` downloads one file under the policy above, `lrvx.archives._range.fetch_days` fetches the days of a range and handles missing ones, and `lrvx.archives._cache.exchange_dir` resolves the cache directory.
- `check_symbol(symbol, market)` — the local symbol shape check, and `DAY_UTC_OFFSET_HOURS`, the UTC offset in hours of the exchange's day boundary.

Register the new module in `archives/__init__.py`, add a `lrvx archive <exchange>` subparser in `lrvx/cli.py`, and write tests under `python/tests/` whose fixtures copy the real header and row shapes, plus a real-download test gated on `LRVX_ARCHIVE_NETWORK_TESTS=1`. The CLI and tests work generically against the `ArchiveReader` Protocol; no changes elsewhere in the framework are required.

## See also

- [Import Binance public archives](import-binance-archive.md) — trade stream.
- [Import Binance book archives](import-binance-book-archive.md) — bookTicker (best bid / ask) for um-futures and cm-futures.
- [Merge multiple tapes on read](multi-tape.md) — `MergedTapeReader` semantics across multiple exchanges.
