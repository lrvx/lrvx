"""Calibrate a LiveQueuePositionEstimator against synthetic ground truth."""
import math
import tempfile
from pathlib import Path

import lrvx

estimator = lrvx.LiveQueuePositionEstimator()
cal = lrvx.LiveQueueCalibrator(estimator)

# Synthetic ground truth following a known model:
#   ground = estimator * exp(-elapsed/half_life) * shrink ** events
TRUE_HALF_LIFE_NS = 60_000_000_000  # 60s
TRUE_SHRINK = 0.85
for i in range(200):
    estimator_value = 100.0 + (i % 7) * 5.0
    elapsed = (i % 60) * 1_000_000_000
    events = i % 5
    decay = math.exp(-float(elapsed) / float(TRUE_HALF_LIFE_NS))
    ground = estimator_value * decay * (TRUE_SHRINK ** events)
    cal.record_sample(order_id=i, estimator_value=estimator_value,
                       ground_truth=ground, elapsed_ns=elapsed,
                       shrink_events=events)

result = cal.fit()
print(f"fitted half_life_ns = {result.half_life_ns:_}")
print(f"fitted shrink_factor = {result.shrink_factor}")
print(f"residual_rmse        = {result.residual_rmse:.6f}")

# Push into the live estimator, then ship the parameters to disk.
cal.apply(result)
with tempfile.TemporaryDirectory() as d:
    path = Path(d) / "live_queue.json"
    cal.export(path)
    loaded = lrvx.LiveQueueCalibrator.load(path)
    print(f"roundtrip: half_life = {loaded.half_life_ns}, "
          f"shrink = {loaded.shrink_factor}")
