"""The book-top feed from Python.

``Runner.on_book_top`` pushes a touch-only frame
(``BookUpdateType.TOP_OF_BOOK``): it moves the strategy's pull accessors
without firing ``on_book_update`` -- the depth book ignores the frame.
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

if __name__ == "__main__":
    unittest.main()
