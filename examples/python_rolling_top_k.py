"""Sliding top-K threshold round-trip — generate a synthetic price
series, derive a per-bar metric, compute the rolling K-th largest
value across the trailing window, and print a few entries to confirm
the helper does what the doc claims.

This is the CI-runnable companion to
[Rolling top-K thresholds](../how-to/rolling-thresholds.md). It runs
without lrvx's C extension because ``lrvx.rolling`` is pure
numpy — the import still goes through the package so the example
also smoke-tests that the module ships.

Usage:
    cd /path/to/lrvx
    PYTHONPATH=build/python python3 docs/examples/python_rolling_top_k.py
"""
from __future__ import annotations

import numpy as np

from lrvx import rolling


def main() -> None:
    rng = np.random.default_rng(0)
    n = 1_000
    # Synthetic per-bar metric: range in bps. Heavier tail so the
    # top-K threshold has a meaningful shape.
    metric = np.abs(rng.standard_t(df=4, size=n)) * 10.0

    window = 100   # last 100 bars
    k = 3          # 3rd-largest of the trailing window
    thr = rolling.top_k_threshold(metric, window=window, k=k)

    # Pick a bar past the warmup and confirm the helper matches what
    # the explicit np.partition call returns for the same window.
    i = 500
    win = metric[i - window : i]
    expected = float(np.partition(win, -k)[-k])
    got = float(thr[i])
    assert abs(got - expected) < 1e-12, (got, expected)
    print(
        f"top-{k} threshold over trailing {window} bars at bar {i}: "
        f"{got:.4f} (matches np.partition)."
    )

    # Number of bars whose metric exceeds the trailing K-th largest —
    # the kind of count an extreme-event filter cares about.
    fired = np.sum(metric[window:] >= thr[window:])
    print(f"bars firing the threshold: {int(fired)} / {n - window}")


if __name__ == "__main__":
    main()
