"""Exchange archive importers — convert public historical tick data
into the lrvx tape format consumed by Engine, BacktestRunner, and
the tape aggregators.

Each exchange ships as its own submodule with a small shared spine:

  * ``aggtrades_to_lrvx`` / ``trades_to_lrvx`` — single-day
    convert.
  * ``range_to_lrvx`` — multi-day with optional HTTP mirror cache.
  * CLI: ``lrvx archive {binance | bybit | okx | bitget} ...``.

The downloaded zip / csv.gz files share a single on-disk cache rooted
at ``~/.lrvx/archive-cache`` (overridable via ``LRVX_ARCHIVE_CACHE``);
each exchange namespaces its files under
``<cache_root>/<exchange>/<...>/``.

Submodules: ``binance`` (aggTrades and bookTicker), ``bybit``,
``okx`` and ``bitget``. A file whose header matches no layout the
exchange publishes raises ``UnknownLayoutError``; a file with rows of
which none parse raises ``NothingParsedError``.
"""
from . import _cache, archive_reader, binance, bitget, bybit, okx
from ._layout import NothingParsedError, UnknownLayoutError

cache_root = _cache.cache_root
ArchiveReader = archive_reader.ArchiveReader
TradeEvent = archive_reader.TradeEvent

__all__ = [
    "ArchiveReader",
    "NothingParsedError",
    "TradeEvent",
    "UnknownLayoutError",
    "cache_root",
    "binance",
    "bitget",
    "bybit",
    "okx",
]
