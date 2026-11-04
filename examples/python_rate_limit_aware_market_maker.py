"""Rate-limit-aware market maker — respects the venue's per-endpoint budget.

Real venues split rate budgets per endpoint family (trading vs market
data vs account queries). A naive quote-cancel-replace loop bursts
through the trading budget and produces a stream of REJECTED_RATE_LIMIT
events. This recipe shows how to drive that scenario against the venue
stack and inspect the resulting fills.

Demonstrates:
- VenueStack ships a venue-realistic RateLimitPolicy
- Per-endpoint budgets (Trading / MarketData / Account family)
- The VenueExecutor surface returned by stack.executor()
"""

import lrvx

BTC = 1


def main():
    stack = lrvx.VenueStack.binance_um_futures(account_id=1, equity=10_000.0)
    # RateLimitPolicy is wired by the venue factory and attached to the
    # executor. Submits past the budget surface as REJECTED_RATE_LIMIT
    # order events; the executor records fills only for accepted ones.

    exec_ = stack.executor()

    # Feed an initial trade so the matching engine has a price reference.
    exec_.on_trade_qty(BTC, 50_000.0, 10.0, True)

    # A quoting loop that issues a limit order every tick. Real market
    # makers do this thousands of times per second; without rate
    # awareness the venue will ban the account.
    for tick in range(200):
        side = "buy" if tick % 2 == 0 else "sell"
        price = 50_000.0 - tick * 0.5 if side == "buy" else 50_000.0 + tick * 0.5
        exec_.submit_order(
            id=tick + 1,
            side=side,
            price=price,
            quantity=0.001,
            type="limit",
            symbol=BTC,
            tif="gtc",
        )

    print(f"fills recorded: {exec_.fill_count}")
    # Venue policy is shaped after Binance UM's published limits — at
    # this cadence many submits typically get rejected before they
    # reach the book, so fill_count stays well below 200.


if __name__ == "__main__":
    main()
