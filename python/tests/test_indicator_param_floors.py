"""Every registry indicator refuses integer arguments below their floor.

The floors are include/lrvx/indicator/params.h: a period of at least 1, 2
for a rolling standard deviation, correlation, entropy or choppiness, 3 for
skewness, 4 for kurtosis. Below it the constructor and the batch function
both raise LrvxError E_IND_001. Before, only SMA/RMA/Bollinger/CCI (and
VWAP) did; the others returned an all-NaN array, RSI(0) and KAMA with a zero
fast or slow length returned numbers, and ShannonEntropy with 0 bins crashed
the interpreter. At the floor nothing changes.
"""

from __future__ import annotations

import unittest

import numpy as np

import lrvx

N = 40
_rng = np.random.default_rng(7)
C = 100 + np.cumsum(_rng.standard_normal(N))
H = C + 1
L = C - 1
O = C + 0.3

# name: (class, class args at the floor, batch(args) at the floor or None,
#        index of each integer argument in the class args)
ROWS = {
    "ema": (lrvx.EMA, [1], lambda a: lrvx.ema(C, a[0])),
    "sma": (lrvx.SMA, [1], lambda a: lrvx.sma(C, a[0])),
    "rma": (lrvx.RMA, [1], lambda a: lrvx.rma(C, a[0])),
    "rsi": (lrvx.RSI, [1], lambda a: lrvx.rsi(C, a[0])),
    "kama": (lrvx.KAMA, [1, 1, 1], None),
    "dema": (lrvx.DEMA, [1], lambda a: lrvx.dema(C, a[0])),
    "tema": (lrvx.TEMA, [1], lambda a: lrvx.tema(C, a[0])),
    "slope": (lrvx.Slope, [1], lambda a: lrvx.slope(C, a[0])),
    "skewness": (lrvx.Skewness, [3], lambda a: lrvx.skewness(C, a[0])),
    "kurtosis": (lrvx.Kurtosis, [4], lambda a: lrvx.kurtosis(C, a[0])),
    "rolling_zscore": (lrvx.RollingZScore, [2], lambda a: lrvx.rolling_zscore(C, a[0])),
    "shannon_entropy": (lrvx.ShannonEntropy, [2, 2], lambda a: lrvx.shannon_entropy(C, a[0], a[1])),
    "autocorrelation": (lrvx.AutoCorrelation, [2, 1], lambda a: lrvx.autocorrelation(C, a[0], a[1])),
    "atr": (lrvx.ATR, [1], lambda a: lrvx.atr(H, L, C, a[0])),
    "cci": (lrvx.CCI, [1], lambda a: lrvx.cci(H, L, C, a[0])),
    "chop": (lrvx.CHOP, [2], lambda a: lrvx.chop(H, L, C, a[0])),
    "stochastic": (lrvx.Stochastic, [1, 1], lambda a: lrvx.stochastic(H, L, C, a[0], a[1])),
    "parkinson_vol": (lrvx.ParkinsonVol, [1], lambda a: lrvx.parkinson_vol(H, L, a[0])),
    "rogers_satchell_vol": (lrvx.RogersSatchellVol, [1],
                            lambda a: lrvx.rogers_satchell_vol(O, H, L, C, a[0])),
    "correlation": (lrvx.Correlation, [2], lambda a: lrvx.rolling_correlation(C, H, a[0])),
    "macd": (lrvx.MACD, [1, 1, 1], lambda a: lrvx.macd(C, a[0], a[1], a[2])),
    "bollinger": (lrvx.Bollinger, [1], lambda a: lrvx.bollinger(C, a[0], 2.0)),
}


class IndicatorParamFloorTests(unittest.TestCase):

    def test_every_registry_row_is_covered(self):
        self.assertEqual(len(lrvx.list_indicators()), len(ROWS))

    def _refused(self, fn, what):
        with self.assertRaises(lrvx.LrvxError, msg=what) as cm:
            fn()
        self.assertEqual(cm.exception.code, "E_IND_001", what)
        self.assertIn("must be at least", str(cm.exception), what)

    def test_below_the_floor_is_refused(self):
        for name, (cls, floor, batch) in ROWS.items():
            for k in range(len(floor)):
                bad = list(floor)
                bad[k] -= 1
                with self.subTest(row=name, arg=k):
                    self._refused(lambda: cls(*bad), f"{name} class {bad}")
                    if batch is not None:
                        self._refused(lambda: batch(bad), f"{name} batch {bad}")

    def test_kama_batch_refuses_a_zero_period(self):
        self._refused(lambda: lrvx.kama(C, 0), "kama batch 0")

    def test_at_the_floor_it_works(self):
        for name, (cls, floor, batch) in ROWS.items():
            with self.subTest(row=name):
                cls(*floor)
                if batch is not None:
                    batch(floor)

    def test_shannon_entropy_with_no_bins_does_not_crash(self):
        self._refused(lambda: lrvx.ShannonEntropy(10, 0), "ShannonEntropy(10, 0)")
        self._refused(lambda: lrvx.shannon_entropy(C, 10, 0), "shannon_entropy(c, 10, 0)")


if __name__ == "__main__":
    unittest.main()
