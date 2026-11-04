"""
Multi-symbol + multi-timeframe backtest.

Loads BTC and ETH 1m data, resamples to 5m,
runs a pairs/spread strategy across both.

Usage:
    cd /Users/e/dev/lrvx
    PYTHONPATH=build/python .venv/bin/python3 examples/python_multi_symbol.py
"""

import os
import numpy as np
import lrvx

DIR = os.path.join(os.path.dirname(__file__), "data")

engine = lrvx.Engine(initial_capital=10_000, fee_rate=0.0004)

# load two symbols
engine.load_csv(os.path.join(DIR, "btcusdt_1m.csv"))
engine.load_csv(os.path.join(DIR, "ethusdt_1m.csv"))

print(f"symbols: {engine.symbols}")
print(f"  BTCUSDT: {engine.bar_count('BTCUSDT')} bars")
print(f"  ETHUSDT: {engine.bar_count('ETHUSDT')} bars")

# resample to 5m
engine.resample("BTCUSDT", "BTCUSDT_5m", "5m")
engine.resample("ETHUSDT", "ETHUSDT_5m", "5m")
print(f"  BTCUSDT_5m: {engine.bar_count('BTCUSDT_5m')} bars")
print(f"  ETHUSDT_5m: {engine.bar_count('ETHUSDT_5m')} bars")

# indicators on both timeframes
btc_1m = engine.close("BTCUSDT")
eth_1m = engine.close("ETHUSDT")
btc_5m = engine.close("BTCUSDT_5m")
eth_5m = engine.close("ETHUSDT_5m")

print(f"\n1m: BTC={btc_1m[-1]:.2f}  ETH={eth_1m[-1]:.2f}")
print(f"5m: BTC={btc_5m[-1]:.2f}  ETH={eth_5m[-1]:.2f}")

btc_rsi = lrvx.rsi(btc_1m, 14)
eth_rsi = lrvx.rsi(eth_1m, 14)
btc_bb = lrvx.bollinger(btc_5m, 20, 2.0)
eth_bb = lrvx.bollinger(eth_5m, 20, 2.0)

print(f"\nRSI(14) 1m:  BTC={btc_rsi[-1]:.1f}  ETH={eth_rsi[-1]:.1f}")
print(f"BB(20) 5m:   BTC={btc_bb['middle'][-1]:.0f}  ETH={eth_bb['middle'][-1]:.0f}")

# -- pairs strategy: spread mean-reversion on 1m --
# compute spread ratio
n = min(len(btc_1m), len(eth_1m))
ratio = btc_1m[:n] / eth_1m[:n]
ratio_sma = lrvx.sma(ratio, 60)
ratio_std = np.zeros(n)
for i in range(60, n):
    ratio_std[i] = np.std(ratio[i-60:i])

ts_btc = engine.ts("BTCUSDT")
sig = lrvx.SignalBuilder()
pos = 0  # 0=flat, 1=long spread, -1=short spread

for i in range(60, n):
    if ratio_std[i] == 0:
        continue
    z = (ratio[i] - ratio_sma[i]) / ratio_std[i]

    if z < -1.5 and pos <= 0:
        # spread too low: buy BTC, sell ETH
        sig.buy(int(ts_btc[i]), 0.001, "BTCUSDT")
        sig.sell(int(ts_btc[i]), 0.01, "ETHUSDT")
        if pos == -1:
            # close previous short spread
            sig.sell(int(ts_btc[i]), 0.001, "BTCUSDT")
            sig.buy(int(ts_btc[i]), 0.01, "ETHUSDT")
        pos = 1
    elif z > 1.5 and pos >= 0:
        # spread too high: sell BTC, buy ETH
        sig.sell(int(ts_btc[i]), 0.001, "BTCUSDT")
        sig.buy(int(ts_btc[i]), 0.01, "ETHUSDT")
        if pos == 1:
            sig.buy(int(ts_btc[i]), 0.001, "BTCUSDT")
            sig.sell(int(ts_btc[i]), 0.01, "ETHUSDT")
        pos = -1
    elif abs(z) < 0.5 and pos != 0:
        # mean revert -- close
        if pos == 1:
            sig.sell(int(ts_btc[i]), 0.001, "BTCUSDT")
            sig.buy(int(ts_btc[i]), 0.01, "ETHUSDT")
        else:
            sig.buy(int(ts_btc[i]), 0.001, "BTCUSDT")
            sig.sell(int(ts_btc[i]), 0.01, "ETHUSDT")
        pos = 0

print(f"\n{len(sig)} signals")
stats = engine.run(sig)
print(stats)
print(f"  {stats.initial_capital:,.2f} -> {stats.final_capital:,.2f}  {stats.return_pct:+.4f}%")
print(f"  trades={stats.total_trades} wr={stats.win_rate*100:.1f}% sharpe={stats.sharpe_ratio:.4f}")
