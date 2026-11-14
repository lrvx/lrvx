"""Enable self-trade prevention on the simulated executor."""
import lrvx

exec = lrvx.SimulatedExecutor()
exec.set_queue_model("tob", 1)

# Modes: 'none' (default) | 'cancel_newest' | 'cancel_oldest' |
#        'cancel_both'    | 'decrement'.
exec.set_stp_mode("cancel_newest")

exec.on_best_levels(symbol=1, bid_price=49_000, bid_qty=1.0,
                    ask_price=51_000, ask_qty=1.0)

# Rest a BUY @ 50500.
exec.submit_order(id=1, side="buy", price=50_500.0, quantity=1.0,
                  type="limit", symbol=1)

# Send a SELL @ 50000 — crosses our own BUY. STP rejects the new one.
exec.submit_order(id=2, side="sell", price=50_000.0, quantity=1.0,
                  type="limit", symbol=1)
print("done — incoming SELL was rejected with reason 'stp_cancel_newest'")
