"""Paper mode and a tick-replay backtest are the same run.

One strategy class runs twice over the same recorded trades: once through
``BacktestRunner.run_tape`` (the tape replay), once through ``PaperBroker``
fed the trades read back from that tape, one ``observe_trade`` each. The
strategy exercises what used to differ in paper mode: it arms a stop and a
take-profit from ``on_fill``, modifies and cancels a resting limit, and sends
an off-tick limit that the tick rule refuses. Both runs must hand the strategy
the same order events, with the same fields, in the same order, the same
``ctx.position`` and ``position()`` inside each callback, and the same final
position and fills.
"""
from __future__ import annotations

import shutil
import tempfile
import unittest
from pathlib import Path

import lrvx
from lrvx import paper, tape

NS = 1_000_000_000
TICK = 0.01


def _price_path() -> list[float]:
    # Up through the take-profit at 101.00, back down through the stop at
    # 99.00, and on down past the resting buy's price. Every price is on the
    # 0.01 tick.
    up = [100.00 + 0.05 * i for i in range(25)]          # 100.00 .. 101.20
    down = [101.20 - 0.10 * i for i in range(1, 45)]     # 101.10 .. 96.80
    return [round(p, 2) for p in up + down]


def _record_tape(work: Path, sym_name: str) -> Path:
    out = work / "tape"
    out.mkdir()
    registry = lrvx.SymbolRegistry()
    sym = int(registry.add_symbol("sim", sym_name, tick_size=TICK))
    recorder = tape.make_recorder_hook(out)
    runner = lrvx.Runner(registry, on_signal=lambda _: None)
    runner.set_market_data_recorder(recorder)
    runner.start()
    for i, px in enumerate(_price_path()):
        runner.on_trade(sym, px, 1.0 + (i % 3), i % 2 == 0, (i + 1) * NS)
    runner.stop()
    recorder.close()
    return out


def _trades_from_tape(path: Path) -> list[tuple[int, float, float, bool]]:
    rows = lrvx.DataReader(str(path)).read_trades()
    return [
        (int(r["exchange_ts_ns"]), int(r["price_raw"]) / 1e8, int(r["qty_raw"]) / 1e8,
         int(r["side"]) == 0)
        for r in rows
    ]


class _Exercise(lrvx.Strategy):
    """Enters on the first trade and manages the position from callbacks."""

    def __init__(self, symbols):
        super().__init__(symbols)
        self.log: list[tuple] = []
        self.trades_seen = 0
        self.resting = None
        self.parked = None
        self.armed = False

    def _record(self, kind, ctx, ev):
        self.log.append((
            kind, ev.order_id, ev.status, ev.side, ev.order_type, ev.fill_qty,
            ev.fill_price, ev.reject_reason, ev.exchange_ts_ns, ev.submitted_at_ns,
            ev.accepted_at_ns, ev.first_fill_at_ns, ev.canceled_at_ns, ev.rejected_at_ns,
            ev.triggered_at_ns, ev.is_maker, ctx.position, self.position(),
        ))

    def on_trade(self, ctx, trade):
        self.trades_seen += 1
        if self.trades_seen == 1:
            self.market_buy(1.0)
            self.limit_buy(99.995, 1.0)          # off the 0.01 tick: refused
            self.resting = self.limit_buy(97.50, 1.0)
            self.parked = self.limit_buy(95.00, 1.0)
        elif self.trades_seen == 6:
            self.modify_order(self.resting, 97.60, 1.0)
        elif self.trades_seen == 50:
            self.cancel_order(self.parked)
        self.log.append(("trade", self.trades_seen, ctx.position))

    def on_fill(self, ctx, ev):
        self._record("fill", ctx, ev)
        if not self.armed and ev.order_type == "market" and ev.status == "FILLED":
            # Protect the entry from inside the fill callback, the case
            # paper mode used to drop entirely.
            self.armed = True
            self.stop_market("sell", 99.00, 0.5)
            self.take_profit_market("sell", 101.00, 0.5)

    def on_order_update(self, ctx, ev):
        self._record("update", ctx, ev)


def _backtest(tape_dir: Path, sym_name: str):
    registry = lrvx.SymbolRegistry()
    sym = int(registry.add_symbol("sim", sym_name, tick_size=TICK))
    runner = lrvx.BacktestRunner(registry, 0.0, 100_000.0)
    strat = _Exercise([sym])
    runner.set_strategy(strat)
    runner.run_tape(str(tape_dir))
    return strat, strat.position()


def _paper(trades, sym_name: str):
    registry = lrvx.SymbolRegistry()
    sym = int(registry.add_symbol("sim", sym_name, tick_size=TICK))
    broker = paper.PaperBroker(registry=registry)
    strat = _Exercise([sym])
    broker.runner.add_strategy(strat)
    broker.start()
    for ts, px, qty, is_buy in trades:
        broker.observe_trade(sym, px, qty, is_buy, ts)
    broker.stop()
    return strat, strat.position(), broker


class PaperBacktestEquivalence(unittest.TestCase):
    def setUp(self) -> None:
        self.work = Path(tempfile.mkdtemp(prefix="lrvx_paper_eq_"))
        self.tape = _record_tape(self.work, "BTCUSDT")
        self.trades = _trades_from_tape(self.tape)

    def tearDown(self) -> None:
        shutil.rmtree(self.work, ignore_errors=True)

    def test_same_events_fills_and_position(self) -> None:
        bt, bt_pos = _backtest(self.tape, "BTCUSDT")
        pp, pp_pos, broker = _paper(self.trades, "BTCUSDT")

        self.assertEqual(len(self.trades), len(_price_path()))
        self.assertEqual(pp.log, bt.log)
        self.assertEqual(pp_pos, bt_pos)

        events = [e for e in bt.log if e[0] != "trade"]
        statuses = [(e[0], e[2], e[4]) for e in events]
        # The run is not vacuous: each path that used to differ shows up.
        self.assertIn(("fill", "FILLED", "market"), statuses)
        self.assertIn(("update", "PENDING_TRIGGER", "stop_market"), statuses)
        self.assertIn(("fill", "FILLED", "stop_market"), statuses)
        self.assertIn(("fill", "FILLED", "tp_market"), statuses)
        self.assertIn(("update", "REPLACED", "limit"), statuses)
        self.assertIn(("update", "CANCELED", "limit"), statuses)
        self.assertTrue(any(e[2] == "REJECTED" and e[7] == "price_off_tick" for e in events),
                        "the off-tick limit is refused by the tick rule in both runs")

        # The runner's position tracker sees a fill before the strategy does,
        # in both runs: position() inside the entry's on_fill counts it.
        entry = next(e for e in events if e[0] == "fill" and e[4] == "market")
        self.assertEqual(entry[-1], 1.0)
        self.assertEqual(pp_pos, 0.0)
        self.assertEqual(len(broker.fills_list()), 3)  # entry, take-profit, stop


if __name__ == "__main__":
    unittest.main()
