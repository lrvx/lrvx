# Train a reinforcement-learning agent on a lrvx tape

`lrvx.rl_env.LrvxTradingEnv` is a Gymnasium-compatible environment that drives an agent through the trades in a captured `.lrvx` tape. It speaks the standard `Env` protocol (`reset`, `step`, `render`, `close`, `action_space`, `observation_space`, `metadata`) without importing `gymnasium` itself, so plugging it into `stable_baselines3`, `RLlib`, or `CleanRL` does not pull gymnasium into lrvx's dependency surface. The user installs gymnasium and the learner of choice; the env works out of the box.

Two construction paths are available. `LrvxTradingEnv.from_tape(...)` is the lightweight path — trade-by-trade replay with ideal fills, no fees, no funding, no liquidation. `LrvxTradingEnv.from_venue_stack(stack, tape=...)` plugs the env on top of a `VenueStack` so every action routes through the same simulated executor used in backtest and paper trading; fees and funding feed back into the reward via the cross-margin Account, and liquidation terminates the episode. The venue-stack path also carries the continuous action space, limit-order semantics, and multi-symbol portfolios documented below.

## Quick start

```python
from lrvx.rl_env import LrvxTradingEnv

env = LrvxTradingEnv.from_tape(
    "./tapes/bybit-btc-2026-05-07",
    qty=0.01,
    window_size=16,
)

obs, info = env.reset(seed=42)
total_reward = 0.0
done = False
while not done:
    action = env.action_space.sample()  # plug in your policy here
    obs, reward, terminated, truncated, info = env.step(action)
    total_reward += reward
    done = terminated or truncated

print(total_reward, info["realized_pnl"], info["position"])
```

`from_tape` loads the entire tape into memory at construction time. For long captures, slice the tape upstream; the env constructor also accepts a plain list of `(ts_ns, price, qty, side)` tuples if you want to drive it from a non-tape source.

## Action and observation spaces

The default action space is `Discrete(3)`:

| Action | Meaning |
|---|---|
| 0 | Hold (no order). |
| 1 | Go long `qty` (or stay long if already there). |
| 2 | Go short `qty` (or stay short if already there). |

Switching position closes the existing one at the current price (realizes PnL) and opens the new one.

The default observation is a `Box` of shape `(window_size + 2,)`:

- The first `window_size` entries are the most recent prices, normalized by the first observed price (so the values stay around 1.0).
- One entry for current position quantity (signed).
- One entry for unrealized PnL since the last position change.

Override `window_size` at construction. If you want a different observation, subclass `LrvxTradingEnv` and override `_observation`.

## Reward

The default reward is the change in total PnL (realized plus unrealized) since the previous step. Pass `reward_fn=lambda env, ctx: ...` to compute your own; the callback receives the env and a context dict (`ts_ns`, `price`, `position`, `realized_pnl`, `unrealized_pnl`, `step`) and returns a float.

```python
def risk_adjusted_reward(env, ctx):
    pnl_delta = env._last_total_pnl  # if you read internals
    drawdown_penalty = max(0.0, -ctx["unrealized_pnl"]) * 0.1
    return float(pnl_delta - drawdown_penalty)

env = LrvxTradingEnv.from_tape(path, reward_fn=risk_adjusted_reward)
```

The default in the `from_tape` path ignores transaction costs entirely. For honest training, switch to `from_venue_stack` — the venue-stack path computes reward as the change in account equity at mark, with taker (or maker, if `is_maker=True`) fees deducted on each fill via the stack's fee schedule. Funding accrued by the schedule and realized PnL on close are folded into equity automatically.

## Venue-stack backed env

```python
import lrvx
from lrvx.rl_env import LrvxTradingEnv

stack = lrvx.VenueStack.binance_um_futures(account_id=1, equity=10_000.0)

env = LrvxTradingEnv.from_venue_stack(
    stack,
    tape="./tapes/btc-2026-05-07",
    qty=0.01,
    window_size=16,
    symbol_id=1,
)
```

What changes versus `from_tape`:

- `step()` submits market orders through `stack.executor()` (the same `VenueExecutor` returned to any other Python caller of the stack), feeds the current trade tick to the matching engine, and drains the resulting fills.
- Fees come from `stack.fees().fee_for(...)` and are deducted from account equity on every fill. The fee schedule's 30d rolling notional advances on each fill, so the tier moves with realized volume — same behavior as a backtest.
- The cross-margin Account's `set_mark` is called every step, and `stack.liquidation().on_mark(...)` runs the liquidation walk. Episodes terminate on the first liquidation event.
- Reward is the change in `account.equity() + account.total_unrealised_pnl()` since the previous step. Fees, funding accruals, realized PnL on close, and unrealized PnL on mark all fold in naturally.
- `info` gains `equity`, `unrealized_pnl`, `equity_at_mark`, `fee_tier`, and `liquidation_outcome` fields so the agent's training loop can log the venue-side state.
- `reset()` restores the stack's account to the equity it had when the env was constructed, clears open positions and fills, and cancels resting orders — the standard training loop resets the env every episode, so this keeps repeated episodes on the same tape independent of each other. The one thing `reset()` does not clear is the executor's own order-matching and queue-position state, so a market fill's simulated price can differ by a tick between two otherwise-identical episodes.

Same strategy class, same data, the only thing that differs from `from_tape` is the realism around the fills. Pick this path for any training that will hand the trained policy to `PaperBroker` or `CcxtBroker` — the physics will match.

## Continuous actions

`from_venue_stack` defaults to a continuous action space — a `Box((3,))` with one axis for signed quantity, one for price offset in ticks, one for time-in-force. Discrete(3) stays available as `action_mode="discrete"`.

| Axis | Range | Meaning |
|---|---|---|
| 0 | `[-1.0, +1.0]` | Signed qty as a fraction of `max_position`. `+1.0` = full long, `-1.0` = full short, `0.0` = flat |
| 1 | `[-N, +N]` ticks (`N = max_price_offset_ticks`, default 50) | Limit price offset from mid. `0` means market |
| 2 | `[0.0, 2.0]` | TIF, rounded to int: `0=GTC`, `1=IOC`, `2=Post-only` |

```python
env = LrvxTradingEnv.from_venue_stack(
    stack, tape=tape,
    qty=0.01, max_position=0.05,
    tick_size=0.01,
    max_price_offset_ticks=50,
    # action_mode="continuous" is the default
)
# action: [signed_qty_fraction, price_offset_ticks, tif_flag]
obs, reward, term, trunc, info = env.step([0.5, 0.0, 0.0])    # market buy 50% of max_position
obs, reward, term, trunc, info = env.step([-1.0, 2.0, 2.0])   # post-only limit sell at mid + 2 ticks
```

Decode rules:

- `price_offset_ticks == 0` (after rounding) means **market order** — TIF axis is ignored, the order routes through the executor at the most recent trade price.
- `price_offset_ticks != 0` means **limit order**. The price is `mid + offset * tick_size * side_sign`, where mid is the latest trade price (an approximation — the env does not read best bid / best ask) and `side_sign` is `+1` for buys, `-1` for sells.
- The TIF axis rounds to the nearest int. Out-of-range values are clipped to the box bounds.
- Out-of-bounds actions are **clipped** (not raised). A `RuntimeWarning` is emitted and `info["action_clipped"] = True` so a learner that occasionally samples outside the box does not crash the env.

Pass `action_mode="discrete"` to keep the `Discrete(3)` interface instead. Multi-symbol mode requires `action_mode="continuous"`.

## Open-order observation slots

In venue-stack mode the observation gains a configurable bank of open-order slots. Each slot is four floats — signed qty remaining (as a fraction of `max_position`), age in steps (as a fraction of `window_size`), distance from the latest price in ticks (as a fraction of `max_price_offset_ticks`, clipped to `[-1, 1]`), and a queue position proxy in `[0, 1]`. Unused slots are zero-padded so the observation shape stays constant.

`n_open_slots` defaults to 4 in single-symbol venue-stack mode, 2 *per symbol* in multi-symbol mode, and 0 in the bare `from_tape` path (no executor → no resting orders to track). Set it explicitly to override:

```python
env = LrvxTradingEnv.from_venue_stack(
    stack, tape=tape, qty=0.01,
    n_open_slots=8,   # carry up to 8 resting orders in the obs
)
# observation_space.shape == (window_size + 2 + 4 * 8,)
```

When the agent submits a non-market order through the venue executor it is recorded; fills bump down the remaining quantity, and the entry drops off when `qty_remaining ≤ 0` or the order is canceled. The slot ordering is stable by submit step, so the same resting order keeps the same slot index across observations.

`info["open_orders"]` exposes the count of currently-resting orders for logging.

## Reject penalty

The simulated executor silently drops orders that fail rate-limit, venue-availability, or post-only-cross checks. The env detects these as a submit that produced neither a fill nor a resting order entry and surfaces them as `info["rejected"] = True`. Set `reject_penalty=...` at construction to subtract that amount from the reward whenever a reject is detected:

```python
env = LrvxTradingEnv.from_venue_stack(
    stack, tape=tape, qty=0.01,
    reject_penalty=10.0,   # subtract 10 from reward on each rejected submit
)
```

Default `0.0` leaves the behaviour untouched. The penalty is on top of the usual equity-delta reward, not a replacement.

## One policy, three deployment modes

The venue-stack-backed env exists to produce a policy you can then run unchanged through `PaperBroker` and `CcxtBroker`. `lrvx.rl_env` ships three small pieces to close that loop:

- `ObservationBuilder` — stateful builder that turns a stream of trades plus a current position into the same observation vector the env uses. Plug live ticks via `on_trade(price)`, update position via `set_position`, and call `build()` whenever the model needs an input.
- `ActionDecoder` — pure function that maps a continuous `Box((3,))` action to a structured intent (market or limit, side, quantity, price, TIF). Same decode logic the env uses internally.
- `make_rl_policy` — produces a `lrvx.Strategy` subclass that on every `on_trade` updates the builder, runs `model.predict(obs)`, decodes, and emits the corresponding order through the runner's signal callback.

```python
import lrvx
from lrvx.rl_env import (
    LrvxTradingEnv, ObservationBuilder, ActionDecoder, make_rl_policy,
)
from stable_baselines3 import PPO

# 1. Train on a tape via the venue-stack-backed env
stack = lrvx.VenueStack.binance_um_futures(account_id=1, equity=10_000.0)
env = LrvxTradingEnv.from_venue_stack(
    stack, tape="./tapes/btc-2026-05-07",
    qty=0.01, max_position=0.05,
    tick_size=0.01, max_price_offset_ticks=50,
    n_open_slots=4,
)
model = PPO("MlpPolicy", env, verbose=1).learn(total_timesteps=100_000)

# 2. Wrap it as a lrvx.Strategy via the shared builder + decoder
builder = ObservationBuilder(
    window_size=env.window_size, n_open_slots=env.n_open_slots,
    tick_size=env.tick_size,
    max_price_offset_ticks=env.max_price_offset_ticks,
    max_position=env.max_position,
)
decoder = ActionDecoder(
    max_position=env.max_position,
    tick_size=env.tick_size,
    max_price_offset_ticks=env.max_price_offset_ticks,
)
policy = make_rl_policy(
    model, symbol_id=1,
    observation_builder=builder, action_decoder=decoder,
)

# 3a. Paper trading — same policy, live feed, simulated fills
from lrvx.paper import PaperBroker

broker = PaperBroker(registry=registry)
broker.set_strategy(policy)   # the broker owns its BacktestRunner session
broker.start()
# feed trades from your live source:
#   broker.observe_trade(symbol_id, price, qty, is_buy, ts_ns)

# 3b. Live — swap PaperBroker for CcxtBroker, the policy is unchanged
from lrvx.ccxt import CcxtBroker

broker = CcxtBroker("binanceusdm", api_key="...", secret="...")
sym = await broker.add_symbol("BTC/USDT:USDT")   # async; tick size from the market
broker.add_strategy(policy)
await broker.run()
```

`PaperBroker` and `CcxtBroker` are in `lrvx.paper` and
`lrvx.ccxt`; neither is re-exported at the top level. Each owns
its own `Runner` (and `CcxtBroker` its own `SymbolRegistry`), so
there is no separate `Runner` to construct. `CcxtBroker`'s first
argument is a ccxt.pro exchange *id* string, not an exchange object;
pass a constructed exchange through `exchange=` only for tests.

The model, builder, and decoder are byte-for-byte identical across all three modes; only the broker behind the signal callback differs. Anything the model learned about queue position, ack latency, fees, or funding in training will continue to apply in paper and live, because the underlying simulated executor is the same one the paper broker uses and the live broker mirrors.

## Walk-forward training

Training on the whole tape and reporting that number is the most common way RL trading projects fool themselves. `WalkForwardRL` ships the same anchored / sliding window discipline `WalkForwardRunner` uses for supervised backtests, with a fresh `VenueStack` per fold so no fee tier, rolling notional, or insurance fund state leaks across folds.

```python
import lrvx
from lrvx.rl_env import WalkForwardRL
from stable_baselines3 import PPO

wf = WalkForwardRL(
    venue_stack_factory=lambda: lrvx.VenueStack.binance_um_futures(42, 10_000.0),
    tape="./tapes/btc-2026-05",
    train_window_days=14,
    test_window_days=3,
    n_folds=6,
    mode="anchored",     # or "sliding"
    env_kwargs={
        "qty": 0.01, "max_position": 0.05,
        "window_size": 32, "tick_size": 0.01,
        "max_price_offset_ticks": 50,
        "n_open_slots": 4,
    },
)

for train_env, test_env in wf:
    model = PPO("MlpPolicy", train_env, verbose=0).learn(100_000)
    metrics = wf.evaluate(model, test_env)
    print(f"  fold return {metrics['return_pct']:+.2f}% "
          f"sharpe {metrics['sharpe']:+.2f} "
          f"dd {metrics['max_drawdown_pct']:.2f}%")

agg = wf.aggregate()
print(
    f"\nfolds={agg['n_folds']}  "
    f"mean={agg['mean_return_pct']:+.2f}%  std={agg['std_return_pct']:.2f}  "
    f"sign-match={agg['sign_match']:.0%}  worst={agg['worst_return_pct']:+.2f}%"
)
```

What the modes do:

- **`anchored`** — the train window starts at the first trade and expands fold by fold. Test windows tile forward in `test_window_days` steps. Models trained on every fold see all prior history.
- **`sliding`** — both windows slide forward together by `test_window_days` per fold. Each model sees only the most recent `train_window_days` of history. Use this when you suspect regime drift.

The aggregate schema (`mean_return_pct`, `std_return_pct`, `sign_match`, `worst_return_pct`, `mean_sharpe`, `mean_max_drawdown_pct`, `n_folds`) matches the supervised walk-forward output, so RL and non-RL sweeps land in one comparison table.

## Multi-symbol portfolios

Pass `tapes={symbol_id: tape, ...}` instead of `tape=...` to switch the env into multi-symbol mode. Observation and action spaces become Dict-shaped, the cross-margin Account walks every event's mark, and the agent sees an account-level slot alongside the per-symbol slots.

```python
import lrvx
from lrvx.rl_env import LrvxTradingEnv

stack = lrvx.VenueStack.binance_um_futures(account_id=1, equity=10_000.0)

env = LrvxTradingEnv.from_venue_stack(
    stack,
    tapes={
        1: "./tapes/btcusdt-2026-05-07",   # symbol id 1
        2: "./tapes/ethusdt-2026-05-07",   # symbol id 2
    },
    qty=0.01,
    max_position=0.05,
    window_size=32,
    tick_size=0.01,
    n_open_slots=2,           # per symbol; 2 is also the multi-symbol default
)

# Dict observation: {"1": Box((window+2+4*n_open,)), "2": Box(...), "account": Box((3,))}
# Dict action:      {"1": Box((3,)), "2": Box((3,))}
obs, info = env.reset(seed=0)

# Long BTC, short ETH market orders
action = {"1": [+1.0, 0.0, 0.0], "2": [-1.0, 0.0, 0.0]}
obs, reward, terminated, truncated, info = env.step(action)
```

What the multi-symbol path does:

- All per-symbol tapes are sorted into a single event stream by `ts_ns`. One env step consumes exactly one event; the event's symbol gets its mark and price-window updated.
- Positions, entry prices, open orders, and observation builders are tracked separately for each symbol. Switching one symbol's position has no effect on another's bookkeeping.
- All positions share the stack's single cross-margin Account. After every event the liquidation engine walks; the first liquidation event terminates the episode.
- The `"account"` observation key carries `[equity, total_notional, total_unrealized_pnl]`. The agent learns portfolio-level risk through this slot.
- Multi-symbol mode requires `action_mode="continuous"`. The Discrete(3) shorthand does not generalise meaningfully to a dict-of-actions.

Plugging this into Stable-Baselines3 needs `MultiInputPolicy` (Dict obs / Dict action support is built in). For RLlib see their `Multi-Agent` and `Dict` observation guides; for CleanRL roll a small wrapper that flattens to one big Box if your trainer needs it.

## Alpha-decay gate

`scripts/rl_alpha_decay_gate.py` is the CI-enforceable counterpart to the "one policy, three deployment modes" claim. It generates a deterministic synthetic tape from a seed, runs a fixed stub policy through `LrvxTradingEnv` and through `PaperBroker` (mirroring what `make_rl_policy` would do behind a runner), and asserts that the absolute equity change between the two paths stays within a configurable cap.

```bash
python scripts/rl_alpha_decay_gate.py --max-decay 0.30
```

Sample output:

```
# generated 4000 synthetic trades (seed=42)
running env path...
running paper path...

env   start=10000.0000 end=9999.9306 return=-0.0007% sharpe=-0.0286
paper start=10000.0000 end=9999.8890 return=-0.0011% sharpe=-0.0255
Δequity_env=-0.0694 Δequity_paper=-0.1110
decay=4.15% (cap=30.00%)
PASS: decay within cap
```

The gate is wired into the Linux CI job alongside the Python example runs. A failure means some change inside the venue-stack or the RL adapter pipeline shifted the env's physics away from PaperBroker's, even though both nominally share the same simulated executor configuration.

All inputs are synthetic to keep the repo free of redistributable market data. The optional `--tape /path/to/real.lrvx` flag exists for local sanity checks against private data; when set, the gate refuses to write CI artifacts so private market data can not leak into public logs.

The gate measures the gap between training-time physics and broker-time physics, not absolute profitability. A deliberately mediocre stub policy is used as the fixture so the decay number stays stable across seeds and the gate catches drift rather than alpha.

## stable_baselines3 with continuous actions

```python
from stable_baselines3 import PPO

env = LrvxTradingEnv.from_venue_stack(stack, tape=tape, qty=0.01)
model = PPO("MlpPolicy", env, verbose=1)
model.learn(total_timesteps=100_000)
```

`MlpPolicy` handles the Box action space natively. For discrete-only runners (e.g. DQN), construct with `action_mode="discrete"`.

## Plugging into stable_baselines3

```python
import gymnasium as gym
from stable_baselines3 import PPO
from lrvx.rl_env import LrvxTradingEnv

env = LrvxTradingEnv.from_tape("./tapes/btc-2026-05-07", qty=0.01)
# stable_baselines3 wraps any Gymnasium env; LrvxTradingEnv passes
# the duck-typed Env protocol it expects.
model = PPO("MlpPolicy", env, verbose=1)
model.learn(total_timesteps=100_000)
```

`stable_baselines3` does not introspect the env class identity; it calls the documented Env methods. The duck-typed `_DiscreteSpace` and `_BoxSpace` mirror the gymnasium API closely enough for both observation and action sampling to work. If your library is stricter, wrap the env in `gym.make` with a custom registration or substitute the spaces with real `gymnasium.spaces.Discrete` / `gymnasium.spaces.Box` objects post-construction.

## What this is not

- A live agent runtime. The env consumes a captured tape; for live RL you need an outer loop that feeds new trades and re-runs the policy. The same `step` / `reset` API applies, but the data plumbing is up to you.
- A multi-symbol simulator in the bare `from_tape` path. `from_tape` is single-symbol and `Discrete(3)` only; multi-symbol portfolios and the continuous `Box` action space require `from_venue_stack`.

## See also

* [Record and replay tapes](tape-record.md). The format the env consumes.
* [Backtest with realistic fills](backtest-realistic-fills.md). Slippage and queue knobs on the simulator side.
* [Backtest with latency](backtest-with-latency.md). Latency primitives that pair with the env when you want fill realism.
