# Build feature flags

CMake options for tailoring a lrvx build to a specific use case. Two prefixes, by purpose:

- `LRVX_ENABLE_*` — capabilities of the core library (backtest module, LZ4 compression, Tracy profiler, CPU affinity).
- `LRVX_BUILD_*` — optional build artefacts (binding wheels, demo, tools, tests, benchmarks, connectors).

Most options default to OFF unless noted. The exceptions default ON: `LRVX_NATIVE`, `LRVX_ENABLE_LZ4`, and **`LRVX_BUILD_VENUE`** — and because `LRVX_BUILD_VENUE=ON` force-enables `LRVX_ENABLE_BACKTEST`, a bare `cmake -B build` builds the core library **plus the venue and backtest modules** (venue is disabled automatically on MSVC/clang-cl, which lack native 128-bit ints). To get only the core library, pass `-DLRVX_BUILD_VENUE=OFF`. `LRVX_ENABLE_ONNX` and `LRVX_ENABLE_AF_XDP` default OFF and hard-fail if their libraries are absent.

A third prefix gates compiler/portability flags rather than features:

- `LRVX_NATIVE` — controls whether Release builds target the build host's exact ISA (`-march=native`) or a portable baseline. Default ON for fastest local builds; flip OFF when shipping artefacts to machines whose CPUs you do not control (e.g. `lrvx` wheels do this automatically).

## Capabilities

| Flag | Default | Effect |
|---|---|---|
| `LRVX_ENABLE_BACKTEST` | OFF | Compile `src/backtest/` into the core library. Required for `BacktestRunner`, `WalkForwardRunner`, `GridSearch`. |
| `LRVX_ENABLE_LZ4` | **ON** | Link LZ4 for replay-log compression. Tries `find_package(lz4 CONFIG)` first, then `find_library`, then pkg-config, then a vendored LZ4 source via FetchContent — so the dependency self-resolves on every supported platform. Set to OFF only if you specifically need an LZ4-free build. |
| `LRVX_ENABLE_ONNX` | OFF | Compiles the ONNX Runtime inference nodes (`lrvx/ml/`) into the core library, plus `test_onnx_inference` once tests are on. Hard-fails at configure time when the onnxruntime library or headers are missing; there is no bundled fallback. No CI workflow turns this on: none of the runners carry the onnxruntime C++ SDK. Check it locally with `-DLRVX_ENABLE_ONNX=ON -DLRVX_BUILD_TESTS=ON` after installing onnxruntime. |
| `LRVX_ENABLE_AF_XDP` | OFF | Compiles the AF_XDP receive-path backend (`lrvx/net/af_xdp_receive_path.h`). Linux only; hard-fails at configure time when libxdp/libbpf are missing. No test is gated on this flag, and no CI runner has the kernel or library support it needs. |
| `LRVX_ENABLE_TRACY` | OFF | Links the Tracy profiler client for runtime instrumentation; off in production builds. No test is gated on this flag, so CI never builds with it on. |
| `LRVX_ENABLE_CPU_AFFINITY` | OFF | Compiles pthread / NUMA-aware affinity helpers. Linux only; the calls are no-ops on macOS and Windows. **Warning:** can hurt performance on busy or shared systems — use it only on isolated, dedicated hardware. The gated `test_cpu_affinity` suite runs in the `affinity-tests` job, but only on the Sunday 3am schedule, not on pull requests. |
| `LRVX_ENABLE_DEV_SETUP` | OFF | Install the project's pre-commit hook into `.git/hooks/` at configure time. Developer-only. |

## Artefacts

| Flag | Default | Effect | Depends on |
|---|---|---|---|
| `LRVX_BUILD_TESTS` | OFF | Build the GoogleTest suite under `tests/`. | `find_package(GTest)` |
| `LRVX_BUILD_BENCHMARKS` | OFF | Build the Google Benchmark suite under `benchmarks/`. | benchmark, optionally `LRVX_ENABLE_BACKTEST` |
| `LRVX_BUILD_DEMO` | OFF | Build the `lrvx_demo` executable. | optionally `LRVX_ENABLE_BACKTEST` |
| `LRVX_BUILD_TOOLS` | OFF | Build the CLI tools under `tools/`. | optionally `LRVX_ENABLE_BACKTEST` |
| `LRVX_BUILD_PYTHON` | OFF | Build `lrvx` pybind11 binding. | pybind11, Python 3.10+ |
| `LRVX_BUILD_NODE` | OFF | Documentation flag. The Node addon is built out-of-tree by `npm run build`; this flag exists for parity in the CI matrix. CMake itself does not invoke npm. | npm, Node 20+ |
| `LRVX_BUILD_CAPI` | OFF | Build the `liblrvx_capi.so` shared library — the integration point for any other language. Forces the static `lrvx` library to be position-independent, and force-enables `LRVX_ENABLE_BACKTEST` (the C API exposes the backtest module). | — |
| `LRVX_BUILD_CODON` | OFF | Build the Codon strategy support layer under `codon/`. | `LRVX_BUILD_CAPI=ON`, Codon compiler |
| `LRVX_BUILD_QUICKJS` | OFF | Build the embedded JS strategy runtime. | `LRVX_BUILD_CAPI=ON` |
| `LRVX_BUILD_CONNECTORS` | OFF | Build the native exchange connectors module under `connectors/`. | OpenSSL / zlib / libcurl, plus `ixwebsocket` and `simdjson` via FetchContent |

### `LRVX_CONNECTORS` selector

When `LRVX_BUILD_CONNECTORS=ON`, the cache string `LRVX_CONNECTORS` chooses which venues to compile.

```bash
cmake -B build -DLRVX_BUILD_CONNECTORS=ON \
                -DLRVX_CONNECTORS="bybit;bitget"
```

The empty string (the default) means "all venues currently in `connectors/src/`". Listing an unknown venue produces a configure-time error with the available set.

## Compiler options

| Flag | Default | Effect |
|---|---|---|
| `LRVX_NATIVE` | **ON** | Add `-march=native` to Release builds. Fastest on the build host, but the resulting `.so` / `.a` will fault with SIGILL on any CPU lacking an instruction the build host had (e.g. AVX-512 → consumer x86). When OFF on x86_64, falls back to `-march=x86-64-v3` (AVX2/BMI2/FMA baseline, supported since ~2015); on arm64 the compiler default is used. Distribution paths (wheels, prebuilt binaries) must build with `LRVX_NATIVE=OFF`. |
| `LRVX_LTO` | **ON** | Link-time optimisation in Release builds: `-flto` on GCC and Clang, `/GL` + `/LTCG` on MSVC. Every executable that links the static library redoes the whole-program step at link time, so a build with `LRVX_BUILD_TESTS=ON` pays it once per test binary and ccache cannot help (it caches compiles, not links). Turn it OFF for build-and-test runs; keep it ON for anything you ship or measure. The per-push Linux CI jobs and the flag matrix run with it OFF; macOS, MSVC, the wheels and the nightly benchmarks keep it ON. |

## Build type, and why it is not a neutral choice

`CMAKE_BUILD_TYPE` **defaults to `Release`**. Leaving it unset used to be the
documented default in every build recipe, and it changes the ABI:

Without `NDEBUG`, `LRVX_SCALE_CHECKS` turns on
([`util/base/scale_check.h`](https://github.com/lrvx/lrvx/blob/main/include/lrvx/util/base/scale_check.h))
and adds an `int64_t _scale` member to `Decimal`. `sizeof(Price)` goes from 8
to 16, and every struct holding a `Price`, `Quantity` or `Volume` changes
layout with it.

That matters because it does not fail to link. A consumer that compiles its own
translation units against a prebuilt archive — the Node addon does exactly this
— reads fields at the wrong offsets and corrupts memory. The symptoms look
nothing like a build problem: `registerSymbol()` returning 0, allocator
checksum aborts, SIGSEGV in unrelated code.

So:

- Do not mix build types between the core archive and anything linking it. The
  Node addon enforces this: it reads `CMAKE_BUILD_TYPE` out of the core's
  `CMakeCache.txt` and fails the build on a mismatch, or when it cannot read
  one at all.
- `Debug` is fine as long as *everything* is Debug.
- `LRVX_SCALE_CHECKS` is derived from `NDEBUG`, not set directly.

## Recommended configurations

```bash
# Research / Python-only
cmake -B build -DLRVX_BUILD_PYTHON=ON -DLRVX_ENABLE_BACKTEST=ON

# Production trading service (Python + Node + connectors, narrowed)
cmake -B build -DLRVX_BUILD_PYTHON=ON \
                -DLRVX_BUILD_NODE=ON \
                -DLRVX_BUILD_CONNECTORS=ON \
                -DLRVX_CONNECTORS="bybit;bitget" \
                -DLRVX_ENABLE_BACKTEST=ON \
                -DLRVX_ENABLE_LZ4=ON

# Minimal CI gate — just confirm the core library compiles
cmake -B build -DLRVX_BUILD_TESTS=ON

# Full developer build (everything)
cmake -B build -DLRVX_BUILD_TESTS=ON -DLRVX_BUILD_BENCHMARKS=ON \
                -DLRVX_BUILD_DEMO=ON -DLRVX_BUILD_TOOLS=ON \
                -DLRVX_BUILD_PYTHON=ON -DLRVX_BUILD_CAPI=ON \
                -DLRVX_BUILD_CODON=ON -DLRVX_BUILD_QUICKJS=ON \
                -DLRVX_BUILD_CONNECTORS=ON \
                -DLRVX_ENABLE_BACKTEST=ON -DLRVX_ENABLE_LZ4=ON \
                -DLRVX_ENABLE_CPU_AFFINITY=ON
```

## FAQ

**Why is everything OFF by default?**
A backtest-only Python user shouldn't pay the cost of the QuickJS runtime, nor a Codon developer the CCXT dependency tree. Each consumer opts in to what they need. The CI matrix in `build-matrix.yml` exercises every option in isolation so a default-OFF assumption can't silently rot.

**Can I add a new option?**
Yes. Decide which prefix fits — capability of the core library is `ENABLE`, optional artefact is `BUILD`. Add the option, the conditional `add_subdirectory` (or whatever the artefact needs), a row in the table above, and a matrix config in `build-matrix.yml` that exercises it in isolation.

**Why no aggregate `LRVX_FULL=ON` flag?**
Aggregate flags drift. Today "full" means `BUILD_PYTHON + BUILD_NODE + BUILD_CAPI + …`; if a new artefact lands, downstream that pinned `LRVX_FULL` for "everything" suddenly drags in something they didn't ask for. The `full-build` config in the CI matrix already serves the "does everything compile together" purpose without giving users a foot-gun. If you want a one-line preset for your own machine, [CMakePresets.json](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html) is the standard escape hatch.
