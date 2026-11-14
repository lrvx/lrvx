"""Submit a native iceberg order and watch its hidden remainder drain."""
import lrvx

exec = lrvx.SimulatedExecutor()

# Instant refresh is the default; set non-zero for venues with refresh delay.
exec.set_iceberg_refresh_latency(0)

# 10 BTC total, 2 BTC visible per slice.
exec.submit_iceberg(order_id=1, side="buy", price=100.0,
                     total_quantity=10.0, visible_quantity=2.0)

hidden = exec.iceberg_hidden_remaining_raw(1)
print(f"after submit: hidden remainder raw = {hidden}")
assert hidden > 0

# Cancel cleans up both visible and hidden portions in one call.
exec.cancel_order(1)
print(f"after cancel: hidden remainder = {exec.iceberg_hidden_remaining_raw(1)}")
