"""Switch a SimulatedExecutor between FIFO, pure pro-rata, and hybrid matching."""
import lrvx

exec = lrvx.SimulatedExecutor()

# Pure pro-rata: every order at the level shares the trade by size.
exec.set_queue_model("pro_rata", depth=4)

# Hybrid: first 3 orders consume the trade FIFO, rest split pro-rata.
exec.set_queue_model("pro_rata_with_fifo", depth=4)
exec.set_queue_fifo_top_n(3)

# Back to default FIFO at the top of book.
exec.set_queue_model("tob", depth=1)

print("queue model setters ok")
