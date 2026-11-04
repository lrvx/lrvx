"""Track volume tiers and compute realistic fees per fill."""
import lrvx

# Canned profile: Binance UM futures 10-tier ladder.
sched = lrvx.FeeSchedule.binance_um_futures()

# Or build manually:
custom = lrvx.FeeSchedule()
custom.add_tier(min_notional_30d=0,         maker_bps=2.0, taker_bps=4.0)
custom.add_tier(min_notional_30d=1_000_000, maker_bps=1.0, taker_bps=3.0)
custom.add_tier(min_notional_30d=10_000_000, maker_bps=-0.5, taker_bps=2.0)

ts = 1_000_000_000

# A taker fill of 1000 USDT at the Regular tier costs 0.4 USDT.
fee = sched.fee_for(ts_ns=ts, notional=1000.0, is_maker=False)
print(f"taker fee @ tier 0: {fee:.4f} USDT")

# Push 1M notional through and re-query — tier advances, fee drops.
sched.record_fill(ts, 1_200_000.0)
fee2 = sched.fee_for(ts_ns=ts, notional=1000.0, is_maker=False)
print(f"after 1.2M volume: tier={sched.current_tier_index()}, fee={fee2:.4f}")

# Maker rebate kicks in at VIP-9.
sched.record_fill(ts, 1_000_000_000.0)
print(f"current tier={sched.current_tier_index()}, "
      f"rolling 30d notional={sched.rolling_notional_30d():,.0f}")

# Maker fill at top tier — negative number means received rebate.
maker_fee = sched.fee_for(ts_ns=ts, notional=10_000.0, is_maker=True)
print(f"maker rebate @ top tier: {maker_fee:+.4f} USDT")
