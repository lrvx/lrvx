"""Shared protocol + types for exchange-archive importers.

Each exchange submodule (``binance``, ``bybit``, ...) implements its
own ``trades_to_lrvx`` / ``range_to_lrvx`` API at the top
level. The Protocol below pins the contract those module-level
functions follow so consumers can write generic plumbing (CLI
dispatch, doc rendering, future cross-exchange normalizers) without
hard-coding the exchange names.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Optional, Protocol, Union, runtime_checkable


@dataclass
class TradeEvent:
    """Cross-exchange normalised trade event. Importers emit these
    on the parse path; the writer side converts to the lrvx
    ``TradeRecord`` byte layout via ``DataWriter.write_trades``.

    ``side`` follows ``lrvx::Side`` (BUY = 0, SELL = 1) — active flow
    direction, not the side of the resting maker.
    """

    exchange_ts_ns: int
    price: float
    qty: float
    trade_id: int
    side: int


@runtime_checkable
class ArchiveReader(Protocol):
    """Contract every exchange submodule satisfies for its trade
    archive importer.

    Module-level functions (not class methods) make the surface
    easier to type from a notebook (`lrvx.archives.bybit.range_to_lrvx`)
    and keep each importer's internal state private. The Protocol is
    runtime_checkable so the CLI can ``isinstance(mod, ArchiveReader)``
    when dispatching ``lrvx archive <exchange>``.
    """

    def trades_to_lrvx(  # type: ignore[empty-body]
        self,
        csv_path: Union[str, Path],
        out_tape: Union[str, Path],
        *,
        symbol_id: int = 1,
        symbol_name: str = "",
        market: str = "",
        exchange_id: int = 0,
        exchange_name: str = "",
        append: bool = True,
        max_segment_mb: int = 256,
        compression: str = "none",
        write_metadata: bool = True,
    ): ...

    def range_to_lrvx(  # type: ignore[empty-body]
        self,
        symbol: str,
        market: str,
        date_from,
        date_to,
        out_tape: Union[str, Path],
        *,
        mirror: Optional[Union[str, Path]] = None,
        parallel: int = 4,
        symbol_id: int = 1,
        exchange_id: int = 0,
        exchange_name: str = "",
        append: bool = True,
        max_segment_mb: int = 256,
        compression: str = "none",
        skip_missing: bool = False,
    ): ...


__all__ = ["TradeEvent", "ArchiveReader"]
