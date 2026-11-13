"""Strings that name one of a fixed set of values are read strictly.

Across the bindings a side, a time in force, a cost basis, a mode was read
with a comparison and a fallback: anything but ``"buy"`` was a sell,
anything but ``"ioc"`` / ``"fok"`` / ``"gtd"`` / ``"post_only"`` a GTC
order, anything but ``"lifo"`` / ``"average"`` FIFO. A typo did something
nobody asked for and said nothing. Each now raises ``ValueError`` naming the
value.
"""
from __future__ import annotations

import pytest

import lrvx

BAD_SIDES = ["Buy", "BUY", "long", "", "sel"]


def _names(excinfo, value):
    return f"'{value}'" in str(excinfo.value)


# ── side ─────────────────────────────────────────────────────────────


@pytest.mark.parametrize("side", BAD_SIDES)
def test_simulated_executor_submit_order_refuses_an_unknown_side(side):
    sim = lrvx.SimulatedExecutor()
    with pytest.raises(ValueError) as excinfo:
        sim.submit_order(1, side, 100.0, 1.0, type="limit", symbol=1)
    assert _names(excinfo, side)
    assert sim.fill_count == 0


def test_simulated_executor_bracket_and_iceberg_refuse_an_unknown_side():
    sim = lrvx.SimulatedExecutor()
    with pytest.raises(ValueError, match="'long'"):
        sim.submit_bracket(1, 1, "long", "limit", 100.0, 1.0, "sell", "limit", 110.0,
                           "sell", "stop_market", 90.0)
    with pytest.raises(ValueError, match="'short'"):
        sim.submit_bracket(1, 1, "buy", "limit", 100.0, 1.0, "short", "limit", 110.0,
                           "sell", "stop_market", 90.0)
    with pytest.raises(ValueError, match="'short'"):
        sim.submit_bracket(1, 1, "buy", "limit", 100.0, 1.0, "sell", "limit", 110.0,
                           "short", "stop_market", 90.0)
    with pytest.raises(ValueError, match="'Buy'"):
        sim.submit_iceberg(2, "Buy", 100.0, 10.0, 1.0, 1)


def test_venue_executor_refuses_an_unknown_side_and_tif():
    # The stack owns the executor, so it is kept alive beside it.
    stack = lrvx.VenueStack.binance_um_futures(account_id=1, equity=100_000.0)
    venue = stack.executor()
    with pytest.raises(ValueError, match="'Sell'"):
        venue.submit_order(1, "Sell", 100.0, 1.0, type="limit", symbol=1)
    with pytest.raises(ValueError, match="'day'"):
        venue.submit_order(1, "sell", 100.0, 1.0, type="limit", symbol=1, tif="day")


def test_backtest_result_record_fill_refuses_an_unknown_side():
    result = lrvx.BacktestResult()
    with pytest.raises(ValueError, match="'b'"):
        result.record_fill(1, 1, "b", 100.0, 1.0, 1_000, False)


def test_l3_book_refuses_an_unknown_side():
    book = lrvx.L3Book()
    with pytest.raises(ValueError, match="'bid'"):
        book.add_order(1, 100.0, 1.0, "bid")
    with pytest.raises(ValueError, match="'ask'"):
        book.build_from_snapshot([{"id": 1, "price": 100.0, "quantity": 1.0, "side": "ask"}])
    assert book.add_order(2, 100.0, 1.0, "buy") == "ok"


def test_position_trackers_refuse_an_unknown_side():
    tracker = lrvx.PositionTracker()
    with pytest.raises(ValueError, match="'long'"):
        tracker.on_fill(1, "long", 100.0, 1.0)
    assert tracker.position(1) == 0.0, "nothing was recorded as a sell"
    groups = lrvx.PositionGroupTracker()
    with pytest.raises(ValueError, match="'long'"):
        groups.open_position(1, 1, "long", 100.0, 1.0)


# ── time in force ────────────────────────────────────────────────────


@pytest.mark.parametrize("tif", ["GTC", "day", "", "post-only"])
def test_simulated_executor_submit_order_refuses_an_unknown_tif(tif):
    sim = lrvx.SimulatedExecutor()
    with pytest.raises(ValueError) as excinfo:
        sim.submit_order(1, "buy", 100.0, 1.0, type="limit", symbol=1, tif=tif)
    assert _names(excinfo, tif)


@pytest.mark.parametrize("tif", ["gtc", "ioc", "fok", "gtd", "post_only"])
def test_every_real_tif_is_still_taken(tif):
    sim = lrvx.SimulatedExecutor()
    sim.submit_order(1, "buy", 100.0, 1.0, type="limit", symbol=1, tif=tif,
                     expires_at_ns=10**18 if tif == "gtd" else 0)


# ── the other choices the sweep found ────────────────────────────────


def test_cost_basis_is_fifo_lifo_or_average():
    for name in ("fifo", "lifo", "average"):
        lrvx.PositionTracker(name)
    with pytest.raises(ValueError, match="'avg'"):
        lrvx.PositionTracker("avg")


def test_simulated_executor_modes_are_named():
    sim = lrvx.SimulatedExecutor()
    sim.set_stp_mode("none")
    sim.set_stp_mode("cancel_both")
    with pytest.raises(ValueError, match="'cancel_new'"):
        sim.set_stp_mode("cancel_new")
    sim.set_bracket_child_arm_mode("on_full_fill")
    sim.set_bracket_child_arm_mode("on_partial_fill")
    with pytest.raises(ValueError, match="'partial'"):
        sim.set_bracket_child_arm_mode("partial")


def test_rate_limit_action_is_named():
    policy = lrvx.RateLimitPolicy()
    policy.try_consume("submit", 0)
    with pytest.raises(ValueError, match="'submitt'"):
        policy.try_consume("submitt", 0)


def test_hidden_order_policy_is_named():
    estimator = lrvx.LiveQueuePositionEstimator()
    estimator.set_hidden_order_policy("ignore")
    with pytest.raises(ValueError, match="'trust'"):
        estimator.set_hidden_order_policy("trust")


def test_outage_names_are_checked():
    availability = lrvx.VenueAvailability()
    availability.schedule_outage_ex(0, 10, outage_type="total", on_open_orders="hold")
    with pytest.raises(ValueError, match="'partial'"):
        availability.schedule_outage_ex(0, 10, outage_type="partial")
    with pytest.raises(ValueError, match="'keep'"):
        availability.schedule_outage_ex(0, 10, on_open_orders="keep")


def test_walk_forward_mode_is_anchored_or_sliding():
    registry = lrvx.SymbolRegistry()
    registry.add_symbol("test", "BTC", 0.01)
    with pytest.raises(ValueError, match="'rolling'"):
        lrvx.WalkForwardRunner(registry, mode="rolling")


def test_segment_operations_name_their_modes(tmp_path):
    with pytest.raises(ValueError, match="'zstd'"):
        lrvx.merge([str(tmp_path / "in.lrvx")], str(tmp_path / "out"), compression="zstd")
    with pytest.raises(ValueError, match="'daily'"):
        lrvx.split(str(tmp_path / "in.lrvx"), str(tmp_path / "out"), mode="daily")
    with pytest.raises(ValueError, match="'parquet'"):
        lrvx.export_data(str(tmp_path / "in.lrvx"), str(tmp_path / "out"), format="parquet")
    with pytest.raises(ValueError, match="'gzip'"):
        lrvx.recompress(str(tmp_path / "in.lrvx"), str(tmp_path / "out.lrvx"),
                        compression="gzip")


# ── an unattached strategy still checks ──────────────────────────────


class Idle(lrvx.Strategy):
    pass


@pytest.mark.parametrize("method, args", [
    ("emit_stop_market", (1, "Buy", 100.0, 1.0)),
    ("emit_stop_limit", (1, "Buy", 100.0, 99.0, 1.0)),
    ("emit_take_profit_market", (1, "Buy", 100.0, 1.0)),
    ("emit_take_profit_limit", (1, "Buy", 100.0, 99.0, 1.0)),
    ("emit_trailing_stop", (1, "Buy", 1.0, 1.0)),
    ("emit_trailing_stop_percent", (1, "Buy", 100, 1.0)),
])
def test_an_unattached_strategy_refuses_an_unknown_side(method, args):
    """It returned 0, the answer for 'not attached', before it looked."""
    strategy = Idle([1])
    with pytest.raises(ValueError, match="'Buy'"):
        getattr(strategy, method)(*args)


@pytest.mark.parametrize("method", ["emit_limit_buy_tif", "emit_limit_sell_tif"])
def test_an_unattached_strategy_refuses_an_unknown_tif(method):
    strategy = Idle([1])
    with pytest.raises(ValueError, match="'day'"):
        getattr(strategy, method)(1, 100.0, 1.0, "day")
    with pytest.raises(ValueError, match="gtd"):
        getattr(strategy, method)(1, 100.0, 1.0, "gtd")


def test_an_unattached_strategy_with_good_arguments_still_returns_zero():
    strategy = Idle([1])
    assert strategy.emit_stop_market(1, "buy", 100.0, 1.0) == 0
    assert strategy.emit_limit_buy_tif(1, 100.0, 1.0, "ioc") == 0
    assert strategy.stop_market("sell", 100.0, 1.0) == 0
    with pytest.raises(ValueError, match="'Sell'"):
        strategy.stop_market("Sell", 100.0, 1.0)
    with pytest.raises(ValueError, match="'day'"):
        strategy.limit_buy(100.0, 1.0, tif="day")
