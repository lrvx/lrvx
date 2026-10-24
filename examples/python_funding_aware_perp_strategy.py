"""Funding-aware perp backtest — settle funding at every interval boundary.

Perpetual futures pay funding at fixed intervals (8h on Binance UM,
configurable per venue). A position-flat strategy should account for
funding settlements when computing PnL, or the backtest systematically
underestimates the holding cost.

Demonstrates:
- VenueStack venue factory wires the FundingSchedule
- FundingSchedule.tick advances internal lastTickNs and emits payments
- Each payment has sign based on (position_signed × rate)
"""

import lrvx

BTC = 1


def main():
    stack = lrvx.VenueStack.binance_um_futures(account_id=1, equity=10_000.0)
    acct = stack.account()
    funding = stack.funding()

    # 0.05 BTC long perp at 50k.
    acct.open_position(symbol=BTC, quantity=0.05, entry_price=50_000.0)

    total_funding = 0.0
    eight_hours_ns = 8 * 3600 * 1_000_000_000
    # Walk 5 funding intervals (~40h).
    for boundary in range(1, 6):
        ts = boundary * eight_hours_ns
        # FundingSchedule emits a payment per (symbol, position, mark) at
        # every interval boundary in (lastTickNs, ts]. Sign convention:
        # positive funding rate → long pays → amount negative.
        payments = funding.tick(
            ts, [BTC], [0.05], [50_000.0 + 100 * boundary])
        for p in payments:
            print(f"funding[{boundary}] symbol={p.symbol} rate={p.rate:.6f} "
                  f"amount={p.amount:.2f}")
            total_funding += p.amount
            acct.add_equity(p.amount)

    print(f"total funding paid/received: {total_funding:.2f}")
    print(f"equity after funding: {acct.equity():.2f}")


if __name__ == "__main__":
    main()
