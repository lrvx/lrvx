"""Date ranges, the day boundary, symbol shapes and the fetch of a
range of days, shared by the archive importers.

``fetch_days`` downloads (or finds in the mirror) every day of a range
in parallel and decides what a missing day means: a range in which no
day is published raises ``NoFilesError`` naming the range and the first
URL tried, with or without ``skip_missing``; a range with some days
missing raises ``NotPublished`` for the first of them unless
``skip_missing`` is set, in which case those days are left out. A
download that fails for any other reason is never skipped.
"""
from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from datetime import date, datetime, time, timedelta, timezone
from typing import Callable, List, Optional, Tuple, TypeVar, Union

from ._download import NotPublished

DateLike = Union[str, date]
T = TypeVar("T")


class NoFilesError(FileNotFoundError):
    """No day of the requested range is published."""


def coerce_day(d: DateLike) -> date:
    return d if isinstance(d, date) else date.fromisoformat(d)


def days_between(date_from: DateLike, date_to: DateLike) -> List[date]:
    """Every day from ``date_from`` to ``date_to``, both included."""
    cur, end = coerce_day(date_from), coerce_day(date_to)
    out = []
    while cur <= end:
        out.append(cur)
        cur += timedelta(days=1)
    return out


def utc_span(date_from: DateLike, date_to: DateLike,
             utc_offset_hours: int) -> Tuple[datetime, datetime]:
    """The UTC instants a range of exchange days covers, when the
    exchange cuts its days at midnight UTC+``utc_offset_hours``."""
    shift = timedelta(hours=utc_offset_hours)
    start = datetime.combine(coerce_day(date_from), time(), timezone.utc) - shift
    end = (datetime.combine(coerce_day(date_to), time(), timezone.utc)
           + timedelta(days=1) - shift)
    return start, end


def ns_to_iso(ns: int) -> str:
    """Nanoseconds since the epoch as ``YYYY-MM-DDTHH:MM:SS.mmmZ``;
    ``-`` for 0 (nothing written)."""
    if ns <= 0:
        return "-"
    secs, rem = divmod(int(ns), 1_000_000_000)
    dt = datetime.fromtimestamp(secs, tz=timezone.utc)
    return dt.strftime("%Y-%m-%dT%H:%M:%S.") + f"{rem // 1_000_000:03d}Z"


def is_upper_alnum(token: str) -> bool:
    """Digits and letters with no lower-case one (letters outside ASCII,
    which some exchange symbols carry, count as upper case)."""
    return bool(token) and all(
        c.isdigit() or (c.isalpha() and not c.islower()) for c in token)


def bad_symbol(exchange: str, market: str, symbol: str, expected: str,
               example: str) -> ValueError:
    return ValueError(
        f"'{symbol}' is not a {exchange} {market} symbol: expected "
        f"{expected}, e.g. {example}")


def fetch_days(days: List[date], fetch_day: Callable[[date], T], *,
               parallel: int, skip_missing: bool,
               what: str) -> List[Tuple[date, Optional[T]]]:
    """``(day, fetch_day(day))`` for every day, ``None`` for a day the
    archive does not publish; see the module docstring for when a
    missing day raises. ``what`` names the request in errors, e.g.
    ``"binance um-futures aggTrades BTCUSDT"``."""

    def one(day: date) -> Tuple[date, Optional[T], Optional[NotPublished]]:
        try:
            return day, fetch_day(day), None
        except NotPublished as exc:
            return day, None, exc

    with ThreadPoolExecutor(max_workers=max(1, int(parallel))) as ex:
        results = list(ex.map(one, days))

    missing = [(d, exc) for d, _, exc in results if exc is not None]
    if days and len(missing) == len(days):
        raise NoFilesError(
            f"{what} from {days[0].isoformat()} to {days[-1].isoformat()} "
            f"produced no files: the archive publishes none of the "
            f"{len(days)} day(s); first URL tried: {missing[0][1].url}")
    if missing and not skip_missing:
        day, exc = missing[0]
        more = f" and {len(missing) - 1} other day(s)" if len(missing) > 1 else ""
        raise NotPublished(
            exc.url, exc.status,
            f"{what}: the archive has no file for {day.isoformat()} "
            f"({exc.url}){more}; pass skip_missing=True (--skip-missing) to "
            f"step over days the archive does not publish")
    return [(d, v) for d, v, _ in results]
