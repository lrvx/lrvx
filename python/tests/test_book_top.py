"""The book-top feed, end to end from Python.

``Runner.on_book_top`` pushes a touch-only frame
(``BookUpdateType.TOP_OF_BOOK``): it moves the strategy's pull accessors
without firing ``on_book_update`` -- the depth book ignores the frame.
``SimulatedExecutor.on_top_of_book`` moves the simulated venue's touch
while the depth behind it survives, and ``on_best_levels`` lands on the
same path.
"""

from __future__ import annotations

import unittest

import lrvx


class BookCounter(lrvx.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self.book_calls = 0

    def on_book_update(self, ctx):
        self.book_calls += 1


class RunnerBookTop(unittest.TestCase):
    def setUp(self):
        self.registry = lrvx.SymbolRegistry()
        self.sym = self.registry.add_symbol("test", "BTC", tick_size=0.01)
        self.strategy = BookCounter([self.sym])
        self.runner = lrvx.Runner(self.registry, lambda sig: None)
        self.runner.add_strategy(self.strategy)
        self.runner.start()

    def tearDown(self):
        self.runner.stop()

    def test_top_moves_the_touch_without_a_book_callback(self):
        self.runner.on_book_snapshot(self.sym, [100.00], [2.5], [100.05], [1.25])
        self.assertEqual(self.strategy.book_calls, 1)

        self.runner.on_book_top(self.sym, (100.02, 4.0), (100.03, 3.0))
        self.assertEqual(self.strategy.book_calls, 1, "the depth book ignores a top frame")
        self.assertEqual(self.strategy.best_bid(), 100.02)
        self.assertEqual(self.strategy.best_ask(), 100.03)

    def test_none_side_means_empty_side(self):
        self.runner.on_book_snapshot(self.sym, [100.00], [2.5], [100.05], [1.25])
        self.runner.on_book_top(self.sym, (100.02, 4.0), None)
        self.assertEqual(self.strategy.best_bid(), 100.02)
        self.assertIsNone(self.strategy.best_ask())


class ExecutorTopOfBook(unittest.TestCase):
    def test_taker_pays_the_touch_the_top_moved(self):
        sim = lrvx.SimulatedExecutor()
        sim.on_book_snapshot(1, [(99.0, 5.0)], [(101.0, 1.0), (102.0, 5.0)])
        sim.on_top_of_book(1, (99.5, 5.0), (100.5, 2.0))

        sim.submit_order(1, "buy", 0.0, 1.0, type="market", symbol=1)
        fills = sim.fills_list()
        self.assertEqual(len(fills), 1)
        self.assertEqual(fills[0]["price"], 100.5, "the touch, not the snapshot's stale 101")

    def test_none_side_means_empty_side(self):
        sim = lrvx.SimulatedExecutor()
        sim.on_top_of_book(1, None, (100.5, 2.0))
        sim.submit_order(1, "sell", 0.0, 1.0, type="market", symbol=1)
        self.assertEqual(sim.fills_list(), [], "no bid side, nothing for a market sell")

    def test_best_levels_keep_deeper_depth(self):
        # on_best_levels lands on the touch path: the snapshot's depth
        # behind the quoted best survives, so a buy bigger than the best
        # level walks into it instead of printing at a one-level book.
        sim = lrvx.SimulatedExecutor()
        sim.on_book_snapshot(1, [(99.0, 5.0)], [(101.0, 1.0), (102.0, 5.0)])
        sim.on_best_levels(1, 99.5, 5.0, 100.5, 1.0)

        sim.submit_order(1, "buy", 0.0, 3.0, type="market", symbol=1)
        fills = sim.fills_list()
        self.assertEqual(len(fills), 1)
        self.assertAlmostEqual(
            fills[0]["price"], (100.5 + 101.0 + 102.0) / 3.0, places=6)


if __name__ == "__main__":
    unittest.main()
