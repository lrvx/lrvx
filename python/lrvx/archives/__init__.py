"""Exchange archive importers — convert public historical tick data
into the lrvx tape format consumed by Engine, BacktestRunner, and
the tape aggregators.

Each exchange ships as its own submodule with a small shared spine:

  * ``aggtrades_to_lrvx`` / ``trades_to_lrvx`` — single-day
    convert.
  * ``range_to_lrvx`` — a range of the exchange's days, downloaded into
    the shared cache.
  * ``check_symbol`` — the local symbol shape check the range form runs
    before any request; ``DAY_UTC_OFFSET_HOURS`` — 0 for exchanges
    whose days are UTC days (Binance, Bybit), 8 for UTC+8 (OKX, Bitget).
  * CLI: ``lrvx archive {binance | bybit | okx | bitget} ...``.

The downloaded zip / csv.gz files share a single on-disk cache rooted
at ``~/.lrvx/archive-cache`` (overridable via ``LRVX_ARCHIVE_CACHE``),
laid out as ``<cache_root>/<exchange>/<market>/<symbol>/<file>``; a
range form's ``mirror`` replaces ``<cache_root>/<exchange>``. Every
download follows one policy (``_download``): a file the archive does not
publish is asked for once and raises ``NotPublished``; transient
failures are retried; a file is renamed into the cache only once
complete and readable. A range in which no day is published raises
``NoFilesError``.

Submodules: ``binance`` (aggTrades and bookTicker), ``bybit``,
``okx`` and ``bitget``. A file whose header matches no layout the
exchange publishes raises ``UnknownLayoutError``; a file with rows of
which none parse raises ``NothingParsedError``.
"""
from . import _cache, archive_reader, binance, bitget, bybit, okx
from ._download import CorruptArchiveError, DownloadError, NotPublished
from ._layout import NothingParsedError, UnknownLayoutError
from ._range import NoFilesError

cache_root = _cache.cache_root
ArchiveReader = archive_reader.ArchiveReader
TradeEvent = archive_reader.TradeEvent

__all__ = [
    "ArchiveReader",
    "CorruptArchiveError",
    "DownloadError",
    "NoFilesError",
    "NotPublished",
    "NothingParsedError",
    "TradeEvent",
    "UnknownLayoutError",
    "cache_root",
    "binance",
    "bitget",
    "bybit",
    "okx",
]
