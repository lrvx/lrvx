"""Attach a venue rate-limit policy to the simulated executor."""
import lrvx

exec = lrvx.SimulatedExecutor()

# Use a canned profile.
policy = lrvx.RateLimitPolicy.binance_um_futures()
exec.set_rate_limit_policy(policy)

# Or build one manually:
custom = lrvx.RateLimitPolicy()
custom.add_bucket(name="orders_10s", window_ns=10_000_000_000, capacity=50)
custom.add_bucket(name="orders_60s", window_ns=60_000_000_000, capacity=300)
custom.set_ban(after_consecutive_rejects=3, ban_duration_ns=180_000_000_000)
exec.set_rate_limit_policy(custom)

# Inspect remaining capacity per bucket.
now_ns = 5_000_000_000
for s in custom.bucket_states(now_ns):
    print(f"{s['name']}: used={s['used']} / {s['capacity']}")
