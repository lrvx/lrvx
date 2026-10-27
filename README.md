[![CI](https://github.com/lrvx/lrvx/actions/workflows/ci.yml/badge.svg)](https://github.com/lrvx/lrvx/actions)
[![Docs](https://img.shields.io/badge/docs-site-blue)](https://docs.lrvx.dev)
[![PyPI](https://img.shields.io/pypi/v/lrvx?v=1)](https://pypi.org/project/lrvx/)
[![npm](https://img.shields.io/npm/v/@lrvx/lrvx)](https://www.npmjs.com/package/@lrvx/lrvx)

## lrvx

lrvx is a framework for building trading systems.

Strategies, backtests, paper trading, and live execution sit behind one toolkit. AI agents discover the surface and drive it end-to-end through an MCP control plane. One strategy class runs backtest, paper, and live. Bindings for Python, Node.js, Codon, embedded JavaScript, and a stable C API.

Documentation is available at [docs.lrvx.dev](https://docs.lrvx.dev)

## Language bindings

| Language | Install | Docs |
|----------|---------|------|
| Python | `pip install lrvx` | [reference](https://docs.lrvx.dev/reference/python/) |
| Node.js | `npm install @lrvx/lrvx` | [reference](https://docs.lrvx.dev/reference/node/) |
| Codon | build from source | [reference](https://docs.lrvx.dev/reference/codon/) |
| JavaScript (embedded) | bundled with C++ build | [reference](https://docs.lrvx.dev/reference/quickjs/) |
| C API | `liblrvx_capi.so` | [reference](https://docs.lrvx.dev/reference/api/capi/lrvx_capi/) |

All bindings expose the same strategy API, indicators, order books, backtesting, and data I/O. The C API is the integration point for adding support for any other language.

## AI companion

`lrvx-mcp` is a Model Context Protocol server that gives AI coding agents (Cursor, Claude Code, Cline) grounded access to the lrvx surface — symbol lookups across bindings, scaffolders, indicator and backtest tools, full-text doc search.

```bash
pip install lrvx-mcp
```

See [the package README](./mcp/README.md) for setup and the full tool list.

## Connectors

Native exchange connectors (Bybit, Bitget, Hyperliquid, Polymarket) live under [`connectors/`](./connectors/) and build with `-DLRVX_BUILD_CONNECTORS=ON`. The flag defaults to OFF so a backtest-only or research build doesn't pay the dependency cost. See the [connectors README](./connectors/README.md) for adapter notes and the optional Polymarket Rust toolchain step.

## Venue

Beyond trading on a market, lrvx can be one: the optional [`venue/`](./venue/) module adds a matching engine and the machinery around it, so many participants can trade against a single order book. See the [venue docs](https://docs.lrvx.dev/venue/).

## Build options

The CMake options that gate every optional artefact (bindings, demo, tools, tests, benchmarks, connectors) are catalogued in [`docs/build/feature-flags.md`](https://docs.lrvx.dev/build/feature-flags/). Defaults are OFF so a bare `cmake -B build` produces only the core C++ static library.

## Commercial Services

For commercial support, enterprise connectors, and custom development, visit [lrvx.dev/services](https://lrvx.dev/services).

## Contributing

Contributions are welcome via pull requests.
Please follow the existing structure and naming conventions.
Tests, benchmarks, and documentation should be included where appropriate.
Code style is enforced via `clang-format`. See [contributing guide](https://docs.lrvx.dev/how-to/contributing/).

## License

lrvx is licensed under the MIT License. See [LICENSE](./LICENSE) for details.

## Disclaimer

lrvx is provided "as is" without warranty of any kind, express or implied.
The authors are not liable for any damages or financial harm arising from its use, including trading losses or system failures.
This software is intended for educational and research purposes. Production use is at your own risk.
See [DISCLAIMER.md](./DISCLAIMER.md) for full legal notice.
