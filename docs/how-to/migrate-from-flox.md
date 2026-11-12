# Migrate from FLOX

lrvx is the new name of FLOX. The engine, the bindings and the on-disk formats are the same; the packages, import names, commands, environment variables and CMake options carry the new name. Existing tapes and run traces read unchanged, and the `flox-py` and `flox-mcp` packages remain as deprecated shims, so Python code can move one piece at a time. The old environment variable and CMake option names are not read; the tables below map each one to its new name.

## New names

| What | FLOX | lrvx |
|---|---|---|
| Python package (PyPI) | `flox-py` | `lrvx` |
| Python import | `import flox_py` | `import lrvx` |
| Python CLI | `flox` | `lrvx` |
| MCP server package (PyPI) | `flox-mcp` | `lrvx-mcp` |
| MCP server import / CLI | `flox_mcp` / `flox-mcp` | `lrvx_mcp` / `lrvx-mcp` |
| npm package | `@flox-foundation/flox` | `@lrvx/lrvx` |
| Codon package | `from flox.strategy import ...` | `from lrvx.strategy import ...` |
| Embedded QuickJS global | `flox` | `lrvx` |
| C++ namespace / headers | `flox::`, `<flox/...>` | `lrvx::`, `<lrvx/...>` |
| CMake package / target | `find_package(flox)`, `flox::flox` | `find_package(lrvx)`, `lrvx::lrvx` |
| C API | `libflox_capi`, `flox_*`, `Flox*` | `liblrvx_capi`, `lrvx_*`, `Lrvx*` |
| Tape / run / index | `.floxlog`, `.floxrun`, `index.floxidx` | `.lrvx`, `.lrvxrun`, `index.lrvxidx` |
| Environment variables | `FLOX_*` | `LRVX_*` |
| CMake options | `FLOX_*` | `LRVX_*` |
| Repository | `github.com/FLOX-Foundation/flox` | `github.com/lrvx/lrvx` |
| Documentation | `flox-foundation.github.io/flox` | `docs.lrvx.dev` |

## Install the new packages

```bash
pip uninstall flox-py flox-mcp
pip install lrvx            # add lrvx-mcp for the MCP server
npm uninstall @flox-foundation/flox
npm install @lrvx/lrvx
```

Then replace the imports: `import flox_py` (or `import flox_py as flox`) becomes `import lrvx`, `require('@flox-foundation/flox')` becomes `require('@lrvx/lrvx')`, and `from flox.<module> import ...` in Codon becomes `from lrvx.<module> import ...`. For the MCP server, run `lrvx-mcp init` to write a client config that launches `lrvx-mcp`; the server entry is named `lrvx`, so tools appear under that prefix in the client. An `mcpServers.flox` entry that launches `flox-mcp` is replaced rather than kept next to it, and the engine URL and token it carried move over.

## Compatibility packages

`flox-py` 0.10.2 and `flox-mcp` 0.10.2 are deprecated shims for code that cannot move yet:

- `flox-py` depends on the matching `lrvx` release. `import flox_py` emits one `DeprecationWarning` and re-exports everything `lrvx` exports; `flox_py.<module>` (for example `flox_py.cli` or `flox_py.tape`) is the same module object as `lrvx.<module>`, and `flox_py.FloxError` is `lrvx.LrvxError`. The `flox` command prints a deprecation line to stderr and runs `lrvx`.
- `flox-mcp` does the same for `lrvx-mcp`: `import flox_mcp` forwards to `lrvx_mcp`, and the `flox-mcp` command runs `lrvx-mcp`.

The shims add nothing of their own and receive no further development. They are built from `compat/` in the repository with `scripts/build_compat_shims.sh <outdir>`.

## Environment variables

Every variable reads only its `LRVX_*` name; the `FLOX_*` names are not read, so rename them in shell profiles, service units, CI settings and `.env` files.

| FLOX | lrvx | Read by |
|---|---|---|
| `FLOX_LIVE` | `LRVX_LIVE` | the `live` project template (`lrvx new --template=live`) |
| `FLOX_ARCHIVE_CACHE` | `LRVX_ARCHIVE_CACHE` | the exchange archive importers |
| `FLOX_RUNTIME_STATE` | `LRVX_RUNTIME_STATE` | `lrvx-mcp` runtime tools |
| `FLOX_CONTROL_URL` | `LRVX_CONTROL_URL` | `lrvx-mcp` control and analytics tools |
| `FLOX_CONTROL_TOKEN` | `LRVX_CONTROL_TOKEN` | `lrvx-mcp` control and analytics tools |
| `FLOX_HL_SIGNER_SOCKET` | `LRVX_HL_SIGNER_SOCKET` | the Hyperliquid connector's signer client |
| `FLOX_MCP_REQUIRE_DEPS` | `LRVX_MCP_REQUIRE_DEPS` | the `lrvx-mcp` test suite |
| `FLOX_FUZZ_OPS` | `LRVX_FUZZ_OPS` | the venue differential fuzz test, for a deep run |
| `FLOX_UPDATE_GOLDEN` | `LRVX_UPDATE_GOLDEN` | the venue golden replay test, to rewrite its hashes |
| `FLOX_UPDATE_SESSION_TABLE` | `LRVX_UPDATE_SESSION_TABLE` | the venue session test, to rewrite its golden table |
| `FLOX_BENCH_RT` | `LRVX_BENCH_RT` | `batch_delivery_benchmark`, to opt in to real-time scheduling |
| `FLOX_LIBCLANG_PATH` | `LRVX_LIBCLANG_PATH` | the C API code generator in `tools/codegen` |
| `FLOX_LIBCLANG` | `LRVX_LIBCLANG` | `scripts/check_gate_reachability.py` |
| `FLOX_COMPILE_COMMANDS`, `FLOX_CMAKE` | `LRVX_COMPILE_COMMANDS`, `LRVX_CMAKE` | `scripts/check_doc_cpp_examples.py` |
| `FLOX_PY_BUILD_DIR` | `LRVX_PY_BUILD_DIR` | `scripts/gen_pyi_stubs.py` |
| `FLOX_CTEST_BUILD_DIR` | `LRVX_CTEST_BUILD_DIR` | `scripts/run-with-sanitizer-scan.sh` |

Default locations carry the new name: the archive cache is `~/.lrvx/archive-cache` and the runtime snapshot is `/tmp/lrvx-runtime-state.json`. Point `LRVX_ARCHIVE_CACHE` at the old `~/.flox/archive-cache` to keep the downloads already cached there.

## CMake options

CMake does not recognise the `FLOX_*` option names. A `FLOX_*` value passed with `-D` sets nothing, and CMake lists it under "Manually-specified variables were not used by the project"; one set by a preset, a parent project or an existing build directory's cache is ignored without a warning. Each option keeps its default until it is set under its `LRVX_*` name.

| FLOX | lrvx |
|---|---|
| `FLOX_BUILD_TESTS` | `LRVX_BUILD_TESTS` |
| `FLOX_BUILD_BENCHMARKS` | `LRVX_BUILD_BENCHMARKS` |
| `FLOX_BUILD_DEMO` | `LRVX_BUILD_DEMO` |
| `FLOX_BUILD_TOOLS` | `LRVX_BUILD_TOOLS` |
| `FLOX_BUILD_PYTHON` | `LRVX_BUILD_PYTHON` |
| `FLOX_BUILD_NODE` | `LRVX_BUILD_NODE` |
| `FLOX_BUILD_CAPI` | `LRVX_BUILD_CAPI` |
| `FLOX_BUILD_CODON` | `LRVX_BUILD_CODON` |
| `FLOX_BUILD_QUICKJS` | `LRVX_BUILD_QUICKJS` |
| `FLOX_BUILD_CONNECTORS` | `LRVX_BUILD_CONNECTORS` |
| `FLOX_BUILD_VENUE` | `LRVX_BUILD_VENUE` |
| `FLOX_BUILD_CONNECTOR_INTEGRATION_TESTS` | `LRVX_BUILD_CONNECTOR_INTEGRATION_TESTS` |
| `FLOX_ENABLE_BENCHMARKS`, `FLOX_ENABLE_TESTS`, `FLOX_ENABLE_DEMO`, `FLOX_ENABLE_TOOLS`, `FLOX_ENABLE_PYTHON`, `FLOX_ENABLE_CAPI`, `FLOX_ENABLE_CODON`, `FLOX_ENABLE_QUICKJS`, the older spellings of the `FLOX_BUILD_*` options | the matching `LRVX_BUILD_*` |
| `FLOX_ENABLE_BACKTEST` | `LRVX_ENABLE_BACKTEST` |
| `FLOX_ENABLE_LZ4` | `LRVX_ENABLE_LZ4` |
| `FLOX_ENABLE_ONNX` | `LRVX_ENABLE_ONNX` |
| `FLOX_ENABLE_AF_XDP` | `LRVX_ENABLE_AF_XDP` |
| `FLOX_ENABLE_TRACY` | `LRVX_ENABLE_TRACY` |
| `FLOX_ENABLE_CPU_AFFINITY` | `LRVX_ENABLE_CPU_AFFINITY` |
| `FLOX_ENABLE_DEV_SETUP` | `LRVX_ENABLE_DEV_SETUP` |
| `FLOX_ENABLE_POLYMARKET_ORDER_EXECUTOR` | `LRVX_ENABLE_POLYMARKET_ORDER_EXECUTOR` |
| `FLOX_NATIVE` | `LRVX_NATIVE` |
| `FLOX_VENUE_LITE` | `LRVX_VENUE_LITE` |
| `FLOX_VENUE_PERIMETER` | `LRVX_VENUE_PERIMETER` |
| `FLOX_FETCH_GTEST` | `LRVX_FETCH_GTEST` |
| `FLOX_FIX_TLS` | `LRVX_FIX_TLS` |
| `FLOX_RUN_CONNECTOR_INTEGRATION_TESTS` | `LRVX_RUN_CONNECTOR_INTEGRATION_TESTS` |
| `FLOX_CONNECTORS` | `LRVX_CONNECTORS` |
| `FLOX_DEFAULT_EVENTBUS_CAPACITY` | `LRVX_DEFAULT_EVENTBUS_CAPACITY` |
| `FLOX_DEFAULT_EVENTBUS_MAX_CONSUMERS` | `LRVX_DEFAULT_EVENTBUS_MAX_CONSUMERS` |
| `FLOX_NODE_CPU_AFFINITY` | `LRVX_NODE_CPU_AFFINITY` |
| `FLOX_NODE_SANITIZE` | `LRVX_NODE_SANITIZE` |

## Compile-time defines

Preprocessor switches follow the same rule: a `FLOX_*` define on the compiler command line or in a config header is not read.

| FLOX | lrvx |
|---|---|
| `FLOX_DISABLE_LOGGING` | `LRVX_DISABLE_LOGGING` |
| `FLOX_DEFAULT_ORDER_TRACKER_CAPACITY` | `LRVX_DEFAULT_ORDER_TRACKER_CAPACITY` |
| `FLOX_DEFAULT_CONNECTOR_POOL_CAPACITY` | `LRVX_DEFAULT_CONNECTOR_POOL_CAPACITY` |
| `FLOX_SCALE_CHECKS` | `LRVX_SCALE_CHECKS` |
| `FLOX_FORCE_PORTABLE_INT128` | `LRVX_FORCE_PORTABLE_INT128` |
| `FLOX_FIX_TLS` | `LRVX_FIX_TLS` |
| `FLOX_ENABLE_TRACY` | `LRVX_ENABLE_TRACY` |
| `FLOX_UNIT_TEST` | `LRVX_UNIT_TEST` |

## Tapes, run traces and indexes

The binary formats are the same under both names and the segment sentinel spells `FLOX`, so nothing needs converting.

- Directories of `.floxlog` segments read as they are. Every reader, the validator, the index builders, merge and split, and `lrvx tape view` accept `.floxlog` next to `.lrvx`, and a directory that mixes the two reads as one tape, in time order wherever the segments prove it, whatever their extension ([read order](../spec/lrvx.md#read-order)). New segments are written as `.lrvx`. See the [`.lrvx` spec](../spec/lrvx.md).
- A `.floxrun` trace directory opens like any other: `TraceReader` takes the directory path whatever its suffix, `lrvx tape view` accepts `.floxrun`, and bundle validation reads `expected.floxrun/` from bundles packed under the old name. See the [`.lrvxrun` spec](../spec/lrvxrun.md).
- A global index written as `index.floxidx` loads by path with `GlobalIndexBuilder::load`; rebuilding writes `index.lrvxidx`. Per-segment indexes live inside the segment files and need nothing.

Renaming existing files to the new extensions is optional.
