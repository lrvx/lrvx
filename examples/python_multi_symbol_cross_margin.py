"""Multi-symbol cross-margin backtest with on_marks + stale-mark guard.

A profitable BTC short backs a losing ETH long via shared account
equity. Demonstrates the atomic multi-symbol mark update path
and the stale-mark guard that catches silent footguns where the
caller forgets to refresh one symbol's mark before walking
liquidations.

Exercises:
- VenueStack factory
- Cross-margin Account
- on_marks atomic multi-symbol update
- Stale-mark guard for liquidation safety
"""

import lrvx

BTC = 1
ETH = 2


def main():
    stack = lrvx.VenueStack.binance_um_futures(account_id=7, equity=50_000.0)
    acct = stack.account()
    liq = stack.liquidation()

    # BTC short profits as the market drops; ETH long bleeds.
    acct.open_position(symbol=BTC, quantity=-0.5, entry_price=50_000.0)
    acct.open_position(symbol=ETH, quantity=10.0, entry_price=3_000.0)

    # 1-minute budget for stale marks. Anything older flags as stale.
    stale_budget_ns = 60 * 1_000_000_000

    for i in range(10):
        ts = i * 5 * 60 * 1_000_000_000  # 5-minute ticks

        # Refresh BOTH symbols atomically. The on_marks call updates
        # every attached account's marks before walking, so the
        # cross-margin check sees both fresh prices in one pass.
        marks = [
            (BTC, 50_000.0 - 200.0 * i),  # BTC dropping
            (ETH, 3_000.0 - 30.0 * i),    # ETH dropping
        ]

        # on_marks refreshes every attached account's marks and walks
        # the liquidation check in one pass, so the freshness assertion
        # belongs after it: before the first call no mark has ever been
        # set and every position looks stale by construction.
        out = liq.on_marks(marks, ts_ns=ts)

        # Both symbols were just stamped with ts, so nothing is stale.
        # This fires only if a symbol was left out of the marks list.
        if acct.has_stale_marks(now_ns=ts, budget_ns=stale_budget_ns):
            raise RuntimeError(f"tick {i}: stale marks; refresh before walking")
        print(f"tick {i}: liq={out['liquidations_count']} "
              f"upnl={acct.total_unrealised_pnl():.2f}")

    print(f"final equity: {acct.equity():.2f}")
    print(f"final positions: {acct.position_count()}")


if __name__ == "__main__":
    main()
