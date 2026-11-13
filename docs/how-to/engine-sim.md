# Run a paper engine with `lrvx engine sim`

`lrvx engine sim` boots a strategy against a recorded tape behind a control
server, so an AI agent or an operator can inspect and steer a running engine
without a live exchange. It wires up a `SimulatedExecutor`, a kill switch, a
`ControlServer` and a runtime-state writer in one command.

## Run it

```bash
lrvx engine sim --strategy ./my_strategy.py --tape ./tapes/bybit-btc-2026-05-07
```

The command replays the tape through the strategy, then keeps serving until
Ctrl+C so the engine can be introspected after the data ends.

| Argument | Purpose |
|----------|---------|
| `--strategy PATH` | A `.py` file holding exactly one `lrvx.Strategy` subclass. Two or zero subclasses is an error. |
| `--tape PATH` | A `.lrvx` directory, as written by [`lrvx tape record`](tape-record.md). |
| `--tape-symbol-id ID` | Which symbol id *in the tape* feeds the strategy. Required when the tape holds more than one — see below. |
| `--symbol-name NAME` | Symbol to register (default `BTCUSDT`). |
| `--exchange TAG` | Exchange tag for the registered symbol (default `sim`). |
| `--tick-size SIZE` | Tick size for the registered symbol (default `0.01`). |
| `--host` / `--port` | ControlServer bind address (default `127.0.0.1:8765`). |
| `--token TOKEN` | Explicit ControlServer token. Generated if omitted. |
| `--state-file PATH` | Runtime-state snapshot for the tier-4 MCP read tools. |
| `--threaded` | Use the Disruptor runner instead of the synchronous one. |

## Multi-symbol tapes need `--tape-symbol-id`

The command registers exactly one symbol, but a tape carries whatever ids
whoever recorded it assigned — `lrvx tape record` numbers them `1..N` in
encounter order, and those numbers mean nothing outside that tape.

With one symbol in the tape the mapping is unambiguous and happens
automatically. With more than one the command stops rather than guess:

```
lrvx engine sim: the tape holds 2 symbols ([1, 2]) but this command registers
one. Re-run with --tape-symbol-id <id> to choose which one feeds the strategy.
```

Pick the id with [`lrvx tape inspect`](tape-record.md), which prints the symbol
ids present, then pass it:

```bash
lrvx engine sim --strategy ./s.py --tape ./tapes/multi --tape-symbol-id 2
```

The trade count reported at the end counts trades *delivered to the strategy*,
not trades read from the tape, so it is the number to check when a filter is in
play.

## What the simulator sees and does

The tape is replayed by `Runner.replay_tape`, which is the loop
`BacktestRunner.run_tape` uses with the engine's simulator in the place of the
backtest's own. Every event the tape holds for the chosen symbol (trades, book
snapshots, book deltas, top-of-book frames) moves the simulator's clock and is
shown to the simulator before the strategy sees it, in the tape's time order.
A market order fills at the next event, and a resting limit order fills from
the book as it does in a backtest of the same tape. The strategy receives the
trades, the book snapshots and the tops (`on_trade`, `on_book_update`); a live
runner has no entry for a book delta, so deltas move the simulator's book only.
The simulator is reset when the replay starts.

The replay used to feed trades only, so a resting limit order, which the
simulator fills from a book, stayed open for the whole tape.

The strategy's orders reach the simulator through the runner's executor hook:

| Strategy call | Simulator |
|---------------|-----------|
| an order (`market_buy`, `limit_sell`, ...) | `submit_order` under the order's id, with the trigger of a stop or take-profit order and the offset of a trailing stop. They used to be left out, so a stop reached the simulator without its trigger. A `trailing_stop_percent` goes with its callback rate (`trailing_bps`), which `Order.trailing_bps` carries to an executor; it used to be refused |
| `cancel_order(id)` | `cancel_order(id)` |
| `cancel_all_orders()` | `cancel_all(symbol)` |
| `modify_order(id, price, qty)` | `cancel_order(id)`, then `submit_order` of the order with its new price and quantity under the same id. A modify used to be dropped: the hook had no `replace`. |

After each event the simulator's new fills are reported to the runner. Every
fill goes to the strategy that placed the order as an order event
(`Runner.on_order_event`): `on_fill(ctx, event)` runs with `event.status`
`"PARTIALLY_FILLED"`, or `"FILLED"` for the fill that completes the order, the
fill's own quantity and price, and `ctx.position` already counting it, as in a
backtest. Only the closing of an order used to be reported, so `on_fill` never
ran and the strategy read a position of 0 for the whole replay. The simulator's
Python surface reports fills only, so `on_order_update` (accepted, cancelled,
rejected) is not called here.

An order filled completely is then reported closed (`Runner.order_closed`),
which drops it from the record the runner keeps for `modify_order`. A later
`modify_order` of a filled order is refused by the runner with
`[unknown_order]` in `lrvx.last_error()` and is not sent to the simulator as a
new order. A partly filled order stays on record until the rest fills.

Orders placed or cancelled over the control server go through the same bridge
as the strategy's: under the same lock (the replay holds it while the simulator
takes an event, so a control call never lands mid-event), into the same account
of working orders, so their fills are reported like any other. A control-server
cancel of a strategy's order, or a cancel-all, is passed on to
`Runner.order_closed`, so the strategy's later `modify_order` of it is refused
with `[unknown_order]`. The control server used to call the simulator itself,
from its own thread and with no lock, and what it cancelled stayed on the
runner's record.

With `--threaded` the strategy runs on the runner's consumer threads and the
replay waits for them: after each event `Runner.replay_tape` returns to the
tape only once the strategy has been handed the event and has sent whatever
orders it made it send, and again once it has heard the fills reported after
it (`Runner.drain`). An order therefore meets the tape at the same event as on
the synchronous runner, and the two replays of one tape fill at the same prices
and times. The replay used not to wait, so an order was submitted wherever the
strategy thread happened to be, often after the tape had ended.

## When the control server opens

The replay resets the simulator when it starts, as a backtest run does. The
control server and the state writer open after that reset and before the first
event (`Runner.replay_tape(on_start=...)`), so an order placed over MCP as soon
as the banner prints is in the simulator for the whole tape. The server used to
open first, and an order placed before the replay began was wiped by the reset.

## The state file

`--state-file` is rewritten once a second from the simulator's fills:

| Section | Holds |
|---------|-------|
| `positions` | one row per symbol with a non-zero net of the fills: `qty` (signed), `avg_price` (average entry) and `unrealized_pnl`, the position valued at the last price the strategy saw for the symbol |
| `open_orders` | the orders sent to the simulator, by the strategy or over the control server, that it has not finished: id, side, type, the quantity still to fill and the price |
| `pnl` | realized PnL of the closed quantity, the unrealized PnL above, and the number of fills in `trades`. `fees` is 0: the simulator charges none here |

All three used to be written empty, whatever the simulator held. The account is
the simulator's as a whole: an order placed over the control server counts in
it like the strategy's.

## Drive it from an agent

The banner prints a ready-made `lrvx-mcp init` invocation:

```bash
lrvx-mcp init --overwrite --engine-url http://127.0.0.1:8765 --token <generated>
```

From there the MCP tools can read positions, PnL and the event log, and flip
the kill switch — see [Inspect a running engine over MCP](mcp-runtime-inspection.md)
and [Control a running engine over MCP](mcp-control-plane.md). The kill switch gates the control server's
mutating endpoints *and* halts the strategy's own signals.

## Scope

`sim` is the only engine mode wired today, and it is paper-trading only — it
never places a real order. For live execution see
[Connect lrvx to a CCXT exchange](ccxt-adapter.md).

## Related

- [Record and replay market data](tape-record.md) — produce the tape this consumes
- [Paper trading](paper-trading.md) — the same strategy class against a live feed
- [Scaffold a project (`lrvx new`)](lrvx-new.md) — a strategy file to point `--strategy` at
