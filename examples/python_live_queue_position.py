"""Track a resting order's queue position from live trade + book events."""
import lrvx

est = lrvx.LiveQueuePositionEstimator()
est.set_confidence_half_life_ns(60_000_000_000)  # 60s decay
est.set_shrink_attribution_factor(0.85)

# Order placed: we joined a level with 2.0 already resting ahead.
SYM = 1
BUY, SELL = 0, 1
est.on_order_placed(symbol=SYM, side=BUY, price=50_000.0, order_id=42,
                    order_qty=0.5, level_qty_now=2.0, ts_ns=0)

# Trade tape: 1.0 unit prints at our level. The queue-ahead heuristic
# subtracts it.
est.on_trade(symbol=SYM, price=50_000.0, qty=1.0, ts_ns=1_000_000_000)

# Book update: level shrank from 1.5 -> 1.0 without an explaining trade.
# Treated as a cancellation attribution; confidence drops one notch.
est.on_level_update(symbol=SYM, side=BUY, price=50_000.0, new_qty=1.0,
                    ts_ns=2_000_000_000)

snap = est.snapshot(order_id=42, now_ns=2_000_000_000)
assert snap is not None
print(f"queue_ahead_est={snap['queue_ahead_est']:.4f}  "
      f"confidence={snap['confidence']:.3f}  "
      f"last_update_ns={snap['last_update_ns']}")
