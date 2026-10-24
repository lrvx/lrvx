"""Use FOK / IOC / GTD / reduce-only with SimulatedExecutor."""
import lrvx

exec = lrvx.SimulatedExecutor()
exec.set_queue_model("tob", 1)
exec.on_best_levels(symbol=1, bid_price=50_000.0, bid_qty=5.0,
                    ask_price=50_001.0, ask_qty=5.0)

# FOK: fully fill or reject. Asks 5.0 available, request 1.0 → fills.
exec.submit_order(id=1, side="buy", price=50_001.0, quantity=1.0,
                  type="limit", symbol=1, tif="fok")

# IOC: take what crosses now, cancel remainder.
exec.submit_order(id=2, side="buy", price=50_001.0, quantity=0.5,
                  type="limit", symbol=1, tif="ioc")

# GTD: rests like GTC but auto-cancels at the absolute deadline.
expires = 5_000_000_000  # 5 seconds since simulator start
exec.submit_order(id=3, side="buy", price=49_500.0, quantity=1.0,
                  type="limit", symbol=1, tif="gtd",
                  expires_at_ns=expires)

# reduce_only: rejects if it would open / grow the position. After the
# two buys above we are long 1.5; a reduce-only sell shrinks toward
# flat. A reduce-only buy would be rejected.
exec.submit_order(id=4, side="sell", price=50_000.0, quantity=0.5,
                  type="limit", symbol=1, tif="ioc", reduce_only=True)
