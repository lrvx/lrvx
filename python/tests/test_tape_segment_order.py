"""A tape imported out of day order replays in time order.

Each archive import writes a new segment named by the wall-clock time it was
opened, so importing day 3 and then day 1 lists day 3's segment first. The
reader orders segments by their first event, so a backtest over such a tape
sees time run forward, and the reader's stats say the segments were reordered.
"""
from __future__ import annotations

import io
import sys
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
for cand in ("build/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        sys.path.insert(0, str(p))
        break

import lrvx  # noqa: E402
from lrvx.archives import binance as bnc  # noqa: E402

DAY_MS = 86_400_000
DAY1_MS = 1_704_067_200_000  # 2024-01-01T00:00:00Z


def _day_zip(dest: Path, day_index: int, first_agg_id: int, n: int = 25) -> Path:
    start = DAY1_MS + day_index * DAY_MS
    buf = io.StringIO()
    for i in range(n):
        agg = first_agg_id + i
        buf.write(f"{agg},{42000 + i},0.01,{agg},{agg},{start + i * 60_000},"
                  f"{'True' if i % 2 else 'False'},True\n")
    with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(dest.with_suffix(".csv").name, buf.getvalue())
    return dest


def _import_days(tmp: Path, order):
    tape = tmp / "tape"
    for day_index in order:
        z = _day_zip(tmp / f"BTCUSDT-aggTrades-2024-01-0{day_index + 1}.zip",
                     day_index, first_agg_id=1000 * (day_index + 1))
        bnc.aggtrades_to_lrvx(z, tape, symbol_id=1, symbol_name="BTCUSDT",
                              market="um-futures")
    return tape


def test_reader_orders_days_imported_out_of_order(tmp_path):
    tape = _import_days(tmp_path, [2, 0, 1])
    reader = lrvx.DataReader(str(tape))
    first_by_file = [s["first_event_ns"] for s in reader.segments()]
    assert first_by_file != sorted(first_by_file), "fixture must be out of file order"

    trades = reader.read_trades()
    ts = [int(t) for t in trades["exchange_ts_ns"]]
    assert len(ts) == 75
    assert ts == sorted(ts)
    assert reader.stats()["segments_reordered"] > 0

    start = (DAY1_MS + DAY_MS + 30 * 60_000) * 1_000_000
    tail = [int(t) for t in lrvx.DataReader(str(tape)).read_trades_from(start)["exchange_ts_ns"]]
    assert tail == [t for t in ts if t >= start]


def test_reader_file_name_order_switch(tmp_path):
    tape = _import_days(tmp_path, [2, 0])
    trades = lrvx.DataReader(str(tape), order_segments_by_time=False).read_trades()
    ts = [int(t) for t in trades["exchange_ts_ns"]]
    assert ts[0] > ts[-1]


def test_run_tape_sees_time_run_forward(tmp_path):
    tape = _import_days(tmp_path, [2, 0, 1])

    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("binance", "BTCUSDT", tick_size=0.01)
    seen = []

    class Recorder(lrvx.Strategy):
        def on_trade(self, ctx, trade):
            seen.append(int(trade.timestamp_ns))

    bt = lrvx.BacktestRunner(reg, fee_rate=0.0, initial_capital=10_000.0)
    bt.set_strategy(Recorder(symbols=[sym]))
    bt.run_tape(str(tape))

    assert len(seen) == 75
    backwards = [(a, b) for a, b in zip(seen, seen[1:]) if b < a]
    assert not backwards, f"time ran backwards {len(backwards)} times, first {backwards[0]}"
