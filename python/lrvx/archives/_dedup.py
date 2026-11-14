"""Append dedup and bounded-memory streaming shared by the archive importers.

An incoming row is skipped when its trade id is already on the tape
for the same symbol within the incoming file's time range. A trade
keeps its exchange timestamp, so a copy of it on the tape can only sit
inside that range; the tape read is bounded to it, which keeps each
import at roughly one file's worth of tape instead of the whole tape.

Membership, not ``id <= max``, so days imported out of order, and a
day filling a gap between days already on the tape, land in full.

Book importers use the same rule on a book record's ``seq``
(``existing_book_seqs``).

Trade importers go through ``TradeFile``: the CSV is parsed once, in
batches of ``_BATCH_ROWS`` rows, into numpy records spilled to a
temporary file, so the file's time range is known before the first
write and no more than a batch of parsed rows is ever in memory. The
rows come back out in the order a whole-file import writes them: file
order, or a stable sort on the timestamp, merged from sorted runs when
the file is not already in time order.
"""
from __future__ import annotations

import os
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterable, Iterator, Optional

# Rows parsed, deduplicated and written per step. Tests patch it smaller.
_BATCH_ROWS = 16_384


def existing_trade_ids(out_tape: Path, symbol_id: int,
                       from_ns: int, to_ns: int, *,
                       windows: int = 1) -> "object":
    """Return the trade ids on the tape for ``symbol_id`` with an
    exchange timestamp in ``[from_ns, to_ns]``, as a sorted numpy uint64
    array (empty when the tape is missing or unreadable).

    ``windows`` splits the range into that many consecutive inclusive
    time windows read one after another, so the reader never holds more
    than one window's trades; the ids returned are the same."""
    import numpy as np

    empty = np.empty(0, dtype=np.uint64)
    if not out_tape.exists():
        return empty
    from_ns, to_ns = int(from_ns), int(to_ns)
    windows = max(1, int(windows))
    width = (to_ns - from_ns) // windows + 1
    parts = []
    try:
        import lrvx

        lo = from_ns
        while lo <= to_ns:
            hi = min(to_ns, lo + width - 1)
            reader = lrvx.DataReader(str(out_tape), from_ns=lo, to_ns=hi,
                                     symbols=[int(symbol_id)])
            trades = reader.read_trades()
            if trades.size:
                parts.append(trades["trade_id"].astype(np.uint64))
            del trades, reader
            lo = hi + 1
    except Exception:
        return empty
    if not parts:
        return empty
    ids = parts[0] if len(parts) == 1 else np.concatenate(parts)
    ids.sort()
    return ids


def existing_book_seqs(out_tape: Path, symbol_id: int,
                       from_ns: int, to_ns: int) -> "object":
    """Return the ``seq`` of every book record on the tape for
    ``symbol_id`` with an exchange timestamp in ``[from_ns, to_ns]``, as
    a sorted numpy uint64 array (empty when the tape is missing or
    unreadable)."""
    import numpy as np

    empty = np.empty(0, dtype=np.uint64)
    if not out_tape.exists():
        return empty
    try:
        import lrvx

        reader = lrvx.DataReader(str(out_tape), from_ns=int(from_ns),
                                 to_ns=int(to_ns), symbols=[int(symbol_id)])
        headers, _ = reader.read_book_updates()
        if headers.size == 0:
            return empty
        return np.unique(np.asarray(headers["seq"], dtype=np.uint64))
    except Exception:
        return empty


def trade_dtype(id_type: str = "u8", extra: Iterable[str] = ()) -> "object":
    """Record layout of a parsed trade row: ``(id, price, qty, ts, side,
    *extra)``, ``ts`` in the importer's own unit, extras as float64."""
    import numpy as np

    return np.dtype([("id", id_type), ("price", "f8"), ("qty", "f8"),
                     ("ts", "i8"), ("side", "u1")]
                    + [(name, "f8") for name in extra])


class TradeFile:
    """One archive file's parsed trade rows, kept on disk.

    ``rows`` yields tuples laid out as ``dtype``. They are parsed once,
    when the object is built, in batches of ``_BATCH_ROWS``; each batch
    is stably sorted on ``ts`` when ``sort`` is set and appended to a
    temporary file as one run. The row count, the smallest and largest
    ``ts`` and the largest ``id`` are known from then on.

    ``batches(on_tape)`` yields the rows whose id is not in ``on_tape``
    (a sorted uint64 array), in file order or, with ``sort``, in the
    order of a stable sort of the whole file on ``ts``, a batch at a
    time. It can be iterated more than once and yields the same rows
    each time.

    Use as a context manager; leaving it deletes the temporary file.

    With ``spill_dir`` the rows are kept in a file of that directory
    instead of a private temporary one, and the object pickles: a worker
    process parses a file into it and hands it to the process that
    writes the tape, which closes it (deleting the file) when done. The
    directory's owner removes whatever is left there."""

    def __init__(self, rows: Iterable[tuple], dtype, *, sort: bool,
                 spill_dir: Optional[Path] = None) -> None:
        import numpy as np

        self.dtype = np.dtype(dtype)
        self.sort = bool(sort)
        self.batch_rows = max(1, int(_BATCH_ROWS))
        self.rows_read = 0
        self.ts_min: Optional[int] = None
        self.ts_max: Optional[int] = None
        self.id_max = 0
        self.rows_skipped = 0
        self.rows_kept = 0
        self._in_order = True
        self._last_ts: Optional[int] = None
        self._runs = []
        if spill_dir is None:
            self._dir = tempfile.TemporaryDirectory(prefix="lrvx-archive-import-")
            self._path = Path(self._dir.name) / "rows.bin"
        else:
            fd, name = tempfile.mkstemp(dir=spill_dir, prefix="rows-", suffix=".bin")
            os.close(fd)
            self._dir = None
            self._path = Path(name)
        try:
            with open(self._path, "wb") as out:
                pending = []
                for row in rows:
                    pending.append(row)
                    if len(pending) >= self.batch_rows:
                        self._spill(out, pending)
                        pending = []
                if pending:
                    self._spill(out, pending)
        except BaseException:
            self.close()
            raise

    def _spill(self, out, pending: list) -> None:
        import numpy as np

        run = np.array(pending, dtype=self.dtype)
        ts = run["ts"]
        lo, hi = int(ts.min()), int(ts.max())
        self.ts_min = lo if self.ts_min is None else min(self.ts_min, lo)
        self.ts_max = hi if self.ts_max is None else max(self.ts_max, hi)
        self.id_max = max(self.id_max, int(run["id"].max()))
        run_in_order = bool(np.all(ts[1:] >= ts[:-1]))
        if self._in_order:
            self._in_order = run_in_order and (self._last_ts is None
                                               or int(ts[0]) >= self._last_ts)
        self._last_ts = int(ts[-1])
        if self.sort and not run_in_order:
            run = run[np.argsort(ts, kind="stable")]
        self._runs.append((self.rows_read, len(run)))
        run.tofile(out)
        self.rows_read += len(run)

    def close(self) -> None:
        if self._dir is not None:
            self._dir.cleanup()
        else:
            try:
                self._path.unlink()
            except FileNotFoundError:
                pass

    def __getstate__(self) -> dict:
        if self._dir is not None:
            raise TypeError("a TradeFile pickles only when built with spill_dir")
        return dict(self.__dict__)

    def __enter__(self) -> "TradeFile":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def on_tape(self, out_tape: Path, symbol_id: int,
                ts_ns_per_unit: int = 1) -> "object":
        """Ids already on the tape for ``symbol_id`` within this file's
        time range, read in time windows sized to hold about a quarter
        batch of rows each when the tape is as dense as the file."""
        import numpy as np

        if not self.rows_read:
            return np.empty(0, dtype=np.uint64)
        # A quarter batch of the file's rows per window: the reader's own
        # buffers, not just the ids kept, scale with the window.
        windows = -(-self.rows_read // max(1, self.batch_rows // 4))
        return existing_trade_ids(out_tape, symbol_id,
                                  self.ts_min * ts_ns_per_unit,
                                  self.ts_max * ts_ns_per_unit,
                                  windows=windows)

    def batches(self, on_tape) -> Iterator["object"]:
        """Yield the rows not on the tape, in write order, as record
        arrays. ``rows_skipped`` and ``rows_kept`` count the last pass."""
        import numpy as np

        self.rows_skipped = 0
        self.rows_kept = 0
        source = (self._merged() if self.sort and not self._in_order
                  else self._in_file_order())
        for batch in source:
            if on_tape.size:
                ids = batch["id"].astype(np.uint64)
                at = np.searchsorted(on_tape, ids)
                np.minimum(at, on_tape.size - 1, out=at)
                fresh = on_tape[at] != ids
                if not fresh.all():
                    self.rows_skipped += int(len(batch) - fresh.sum())
                    batch = batch[fresh]
            if len(batch):
                self.rows_kept += len(batch)
                yield batch

    def _read(self, f, start: int, count: int):
        import numpy as np

        f.seek(start * self.dtype.itemsize)
        return np.fromfile(f, dtype=self.dtype, count=count)

    def _in_file_order(self) -> Iterator["object"]:
        with open(self._path, "rb") as f:
            for start in range(0, self.rows_read, self.batch_rows):
                yield self._read(f, start,
                                 min(self.batch_rows, self.rows_read - start))

    def _merged(self) -> Iterator["object"]:
        """K-way merge of the sorted runs into the order of a stable
        sort on ``ts``: key ``(ts, run, position in run)``.

        Each step holds at most one read window per run. The rows that
        can be emitted are those at or below the smallest last-loaded
        key among runs that still have rows on disk: nothing unread can
        sort before it. Rows tied on ``ts`` with that bound are held
        back in runs after the bounding one, since the bounding run's
        unread rows come first. The bounding run empties its window
        every step, so every step makes progress."""
        import numpy as np

        k = len(self._runs)
        window = max(1, self.batch_rows // k)
        cursor = [start for start, _ in self._runs]
        end = [start + count for start, count in self._runs]
        held = [None] * k
        with open(self._path, "rb") as f:
            while True:
                for r in range(k):
                    if (held[r] is None or not len(held[r])) and cursor[r] < end[r]:
                        n = min(window, end[r] - cursor[r])
                        held[r] = self._read(f, cursor[r], n)
                        cursor[r] += n
                live = [r for r in range(k) if held[r] is not None and len(held[r])]
                if not live:
                    return
                bound = min(((int(held[r]["ts"][-1]), r) for r in live
                             if cursor[r] < end[r]), default=None)
                parts = []
                for r in live:
                    rows = held[r]
                    if bound is None:
                        take = len(rows)
                    else:
                        take = int(np.searchsorted(
                            rows["ts"], bound[0],
                            side="right" if r <= bound[1] else "left"))
                    if take:
                        parts.append(rows[:take])
                        held[r] = rows[take:]
                out = parts[0] if len(parts) == 1 else np.concatenate(parts)
                yield out[np.argsort(out["ts"], kind="stable")]


@dataclass
class ParsedTrades:
    """One archive file parsed and not yet written: its rows, how many
    data rows failed to parse, and what else the importer noted
    (``extra``). Built where the file is parsed, in a worker process or
    in the caller; ``rows`` is closed by whoever writes it."""

    path: Path
    rows: TradeFile
    rows_invalid: int = 0
    extra: dict = field(default_factory=dict)


class WrittenTrades:
    """What ``write_trade_file`` wrote: the count the writer accepted, and
    the timestamp (ns) of the first and last row and the id of the last
    row, in write order. All zero when nothing was written."""

    def __init__(self) -> None:
        self.count = 0
        self.first_ts_ns = 0
        self.last_ts_ns = 0
        self.last_id = 0


def write_trade_file(trade_file: TradeFile, out_tape: Path, *,
                     append: bool, symbol_id: int, exchange_id: int,
                     max_segment_mb: int, compression: str,
                     ts_ns_per_unit: int = 1,
                     then: Optional[Callable] = None) -> WrittenTrades:
    """Write the rows of ``trade_file`` that are not on the tape yet (all
    of them without ``append``) through one ``lrvx.DataWriter``, a batch
    at a time. The writer is opened on the first row, so an import that
    writes nothing creates no segment.

    ``then(writer, batches)``, when given, runs after every trade is
    written, on the same writer, with a fresh pass over the same rows."""
    import numpy as np

    on_tape = (trade_file.on_tape(out_tape, symbol_id, ts_ns_per_unit)
               if append else np.empty(0, dtype=np.uint64))
    done = WrittenTrades()
    writer = None
    try:
        for batch in trade_file.batches(on_tape):
            if writer is None:
                import lrvx

                out_tape.mkdir(parents=True, exist_ok=True)
                writer = lrvx.DataWriter(str(out_tape),
                                         max_segment_mb=int(max_segment_mb),
                                         exchange_id=int(exchange_id),
                                         compression=compression)
                writer_fresh = True
            ts = batch["ts"].astype(np.int64) * ts_ns_per_unit
            done.count += int(writer.write_trades(
                exchange_ts_ns=ts,
                recv_ts_ns=ts,
                prices=np.ascontiguousarray(batch["price"]),
                quantities=np.ascontiguousarray(batch["qty"]),
                trade_ids=batch["id"].astype(np.uint64),
                symbol_ids=np.full(len(batch), int(symbol_id), dtype=np.uint32),
                sides=np.ascontiguousarray(batch["side"]),
            ))
            if writer_fresh:
                done.first_ts_ns = int(ts[0])
                writer_fresh = False
            done.last_ts_ns = int(ts[-1])
            done.last_id = int(batch["id"][-1])
        if writer is not None and then is not None:
            then(writer, trade_file.batches(on_tape))
    finally:
        if writer is not None:
            writer.close()
    return done
