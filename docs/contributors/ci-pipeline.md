# CI Pipeline

Mental model for the CI workflow under [`.github/workflows/`](../../.github/workflows/) — what runs in what order, what fails fast, and what to regenerate locally before pushing.

## Workflow shape

```
┌────────────────────────────────────────────────────────────────────┐
│  Quick gates (parallel, ~30s each)                                  │
│                                                                     │
│  ┌──────────────┐   ┌─────────────────────┐   ┌─────────────────┐  │
│  │ format-check │   │ verify-docs-current │   │  codegen-check  │  │
│  └──────────────┘   └─────────────────────┘   └─────────────────┘  │
└────────────────────────────────────────────────────────────────────┘
                              │
                  needs: [format-check, verify-docs-current]
                              │
                              ▼
┌────────────────────────────────────────────────────────────────────┐
│  OS build matrix (parallel, ~5–10 min each)                         │
│                                                                     │
│  ┌──────────┐ ┌───────────┐ ┌───────┐ ┌─────────────┐ ┌──────────┐ │
│  │linux-gcc │ │linux-clang│ │ macos │ │windows-msvc │ │win-clang │ │
│  └──────────┘ └───────────┘ └───────┘ └─────────────┘ └──────────┘ │
│  ┌────────────────────┐ ┌────────────────────────┐                 │
│  │sanitizers (address)│ │sanitizers (undefined)  │                 │
│  └────────────────────┘ └────────────────────────┘                 │
└────────────────────────────────────────────────────────────────────┘
```

`codegen-check` runs in a separate workflow ([`codegen.yml`](../../.github/workflows/codegen.yml)) — it's not in the dependency chain because it's already fast (~30s) and runs in parallel.

## Fast-fail wiring

Two mechanisms keep the pipeline cheap on bad pushes:

1. **`needs:` dependency.** Every OS build job in [`ci.yml`](../../.github/workflows/ci.yml) declares `needs: [format-check, verify-docs-current]`. If either quick gate fails, the multi-OS matrix is skipped — saving ~50 compute minutes (10 min × 5 jobs).
2. **`concurrency.cancel-in-progress`.** A new push to the same branch / PR cancels the in-flight workflow run for that ref. No more "two builds racing on stale code".
3. **`push:` is filtered to `main`.** With an unfiltered `push:` alongside `pull_request:`, every push to a branch with an open PR ran the whole matrix twice, and three `clang-format` jobs because `build-matrix.yml` duplicated `ci.yml`'s. A branch now gets exactly one run, from `pull_request`.

Both are at the workflow top:

```yaml
concurrency:
  group: ${{ github.workflow }}-${{ github.ref }}
  cancel-in-progress: true

jobs:
  linux-gcc:
    needs: [format-check, verify-docs-current]
    ...
```

## What runs in each gate

### `format-check`

Just `./scripts/check-format.sh` — runs `clang-format --dry-run -Werror` on every C/C++/header file. Ten seconds. Failures show the exact lines that differ from the configured style; fix is `clang-format -i <file>`.

### `verify-docs-current`

Runs eight checks in sequence (each takes <5s):

| Step | What it does | If it fails |
|------|--------------|-------------|
| `gen_indicator_docs.py` | Indicator reference matches `registry.def` | Run `python3 scripts/gen_indicator_docs.py` |
| `gen_llms_txt.py --check` | `docs/llms.txt` + `llms-full.txt` match `docs/` | Run `python3 scripts/gen_llms_txt.py` |
| `check_dts_exports.py` | `node/index.d.ts` matches NAPI exports | Edit `.d.ts` to add/remove the listed names |
| `check_binding_parity.py` | pybind11/NAPI/Codon/QuickJS coverage matches IDL | See [parity-gate.md](parity-gate.md) |
| `check_error_codes.py` | Every error code has a doc page; pages aren't stale | Add the doc page or remove the unused code |
| `check_test_gating.py` | Every `tests/*.cpp` is registered in CMake | Register the target — see [test-gating.md](test-gating.md) |
| `check_suite_discovery.py` | Every Python/Node test file is reachable by the suite runners, and nobody re-listed files by hand | Rename the file to `test_*`, add cases, or drop the hand-written step |
| `check_quickjs_registration.py` | Every `__lrvx_*` global the JS layer calls has an `addGlobalFunc` registration | Register the global in `js_bindings.cpp` |
| `check_quickjs_lrvx_typings.py` | Every member of the QuickJS `lrvx` object has a typing in `quickjs/types/lrvx.d.ts`, and every typed member exists | Add the typing to the `declare const lrvx` block, or remove the stale one |
| `gen_api_index.py --check` | `docs/reference/python/_api_index.md` matches `.pyi` | Run `python3 scripts/gen_api_index.py` |
| `check_doc_snippets.py` | Doc snippets follow `--8<--` include pattern | Refactor inline snippets into includes |
| `sync_mcp_data.py --check` | `mcp/lrvx_mcp/data/` matches source | Run `python3 scripts/sync_mcp_data.py` |
| `check_sanitizer_scan.py` | Every job that builds with `-fsanitize=` runs its tests through the transcript scan | Wrap the step in `scripts/run-with-sanitizer-scan.sh` |
| lrvx-mcp pytest | MCP server unit tests, with the compiled binding required | Fix the broken test, or build `_lrvx` if the step reports a missing dependency |

### `linux-gcc` — the binding suites

The one job that builds every binding, so it is where the binding-level checks
live. All of them run the whole suite, not a list:

| Step | What it does | If it fails |
|------|--------------|-------------|
| `pytest python/tests -q` | The entire Python suite, under `PYTHONPATH=build/python` | Fix the test. Reproduce locally with `scripts/ci-local.sh`, which uses the same `PYTHONPATH` — a *relative* path, which is why a subprocess spawned with a different `cwd` must absolutise it |
| `for f in test/test_*.js` | Every Node test file | Fix the test |
| `check_binding_smoke.py` | Calls ~760 zero-argument methods across both bindings and fails on binding-level errors (unregistered return type, signature the addon cannot accept) | The wrapper is broken, not the input — see [parity-gate.md](parity-gate.md) |

Both suites used to be invoked as one named step per file — roughly 35 of them.
A test file nobody remembered to add simply never ran: 34 of 69 Python files and
9 of 21 Node files, including every regression gate written for the
binding-surface audit. `check_suite_discovery.py` now fails CI if a test file
would not be picked up, or if anyone re-lists files by hand.

### `codegen-check` (separate workflow)

Re-runs the IDL→header/codon/markdown emitters and verifies the output matches what's committed. If you edited `lrvx_capi_spec.hpp` and forgot to re-run `regenerate.sh`, this fails.

### OS build matrix

Builds the full project (engine + C ABI + tests + benchmarks + Python + Node + Codon + QuickJS), runs `ctest`, runs all the integration tests, runs cross-binding parity tests (Python ↔ Node, same C++ math), and exercises example programs.

## Green does not mean checked

Two steps in this pipeline used to report success while checking less than
their names implied. Both are fixed. The shapes are worth knowing because they
recur.

### A sanitizer report inside a passing test

`ctest --output-on-failure` prints the output of the tests ctest decided had
failed. A test whose assertions all pass, but which printed a sanitizer report
on the way, is recorded as `Passed` and its output never reaches the log. The
undefined-behavior sanitizer does that by default: it prints
`runtime error: ...`, carries on, and the process exits 0.

That is the case sanitizers are run for. Twice during the 2026-09 audit a real
defect in production code turned up exactly that way, under fully green
assertions: a data race on the logger's file descriptor, and an out-of-bounds
vector read in the execution simulator. Both were found by hand, by re-running
with full output and grepping a few thousand lines of transcript.

The run reads its own transcript now. Every sanitizer job calls the suite
through [`scripts/run-with-sanitizer-scan.sh`](../../scripts/run-with-sanitizer-scan.sh).
It runs the command, keeps its exit code, and also scans ctest's per-test log —
`build/Testing/Temporary/LastTest.log`, which holds every test's output
whatever its verdict — for Address, Leak, Thread, Memory and
UndefinedBehavior report banners. A report fails the step even when the command
returned 0, and the failure names the test and the verdict ctest gave it.

Locally, same call:

```bash
scripts/run-with-sanitizer-scan.sh ctest --output-on-failure --test-dir build
```

`scripts/check_sanitizer_scan.py` keeps the wiring in place: a job that
compiles with `-fsanitize=` and does not wrap its test step fails the docs
gate. The matcher has a `--self-test` that feeds it every report shape it
claims to catch, plus text it must not match — an unwatched detector is not a
detector.

### A parity gate that skipped a quarter of what it named

`check_binding_parity.py` is the cross-binding parity gate and the project has
four bindings: pybind11, NAPI, Codon, QuickJS. The per-group loop called the
first three. The `quickjs` key existed in the manifest and was read by no line
of code, and the closing message said "all bindings in parity". A group with no
QuickJS implementation at all passed, which is how a composite-book function
shipped missing from QuickJS with this green.

QuickJS is read now, against the `addGlobalFunc` registration table in
`src/quickjs/js_bindings.cpp` — the only list of what a strategy can actually
call. A `quickjs: required` entry names the globals rather than deriving them,
because the `__` plus C-API-name convention has real exceptions
(`__lrvx_vprofile_create` wraps `lrvx_volume_profile_create`).

Most groups still carry no `quickjs` entry, so the gate cannot demand one yet.
It counts them and prints the number instead of implying they were checked, and
the closing line now names what it checked per binding. `--require-quickjs`
turns undeclared groups into failures; turn it on in CI once the manifest is
filled in.

### A test suite that quietly shrank

`pytest mcp/tests/` reports `229 passed, 20 skipped` without the compiled
`lrvx` binding and `248 passed, 1 skipped` with it. Both exit 0, and the 19
cases that drop out are the ones that touch real code rather than fixtures.
Nothing separated the two runs but the numbers.

[`mcp/tests/conftest.py`](../../mcp/tests/conftest.py) probes the dependencies
that shrink the suite — the binding and the `mcp` SDK — and closes the run with
a banner naming each missing one and how many cases it cost. Where the run is
meant to be complete, a missing dependency fails it instead of reducing it:
that is the default under `CI`, and `LRVX_MCP_REQUIRE_DEPS=1` / `=0` forces it
either way. A contributor who has not built the C++ side still gets the
pure-Python half, and the count of what did not run.

## The docs sync chain (eight scripts, in order)

The "verify-docs" gates each check that a generated artifact matches what's committed. Several of those artifacts depend on each other — regenerating one re-derives the next:

```
1. tools/codegen/scripts/regenerate.sh  → lrvx_capi.h, golden/, .api/, mcp data
2. cmake --build build                  → liblrvx_capi + Python module
3. scripts/gen_pyi_stubs.py             → .pyi from running pybind11
4. scripts/gen_api_index.py             → docs/reference/python/_api_index.md from .pyi
5. scripts/gen_llms_txt.py              → docs/llms.txt + llms-full.txt (embeds api_index)
6. scripts/sync_mcp_data.py             → mcp/lrvx_mcp/data/ (handled by regenerate.sh, but run manually if you skipped step 1)
7. scripts/gen_indicator_docs.py        → docs/reference/codon/indicators.md (only if you touched registry.def)
8. python3 scripts/check_binding_parity.py  → manifests vs bindings
```

**Skipping any one of those usually means a CI failure that takes ~30s to detect**, but the order matters — `gen_llms_txt.py` reads `_api_index.md`, so regenerating `_api_index.md` after generating `llms-full.txt` leaves them out of sync.

If you've touched the IDL spec or any pybind11/NAPI code, run them all in order before committing. If you've only touched a C++ engine internal that doesn't change the public surface, you don't need to.

## What fails first (debugging guide)

When CI is red, look at the topmost failed step:

- **format-check fails** → run `clang-format -i` on the listed files
- **codegen-check fails** → run `bash tools/codegen/scripts/regenerate.sh`
- **verify-docs-current fails** → look at the specific step name; run the script it names
- **OS build fails** → reproduce locally with the matching toolchain (most issues are platform-specific compile errors visible in the log)
- **OS build but only on sanitizers** → the bug exists, sanitizers caught what regular tests missed; address it, don't disable the sanitizer

The fast-fail wiring means if quick gates fail, build matrix is `SKIPPED` (gray, not red) — which is the design. If you see all build jobs gray and only one quick gate red, fix that gate; everything else will run on the next push.

## Adding a new gate

To add a new check that should block builds:

1. Add a step under `verify-docs-current` (if it's a docs/sync check) or `format-check` (if it's a static check on source).
2. Make sure the script exits non-zero on failure with a clear `::error::` annotation.
3. Verify the build jobs already `needs: [format-check, verify-docs-current]` — they do — so failures will short-circuit the matrix automatically.

If a check needs to run on every OS (e.g. exercising the platform-specific `.dylib` / `.dll`), put it in each OS build job instead. The fast-fail logic still applies — it won't even start if quick gates failed.
