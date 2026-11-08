"""
Read the touch from a strategy: which stamp moved, and what it means.

Replays ``docs/examples/data/touch_tape``: one depth snapshot, three
one-level top-of-book frames, one trade, one symbol. The tape is synthetic,
written for this page; its shape is what a Hyperliquid recording with the
bbo channel on looks like between two depth snapshots.

A depth frame dispatches ``on_book_update`` and stamps both
``last_update_ns`` and ``last_touch_ns``. A top-of-book frame dispatches
nothing: it moves ``best_bid`` / ``best_ask`` / ``mid_price`` and
``last_touch_ns`` only. The trade that follows is the first callback to see
what the tops left behind, and its ``last_update_ns`` is the trade's own
stamp. So "did the touch move since the last book I was shown" is
``ctx.last_touch_ns > <last_update_ns seen in on_book_update>``.

Usage:
    cd /path/to/lrvx
    PYTHONPATH=build/python python3 docs/examples/python_read_the_touch.py
"""

import os

import lrvx

TAPE = os.path.join(os.path.dirname(__file__), "data", "touch_tape")

T0 = 1_700_000_000_000_000_000
SECOND = 1_000_000_000


class TouchWatcher(lrvx.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self.last_book_ns = 0
        self.seen = []

    def on_book_update(self, ctx):
        # A depth frame: the book and the touch were both written just now,
        # so the two stamps agree.
        self.last_book_ns = ctx.last_update_ns
        self.seen.append(("book", ctx.last_update_ns, ctx.last_touch_ns,
                          ctx.best_bid, ctx.best_ask))
        print(f"book   update={ctx.last_update_ns} touch={ctx.last_touch_ns} "
              f"bid={ctx.best_bid} ask={ctx.best_ask}")

    def on_trade(self, ctx, trade):
        # No callback ran for the tops in between. The context carries them:
        # the quotes are the last top's, last_touch_ns is its stamp, and
        # last_update_ns is this trade's.
        moved = ctx.last_touch_ns > self.last_book_ns
        self.seen.append(("trade", ctx.last_update_ns, ctx.last_touch_ns,
                          ctx.best_bid, ctx.best_ask, moved))
        print(f"trade  update={ctx.last_update_ns} touch={ctx.last_touch_ns} "
              f"bid={ctx.best_bid} ask={ctx.best_ask} "
              f"touch_moved_since_last_book={moved} mid={ctx.mid_price}")


def main() -> None:
    registry = lrvx.SymbolRegistry()
    sym = registry.add_symbol("sim", "BTCUSDT", tick_size=0.01)
    runner = lrvx.BacktestRunner(registry, fee_rate=0.0, initial_capital=10_000.0)
    strategy = TouchWatcher([sym])
    runner.set_strategy(strategy)
    runner.run_tape(TAPE)

    # Pinned: the page describes exactly this behaviour.
    assert strategy.seen == [
        ("book", T0, T0, 100.0, 101.0),
        ("trade", T0 + 4 * SECOND, T0 + 3 * SECOND, 100.4, 100.6, True),
    ], strategy.seen

    # The pull-side context after the run says the same.
    ctx = strategy.ctx(sym)
    assert (ctx.best_bid, ctx.best_ask, ctx.mid_price) == (100.4, 100.6, 100.5), ctx
    assert ctx.last_touch_ns == T0 + 3 * SECOND
    assert ctx.last_update_ns == T0 + 4 * SECOND
    print("ok")


if __name__ == "__main__":
    main()
