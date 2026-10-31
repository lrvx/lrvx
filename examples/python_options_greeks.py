"""Compute option greeks (first and second order).

Run from repo root:

    PYTHONPATH=build/python python3 docs/examples/python_options_greeks.py
"""
import lrvx

# First-order greeks for a one-year equity call (rate=carry=0.05).
g = lrvx.greeks(lrvx.OptionType.CALL, spot=100, strike=100, t=1.0, vol=0.20, rate=0.05, carry=0.05)
assert g["delta"] > 0          # call delta is positive
assert g["gamma"] > 0          # gamma is positive for long options
assert g["vega"] > 0
assert g["theta"] < 0          # long option decays (per year; /365 for per-day)

# A put on the same contract has negative delta and identical gamma/vega.
gp = lrvx.greeks(lrvx.OptionType.PUT, spot=100, strike=100, t=1.0, vol=0.20, rate=0.05, carry=0.05)
assert gp["delta"] < 0
assert abs(gp["gamma"] - g["gamma"]) < 1e-12

# Delta-hedge ratio: a long call position is hedged by shorting `delta` units of
# the underlying per contract.
hedge_units = -g["delta"]
assert hedge_units < 0

# Second-order greeks for vol trading.
s = lrvx.second_order_greeks(
    lrvx.OptionType.CALL, spot=100, strike=100, t=1.0, vol=0.20, rate=0.05, carry=0.05
)
assert "vanna" in s and "volga" in s and "charm" in s

# Crypto default (rate=carry=0).
gc = lrvx.greeks(lrvx.OptionType.CALL, spot=70000, strike=70000, t=30 / 365, vol=0.6)
assert gc["delta"] > 0 and gc["vega"] > 0

print(f"delta={g['delta']:.4f} gamma={g['gamma']:.5f} vega={g['vega']:.2f} theta={g['theta']:.2f}")
