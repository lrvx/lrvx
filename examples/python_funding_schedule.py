"""Apply perpetual funding to open positions during a backtest tick loop."""
import lrvx

HOUR = 3600 * 1_000_000_000

# Canned 8-hour profile (Binance UM futures cadence).
sched = lrvx.FundingSchedule.binance_um_futures()
sched.set_constant_rate(0.0001)  # 0.01% per 8h

# Or recorded tape from an archive:
# sched = lrvx.FundingSchedule.tape([(t0, 0.00012), (t0 + 8*HOUR, -0.00005), ...])

# Strategy tick loop:
# - feed current per-symbol position + mark price
# - schedule emits one event per (symbol, boundary) crossed since last call
# - amount is signed: positive = received, negative = paid

events = sched.tick(now_ns=9 * HOUR,
                    symbols=[1, 2],            # BTC, ETH
                    positions=[1.0, -2.0],     # long 1 BTC, short 2 ETH
                    mark_prices=[50_000.0, 3_000.0])

for ev in events:
    print(f"t={ev.timestamp_ns:>20}  sym={ev.symbol}  "
          f"rate={ev.rate:.5f}  amount={ev.amount:+.4f}")
