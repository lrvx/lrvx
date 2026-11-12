# Import multi-exchange public archives

`lrvx.archives` collects the per-exchange importers under one namespace. Each submodule exposes the same two entry points and the matching CLI subcommand:

| Exchange | Module | Trade entry point | CLI |
|---|---|---|---|
| Binance | `lrvx.archives.binance` | `aggtrades_to_lrvx` | `lrvx archive binance ...` |
| Bybit   | `lrvx.archives.bybit`   | `trades_to_lrvx`    | `lrvx archive bybit ...`   |
| OKX     | `lrvx.archives.okx`     | `trades_to_lrvx`    | `lrvx archive okx ...`     |
| Bitget  | `lrvx.archives.bitget`  | `trades_to_lrvx`    | `lrvx archive bitget ...`  |
| Deribit | `lrvx.archives.deribit` | `trades_to_lrvx`    | `lrvx archive deribit ...` |

Adding a new venue is a self-contained module under `lrvx/archives/` that implements `trades_to_lrvx` + `range_to_lrvx` matching the `ArchiveReader` Protocol.

All five trade importers append by default and dedup the same way: a row is skipped when its trade id is already on the tape for the same symbol within the incoming file's time range. A trade keeps its exchange timestamp, so only that range can hold a copy of it, and the dedup read covers that range instead of the whole tape. Re-importing a day adds nothing, a new day lands in full, an overlapping range keeps each trade once, and days may be imported in any order (a later day first, or a day that fills a gap). `--no-append` turns the check off.

Each import writes new segments named by the time of the import, so a tape filled out of order lists its segments out of time order. Readers and `run_tape` order segments by their first event, so the tape still replays forward in time.

The Binance bookTicker importer (`--book t1`) follows the same rule on the book record's update id, and encodes every file on its own, starting from a snapshot, so book days may be imported in any order too. See [Import Binance book archives](import-binance-book-archive.md#append-safe-and-idempotent).

## Bybit

Bybit publishes daily trade ticks at `https://public.bybit.com/` going back two-plus years. The on-disk CSV layout (post-2022):

```text
timestamp, symbol, side, size, price, tickDirection, trdMatchID,
grossValue, homeNotional, foreignNotional
```

`timestamp` is Unix seconds (microsecond decimal precision). `side` is the active flow as `Buy` / `Sell` strings; the importer maps to lrvx's `Side::BUY` / `Side::SELL`. `trdMatchID` is the exchange-assigned trade id, a UUID string that the importer folds into the 64-bit lrvx `trade_id` (low 8 bytes of its MD5). The folded ids carry no order, so append dedup is by membership: a row is skipped when its id is already on the tape for the same symbol within the incoming file's time range. Re-importing a day adds nothing, a new day lands in full, and an overlapping range keeps each trade once. `--no-append` turns the check off.

### URL layout

- spot: `public.bybit.com/spot/<SYMBOL>/<SYMBOL><YYYY-MM-DD>.csv.gz`
- linear: `public.bybit.com/trading/<SYMBOL>/<SYMBOL><YYYY-MM-DD>.csv.gz`
- inverse: `public.bybit.com/trading/<SYMBOL>/<SYMBOL><YYYY-MM-DD>.csv.gz`

The converter accepts the symbol verbatim; cross-exchange symbol normalisation is a connectors concern, not an importer one. Each tape keys its own `(metadata.exchange, name)` so `MergedTapeReader` treats `binance:BTCUSDT` and `bybit:BTCUSDT` as distinct global symbols.

### Example

The script below builds a synthetic Bybit-style gzipped CSV in memory, runs `trades_to_lrvx`, then reads the produced tape back through `DataReader.read_trades`:

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

OKX publishes daily trade ticks on `www.okx.com/cdn/okex/traderecords/` for spot, swap (perpetual), futures, and options. The on-disk CSV columns:

```text
trade_id, side, size, price, timestamp_ms
```

`trade_id` is an integer exchange-assigned id, used directly as the lrvx `trade_id` for dedup. `side` is the active flow as `buy` / `sell` lowercase, mapped to lrvx's `Side::BUY` / `Side::SELL`.

### URL layout

- spot:    `cdn/okex/traderecords/spot/daily/<YYYYMMDD>/<SYMBOL>-trades-<YYYY-MM-DD>.zip`
- swap:    `cdn/okex/traderecords/swap/daily/<YYYYMMDD>/<SYMBOL>-trades-<YYYY-MM-DD>.zip`
- futures: `cdn/okex/traderecords/futures/daily/<YYYYMMDD>/<SYMBOL>-trades-<YYYY-MM-DD>.zip`
- option:  `cdn/okex/traderecords/option/daily/<YYYYMMDD>/<SYMBOL>-trades-<YYYY-MM-DD>.zip`

Symbol naming follows OKX convention (`BTC-USDT` for spot, `BTC-USDT-SWAP` for perp, `BTC-29MAR24-50000-C` for option-chain instruments). The converter accepts the symbol verbatim; cross-exchange normalisation is out of scope.

### Example

The script below builds a synthetic OKX-style zipped CSV in memory and round-trips it through the converter:

```python
--8<-- "examples/python_okx_archive.py"
```

### CLI

```bash
# Single day from a local CSV / zip
lrvx archive okx \
  --csv ./BTC-USDT-SWAP-trades-2024-01-15.zip \
  --out ./tapes/okx-swap-BTC-USDT-SWAP \
  --symbol BTC-USDT-SWAP --market swap

# Multi-day range with download
lrvx archive okx \
  --symbol BTC-USDT-SWAP --market swap \
  --from 2024-01-01 --to 2024-12-31 \
  --out ./tapes/okx-swap-BTC-USDT-SWAP \
  --parallel 4
```

## Bitget

Bitget publishes daily trade ticks on its public archive S3 / CDN mirror. The on-disk CSV columns:

```text
trade_id, price, size, side, timestamp_ms
```

`trade_id` is an integer exchange-assigned id, used directly for append-safe dedup. `side` is the active flow as `buy` / `sell` lowercase. `timestamp_ms` is Unix milliseconds.

Market codes follow Bitget's own API naming: `spot`, `umcbl` (USDT-margined perpetual), `cmcbl` (coin-margined perpetual). The converter accepts them verbatim.

### Example

```python
--8<-- "examples/python_bitget_archive.py"
```

### CLI

```bash
# Single day from a local file
lrvx archive bitget \
  --csv ./BTCUSDT-trades-2024-01-15.zip \
  --out ./tapes/bitget-umcbl-BTCUSDT \
  --symbol BTCUSDT --market umcbl

# Multi-day range with download
lrvx archive bitget \
  --symbol BTCUSDT --market umcbl \
  --from 2024-01-01 --to 2024-12-31 \
  --out ./tapes/bitget-umcbl-BTCUSDT \
  --parallel 4
```

Bitget specifically matters for production reproduction: `md_collector` deployments on Singapore default to Bitget feeds for multi-symbol fixtures, so the archive lets researchers re-run the same `(exchange, name)` keying that the live capture used.

## Deribit

Deribit dominates crypto options volume and is the only venue of this set whose public archive carries options trades as a first-class event type. The on-disk CSV columns (post-2022):

```text
trade_id, timestamp_ms, instrument, side, price, amount,
mark_price, iv, index_price
```

`trade_id` is integer, used directly for append-safe dedup. `side` is `buy` / `sell` lowercase. For an option instrument `mark_price`, `iv` and `index_price` are kept: each trade row also writes an OptionQuote frame carrying them, with `iv` stored as the archive gives it. For a future or perpetual the three columns are not written, since the `TradeRecord` schema has no field for them.

### Instrument naming

- perpetual: `BTC-PERPETUAL`, `ETH-PERPETUAL`, ...
- future:    `BTC-29MAR24`, `ETH-28JUN24`, ... (date-encoded expiry)
- option:    `BTC-29MAR24-50000-C`, `BTC-29MAR24-50000-P` (date-encoded expiry + strike + C/P)

The converter writes one instrument per tape, but option chains are handled by
`lrvx.archives.deribit.load_option_chain`, which converts each instrument's
archive into its own tape under `out_root/<instrument>` and returns a queryable
`OptionChain` over them:

`load_option_chain(instruments, day, out_root, *, mirror, exchange_id=0, compression="none")`
takes a sequence of instrument ids and a day; `mirror` is required (CSVs are read
from there and downloaded if absent). Each instrument gets a distinct `symbol_id`
(its index + 1). The returned `OptionChain` supports `len()`, iteration,
`expiries()`, `strikes(expiry=None)` and `query(expiry=None, strike=None,
option_type=None)`, each yielding `OptionContract` records that carry the tape
path. `parse_option_instrument("BTC-29MAR24-50000-C")` returns
`("BTC", date(2024, 3, 29), 50000.0, "C")` and raises `ValueError` on a perp /
future id.

### Example

```python
--8<-- "examples/python_deribit_archive.py"
```

### CLI

```bash
# Single day from a local file
lrvx archive deribit \
  --csv ./BTC-PERPETUAL-2024-01-15.csv.gz \
  --out ./tapes/deribit-perp-BTC \
  --symbol BTC-PERPETUAL --market perpetual

# Multi-day option-chain instrument
lrvx archive deribit \
  --symbol BTC-29MAR24-50000-C --market option \
  --from 2024-01-01 --to 2024-03-29 \
  --out ./tapes/deribit-opt-BTC-29MAR24-50000-C \
  --parallel 4
```

## Shared download cache

All exchange-specific importers share one on-disk cache, rooted by default at `~/.lrvx/archive-cache/`. Override with `LRVX_ARCHIVE_CACHE=/some/path` (env var) or by passing `mirror=...` to the range form. Each exchange writes under its own subdirectory (`bybit/`, `binance/`, etc.) so the cache layout stays predictable.

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

- `trades_to_lrvx(csv_path, out_tape, *, symbol_id, symbol_name, market, ...)` — parse one day's CSV, write trades via `DataWriter`, append-safe by the venue's trade id (`lrvx.archives._dedup.drop_trades_on_tape` does the dedup described above).
- `range_to_lrvx(symbol, market, date_from, date_to, out_tape, *, mirror, parallel, ...)` — download a date range, hand each day to the single-day function, merge `metadata.json` with the union counters.

Register the new module in `archives/__init__.py`, add a `lrvx archive <exchange>` subparser in `lrvx/cli.py`, and write a synthetic-fixture test under `python/tests/`. The CLI and tests work generically against the `ArchiveReader` Protocol; no changes elsewhere in the framework are required.

## See also

- [Import Binance public archives](import-binance-archive.md) — trade stream.
- [Import Binance book archives](import-binance-book-archive.md) — bookTicker (best bid / ask) for um-futures and cm-futures.
- [Merge multiple tapes on read](multi-tape.md) — `MergedTapeReader` semantics across multiple exchanges.
