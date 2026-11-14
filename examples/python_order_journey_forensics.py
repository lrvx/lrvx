"""Read OrderJourneyTracer analytics from Python.

Wiring note: in C++ `OrderJourneyTracer` is an `IOrderExecutionListener`
fed by `onOrderEvent` (see tests/test_order_journey_tracer.cpp). The
Python class exposes the collector and its analytics, but it is not an
`ExecutionListener` subclass, so `BacktestRunner.add_execution_listener`
does not accept it -- attach the tracer on the C++ side and read the
results here.
"""
import lrvx

tracer = lrvx.OrderJourneyTracer(
    max_orders=10_000,
    max_records_per_order=64,
    sample_rate=1.0,
)

# Every recorded step, as one structured numpy array.
rows = tracer.result()
print("columns:", rows.dtype.names)

# Inspect a single order.
trace = tracer.journey(order_id=42)
for row in trace:
    print(row["seq"], row["status"], row["ts_ns"],
          row["queue_ahead"], row["is_maker"])

# Aggregate analytics. On an empty trace the ratios return NaN.
print("orders:", tracer.order_count())
print("records:", tracer.record_count())
print("median ack latency:", tracer.median_ack_latency_ns(), "ns")
print("median time to first fill:",
      tracer.median_time_to_first_fill_ns(), "ns")
print("maker fill ratio:", tracer.maker_fill_ratio())
print("cancel race loss rate:", tracer.cancel_race_loss_rate())
