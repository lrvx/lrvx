"""
Один класс стратегии — бэктест и лайв.

Один и тот же SMAStrategy работает в трёх режимах:
  - BacktestRunner:    replay CSV через стратегию, SimulatedExecutor заполняет ордера
  - Runner:            синхронный лайв, данные пушатся из коннектора
  - Runner(threaded=True): Disruptor-based, publish lock-free из отдельного потока

Usage:
    cd /path/to/lrvx
    PYTHONPATH=build/python python3 examples/python_backtest_vs_live.py
"""

import os
import time
import lrvx

DATA = os.path.join(os.path.dirname(__file__), "data", "btcusdt_1m.csv")

# ── Символы ───────────────────────────────────────────────────────────

registry = lrvx.SymbolRegistry()
btc = registry.add_symbol("binance", "BTCUSDT", tick_size=0.01)
# btc — объект Symbol: print(btc) → Symbol(binance:BTCUSDT, id=1)
# Прозрачно конвертируется в int везде где нужен id.

# ── Одна стратегия — три режима ────────────────────────────────────────

class SMAStrategy(lrvx.Strategy):
    """SMA(10/30) crossover."""

    def __init__(self, symbols):
        super().__init__(symbols)
        self.fast = lrvx.SMA(10)
        self.slow = lrvx.SMA(30)
        self.trade_count = 0

    def on_start(self):
        print("  SMAStrategy started")

    def on_stop(self):
        print(f"  SMAStrategy stopped  ({self.trade_count} signals emitted)")

    def on_trade(self, ctx, trade):
        fv = self.fast.update(trade.price)
        sv = self.slow.update(trade.price)
        if fv is None or sv is None or not self.slow.ready:
            return
        if fv > sv and ctx.is_flat():
            self.market_buy(0.01)
            self.trade_count += 1
        elif fv < sv and ctx.is_long():
            self.close_position()
            self.trade_count += 1


# ── 1. BacktestRunner — replay исторических данных ─────────────────────

print("── Backtest ──────────────────────────────────────────────────────")

strat = SMAStrategy([btc])

bt = lrvx.BacktestRunner(registry, fee_rate=0.0004, initial_capital=10_000)
bt.set_strategy(strat)
stats = bt.run_csv(DATA)

print(f"  Return   : {stats['return_pct']:+.4f}%")
print(f"  Trades   : {stats['total_trades']}  win={stats['win_rate']*100:.1f}%")
print(f"  Sharpe   : {stats['sharpe_ratio']:.4f}")
print(f"  Max DD   : {stats['max_drawdown_pct']:.4f}%")
print(f"  Net PnL  : {stats['net_pnl']:.4f}")


# ── 2. Runner — синхронный лайв ───────────────────────────────────────

print("\n── Runner (live, sync) ───────────────────────────────────────────")

signals_received: list[lrvx.Signal] = []

def on_signal(sig: lrvx.Signal):
    signals_received.append(sig)

prices = [50000 + i * 50 for i in range(40)]
ts_ns = int(time.time()) * 1_000_000_000

runner = lrvx.Runner(registry, on_signal)
runner.add_strategy(SMAStrategy([btc]))
runner.start()

for i, p in enumerate(prices):
    runner.on_trade(btc, float(p), 0.1, i % 2 == 0, ts_ns)
    ts_ns += 1_000_000_000

runner.stop()
print(f"  Signals received: {len(signals_received)}")
for s in signals_received[:3]:
    print(f"    {s.side:4s}  {s.quantity:.4f} @ {s.price:.2f}  [{s.order_type}]")


# ── 3. Runner(threaded=True) — Disruptor, lock-free publish ───────────

print("\n── Runner (threaded=True) ────────────────────────────────────────")

live_signals = []

threaded_runner = lrvx.Runner(registry, lambda sig: live_signals.append(sig),
                               threaded=True)
threaded_runner.add_strategy(SMAStrategy([btc]))
threaded_runner.start()

ts_ns = int(time.time()) * 1_000_000_000
for i, p in enumerate(prices):
    threaded_runner.on_trade(btc, float(p), 0.1, i % 2 == 0, ts_ns)
    ts_ns += 1_000_000_000

time.sleep(0.05)
threaded_runner.stop()
print(f"  Signals received: {len(live_signals)}")
