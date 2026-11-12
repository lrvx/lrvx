"""CSV layout recognition and parse accounting shared by the importers.

Every archive file is matched against the layouts its exchange is known
to publish, by its header row (or, for a headerless layout, by its
column count and a numeric first field). A file that matches none is
rejected with ``UnknownLayoutError`` naming the header it found, instead
of having every row skipped.

``RowCounts`` tallies the data rows of one file and the ones that did
not parse. ``check_parsed`` turns "data rows were read but none parsed"
into ``NothingParsedError``; a file whose rows all parsed but are all on
the tape already is a legitimate empty import and passes.
"""
from __future__ import annotations

import csv
import gzip
import io
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator, List, Optional, Sequence, Tuple


class UnknownLayoutError(ValueError):
    """The file's header matches no layout the exchange publishes."""


class NothingParsedError(ValueError):
    """The file has data rows and none of them parsed."""


@dataclass(frozen=True)
class Layout:
    """One published CSV layout: its column names in order, and whether
    the file opens with them as a header row."""

    name: str
    columns: Tuple[str, ...]
    has_header: bool = True

    def index(self, column: str) -> int:
        return self.columns.index(column)


class RowCounts:
    """Data rows seen in one file, how many failed to parse, and the
    first one that failed."""

    def __init__(self) -> None:
        self.lines = 0
        self.invalid = 0
        self.first_invalid: Optional[List[str]] = None

    def bad(self, row: List[str]) -> None:
        self.invalid += 1
        if self.first_invalid is None:
            self.first_invalid = list(row)


def column_name(cell: str) -> str:
    """A header cell as compared against a layout: trimmed, lower case,
    without a byte order mark and without a ``/<translation>`` suffix
    (OKX's legacy headers read ``trade_id/<Chinese name>``)."""
    return cell.lstrip("﻿").split("/", 1)[0].strip().lower()


def _is_int(cell: str) -> bool:
    try:
        int(cell.strip())
    except ValueError:
        return False
    return True


def detect(first_row: Sequence[str], layouts: Sequence[Layout], *,
           path: Path, source: str) -> Layout:
    """Return the layout ``first_row`` opens, or raise UnknownLayoutError."""
    names = tuple(column_name(c) for c in first_row)
    for layout in layouts:
        if layout.has_header and names == layout.columns:
            return layout
    for layout in layouts:
        if (not layout.has_header and len(first_row) == len(layout.columns)
                and first_row and _is_int(first_row[0])):
            return layout
    known = "; ".join(
        (",".join(l.columns) if l.has_header
         else f"no header, {len(l.columns)} columns: " + ",".join(l.columns))
        for l in layouts)
    raise UnknownLayoutError(
        f"{path}: unrecognised {source} CSV layout, first row "
        f"{','.join(first_row)!r}; expected one of: {known}")


def check_parsed(path: Path, counts: RowCounts, parsed: int, *,
                 source: str, layout: Optional[Layout]) -> None:
    """Raise NothingParsedError when the file had rows to parse and none
    did."""
    if parsed == 0 and counts.invalid > 0:
        name = layout.name if layout is not None else "?"
        raise NothingParsedError(
            f"{path}: {counts.invalid} of {counts.lines} {source} data rows "
            f"failed to parse with layout '{name}' and none parsed; first "
            f"failing row: {','.join(counts.first_invalid or [])!r}")


def open_text(path: Path) -> Tuple[io.TextIOBase, object]:
    """Open an archive file as text: the single CSV member of a ``.zip``,
    a gzipped CSV, or a plain CSV. Returns the stream and the object to
    close. Bytes that are not UTF-8 (the legacy OKX header) are replaced,
    the data rows being ASCII."""
    name = path.name.lower()
    if name.endswith(".zip"):
        zf = zipfile.ZipFile(path)
        members = [n for n in zf.namelist() if n.lower().endswith(".csv")]
        if not members:
            zf.close()
            raise ValueError(f"no CSV member inside {path}")
        raw = zf.open(members[0])
        return io.TextIOWrapper(raw, encoding="utf-8", errors="replace",
                                newline=""), zf
    if name.endswith(".gz"):
        f = gzip.open(path, "rt", encoding="utf-8", errors="replace",
                      newline="")
        return f, f
    f = open(path, "r", encoding="utf-8", errors="replace", newline="")
    return f, f


class CsvFile:
    """The rows of one archive CSV with its layout detected.

    ``layout`` is set once the first non-empty row has been read; it is
    None for a file with no rows at all. ``rows()`` yields the data rows
    (the header excluded) and counts each in ``counts.lines``."""

    def __init__(self, path: Path, layouts: Sequence[Layout], *,
                 source: str) -> None:
        self.path = path
        self.layouts = layouts
        self.source = source
        self.layout: Optional[Layout] = None
        self.counts = RowCounts()

    def rows(self) -> Iterator[List[str]]:
        stream, ctx = open_text(self.path)
        try:
            reader = csv.reader(stream)
            for row in reader:
                if not row or (len(row) == 1 and not row[0].strip()):
                    continue
                if self.layout is None:
                    self.layout = detect(row, self.layouts, path=self.path,
                                         source=self.source)
                    if self.layout.has_header:
                        continue
                self.counts.lines += 1
                yield row
        finally:
            try:
                ctx.close()
            except Exception:
                pass

    def check(self, parsed: int) -> None:
        check_parsed(self.path, self.counts, parsed, source=self.source,
                     layout=self.layout)


def side_of(token: str) -> Optional[int]:
    """``buy`` -> 0 (lrvx Side::BUY), ``sell`` -> 1 (Side::SELL), any case;
    None for anything else."""
    t = token.strip().lower()
    if t == "buy":
        return 0
    if t == "sell":
        return 1
    return None
