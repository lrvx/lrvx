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

Recent files add a trailing `RPI` column. `timestamp` is Unix seconds with a decimal fraction. `side` is the taker side, `Buy` / `Sell`, mapped to lrvx's `Side::BUY` / `Side::SELL`. `trdMatchID` is a UUID string that the importer folds into the 64-bit lrvx `trade_id` (low 8 bytes of its MD5); the folded ids carry no order, which the membership dedup above does not need. `size` is in the base coin for linear and in USD contracts for inverse. Older files list trades newest first.

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
    parallel=4,
    skip_missing=False,
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

Bitget specifically matters for production reproduction: `md_collector` deployments on Singapore default to Bitget feeds for multi-symbol fixtures, so the archive lets researchers re-run the same `(exchange, name)` keying that the live capture used.

## Shared download cache

All exchange-specific importers share one on-disk cache, rooted by default at `~/.lrvx/archive-cache/`. Override with `LRVX_ARCHIVE_CACHE=/some/path` (env var) or by passing `mirror=...` to the range form. Each exchange writes under its own subdirectory (`bybit/`, `okx/`, `bitget/`) so the cache layout stays predictable. The Binance range form uses a temporary directory unless `mirror` is given.

The cache has no auto-eviction; it grows monotonically. Wipe it whenever you want — the download path is idempotent and will refetch what is missing.

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
- `range_to_lrvx(symbol, market, date_from, date_to, out_tape, *, mirror, parallel, ...)` — download a date range, hand each day to the single-day function, merge `metadata.json` with the union counters.

Register the new module in `archives/__init__.py`, add a `lrvx archive <exchange>` subparser in `lrvx/cli.py`, and write tests under `python/tests/` whose fixtures copy the real header and row shapes, plus a real-download test gated on `LRVX_ARCHIVE_NETWORK_TESTS=1`. The CLI and tests work generically against the `ArchiveReader` Protocol; no changes elsewhere in the framework are required.

## See also

- [Import Binance public archives](import-binance-archive.md) — trade stream.
- [Import Binance book archives](import-binance-book-archive.md) — bookTicker (best bid / ask) for um-futures and cm-futures.
- [Merge multiple tapes on read](multi-tape.md) — `MergedTapeReader` semantics across multiple exchanges.
