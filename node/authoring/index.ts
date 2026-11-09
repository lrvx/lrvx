// Authoring layer for TypeScript strategies.
//
// Lets you write a lrvx strategy as a TS class with decorated methods
// instead of a plain object literal. The runtime surface is exactly the
// same — `compile()` returns the duck-typed Strategy object that
// BacktestRunner / Engine / WalkForwardRunner already accept.
//
// Stage 3 method decorators (no `experimentalDecorators` flag).

import type {
  BarData,
  EmitMethods,
  Strategy,
  Symbol as LrvxSymbol,
  SymbolContext,
  TradeData,
} from "..";

const HANDLERS: unique symbol = Symbol.for(
  "lrvx.authoring.handlers"
);
const SYMBOLS: unique symbol = Symbol.for(
  "lrvx.authoring.symbols"
);

type HandlerName =
  | "onStart"
  | "onStop"
  | "onTrade"
  | "onBar"
  | "onBookUpdate"
  | "onTouch";

interface HandlerMap {
  onStart?: () => void;
  onStop?: () => void;
  onTrade?: (ctx: SymbolContext, trade: TradeData, emit: EmitMethods) => void;
  onBar?: (ctx: SymbolContext, bar: BarData, emit: EmitMethods) => void;
  onBookUpdate?: (ctx: SymbolContext, emit: EmitMethods) => void;
  onTouch?: (ctx: SymbolContext, emit: EmitMethods) => void;
}

interface HandlerCarrier {
  [HANDLERS]?: HandlerMap;
}

interface ClassWithSymbols {
  [SYMBOLS]?: ReadonlyArray<LrvxSymbol | number>;
}

function methodDecorator<K extends HandlerName>(
  name: K
): <This, Args extends unknown[], R>(
  target: (this: This, ...args: Args) => R,
  context: ClassMethodDecoratorContext<This, (this: This, ...args: Args) => R>
) => (this: This, ...args: Args) => R {
  return (target, context) => {
    if (context.private) {
      throw new Error("lrvx decorators cannot be applied to private methods");
    }
    if (context.static) {
      throw new Error("lrvx decorators cannot be applied to static methods");
    }
    context.addInitializer(function (this: unknown) {
      const carrier = this as HandlerCarrier;
      const map: HandlerMap = (carrier[HANDLERS] ??= {});
      map[name] = (target as Function).bind(this) as HandlerMap[K];
    });
    return target;
  };
}

/** Wire the decorated method as the strategy's `onTrade` callback. */
export const onTrade = methodDecorator("onTrade");

/** Wire the decorated method as the strategy's `onBar` callback. */
export const onBar = methodDecorator("onBar");

/** Wire the decorated method as the strategy's `onBookUpdate` callback. */
export const onBookUpdate = methodDecorator("onBookUpdate");

/** Wire the decorated method as the strategy's `onTouch` callback. */
export const onTouch = methodDecorator("onTouch");

/** Wire the decorated method as the strategy's `onStart` callback. */
export const onStart = methodDecorator("onStart");

/** Wire the decorated method as the strategy's `onStop` callback. */
export const onStop = methodDecorator("onStop");

/** Options for the `@strategy` class decorator. */
export interface StrategyOptions {
  /**
   * Symbols this strategy subscribes to. Equivalent to setting
   * `symbols` on a plain Strategy object.
   */
  symbols?: ReadonlyArray<LrvxSymbol | number>;
}

/**
 * Class decorator. Marks a class as a lrvx strategy and records the
 * symbol set on the class itself. Use with `compile(instance)` to
 * produce a runtime Strategy object.
 */
export function strategy(
  options: StrategyOptions = {}
): <T extends abstract new (...args: never[]) => unknown>(
  target: T,
  context: ClassDecoratorContext<T>
) => T {
  return (target, _context) => {
    (target as unknown as ClassWithSymbols)[SYMBOLS] = options.symbols ?? [];
    return target;
  };
}

/** Optional base class. Pure ergonomics; subclassing is not required. */
export abstract class StrategyBase {}

/**
 * Convert a decorated strategy instance into the duck-typed Strategy
 * object that BacktestRunner / Engine accept. Methods are bound to the
 * instance, so callbacks see the original `this`.
 */
export function compile(instance: object): Strategy {
  const carrier = instance as HandlerCarrier;
  const map: HandlerMap = carrier[HANDLERS] ?? {};
  const ctor = (instance as { constructor: ClassWithSymbols }).constructor;
  const symbols = ctor[SYMBOLS] ?? [];

  const out: Strategy = { symbols };
  if (map.onStart) out.onStart = map.onStart;
  if (map.onStop) out.onStop = map.onStop;
  if (map.onTrade) out.onTrade = map.onTrade;
  if (map.onBar) out.onBar = map.onBar;
  if (map.onBookUpdate) out.onBookUpdate = map.onBookUpdate;
  if (map.onTouch) out.onTouch = map.onTouch;
  return out;
}
