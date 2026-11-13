# Strategy

Base class for Codon event-driven strategies. Mirrors C++ `lrvx::Strategy`.

## Class: `Strategy`

### Constructor

```codon
Strategy(symbols: List[int], strategy_id: int = -1, registry: cobj = cobj())
```

**Parameters:**

- `symbols` -- List of symbol IDs to subscribe to
- `strategy_id` -- Subscriber id. Order ids are `(strategy_id << 32) + n`. Left out, the
  strategy holds id `1` until it is added to a `Runner`, which moves it to the lowest id from 1
  that no strategy on that runner holds: default-constructed strategies on one runner get 1, 2,
  3, ... in the order added. Given, the id is kept, and a runner that already holds it refuses
  the strategy. The id the first `Runner` gives is kept the same way: a second `Runner` does not
  renumber the strategy, and refuses it with the duplicate-id `ValueError` when it already holds
  that id. A `BacktestRunner` has one strategy and leaves its id alone. `2097151` is
  refused everywhere: runners take their own order ids from that block. An id of 2^32 or more
  raises `ValueError` from the constructor, and so does 2^32 - 1 (`4294967295`), the block the
  position tracker numbers its explicit fills from
- `registry` -- Symbol-registry handle. When omitted, the underlying handle is created empty and
  `Runner.add_strategy` rebuilds it once a registry is present. Pass the strategy into a `Runner` to
  give it an event source; on its own it has none.

### How a callback reaches your subclass

Your overrides are called through a table of C function pointers that the
engine holds. Codon fills that table when the strategy is handed to a runner,
not when it is constructed, and this is not an implementation detail you can
ignore: a strategy that is never attached never receives anything.

```codon
strat = MyStrategy([btc])
runner.add_strategy(strat)        # installs the callbacks
# or
backtest.set_strategy(strat)      # same, for BacktestRunner
```

The reason for the timing is that the trampolines are generated against your
concrete subclass. Codon resolves method calls statically, so a trampoline
written against `Strategy` would call the empty base method rather than your
override. `add_strategy` and `set_strategy` are generic in the strategy's
type, which is the last point where that type is still visible -- inside
`Strategy.__init__` a subclass has already been erased by `super()`.

Two consequences worth knowing:

- Attaching one strategy to a runner is what makes it live. Constructing it is
  not enough, and a strategy with no callbacks installed raises rather than
  running silently through a backtest that would finish and print a result
  your logic took no part in.
- An exception raised inside a callback is caught at the boundary and reported
  on stderr, naming the callback. It is not allowed to unwind into the C++
  frame that called it, because that is undefined behaviour and the engine
  cannot recover from it.

### Overridable Callbacks

#### `on_trade(ctx, trade)`

Called on each trade event for subscribed symbols.

```codon
def on_trade(self, ctx: SymbolContext, trade: TradeData):
    price = trade.price.to_double()
    # strategy logic here
```

#### `on_book_update(ctx)`

Called on each order book update for subscribed symbols.

```codon
def on_book_update(self, ctx: SymbolContext):
    spread = ctx.book_spread()
    # strategy logic here
```

#### `on_touch(ctx)`

Called when the touch changes: price or size on either side, or a side
appearing or emptying. Fires on a top-of-book frame, and right after
`on_book_update` on a depth frame that moved the touch. `ctx.best_bid()` /
`ctx.best_ask()` are the new touch.

#### `on_bar(ctx, bar)`

Called on each closed OHLC bar.

```codon
def on_bar(self, ctx: SymbolContext, bar: BarData):
    # bar.open, bar.high, bar.low, bar.close, bar.volume, ...
    if bar.close > bar.open and self.position() == 0.0:
        self.market_buy(0.01)
```

#### `on_start()` / `on_stop()`

Lifecycle callbacks.

#### `on_fill(ctx: SymbolContext, ev: OrderEventData)`

Called on each fill (status `PARTIALLY_FILLED` or `FILLED`) for orders this strategy emitted. `ev` is
an `OrderEventData`, described under [Types](types.md); it carries `order_id`, `side`, `fill_qty`,
`fill_price`, `exchange_ts_ns`.

`ctx.position()` in `on_fill` and `on_order_update` already counts the fill being delivered: the
runner's position tracker takes a fill before the strategy hears of it. After a buy of 1 from flat,
`ctx.position()` in that fill's `on_fill` is 1.

The four order callbacks take their event as `OrderEventData` in the base class too, so one program
can attach a plain `Strategy` and a subclass that overrides them. An override declares the same
parameter types.

#### `on_order_update(ctx: SymbolContext, ev: OrderEventData)`

Called on every order-lifecycle status change for orders this strategy emitted: `NEW`, `ACCEPTED`,
`CANCELED`, `REJECTED`, `REPLACED`, `TRIGGERED`, `TRAILING_UPDATED` and the rest. A fill does not
come through here: a `PARTIALLY_FILLED` or `FILLED` event reaches `on_fill` only.

#### `on_queue_position_change(ctx: SymbolContext, ev: OrderEventData)`

A resting limit order's queue position moved with no other lifecycle transition. `ev.queue_ahead` and
`ev.queue_total` carry the current snapshot. Backtest only.

#### `on_market_position_change(ctx, ev)`

A resting limit order's categorical market position transitioned (best, behind_best, mid_spread,
level_empty, crossed). `ev.market_position` is the new state; `ev.distance_to_best_ticks` is signed
ticks from best on our side. Backtest only.

### Signal Emission

In a backtest, prices, triggers and trailing offsets must be whole numbers of the tick the symbol was
registered with (`lrvx_registry_add_symbol(reg, exchange, name, tick)` with `tick > 0`). An order off
the tick is rejected: `on_order_update` receives `REJECTED` with `ev.reject_reason` set to
`price_off_tick`, `trigger_off_tick` or `trailing_offset_off_tick`. A symbol registered with a tick of
0 is not checked. See [SimulatedExecutor](../api/backtest/simulated_executor.md#order-prices-and-the-tick).

Quantities are checked against the quantity step the symbol was registered with through
`lrvx_registry_add_symbol_with_lot(reg, exchange, name, tick, lot)` (importable from `lrvx.runner`)
with `lot > 0`: an order whose quantity is not a whole number of the lot is rejected with
`ev.reject_reason` `quantity_off_lot`. A lot of 0, and `lrvx_registry_add_symbol`, declare none and
quantities are not checked. See
[SimulatedExecutor](../api/backtest/simulated_executor.md#order-quantities-and-the-lot).

`lrvx_registry_get_symbol_tick(reg, u32(sym), value, declared)` and
`lrvx_registry_get_symbol_lot(reg, u32(sym), value, declared)` (both importable from `lrvx.runner`)
read back what an entry holds: they return `u8(1)` for a registered id and fill a `Ptr[float]` with
the value and a `Ptr[u8]` with whether orders are checked against it.

Every emit method, and the string-symbol forms that call them, clears the last error on entry.
`last_error()` from `lrvx.abi`, read right after an emit inside the callback that emitted, is `""`
unless that emit left a reason; a reason an earlier call left (a `validate_segment` that failed, say)
is not read as the emit's. A reason ending in `[kill_switch]`, `[order_validator]` or `[risk_manager]`
says a pre-trade gate dropped the signal, on a live runner and in a backtest alike; which signals each
of them puts to the gates is listed under
[Reading why a signal went nowhere](../python/strategy.md#reading-why-a-signal-went-nowhere).

#### `emit_market_buy(symbol, qty) -> int`

Submit a market buy order. Returns order ID.

#### `emit_market_sell(symbol, qty) -> int`

Submit a market sell order. Returns order ID.

#### `emit_limit_buy(symbol, price, qty) -> int`

Submit a limit buy order. Returns order ID.

#### `emit_limit_sell(symbol, price, qty) -> int`

Submit a limit sell order. Returns order ID.

#### `emit_cancel(order_id)`

Cancel an order by ID.

#### `emit_cancel_all(symbol)`

Cancel all orders for a symbol.

#### `emit_modify(order_id, new_price, new_qty)`

Modify an existing order's price and quantity. It keeps its id, symbol, side and type. `new_price`
is the trigger of a stop-market, take-profit-market or trailing-stop order and the limit price of
every other type. A refused modify arrives in `on_order_update` as `REPLACE_REJECTED`.

#### `emit_stop_market(symbol, side, trigger, qty) -> int`

Submit a stop market order. `side`: 0=BUY, 1=SELL.

#### `emit_stop_limit(symbol, side, trigger, limit_price, qty) -> int`

Submit a stop limit order.

#### `emit_take_profit_market(symbol, side, trigger, qty) -> int`

Submit a take profit market order.

#### `emit_take_profit_limit(symbol, side, trigger, limit_price, qty) -> int`

Submit a take profit limit order.

#### `emit_trailing_stop(symbol, side, offset, qty) -> int`

Submit a trailing stop order.

#### `emit_trailing_stop_percent(symbol, side, callback_bps, qty) -> int`

Submit a trailing stop with percentage callback. `callback_bps`: 100 = 1%.

#### `emit_limit_buy_tif(symbol, price, qty, tif) -> int`

Submit a limit buy with TimeInForce. `tif`: 0=GTC, 1=IOC, 2=FOK, 4=POST_ONLY.

#### `emit_limit_sell_tif(symbol, price, qty, tif) -> int`

Submit a limit sell with TimeInForce.

#### `emit_close_position(symbol) -> int`

Close entire position with a reduce-only market order.

### Context Queries

#### `position(symbol=None) -> float`

Current position quantity. If `symbol` is None, uses the first subscribed symbol.

#### `ctx(symbol=None) -> SymbolContext`

Get a `SymbolContext` for querying per-symbol state. Besides `position()`,
`last_trade_price()`, `best_bid()`, `best_ask()`, `mid_price()` and
`book_spread()`, it answers two exchange timestamps in nanoseconds:
`last_update_ns()`, the last trade, depth frame or bar, and
`last_touch_ns()`, the frame that last wrote the touch. A `TOP_OF_BOOK` frame
moves the second and not the first (it fires `on_touch`, never
`on_book_update`), so the second can be the fresher.

#### `get_order_status(order_id) -> int`

Get order status. Returns -1 if not found.

### String-Symbol Convenience API

These take an optional symbol **name** and resolve it through the strategy's own name map, falling
back to the primary symbol when omitted. Prices and quantities are plain `float`.

| Method | Returns |
|--------|---------|
| `market_buy(qty, symbol=None)` | `int` order id |
| `market_sell(qty, symbol=None)` | `int` order id |
| `limit_buy(price, qty, symbol=None, tif="gtc")` | `int` order id |
| `limit_sell(price, qty, symbol=None, tif="gtc")` | `int` order id |
| `stop_market(side, trigger, qty, symbol=None)` | `int` order id |
| `stop_limit(side, trigger, limit_price, qty, symbol=None)` | `int` order id |
| `take_profit_market(side, trigger, qty, symbol=None)` | `int` order id |
| `take_profit_limit(side, trigger, limit_price, qty, symbol=None)` | `int` order id |
| `trailing_stop(side, offset, qty, symbol=None)` | `int` order id |
| `trailing_stop_percent(side, callback_bps, qty, symbol=None)` | `int` order id |
| `close_position(symbol=None)` | `int` order id |
| `cancel_order(order_id)` | `None` |
| `cancel_all_orders(symbol=None)` | `None` |
| `modify_order(order_id, new_price, new_qty)` | `None` |
| `pos(symbol=None)` | `float` position |
| `last_price(symbol=None)` | `float` |
| `best_bid(symbol=None)` | `Optional[float]`, `None` when the bid side has no level; `0.0` is a price |
| `best_ask(symbol=None)` | `Optional[float]`, `None` when the ask side has no level |
| `mid_price(symbol=None)` | `Optional[float]`, `None` unless both sides have a level |
| `order_status(order_id)` | `int` |

`side` and `tif` are lowercase strings here (`'buy'` / `'sell'`, `'gtc'` / `'ioc'` / ...), unlike the
`emit_*` methods which take the integer constants from [`lrvx.types`](types.md). An unrecognised
`side` falls back to buy and an unrecognised `tif` to GTC. An unknown symbol name raises
`ValueError`.

### Multi-Timeframe Bar Ring

| Method | Description |
|--------|-------------|
| `last_closed_bar(symbol, bar_type, param) -> Optional[BarData]` | Most recent closed bar for that (symbol, timeframe), or `None` before one has closed |
| `last_n_closed_bars(symbol, bar_type, param, n) -> List[BarData]` | Most recent `n` bars, oldest first |
| `bar_ring_capacity() -> int` | Bars retained per (symbol, timeframe) |
| `set_bar_ring_capacity(n)` | Set the retention count |

`param` is nanoseconds for time bars, a count for tick bars, a threshold for volume bars.

### Properties

#### `primary_symbol -> int`

Returns the first symbol in the subscription list.

#### `primary_symbol_name -> str`

The name of the primary symbol, as resolved through the registry.

## Example

```codon
from lrvx.strategy import Strategy
from lrvx.context import SymbolContext
from lrvx.types import TradeData
from lrvx.indicators import EMA

class EmaCrossover(Strategy):
    fast_ema: EMA
    slow_ema: EMA

    def __init__(self, symbols: List[int]):
        super().__init__(symbols)
        self.fast_ema = EMA(12)
        self.slow_ema = EMA(26)

    def on_trade(self, ctx: SymbolContext, trade: TradeData):
        price = trade.price.to_double()
        fast = self.fast_ema.update(price)
        slow = self.slow_ema.update(price)

        if not self.slow_ema.ready:
            return

        sym = self.primary_symbol
        if fast > slow and ctx.is_flat():
            self.emit_market_buy(sym, 1.0)
        elif fast < slow and ctx.is_long():
            self.emit_close_position(sym)
```

## Reporting a live order's updates and fills

`Runner.on_order_event(order_id, symbol, side, status, order_type=0, fill_qty=0.0, fill_price=0.0,
ts_ns=0, reject_reason="", is_maker=False)` reports one order event a venue sent to the strategy
that issued the order, through the callbacks a backtest uses: `ORDER_STATUS_PARTIALLY_FILLED` and
`ORDER_STATUS_FILLED` reach `on_fill` and only `on_fill`, every other status reaches
`on_order_update`. `side` is `"buy"` or `"sell"`; `status` is one of the `ORDER_STATUS_*` constants
the runner module exports and `order_type` an order type code (0 is a limit order), the codes
`OrderEventData` carries. `fill_qty` and `fill_price` are this fill's own.

```codon
from lrvx.runner import Runner, ORDER_STATUS_FILLED

order_id = strategy.emit_limit_buy(sym, 100.0, 2.0)
runner.on_order_event(order_id, sym, "buy", ORDER_STATUS_FILLED, fill_qty=2.0, fill_price=100.0)
```

Each strategy has a position of its own, as in a backtest: the fill counts for the strategy that
issued the order before `on_fill` runs, and another strategy on the same runner still reads its own.
A synchronous runner runs the callbacks inside the call. A threaded runner queues the event and runs
them on the strategy's order-event consumer thread, after the market data published before the
report; `Runner.drain()` returns once everything published so far has been handed over. An unknown
side, or a status or order type that is not a code, raises `ValueError`.
