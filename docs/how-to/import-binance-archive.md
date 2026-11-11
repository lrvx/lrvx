# Import Binance public archives into a lrvx tape

`data.binance.vision` publishes daily aggregate-trade zip archives for spot, USDT-margined perpetuals, and coin-margined perpetuals, going back more than two years. The archive layout is stable, but the per-row work is fiddly: column ordering, header autoskip, millisecond-to-nanosecond rescale, side mapping, and dedup on re-runs.

`lrvx.archives.binance` wraps the converter in two function calls, plus a `lrvx archive binance` CLI subcommand. The output is a regular `.lrvx` tape, which means every aggregator, `MergedTapeReader`, the live recorder hook, and the engine itself read it without special-casing.

## Install

```bash
pip install lrvx
```

No extras are required. `--mirror` downloads use `urllib` from the standard library.

## Convert a single day

The simplest form takes one already-downloaded zip and writes a tape directory. The example below is the same script CI runs on every push: it builds a synthetic Binance-style zip in memory, calls `aggtrades_to_lrvx`, then reads the produced tape back through `DataReader`.

```python
--8<-- "examples/python_binance_archive.py"
```

`csv_path` accepts either the zip published by Binance or the extracted CSV; the reader autoskips a header row when one is present.

Side encoding follows the lrvx convention. `is_buyer_maker = true` becomes `Side::SELL` (the buyer was the resting maker, so the active flow is a seller hitting the bid). `is_buyer_maker = false` becomes `Side::BUY`.

## Convert a date range

For multi-day backfills, `range_to_lrvx` accepts a date range and optionally a local mirror cache. Missing zips are downloaded from `data.binance.vision` in parallel and reused on follow-up calls. The function signature:

```text
binance.range_to_lrvx(
    symbol: str,
    market: str,                # "spot" | "um-futures" | "cm-futures"
    date_from: str | date,      # YYYY-MM-DD, inclusive
    date_to:   str | date,      # YYYY-MM-DD, inclusive
    out_tape:  str | Path,
    *,
    mirror:    str | Path | None = None,
    parallel:  int = 4,
    symbol_id: int = 1,
    skip_missing: bool = False,
    ...
) -> ConvertStats
```

Without `mirror`, the converter writes to a tempdir and discards it after the range is done. Set `skip_missing=True` to keep going if Binance has not published a particular day yet (common at the head of the archive).

## CLI

`lrvx archive binance` is the same surface from the shell. For a date range:

```bash
lrvx archive binance \
  --symbol BTCUSDT \
  --market um-futures \
  --from 2024-01-01 --to 2024-01-31 \
  --out ./tapes/binance-um-BTCUSDT \
  --mirror ./.cache/binance \
  --parallel 4
```

For one-off conversions of a file already on disk, pass `--csv` instead of the date range:

```bash
lrvx archive binance \
  --csv ./BTCUSDT-aggTrades-2024-01-15.zip \
  --out ./tapes/binance-um-BTCUSDT \
  --symbol BTCUSDT --market um-futures --symbol-id 1
```

## Append-safe and idempotent

The converter dedups on `agg_trade_id` against any trades already in the target tape. Re-running the same day, or running an overlapping range, is a no-op: every previously-imported row is skipped, and the writer adds zero new records. The reported `rows_skipped` counter shows how many rows were elided.

## metadata.json

The tape's `metadata.json` is created (or merged into) on every successful conversion. `MergedTapeReader` keys symbols by `(metadata.exchange, name)`, so the Binance archive tapes line up against tapes captured live by the recorder hook. The symbol IDs picked here do not need to match what the live recorder picked; the reader rekeys both into a global ID space at read time.

## What is and is not in the archive

aggTrades only carries print events. There is no book information. The bookTicker archive (best bid / ask, um-futures and cm-futures) has its own converters in `lrvx.archives.binance`:

| Function | Archive |
|---|---|
| `t1_to_lrvx(csv_path, out_tape, ...)` | `T1` / `bookTicker` — top of book |
| `range_book_to_lrvx(symbol, market, book_type, date_from, date_to, out_tape, ...)` | multi-day bookTicker range, downloads and converts in one call |

They write into the same tape directory, so a single lrvx can carry both trades and book updates. See [Import Binance book archives](import-binance-book-archive.md).

Other public archives ship as sibling subcommands under `lrvx archive`: `bybit`, `okx`, `bitget` and `deribit` (`lrvx.archives.deribit`). See [Import multi-exchange archives](import-multi-exchange-archives.md).
