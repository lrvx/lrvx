"""Submit a native bracket order (entry + take-profit + stop) and walk its states."""
import lrvx

exec = lrvx.SimulatedExecutor()

# Buy at 100, take profit at 110, stop at 90.
exec.submit_bracket(bracket_id=1, symbol=1,
                     entry_side="buy", entry_type="limit",
                     entry_price=100.0, quantity=1.0,
                     tp_side="sell", tp_type="limit", tp_price=110.0,
                     stop_side="sell", stop_type="stop_market",
                     stop_trigger_price=90.0)
print("state after submit:", exec.bracket_state(1))

# Drop to 100; entry fills, TP + stop are now armed.
exec.on_bar(1, 100.0)
print("state after entry fill:", exec.bracket_state(1))

# Rally to 110; TP fills, stop is cancelled.
exec.on_bar(1, 110.0)
print("state after take-profit:", exec.bracket_state(1))
