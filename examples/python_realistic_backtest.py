"""Realistic Binance UM perp backtest assembled in one factory call.

Demonstrates the venue stack pattern: every venue subsystem
(executor, account, liquidation, fee schedule, funding schedule, rate
limits, venue availability) is wired with venue-realistic defaults in
a single `lrvx.VenueStack.binance_um_futures(...)` call.

Exercises:
- VenueStack factory
- Cross-margin account
- Multi-symbol marks update via on_marks
- Fee tier transitions driven by aggregate notional
- Funding settlements
"""

import lrvx

BTC = 1
ETH = 2


def main():
    stack = lrvx.VenueStack.binance_um_futures(account_id=42, equity=10_000.0)
    acct = stack.account()
    liq = stack.liquidation()
    fees = stack.fees()
    funding = stack.funding()

    # Open positions on two symbols (mix long / short for the cross
    # account to net out).
    acct.open_position(symbol=BTC, quantity=0.1, entry_price=50_000.0)
    acct.open_position(symbol=ETH, quantity=-2.0, entry_price=3_000.0)

    # Drive a handful of synthetic ticks.
    for i in range(20):
        ts = i * 30 * 60 * 1_000_000_000  # 30-minute spacing
        marks = [
            (BTC, 50_000.0 + 50.0 * i),
            (ETH, 3_000.0 - 5.0 * i),
        ]
        # on_marks does the atomic multi-symbol update and then
        # walks attached accounts for cross-margin liquidation.
        out = liq.on_marks(marks, ts_ns=ts)
        if out["liquidations_count"] > 0:
            print(f"tick {i}: liquidations={out['liquidations_count']}")

        # Record a fill so the 30d VIP tier progresses.
        fees.record_fill(ts_ns=ts, notional=20_000.0)

    print(f"venue: {stack.venue_name()}")
    print(f"equity: {acct.equity():.2f}")
    print(f"total_upnl: {acct.total_unrealised_pnl():.2f}")
    print(f"rolling_30d: {acct.rolling_notional_30d():.0f}")
    print(f"fee_tier: {fees.current_tier_index()}")
    print(f"position_count: {acct.position_count()}")

    # Settle outstanding funding payments for the period.
    payments = funding.tick(
        20 * 30 * 60 * 1_000_000_000,
        [BTC, ETH],
        [acct.position_count() and 0.1 or 0.0, -2.0],
        [50_000.0, 3_000.0],
    )
    print(f"funding_payments: {len(payments)}")


if __name__ == "__main__":
    main()
