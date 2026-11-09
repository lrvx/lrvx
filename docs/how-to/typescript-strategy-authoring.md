# Write a strategy in TypeScript with decorators

The Node binding takes plain object literals as strategies. That works, but TypeScript users tend to want classes and a typed surface they can lean on. The `lrvx/authoring` module is a thin layer on top of that: a class decorator for the symbol set, method decorators for the callbacks, and a `compile()` adapter that turns a decorated instance into the runtime Strategy object the engine already accepts. No new C++ surface, no separate runtime.

This is the same model Pine Script users reach for, except the language is TypeScript and the engine is lrvx.

## Quick start

Install the package and pick a TypeScript version that supports stage 3 decorators (TS 5.0 or newer; the binding's own `tsconfig` ships with TS 6.0.3).

```typescript
import * as lrvx from "@lrvx/lrvx";
import {
  compile,
  onStart,
  onTrade,
  strategy,
  StrategyBase,
} from "lrvx/authoring";

const reg = new lrvx.SymbolRegistry();
const btc = Number(reg.addSymbol("bybit", "BTCUSDT", 0.01));

@strategy({ symbols: [btc] })
class SmaCross extends StrategyBase {
  private readonly fast = new lrvx.SMA(10);
  private readonly slow = new lrvx.SMA(30);

  @onStart
  start(): void {
    console.log("strategy starting");
  }

  @onTrade
  onEachTrade(ctx: lrvx.SymbolContext, t: lrvx.TradeData, emit: lrvx.EmitMethods): void {
    const f = this.fast.update(t.price);
    const s = this.slow.update(t.price);
    if (f === null || s === null) return;
    if (f > s && ctx.position === 0) emit.marketBuy(0.01);
    else if (f < s && ctx.position > 0) emit.marketSell(0.01);
  }
}

const runner = new lrvx.Runner(reg, () => {});
runner.addStrategy(compile(new SmaCross()));
runner.start();
// ... feed trades via runner.onTrade(...) ...
runner.stop();
```

`compile()` returns a `lrvx.Strategy` object. Anywhere a Strategy is accepted (`Runner.addStrategy`, `BacktestRunner.setStrategy`, `WalkForwardRunner.setStrategyFactory`), a compiled instance works the same way.

## Decorators

| Decorator        | Wires up           | Signature                                              |
|------------------|--------------------|--------------------------------------------------------|
| `@strategy(opts)`| Class              | Records `symbols` on the class                         |
| `@onStart`       | Method             | `(): void`                                             |
| `@onStop`        | Method             | `(): void`                                             |
| `@onTrade`       | Method             | `(ctx, trade, emit): void`                             |
| `@onBookUpdate`  | Method             | `(ctx, emit): void`                                    |
| `@onTouch`       | Method             | `(ctx, emit): void`                                    |
| `@onBar`         | Method             | `(ctx, bar, emit): void`                               |

The method decorator binds the function to the instance at construction time and stores it on a hidden symbol-keyed map. `compile()` reads that map and copies the bound functions onto the returned Strategy. Method names do not have to match the callback names; only the decorator matters.

If you decorate the same callback twice on a class, the last one wins. Private methods and static methods are rejected.

## Why use it

`SymbolContext`, `TradeData` and `EmitMethods` come from the binding's own `index.d.ts`, so the IDE knows what `ctx.position` is and what `emit.marketBuy` expects.

Per-strategy state lives on `this`: indicators, position counters, debug flags. No closures over a builder, no shared mutable scope between symbols.

The runtime contract does not change. `compile()` produces the same Strategy object you would have written by hand, so feature parity with the plain-object form is automatic.

## What it is not

- Not a separate engine. The TS class is sugar over the existing object-literal API. There is no extra dispatch and no new C++ surface.
- Not a replacement for `lrvx.Strategy` in Python or the C++ class. The TS authoring layer is Node-only.
- Not a hot-reload mechanism. Reloading a strategy means tearing down the runner and starting a new one. Hot-reload is tracked separately.

## Compiling

The decorators are stage 3, so no `experimentalDecorators` flag is needed. A minimal `tsconfig.json`:

```json
{
  "compilerOptions": {
    "target": "ES2022",
    "module": "commonjs",
    "moduleResolution": "Node10",
    "strict": true,
    "esModuleInterop": true,
    "skipLibCheck": true
  },
  "include": ["src/**/*.ts"]
}
```

`target: ES2022` matters: stage 3 decorators emit cleaner output above ES2022, and the authoring lib itself is compiled at that level.

## See also

- [Strategy classes](strategy-classes.md) for the cross-language Strategy model.
- The full type surface lives in `index.d.ts` next to the package entry point.
