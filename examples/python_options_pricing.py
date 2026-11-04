"""Price European options and back out implied volatility.

Generalized Black-Scholes-Merton: one model covers crypto (rate=carry=0),
Black-76 futures options (carry=0), and dividend-stock / FX cases by varying
the cost-of-carry. Run from repo root:

    PYTHONPATH=build/python python3 docs/examples/python_options_pricing.py
"""
import math

import lrvx

# Crypto: BTC 30-day ATM call, priced off the forward (rate=carry=0 default).
call = lrvx.bs_price(lrvx.OptionType.CALL, spot=70000, strike=70000, t=30 / 365, vol=0.60)
put = lrvx.bs_price(lrvx.OptionType.PUT, spot=70000, strike=70000, t=30 / 365, vol=0.60)
assert call > 0 and abs(call - put) < 1e-3  # ATM, zero carry -> symmetric

# Equity: one-year call on a non-dividend stock at a 5% rate (carry = rate).
eq_call = lrvx.bs_price(
    lrvx.OptionType.CALL, spot=100, strike=100, t=1.0, vol=0.20, rate=0.05, carry=0.05
)
assert abs(eq_call - 10.4506) < 1e-3

# Back out implied vol from a quoted price.
result = lrvx.implied_vol(
    lrvx.OptionType.CALL, price=eq_call, spot=100, strike=100, t=1.0, rate=0.05, carry=0.05
)
assert result["converged"] and abs(result["vol"] - 0.20) < 1e-4

# A price above the no-arbitrage asset bound has no implied vol.
bad = lrvx.implied_vol(lrvx.OptionType.CALL, price=200.0, spot=100, strike=100, t=1.0)
assert not bad["converged"] and math.isnan(bad["vol"])

# Vega (per 1.0 vol change) and the forward price.
vega = lrvx.bs_vega(spot=100, strike=100, t=1.0, vol=0.20, rate=0.05, carry=0.05)
fwd = lrvx.forward_price(spot=100, t=1.0, carry=0.05)
assert vega > 0 and fwd > 100

print(f"crypto call={call:.2f} equity call={eq_call:.4f} iv={result['vol']:.4f} vega={vega:.4f}")
