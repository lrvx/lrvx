"""Apply a heavy-tailed, burst-correlated ack-latency distribution."""
import lrvx

exec = lrvx.SimulatedExecutor()

# Lognormal: median 5ms, sigma 0.7 → p99 ~ 50ms tail.
dist = lrvx.LatencyDistribution.lognormal(median_ns=5_000_000, sigma=0.7)
# Couple successive draws via AR(1): when the venue is slow, the
# next ack tends to be slow too.
dist.set_burst_correlation(0.4)

exec.set_submit_ack_latency_distribution(dist)
exec.set_cancel_ack_latency_distribution(
    lrvx.LatencyDistribution.lognormal(median_ns=8_000_000, sigma=0.6))
exec.set_replace_ack_latency_distribution(
    lrvx.LatencyDistribution.lognormal(median_ns=12_000_000, sigma=0.6))

# Empirical: resample from an observed histogram.
recorded = [1_000_000, 1_500_000, 2_000_000, 3_000_000, 4_500_000,
            6_000_000, 9_000_000, 15_000_000, 35_000_000, 80_000_000]
empirical = lrvx.LatencyDistribution.empirical(recorded)
exec.set_submit_ack_latency_distribution(empirical)

assert dist.median_ns() == 5_000_000
