"""``on_book_top`` on the surfaces whose book callback takes a bool.

``MarketDataRecorderHook``, ``MergedTapeReader.stream_events`` and
``lrvx.tape.replay_tapes`` hand snapshots and deltas to their bool-shaped
book callback as before. A top-of-book frame goes to ``on_book_top`` with
one level or ``None`` per side, and is skipped where that is not defined.
"""

from __future__ import annotations

import os
import tempfile
import unittest

import lrvx
from lrvx import tape as tape_mod

_BASE = 1_700_000_000_000_000_000


def _feed(runner, sym):
    """A snapshot, a top with an empty ask, then another snapshot."""
    runner.on_book_snapshot(sym, [99.0], [1.0], [101.0], [1.0], _BASE)
    runner.on_book_top(sym, (100.5, 2.0), None, _BASE + 1)
    runner.on_book_snapshot(sym, [99.5], [3.0], [101.0], [1.0], _BASE + 2)


def _record_tape(out_dir: str) -> None:
    registry = lrvx.SymbolRegistry()
    sym = registry.add_symbol("hyperliquid", "BTC", tick_size=0.01)
    hook = lrvx.BinaryLogRecorderHook(out_dir, max_segment_mb=4, exchange_id=0,
                                      compression="none", exchange_name="hyperliquid",
                                      instrument_type="perpetual")
    hook.add_symbol(sym, "BTC", "", "", 2, 6)
    runner = lrvx.Runner(registry, on_signal=lambda _s: None)
    runner.set_market_data_recorder(hook)
    runner.start()
    _feed(runner, sym)
    runner.stop()
    hook.close()


class RecorderHookBookTop(unittest.TestCase):
    def _run(self, hook):
        registry = lrvx.SymbolRegistry()
        sym = registry.add_symbol("test", "BTC", tick_size=0.01)
        runner = lrvx.Runner(registry, lambda _s: None)
        runner.set_market_data_recorder(hook)
        runner.start()
        _feed(runner, sym)
        runner.stop()
        return sym

    def test_top_goes_to_on_book_top_and_depth_to_on_book_update(self):
        class Hook(lrvx.MarketDataRecorderHook):
            def __init__(self):
                super().__init__()
                self.books = []
                self.tops = []

            def on_book_update(self, symbol, is_snapshot, bids, asks, ts_ns):
                self.books.append(is_snapshot)

            def on_book_top(self, symbol, bid, ask, ts_ns):
                self.tops.append((symbol, bid, ask, ts_ns))

        hook = Hook()
        sym = self._run(hook)
        self.assertEqual(hook.books, [True, True])
        self.assertEqual(hook.tops, [(int(sym), (100.5, 2.0), None, _BASE + 1)])

    def test_a_hook_without_on_book_top_skips_tops(self):
        class Hook(lrvx.MarketDataRecorderHook):
            def __init__(self):
                super().__init__()
                self.books = []

            def on_book_update(self, symbol, is_snapshot, bids, asks, ts_ns):
                self.books.append(is_snapshot)

        hook = Hook()
        self._run(hook)
        self.assertEqual(hook.books, [True, True])

    def test_an_exception_in_on_book_top_is_contained(self):
        class Hook(lrvx.MarketDataRecorderHook):
            def __init__(self):
                super().__init__()
                self.books = 0

            def on_book_update(self, symbol, is_snapshot, bids, asks, ts_ns):
                self.books += 1

            def on_book_top(self, symbol, bid, ask, ts_ns):
                raise RuntimeError("boom")

        hook = Hook()
        self._run(hook)
        self.assertEqual(hook.books, 2, "the snapshot after the failing top still arrives")


class MergedReaderBookTop(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="lrvx-top-cb-")
        self.tape = os.path.join(self._tmp.name, "tape")
        _record_tape(self.tape)

    def tearDown(self):
        self._tmp.cleanup()

    def test_stream_events_hands_tops_to_on_book_top(self):
        books, tops = [], []
        lrvx.MergedTapeReader([self.tape]).stream_events(
            on_book=lambda ts, recv, sym, tape, snap, bids, asks: books.append(snap),
            on_book_top=lambda ts, recv, sym, tape, bid, ask: tops.append((ts, bid, ask)))
        self.assertEqual(books, [True, True])
        self.assertEqual(tops, [(_BASE + 1, (100_50000000, 2_00000000), None)])

    def test_stream_events_without_on_book_top_skips_tops(self):
        books = []
        lrvx.MergedTapeReader([self.tape]).stream_events(
            on_book=lambda ts, recv, sym, tape, snap, bids, asks: books.append(snap))
        self.assertEqual(books, [True, True])

    def test_false_from_on_book_top_aborts_the_walk(self):
        books = []
        lrvx.MergedTapeReader([self.tape]).stream_events(
            on_book=lambda ts, recv, sym, tape, snap, bids, asks: books.append(snap),
            on_book_top=lambda *args: False)
        self.assertEqual(books, [True], "the walk stopped at the top")

    def test_replay_tapes_hands_tops_to_on_book_top(self):
        books, tops = [], []
        tape_mod.replay_tapes(
            [self.tape],
            on_book=lambda ts, sym, snap, bids, asks: books.append(snap),
            on_book_top=lambda ts, sym, bid, ask: tops.append((ts, bid, ask)))
        self.assertEqual(books, [True, True])
        self.assertEqual(tops, [(_BASE + 1, (100.5, 2.0), None)])

    def test_replay_tapes_with_only_on_book_top(self):
        tops = []
        tape_mod.replay_tapes([self.tape], on_book_top=lambda *a: tops.append(a))
        self.assertEqual(len(tops), 1)


if __name__ == "__main__":
    unittest.main()
