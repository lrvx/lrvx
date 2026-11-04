"""Configure hidden / iceberg order attribution on the live queue estimator."""
import lrvx

est = lrvx.LiveQueuePositionEstimator()

# Mode A: trust the venue's per-trade is_hidden flag.
est.set_hidden_order_policy("trust_trade_flag")

est.on_order_placed(symbol=1, side=0, price=50_000.0, order_id=42,
                    order_qty=0.5, level_qty_now=2.0, ts_ns=0)

# Visible trade — deducts queue.
est.on_trade_with_flag(symbol=1, price=50_000.0, qty=0.5,
                       ts_ns=1_000_000_000, is_hidden=False)
# Hidden trade — accumulator only, no queue deduction.
est.on_trade_with_flag(symbol=1, price=50_000.0, qty=1.0,
                       ts_ns=2_000_000_000, is_hidden=True)

snap = est.snapshot(42, now_ns=2_000_000_000)
assert snap is not None
print(f"queue_ahead_est={snap['queue_ahead_est']:.3f}  "
      f"hidden_volume_seen={snap['hidden_volume_seen']:.3f}  "
      f"confidence={snap['confidence']:.3f}")

# Mode B: infer when trade volume exceeds the cached visible total.
est2 = lrvx.LiveQueuePositionEstimator()
est2.set_hidden_order_policy("infer_if_trade_exceeds_visible")
est2.on_order_placed(symbol=1, side=0, price=50_000.0, order_id=99,
                     order_qty=0.5, level_qty_now=2.0, ts_ns=0)
# Trade reports 5.0 — visible was 2.0, excess 3.0 inferred hidden.
est2.on_trade(symbol=1, price=50_000.0, qty=5.0, ts_ns=1_000_000_000)
snap2 = est2.snapshot(99, now_ns=1_000_000_000)
assert snap2 is not None
print(f"inferred hidden_volume_seen={snap2['hidden_volume_seen']:.3f}")
