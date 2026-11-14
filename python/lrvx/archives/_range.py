"""Date ranges, the day boundary, symbol shapes, the fetch of a range of
days and the conversion of its files, shared by the archive importers.

``fetch_days`` downloads (or finds in the mirror) every day of a range
in parallel and decides what a missing day means: a range in which no
day is published raises ``NoFilesError`` naming the range and the first
URL tried, with or without ``skip_missing``; a range with some days
missing raises ``NotPublished`` for the first of them unless
``skip_missing`` is set, in which case those days are left out. A
download that fails for any other reason is never skipped.

``convert_days`` parses the fetched files in up to ``workers`` processes
and hands each result back to the calling process in file order, where
the importer deduplicates it against the tape and writes it: parsing a
CSV is the slow part of an import, and keeping every write (and every
dedup read of the tape) in one process keeps the tape exactly what a
serial import writes. ``all_or_nothing`` wraps the writes of a range so
that an import which fails leaves the tape as it found it.
"""
from __future__ import annotations

import os
import pickle
import shutil
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from datetime import date, datetime, time, timedelta, timezone
from pathlib import Path
from typing import (Callable, Iterator, List, Optional, Sequence, Tuple,
                    TypeVar, Union)

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


R = TypeVar("R")


def check_workers(workers: Optional[int]) -> None:
    """Raise ValueError unless ``workers`` is None or a count of at least
    one. The range importers call it before any download."""
    if workers is None:
        return
    try:
        ok = (not isinstance(workers, bool) and int(workers) == workers
              and workers >= 1)
    except (TypeError, ValueError):
        ok = False
    if not ok:
        raise ValueError(
            f"workers must be None (one per core) or at least 1, got {workers!r}")


def resolve_workers(workers: Optional[int], n_files: int) -> int:
    """The worker count for a range of ``n_files`` files: ``workers`` as
    given, or with None one per core (``os.cpu_count()``) capped at the
    number of files."""
    check_workers(workers)
    if workers is not None:
        return int(workers)
    return max(1, min(os.cpu_count() or 1, int(n_files)))


def _convert_in_worker(convert_one: Callable, item):
    """Runs ``convert_one(item)`` in a worker. An exception that would not
    survive the trip back to the caller is replaced by a RuntimeError
    carrying its type name and message."""
    try:
        return convert_one(item)
    except BaseException as exc:
        try:
            pickle.loads(pickle.dumps(exc))
        except Exception:
            raise RuntimeError(f"{type(exc).__name__}: {exc}") from None
        raise


def convert_days(items: Sequence, convert_one: Callable[..., R], *,
                 workers: int,
                 then: Optional[Callable[[int, R], object]] = None) -> list:
    """``[convert_one(item) for item in items]``, computed in up to
    ``workers`` processes.

    ``convert_one`` must be picklable (a module-level function): worker
    processes are started with the ``spawn`` method on every platform.
    With ``workers == 1``, or a single item, everything runs in the
    calling process. Results come back in item order. An exception raised
    by ``convert_one`` is raised in the caller with its own type and
    message; for the first failing item in item order, after the items
    before it were handed to ``then``.

    ``then(index, result)``, when given, runs in the calling process on
    each result in item order as soon as it and every result before it
    are in, and what it returns takes the result's place in the list.
    Workers run at most ``2 * workers`` items ahead of the last one
    handed to ``then``, so what the results hold on disk stays bounded
    however long the range."""
    items = list(items)
    check_workers(workers)
    if workers is None:
        raise ValueError("workers must be a count; resolve None with "
                         "resolve_workers first")
    out: list = []
    if not items:
        return out
    n = min(int(workers), len(items))
    if n == 1:
        for i, item in enumerate(items):
            r = convert_one(item)
            out.append(then(i, r) if then is not None else r)
        return out

    import multiprocessing
    from concurrent.futures import ProcessPoolExecutor

    ahead = 2 * n
    pool = ProcessPoolExecutor(max_workers=n,
                               mp_context=multiprocessing.get_context("spawn"))
    try:
        pending = {}
        submitted = 0
        for i in range(len(items)):
            while submitted < len(items) and submitted < i + ahead:
                pending[submitted] = pool.submit(_convert_in_worker, convert_one,
                                                 items[submitted])
                submitted += 1
            r = pending.pop(i).result()
            out.append(then(i, r) if then is not None else r)
    except BaseException:
        pool.shutdown(wait=True, cancel_futures=True)
        raise
    pool.shutdown(wait=True)
    return out


@contextmanager
def spill_dir() -> Iterator[Path]:
    """A temporary directory the workers of one range parse into; removed
    with whatever is left in it when the range is done or has failed."""
    import tempfile

    path = Path(tempfile.mkdtemp(prefix="lrvx-archive-import-"))
    try:
        yield path
    finally:
        shutil.rmtree(path, ignore_errors=True)


@contextmanager
def all_or_nothing(out_tape: Union[str, Path]) -> Iterator[None]:
    """Undo the writes of the ``with`` body when it raises: the segment
    files it added to ``out_tape`` are removed, ``metadata.json`` gets its
    old bytes back (or goes, if it was not there), and a directory the
    body created is removed when nothing else is in it. A tape a writer
    appends to only gains files, so this restores it byte for byte."""
    out_tape = Path(out_tape).expanduser()
    existed = out_tape.is_dir()
    before = {p.name for p in out_tape.iterdir()} if existed else set()
    meta = out_tape / "metadata.json"
    meta_bytes = meta.read_bytes() if meta.is_file() else None
    try:
        yield
    except BaseException:
        if out_tape.is_dir():
            for p in out_tape.iterdir():
                if p.name not in before and p.is_file():
                    try:
                        p.unlink()
                    except OSError:
                        pass
            try:
                if meta_bytes is not None:
                    meta.write_bytes(meta_bytes)
                elif meta.is_file() and "metadata.json" not in before:
                    meta.unlink()
            except OSError:
                pass
            if not existed:
                try:
                    out_tape.rmdir()
                except OSError:
                    pass
        raise
