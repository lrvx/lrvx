# Compose multi-symbol multi-TF entry conditions declaratively

Multi-symbol multi-TF strategies often look like a tangle of nested `if` statements that pull values from per-(symbol, timeframe) caches and check warmup at every step. The composite-condition DSL builds the same logic as a tree of indicator handles and operators, so the boolean state machine is the readable structure.

The DSL pulls bars from the per-(symbol, timeframe) ring populated by `Strategy.last_n_closed_bars`. Warmup is uniform: a condition reports `is_ready() = False` until every leaf in the tree has its window of bars.

## Quick start

```python
from lrvx import Strategy
from lrvx.composite import when, TIME_BARS

H4_NS = 4 * 3600 * 1_000_000_000
M5_NS = 5 * 60 * 1_000_000_000

class TrendFollow(Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self.btc_id = symbols[0]
        self.entry = None

    def on_start(self):
        self.entry = (
            when(self, self.btc_id, TIME_BARS, H4_NS).ema(50)
            > when(self, self.btc_id, TIME_BARS, H4_NS).ema(200)
        ) & (
            when(self, self.btc_id, TIME_BARS, M5_NS).rsi(14) < 30
        )

    def on_bar(self, ctx, bar):
        if self.entry.is_ready() and self.entry.value():
            self.emit_market_buy(ctx.symbol_id, 0.01)
```

There is no `setup()` hook. The strategy hooks are `on_start`, `on_stop`, `on_trade`, `on_bar`, `on_book_update`, `on_touch`, `on_fill`, `on_order_update`, `on_queue_position_change`, `on_market_position_change`. Building the tree inside `on_bar` works too — the nodes are lazy, so rebuilding per bar costs only the tree allocation.

## Building blocks

- `when(strategy, symbol_id, bar_type, param)` returns a handle bound to that timeframe.
- `.sma(period)`, `.ema(period)`, `.rsi(period)`, `.close()` return indicator nodes that compute lazily from the ring.
- Comparison operators (`<`, `<=`, `>`, `>=`, `==`, `!=`) on a node produce a `Condition`. The right-hand side may be another node or a plain number.
- Conditions compose with `&` (and), `|` (or), `~` (not).

Nodes have **no arithmetic operators** — `*`, `+`, `-`, `/` on a node raise `TypeError`. Only comparison and boolean composition are overloaded.

## Cross-symbol pair trade

Because there is no arithmetic on nodes, a scaled comparison is done on the resolved values:

```python
self.btc_close = when(self, btc_id, TIME_BARS, H1_NS).close()
self.eth_sma = when(self, eth_id, TIME_BARS, H1_NS).sma(20)

def spread_short(self) -> bool:
    if not (self.btc_close.is_ready() and self.eth_sma.is_ready()):
        return False
    return self.btc_close.value() > 1.05 * self.eth_sma.value()
```

## Warmup contract

A node returns `nan` for `.value()` when it does not yet have the bars it needs. The condition wrapping it returns `False` for `.value()` if a leaf is not ready, but the strategy should guard with `.is_ready()` to avoid acting on a partial state.

## Performance

The DSL is pure Python and re-evaluates lazily on every `.value()` call. For multi-symbol multi-TF strategies that fire at the rate the engine dispatches bars (per minute, per hour) this is comfortably fast. For per-tick high-frequency code paths, push the comparison logic into the strategy class directly.

## Indicator-grid sugar

When the same indicator runs on every symbol × timeframe slot, declare the cross-product in one shot:

```python
from lrvx.composite import grid, TIME_BARS

H4 = 14_400_000_000_000
M5 = 300_000_000_000

self.ema50 = grid(self, [btc_id, eth_id], [H4, M5]).ema(50)
self.ema50[(btc_id, H4)].value()
for (sym, bt, param), ind in self.ema50:
    if ind.is_ready():
        ...
```

Each cell is a regular `_Indicator` and slots straight into the comparison / boolean operators. Timeframes can be plain integers (treated as Time-bar interval in nanoseconds) or `(bar_type, param)` tuples for tick / volume / range bars.

The grid ships in every binding. Lookup is `g.get(symbol, bar_type, param)` in JS / Codon and `g[(symbol, bar_type, param)]` (or the 2-tuple Time-bar shortcut) in Python.

```javascript
// node
const { grid, BAR_TYPE_TIME } = require('@lrvx/lrvx').composite;
const g = grid(strat, [btcId, ethId], [M5_NS, [BAR_TYPE_TIME, H4_NS]]).ema(50);
const btcM5 = g.get(btcId, BAR_TYPE_TIME, M5_NS);
if (btcM5.isReady()) console.log(btcM5.value());
```

```javascript
// QuickJS — `grid` is global
const g = grid(this, [btcId, ethId], [M5_NS, H4_NS]).ema(50);
const eth4h = g.get(ethId, 0, H4_NS);
```

```codon
# codon
from lrvx.composite import grid, BAR_TYPE_TIME
g = grid(self, [btc_id, eth_id],
         [(BAR_TYPE_TIME, M5_NS), (BAR_TYPE_TIME, H4_NS)]).ema(50)
btc_m5 = g.get(btc_id, BAR_TYPE_TIME, M5_NS)
```

## Cross-binding

The DSL ships in every binding. The Python form uses Python operator overloading; the JS / Codon forms use named methods because their type systems do not give the same operator-overload latitude. The semantics are identical **across the four bindings**.

Note that the DSL's `.ema()` and `.rsi()` are lightweight window computations, NOT the engine's indicators: `.ema()` seeds from the first value in the window (the engine seeds from the SMA of the first `period` values over the full series), and `.rsi()` is a simple gain/loss average (the engine uses Wilder smoothing). Use them for composite entry conditions; for the exact engine values use `lrvx.ema(...)` / `lrvx.rsi(...)` directly.

```javascript
// node
const { when, BAR_TYPE_TIME } = require('@lrvx/lrvx').composite;
const fast = when(strat, btcId, BAR_TYPE_TIME, M5_NS).ema(50);
const slow = when(strat, btcId, BAR_TYPE_TIME, M5_NS).ema(200);
const crossUp = fast.gt(slow);
if (crossUp.isReady() && crossUp.value()) strat.marketBuy({ qty: 0.01 });
```

```javascript
// QuickJS strategy — `when` is a global from the embedded stdlib
class TrendFollow extends Strategy {
  onBar(ctx, bar) {
    const M5 = 5 * 60 * 1000000000;
    const fast = when(this, ctx.symbolId, 0, M5).ema(50);
    const slow = when(this, ctx.symbolId, 0, M5).ema(200);
    if (fast.gt(slow).isReady() && fast.gt(slow).value()) {
      this.marketBuy({ qty: 0.01 });
    }
  }
}
```

```codon
# codon — Compare returns named methods; logical composition uses
# plain `and` / `or` / `not` on the boolean values
from lrvx.composite import when, BAR_TYPE_TIME

class TrendFollow(Strategy):
    def on_bar(self, ctx, bar):
        M5 = 5 * 60 * 1_000_000_000
        fast = when(self, ctx.symbol_id, BAR_TYPE_TIME, M5).ema(50)
        slow = when(self, ctx.symbol_id, BAR_TYPE_TIME, M5).ema(200)
        cross = fast.gt_ind(slow)
        if cross.is_ready() and cross.value():
            self.market_buy(0.01)
```

## See also

- [Multi-TF context helpers](multi-tf-context.md). The `last_closed_bar` / `last_n_closed_bars` accessors the DSL is built on.
- [Multi-symbol indicators](multi-symbol-indicators.md). When the same indicator runs on every symbol.
