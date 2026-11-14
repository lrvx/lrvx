"""BacktestRunner takes the executor's slippage and latency settings.

The keywords are BacktestConfig's fields, snake_cased, and mean what they mean
there: a run through the runner fills exactly where a SimulatedExecutor driven
by hand with the same settings fills, bar for bar and fill for fill.
"""
from __future__ import annotations

import hashlib

import numpy as np
import pytest

import lrvx

MIN = 60_000_000_000
T0 = 1_767_225_600_000_000_000


def _bars(n: int = 40, seed: int = 5):
    rng = np.random.default_rng(seed)
    start = T0 + np.arange(n, dtype=np.int64) * MIN
    end = start + MIN
    close = np.round(100.0 + np.cumsum(rng.integers(-80, 81, n)) * 0.01, 2)
    open_ = np.round(np.concatenate([[100.0], close[:-1]]) + rng.integers(-5, 6, n) * 0.01, 2)
    high = np.round(np.maximum(open_, close) + rng.integers(0, 60, n) * 0.01, 2)
    low = np.round(np.minimum(open_, close) - rng.integers(0, 60, n) * 0.01, 2)
    vol = np.round(rng.uniform(1.0, 50.0, n), 3)
    return {"start_time_ns": start, "end_time_ns": end, "open": open_, "high": high,
            "low": low, "close": close, "volume": vol}


# bar index -> orders the strategy sends from that bar's callback
# ("market", side, qty) | ("limit", side, price_offset_from_close, qty) | ("cancel",)
PLAN = {
    2: [("market", "buy", 1.0)],
    6: [("market", "sell", 1.0)],
    9: [("market", "sell", 2.0)],
    13: [("market", "buy", 2.0)],
    16: [("limit", "buy", -0.30, 1.0)],
    17: [("cancel",)],
    20: [("market", "buy", 1.5)],
    27: [("market", "sell", 1.5)],
}


class Planned(lrvx.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self.i = 0
        self.fills = []
        self.live = []

    def on_bar(self, ctx, bar):
        for step in PLAN.get(self.i, []):
            if step[0] == "market":
                (self.market_buy if step[1] == "buy" else self.market_sell)(step[2])
            elif step[0] == "limit":
                px = round(bar.close + step[2], 2)
                self.live.append(
                    (self.limit_buy if step[1] == "buy" else self.limit_sell)(px, step[3]))
            elif step[0] == "cancel_all":
                self.cancel_all_orders()
                self.live.clear()
            else:
                for oid in self.live:
                    self.cancel_order(oid)
                self.live.clear()
        self.i += 1

    def on_fill(self, ctx, ev):
        self.fills.append((ev.side, round(ev.fill_price, 8), round(ev.fill_qty, 8)))


def _registry():
    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("test", "AAA", tick_size=0.01)
    return reg, sym.id


def _run(bars=None, **kw):
    bars = bars or _bars()
    reg, sid = _registry()
    bt = lrvx.BacktestRunner(reg, 0.0004, 10_000.0, **kw)
    strat = Planned([sid])
    bt.set_strategy(strat)
    stats = bt.run_bars(symbol="AAA", **bars)
    return stats, bt.trades(), bt.equity_curve(), strat.fills


def _by_hand(configure, bars=None):
    """Drive SimulatedExecutor the way run_bars does: clock to the bar end, walk the bar,
    then submit the callback's orders inside the bar-callback window."""
    bars = bars or _bars()
    ex = lrvx.SimulatedExecutor()
    # The runner checks and snaps against the symbol's declared tick, so the
    # hand-driven reference has to know the same tick.
    reg = lrvx.SymbolRegistry()
    assert int(reg.add_symbol("test", "AAA", tick_size=0.01)) == 1
    ex.set_symbol_registry(reg)
    configure(ex)
    oid = 0
    live = []
    for i in range(len(bars["open"])):
        ex.advance_clock(int(bars["end_time_ns"][i]))
        ex.on_bar_ohlc(1, bars["open"][i], bars["high"][i], bars["low"][i], bars["close"][i])
        ex.begin_bar_callback_window()
        for step in PLAN.get(i, []):
            if step[0] == "market":
                oid += 1
                ex.submit_order(oid, step[1], 0.0, step[2], "market", 1)
            elif step[0] == "limit":
                oid += 1
                ex.submit_order(oid, step[1], round(bars["close"][i] + step[2], 2), step[3], "limit", 1)
                live.append(oid)
            elif step[0] == "cancel_all":
                ex.cancel_all(1)
                live.clear()
            else:
                for o in live:
                    ex.cancel_order(o)
                live.clear()
        ex.end_bar_callback_window()
    side = {0: "buy", 1: "sell"}
    return [(side[int(f["side"])], round(f["price_raw"] / lrvx.PRICE_SCALE, 8),
             round(f["quantity_raw"] / lrvx.VOLUME_SCALE, 8)) for f in ex.fills()]


def _digest(stats, trades, equity) -> str:
    h = hashlib.sha256()
    h.update(repr(sorted(stats.items())).encode())
    for d in (trades, equity):
        for k in sorted(d):
            h.update(k.encode())
            h.update(np.ascontiguousarray(d[k]).tobytes())
    return h.hexdigest()


# Captured from the runner before it took slippage or latency keywords, then
# re-captured when stats gained unrealized_pnl and final_equity; without those
# two keys the digest is still the original
# 602c88f18cebfa7082efd12a55d26370e007d0c27074a39e707db6aab4dd13b5.
BASELINE_DIGEST = "4bece33ba2d57c78cc5ae1411acd94071da892846c107ed131a6db18f4916ecd"


def test_defaults_reproduce_the_baseline_bit_for_bit():
    stats, trades, equity, fills = _run()
    assert len(fills) == 7
    assert _digest(stats, trades, equity) == BASELINE_DIGEST


def test_baseline_matches_hand_driven_executor():
    assert _run()[3] == _by_hand(lambda ex: None)


def test_explicit_defaults_equal_no_keywords():
    a = _run()
    b = _run(slippage_model="none", slippage_ticks=0, slippage_tick_size=0.0, slippage_bps=0.0,
             slippage_impact_coeff=0.0, submit_ack_latency_ns=0, submit_ack_jitter_ns=0,
             cancel_ack_latency_ns=0, cancel_ack_jitter_ns=0, cancel_ack_seed=42,
             replace_ack_latency_ns=0, replace_ack_jitter_ns=0, latency=None)
    assert _digest(*a[:3]) == _digest(*b[:3])
    assert a[3] == b[3]


def test_fixed_ticks_matches_hand_driven_executor():
    _, _, _, fills = _run(slippage_model="fixed_ticks", slippage_ticks=3, slippage_tick_size=0.01)
    want = _by_hand(lambda ex: ex.set_default_slippage("fixed_ticks", ticks=3, tick_size=0.01))
    base = _run()[3]
    assert fills == want
    assert fills != base
    # market fills move 3 ticks against the taker; the resting limit (fill 4) does not slip
    assert fills[4] == base[4]
    for k, ((s, p, q), (_, p0, _)) in enumerate(zip(fills, base)):
        if k == 4:
            continue
        if s == "buy":
            assert p == pytest.approx(p0 + 0.03)
        else:
            assert p == pytest.approx(p0 - 0.03)


def test_fixed_bps_matches_hand_driven_executor():
    _, _, _, fills = _run(slippage_model="fixed_bps", slippage_bps=7.5)
    want = _by_hand(lambda ex: ex.set_default_slippage("fixed_bps", bps=7.5))
    assert fills == want
    assert fills != _run()[3]


def test_slippage_changes_pnl_in_the_expected_direction():
    base = _run()[0]
    slipped = _run(slippage_model="fixed_bps", slippage_bps=10.0)[0]
    assert slipped["total_trades"] == base["total_trades"]
    assert slipped["net_pnl"] < base["net_pnl"]


def test_submit_ack_latency_matches_hand_driven_executor():
    _, _, _, fills = _run(submit_ack_latency_ns=5_000_000)
    want = _by_hand(lambda ex: ex.set_submit_ack_latency(5_000_000))
    assert fills == want


def test_submit_ack_latency_on_bars_costs_one_bar():
    """The clock reads the bar's end throughout a bar, so a held order released at the next
    open with any positive deferral is accepted at the open after that."""
    bars = _bars()
    instant = _run(bars)[3]
    delayed = _run(bars, submit_ack_latency_ns=5_000_000)[3]
    # first planned order: sent from bar 2, instant fill at bar 3's open, delayed at bar 4's
    assert instant[0][1] == pytest.approx(bars["open"][3])
    assert delayed[0][1] == pytest.approx(bars["open"][4])
    # a deferral longer than one bar lands one more bar later
    later = _run(bars, submit_ack_latency_ns=MIN + 1)[3]
    assert later[0][1] == pytest.approx(bars["open"][5])


def test_submit_ack_jitter_matches_hand_driven_executor():
    _, _, _, fills = _run(submit_ack_latency_ns=50 * 1_000_000_000,
                          submit_ack_jitter_ns=40 * 1_000_000_000, cancel_ack_seed=9)
    want = _by_hand(lambda ex: ex.set_submit_ack_latency(50 * 1_000_000_000, 40 * 1_000_000_000))
    # the hand-driven executor keeps the default seed; reseed the runner to compare
    _, _, _, fills42 = _run(submit_ack_latency_ns=50 * 1_000_000_000,
                            submit_ack_jitter_ns=40 * 1_000_000_000)
    assert fills42 == want
    assert len(fills) == len(fills42)


def test_latency_model_composes_like_submit_ack_latency():
    model = lrvx.ConstantLatency(0, 5_000_000, 0)
    _, _, _, fills = _run(latency=model)
    want = _by_hand(lambda ex: ex.set_submit_ack_latency(5_000_000))
    assert fills == want
    both = _run(latency=lrvx.ConstantLatency(0, 40 * 1_000_000_000, 0),
                submit_ack_latency_ns=40 * 1_000_000_000)[3]
    want_both = _by_hand(lambda ex: ex.set_submit_ack_latency(80 * 1_000_000_000))
    assert both == want_both


def test_cancel_ack_latency_matches_hand_driven_executor(monkeypatch):
    # A buy limit at 96.40 from bar 16: bar 17 (low 96.47) misses it, bar 17's callback
    # cancels it, bar 18 (low 96.35) would have filled it.
    monkeypatch.setitem(PLAN, 16, [("limit", "buy", -1.27, 1.0)])
    instant = _run()[3]
    _, _, _, fills = _run(cancel_ack_latency_ns=3 * MIN)
    want = _by_hand(lambda ex: ex.set_cancel_ack_latency(3 * MIN))
    assert fills == want
    assert len(fills) == len(instant) + 1, "the cancel in flight lets bar 18 fill the order"
    assert ("buy", 96.4, 1.0) in fills and ("buy", 96.4, 1.0) not in instant
    # a cancel ack shorter than one bar has landed by the next bar's first step
    assert _run(cancel_ack_latency_ns=30 * 1_000_000_000)[3] == instant


def test_cancel_all_honours_cancel_ack_latency(monkeypatch):
    # The same scenario as above, pulled with cancel_all_orders(): cancel-all is the
    # same request as cancelling each order, so the ack latency lets bar 18 fill it.
    monkeypatch.setitem(PLAN, 16, [("limit", "buy", -1.27, 1.0)])
    instant = _run()[3]
    each = _run(cancel_ack_latency_ns=3 * MIN)[3]
    monkeypatch.setitem(PLAN, 17, [("cancel_all",)])
    assert _run()[3] == instant
    fills = _run(cancel_ack_latency_ns=3 * MIN)[3]
    assert fills == each
    assert fills == _by_hand(lambda ex: ex.set_cancel_ack_latency(3 * MIN))
    assert ("buy", 96.4, 1.0) in fills and ("buy", 96.4, 1.0) not in instant
    assert _run(cancel_ack_latency_ns=30 * 1_000_000_000)[3] == instant


def test_settings_survive_across_runs():
    reg, sid = _registry()
    bt = lrvx.BacktestRunner(reg, 0.0004, 10_000.0, slippage_model="fixed_bps", slippage_bps=5.0)
    bars = _bars()
    outs = []
    for _ in range(2):
        strat = Planned([sid])
        bt.set_strategy(strat)
        bt.run_bars(symbol="AAA", **bars)
        outs.append(strat.fills)
    assert outs[0] == outs[1]
    assert outs[0] == _by_hand(lambda ex: ex.set_default_slippage("fixed_bps", bps=5.0))


@pytest.mark.parametrize("kw, match", [
    ({"slippage_model": "fixed_tick"}, "slippage_model"),
    ({"slippage_model": "fixed_ticks"}, "slippage_ticks"),
    ({"slippage_model": "fixed_ticks", "slippage_ticks": 2}, "slippage_tick_size"),
    ({"slippage_model": "fixed_ticks", "slippage_ticks": -1, "slippage_tick_size": 0.01}, "slippage_ticks"),
    ({"slippage_model": "fixed_ticks", "slippage_ticks": 2, "slippage_tick_size": 0.01,
      "slippage_bps": 3.0}, "slippage_bps"),
    ({"slippage_model": "fixed_bps"}, "slippage_bps"),
    ({"slippage_model": "fixed_bps", "slippage_bps": -1.0}, "slippage_bps"),
    ({"slippage_model": "fixed_bps", "slippage_bps": float("nan")}, "slippage_bps"),
    ({"slippage_model": "fixed_bps", "slippage_bps": 2.0, "slippage_ticks": 1}, "slippage_ticks"),
    ({"slippage_model": "volume_impact"}, "slippage_impact_coeff"),
    ({"slippage_bps": 2.0}, "slippage_model"),
    ({"slippage_ticks": 2}, "slippage_model"),
    ({"submit_ack_latency_ns": -1}, "submit_ack_latency_ns"),
    ({"submit_ack_jitter_ns": -1}, "submit_ack_jitter_ns"),
    ({"cancel_ack_latency_ns": -5}, "cancel_ack_latency_ns"),
    ({"replace_ack_jitter_ns": -5}, "replace_ack_jitter_ns"),
])
def test_invalid_settings_raise(kw, match):
    reg, _ = _registry()
    with pytest.raises(lrvx.LrvxError, match=match):
        lrvx.BacktestRunner(reg, 0.0004, 10_000.0, **kw)


@pytest.mark.parametrize("setter", [
    lambda ex, model: ex.set_default_slippage(model, ticks=1, tick_size=0.01),
    lambda ex, model: ex.set_symbol_slippage(1, model, ticks=1, tick_size=0.01),
])
def test_executor_slippage_setters_refuse_an_unknown_model(setter):
    """The hand-driven executor takes the runner's model names, and refuses the rest
    the way the runner does rather than reading them as 'none'."""
    ex = lrvx.SimulatedExecutor()
    with pytest.raises(lrvx.LrvxError) as err:
        setter(ex, "fixed_tick")
    msg = str(err.value)
    for name in ("'none'", "'fixed_ticks'", "'fixed_bps'", "'volume_impact'", "'fixed_tick'"):
        assert name in msg, msg
    # the refused call left the executor without slippage, as it was
    ex.on_best_levels(1, 100.0, 10.0, 100.0, 10.0)
    ex.submit_order(1, "buy", 0.0, 1.0, "market", 1)
    assert ex.fills()[0]["price_raw"] == 100 * lrvx.PRICE_SCALE
    setter(ex, "fixed_ticks")
    ex.submit_order(2, "buy", 0.0, 1.0, "market", 1)
    assert ex.fills()[1]["price_raw"] == round(100.01 * lrvx.PRICE_SCALE)


def test_latency_must_be_a_latency_model():
    reg, _ = _registry()
    with pytest.raises(TypeError):
        lrvx.BacktestRunner(reg, latency=5_000_000)


def test_new_settings_are_keyword_only():
    reg, _ = _registry()
    with pytest.raises(TypeError):
        lrvx.BacktestRunner(reg, 0.0004, 10_000.0, "fixed_bps")


def test_volume_impact_is_refused_where_there_is_no_depth():
    """Bars and trade-only replays carry no book, so the model has no denominator."""
    reg, sid = _registry()
    bt = lrvx.BacktestRunner(reg, slippage_model="volume_impact", slippage_impact_coeff=0.5)
    bt.set_strategy(Planned([sid]))
    with pytest.raises(lrvx.LrvxError, match="volume_impact"):
        bt.run_bars(symbol="AAA", **_bars())
    with pytest.raises(lrvx.LrvxError, match="volume_impact"):
        bt.run_ohlcv(_bars()["end_time_ns"], _bars()["close"], symbol="AAA")


def test_settings_conflict_with_an_external_executor():
    reg, _ = _registry()
    bt = lrvx.BacktestRunner(reg, slippage_model="fixed_bps", slippage_bps=1.0)
    stack = lrvx.VenueStack.binance_um_futures(account_id=42, equity=10_000.0)
    with pytest.raises(lrvx.LrvxError, match="set_venue_stack"):
        bt.set_venue_stack(stack)
    bt2 = lrvx.BacktestRunner(reg, submit_ack_latency_ns=1_000)
    with pytest.raises(lrvx.LrvxError, match="set_venue_stack"):
        bt2.set_venue_stack(stack)
    # a runner left at the defaults still takes a stack
    lrvx.BacktestRunner(reg).set_venue_stack(stack)
