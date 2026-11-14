# lrvx

**Framework for building trading systems.**

Strategies, backtests, paper trading, and live execution sit
behind one toolkit. AI agents discover the surface and drive it
end-to-end through an MCP control plane. One strategy class
runs backtest, paper, and live.

[![GitHub](https://img.shields.io/badge/GitHub-lrvx--dev%2Flrvx-blue?logo=github)](https://github.com/lrvx/lrvx)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](https://github.com/lrvx/lrvx/blob/main/LICENSE)

---

## Getting started

Pick your language:

| | |
|---|---|
| [**Python quickstart**](tutorials/python-quickstart.md) | Indicators, strategies, and backtesting from Python |
| [**Node.js quickstart**](tutorials/node-quickstart.md) | Same API from JavaScript/TypeScript |
| [**C++ quickstart**](tutorials/quickstart.md) | Build lrvx and run the demo |
| [**Architecture**](explanation/architecture.md) | How the components fit together |

---

## Quick example

=== "Python"

    ```python
    import lrvx

    ema = lrvx.EMA(20)
    rsi = lrvx.RSI(14)

    for price in prices:
        e = ema.update(price)
        r = rsi.update(price)
        if e is not None and r is not None:
            print(f"EMA {e:.2f}  RSI {r:.1f}")
    ```

=== "Node.js"

    ```javascript
    const lrvx = require('@lrvx/lrvx');

    const ema = new lrvx.EMA(20);
    const rsi = new lrvx.RSI(14);

    for (const price of prices) {
        const e = ema.update(price);
        const r = rsi.update(price);
        if (e !== null && r !== null) {
            console.log(`EMA ${e.toFixed(2)}  RSI ${r.toFixed(1)}`);
        }
    }
    ```

=== "C++"

    ```cpp
    #include "lrvx/strategy/abstract_strategy.h"
    #include "lrvx/book/events/trade_event.h"

    class MyStrategy : public lrvx::IStrategy {
    public:
        lrvx::SubscriberId id() const override {
            return reinterpret_cast<lrvx::SubscriberId>(this);
        }

        void onTrade(const lrvx::TradeEvent& event) override {
            if (event.trade.symbol == _targetSymbol) {
                processSignal(event.trade.price);
            }
        }

        void start() override { _running = true; }
        void stop() override  { _running = false; }

    private:
        lrvx::SymbolId _targetSymbol;
        bool _running = false;
    };
    ```

---

## Language bindings

| Language | Guide | Reference |
|---|---|---|
| Python | [Python](bindings/python.md) | [reference](reference/python/index.md) |
| Node.js | [Node.js](bindings/node.md) | [reference](reference/node/index.md) |
| Codon | [Codon](bindings/codon.md) | [reference](reference/codon/index.md) |
| JavaScript (embedded) | [JavaScript](bindings/javascript.md) | [reference](reference/quickjs/index.md) |
| C API | [C API](bindings/capi.md) | [reference](reference/api/capi/lrvx_capi.md) |

---

## Documentation

| Section | Description |
|---------|-------------|
| [Tutorials](tutorials/README.md) | Step-by-step lessons for new users |
| [How-To Guides](how-to/README.md) | Solutions for specific problems |
| [Explanation](explanation/README.md) | Architecture and design concepts |
| [Reference](reference/README.md) | API specifications |

---

## Features

| Feature | Description |
|---------|-------------|
| Lock-free event delivery | Disruptor-style ring buffers, busy-spin consumers |
| Zero-allocation hot path | Pre-allocated pools, no heap allocation in market data callbacks |
| CPU affinity support | Pin threads to isolated cores |
| Multi-exchange trading | [CEX coordination](reference/api/cex/composite_book_matrix.md) with aggregation and smart routing |
| Native connectors | [Bybit / Bitget / Hyperliquid / Polymarket](explanation/connectors.md) under `connectors/`, opt-in via `-DLRVX_BUILD_CONNECTORS=ON` |
| Binary replay system | Record live data, replay for backtesting |
| Backtest analytics | [Grid search](how-to/grid-search.md), [walk-forward](how-to/walk-forward.md), [heatmap](how-to/heatmap.md), [White's reality check](how-to/whites-reality-check.md), [MLflow logging](how-to/mlflow.md) |
| Multi-language bindings | Python, Node.js, Codon, JavaScript, C API |
| AI companion | [`lrvx-mcp`](https://github.com/lrvx/lrvx/tree/main/mcp) — Model Context Protocol server for Cursor / Claude Code / Cline |
| Type-safe primitives | Strong types for Price, Quantity, SymbolId |
| Modular architecture | Use only what you need |

---

## Requirements

Pick the row for your binding. The C++ engine itself only matters if you build from source.

| If you use | You need |
|---|---|
| **Python** | Python 3.10+. `pip install lrvx` for prebuilt wheels (CPython 3.10 to 3.15; Linux, macOS, Windows). |
| **Node.js** | Node 20+. `npm install @lrvx/lrvx`. |
| **Codon** | Codon compiler + `lrvx` built with `-DLRVX_BUILD_CAPI=ON`. |
| **C++** | C++23 compiler (GCC 14+ / Clang 18+; MSVC or clang-cl on Windows), CMake 3.22+. |

Optional across all bindings: LZ4 for binary log compression.

---

## License

MIT License
