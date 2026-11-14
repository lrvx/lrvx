"""
Three questions to ask a parameter sweep before trusting its winner.

The sweep here is synthetic so the script runs anywhere: 24 parameter
combinations laid out on a 4 x 6 grid, each with 512 periods of returns.
One region of the grid carries a real edge, the rest is noise.

Usage:
    cd /path/to/lrvx
    PYTHONPATH=build/python python3 docs/examples/python_robustness_statistics.py
"""

import numpy as np

import lrvx

rng = np.random.default_rng(2026)
SHAPE = (4, 6)
PERIODS = 512

# Per-period returns of every combination, row-major over the grid: trial
# index = row * 6 + col. The edge sits in rows 1-2, columns 2-4.
returns = rng.standard_normal((SHAPE[0] * SHAPE[1], PERIODS)) * 0.01
for row in (1, 2):
    for col in (2, 3, 4):
        returns[row * SHAPE[1] + col] += 0.0015

sharpes = returns.mean(axis=1) / returns.std(axis=1, ddof=1)
best = int(np.argmax(sharpes))

# 1. Is the winner of the sweep a fit to the sample? PBO is the fraction of
#    in-sample / out-of-sample splits where the in-sample best lands below
#    the out-of-sample median. Near 0.5 means the selection is a coin toss.
pbo = lrvx.pbo_cscv(returns, num_blocks=8)
print(f"pbo={pbo['pbo']:.3f} over {pbo['splits']} splits, "
      f"mean out-of-sample rank of the in-sample best {pbo['mean_oos_rank']:.2f}")

# 2. Is the winner's Sharpe ratio above what the best of 24 unskilled trials
#    would show? DSR is the probability that it is, after skewness, kurtosis
#    and sample length.
dsr = lrvx.deflated_sharpe(returns[best], trial_sharpes=sharpes)
print(f"best trial {best}: sharpe={dsr['sharpe']:.3f} per period, "
      f"expected max of {len(sharpes)} unskilled trials {dsr['expected_max_sharpe']:.3f}, "
      f"dsr={dsr['dsr']:.3f}")

# 3. Does the winner sit on a plateau or on a spike? The neighbourhood mean
#    of a grid point is the metric averaged over its adjacent cells.
grid = lrvx.grid_neighborhood(sharpes.reshape(SHAPE), radius=1)
row, col = divmod(best, SHAPE[1])
own = sharpes[best]
around = grid["neighbor_mean"][row, col]
print(f"best point ({row}, {col}): own sharpe {own:.3f}, "
      f"neighbours ({grid['neighbor_count'][row, col]}) average {around:.3f}")
for r in range(SHAPE[0]):
    print("  " + " ".join(f"{sharpes[r * SHAPE[1] + c]:+.2f}" for c in range(SHAPE[1])))
