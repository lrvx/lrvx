# Explanation

Understand concepts and design decisions behind lrvx. These pages are language-agnostic; code samples use tabs where they help.

## Overview

| Topic | What You'll Learn |
|-------|-------------------|
| [Architecture](architecture.md) | How components fit together |
| [Bar Types](bar-types.md) | Time, tick, volume, range, bps-range, Renko, Heikin-Ashi — when to use which |
| [Disruptor Pattern](disruptor.md) | Why we use ring buffers (system-level, C++ internals) |
| [Memory Model](memory-model.md) | Zero-allocation event delivery (system-level, C++ internals) |
| [Integration Flow](integration-flow.md) | End-to-end data flow through the system |
| [Indicators](indicators.md) | What each indicator measures and when to use it |
| [Venue types](venue-types.md) | How a venue type (CEX, AMM, hybrid) drives pricing and settlement routing |
| [Per-symbol scale](per-symbol-scale.md) | How fixed-point price/quantity scale is chosen per symbol for DEX-range tokens |
| [On-chain order lifecycle](onchain-order-lifecycle.md) | Pending, reverted, and gas-replaced states for DEX orders |
| [Liquidity-provision signals](liquidity-provision.md) | Provide and withdraw signals for AMM pool positions |
| [Position valuation](position-valuation.md) | The hook for nonlinear (LP, option) position valuation |
| [AMM pricing in backtests](amm-pricing.md) | Constant-product pool pricing, slippage, and price impact for DEX swaps |
| [Market-making quoter](market-making-quoter.md) | Two-sided quote ladder with inventory skew and requote tolerance |
| [AMM DEX connector](amm-dex-connector.md) | Presenting a constant-product pool through the connector interface |
| [Replay-equivalence gate](replay-equivalence-gate.md) | The CI check that defends deterministic backtest replay |
| [The reference backtest](reference-backtest.md) | One strategy, one file, pinned numbers, and the execution rules they follow |
| [Bar-close ordering](bar-close-ordering.md) | Which aggregator fires first when several bars close on the same instant |
| [Connectors](connectors.md) | The native exchange connectors module and the interfaces it implements |
| [MCP control plane](mcp-control-plane.md) | Why the AI-agent control surface is shaped the way it is |
| [Venue-stack reproducibility](venue-stack-reproducibility.md) | What makes a backtest result reproducible across machines |
| [Build feature flags](../build/feature-flags.md) | Every CMake option, what it costs, and what it depends on |

## When to read these

- **Before** diving deep into customization
- **When** you want to understand design trade-offs
- **If** you're debugging performance issues

## A note on language

Some explanation pages — Disruptor Pattern, Memory Model — describe internals of the C++ engine itself. Code in those pages is C++ because that *is* the implementation. If you only use the Python, Node.js, or Codon bindings you can read them as background; the bindings expose the relevant behaviour through their own APIs without you needing to touch C++.
