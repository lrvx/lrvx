"""Liquidation cascade demo: insurance fund drains, then ADL fires."""
import lrvx

eng = lrvx.LiquidationEngine()
eng.add_tier(0.0, 0.005)
eng.set_insurance_fund_capital(1000.0)
eng.set_adl_enabled(True)
eng.set_liquidation_slippage_bps(0.0)

# Underwater long: 10 BTC @ 100, equity 50; at mark 40 → PnL -600, deficit 550.
eng.open_position(account_id=1, symbol=1, quantity=10.0,
                   entry_price=100.0, equity=50.0)
# Profitable short cohort: small one + large one.
eng.open_position(account_id=2, symbol=1, quantity=-5.0,
                   entry_price=100.0, equity=100.0)
eng.open_position(account_id=3, symbol=1, quantity=-10.0,
                   entry_price=100.0, equity=100.0)

# Insurance fund only has 1000 — it absorbs the 550 deficit fully here
# because the cascade is small. To trigger ADL, drain the fund first
# or lower the cap.
out = eng.on_mark(symbol=1, mark_price=40.0)
print("liquidated   :", out["liquidated"])
print("adl_closeouts:", out["adl_closed_out"])
print("fund delta   :", out["insurance_fund_delta"])
print("fund balance :", eng.insurance_fund_balance())

# Drain the insurance fund explicitly, then re-run to see ADL fire.
eng.set_insurance_fund_capital(0.0)
eng.open_position(account_id=4, symbol=1, quantity=10.0,
                   entry_price=100.0, equity=50.0)
out2 = eng.on_mark(symbol=1, mark_price=40.0)
print("\nsecond run (fund depleted):")
print("liquidated   :", out2["liquidated"])
print("adl_closeouts:", out2["adl_closed_out"])
print("cumulative ADL count:", eng.adl_closeouts_count())
