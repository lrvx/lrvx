"""Shared pieces of the opt-in archive tests that download real files.

They run only with ``LRVX_ARCHIVE_NETWORK_TESTS=1`` in the environment;
otherwise they are skipped. Each downloads one small real day through
the importer's own range function into a temporary mirror, checks that
trades were written, and compares the first, the last and a random row
of the downloaded CSV, decoded by hand, with the trade on the tape.
"""
from __future__ import annotations

import csv
import os
import random
from pathlib import Path
from typing import Callable, List, Optional, Sequence, Tuple

ENABLED = os.environ.get("LRVX_ARCHIVE_NETWORK_TESTS") == "1"
SKIP_REASON = "set LRVX_ARCHIVE_NETWORK_TESTS=1 to download real archive files"

# (trade_id or None, price, qty, ts_ns, side)
Trade = Tuple[Optional[int], float, float, int, int]


def csv_rows(path: Path) -> List[List[str]]:
    """Every non-empty row of an archive file, header included."""
    from lrvx.archives import _layout

    stream, ctx = _layout.open_text(Path(path))
    try:
        return [r for r in csv.reader(stream) if r]
    finally:
        ctx.close()


def tape_trades(tape: Path) -> List[Tuple[int, float, float, int, int]]:
    import lrvx

    t = lrvx.DataReader(str(tape)).read_trades()
    return [(int(t["trade_id"][i]), int(t["price_raw"][i]) / 1e8,
             int(t["qty_raw"][i]) / 1e8, int(t["exchange_ts_ns"][i]),
             int(t["side"][i])) for i in range(t.size)]


def assert_sample(case, tape: Path, expected: Sequence[Trade]) -> None:
    """The first, last and a random expected trade are on the tape with
    the same id (when given), price, quantity, timestamp and side, and
    the tape holds exactly as many trades as expected."""
    got = tape_trades(tape)
    case.assertEqual(len(got), len(expected))
    by_ts = {}
    for g in got:
        by_ts.setdefault(g[3], []).append(g)
    picks = [0, len(expected) - 1, random.Random(len(expected)).randrange(len(expected))]
    for i in picks:
        tid, price, qty, ts, side = expected[i]
        match = [g for g in by_ts.get(ts, [])
                 if (tid is None or g[0] == tid)
                 and abs(g[1] - price) < 1e-8 and abs(g[2] - qty) < 1e-8
                 and g[4] == side]
        case.assertTrue(match, f"row {i} {expected[i]} not on the tape")


def expected_from(path: Path, decode: Callable[[List[str]], Optional[Trade]],
                  *, header: bool = True) -> List[Trade]:
    rows = csv_rows(path)
    if header:
        rows = rows[1:]
    return [t for t in (decode(r) for r in rows) if t is not None]
