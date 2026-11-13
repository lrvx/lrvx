#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$ROOT_DIR/build"

cd "$ROOT_DIR"

echo "=== Format check ==="
./scripts/check-format.sh

PY="${PYTHON:-python3}"

# Mirror the linux-gcc job's configure. Two things were missing here and both
# mattered: the LRVX_ENABLE_* names are deprecated aliases, and
# without CAPI / PYTHON / QUICKJS this script built 129 of the 144 test targets
# and never compiled the Codon or QuickJS surface at all -- so "ci-local.sh is
# green" did not mean CI would be.
CODON_FLAG="-DLRVX_BUILD_CODON=OFF"
if command -v codon >/dev/null 2>&1 || [ -x "$HOME/.codon/bin/codon" ]; then
    CODON_FLAG="-DLRVX_BUILD_CODON=ON"
else
    echo "note: codon not found — skipping the Codon targets (CI builds them)"
fi

echo "=== Clean build ==="
rm -rf "$BUILD_DIR"
cmake -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DLRVX_BUILD_TESTS=ON \
    -DLRVX_BUILD_BENCHMARKS=ON \
    -DLRVX_BUILD_DEMO=ON \
    -DLRVX_ENABLE_LZ4=ON \
    -DLRVX_ENABLE_BACKTEST=ON \
    -DLRVX_BUILD_CAPI=ON \
    -DLRVX_BUILD_PYTHON=ON \
    -DLRVX_BUILD_QUICKJS=ON \
    $CODON_FLAG \
    -Dpybind11_DIR="$("$PY" -c 'import pybind11; print(pybind11.get_cmake_dir())' 2>/dev/null)"

echo "=== Build ==="
cmake --build "$BUILD_DIR" -j$(nproc)

# Through the wrapper, same as CI: `ctest --output-on-failure` only prints the
# output of the tests ctest decided had failed, so a sanitizer report inside a
# test whose assertions passed never reaches the screen. The wrapper reads the
# per-test transcript and goes red on a report regardless of the exit code.
echo "=== C++ tests ==="
./scripts/run-with-sanitizer-scan.sh ctest --output-on-failure --test-dir "$BUILD_DIR"

# Run the binding suites exactly the way CI does. This script used to run none
# of them, which is how a test that spawns a subprocess with a *relative*
# PYTHONPATH passed locally (lrvx was pip-installed at an absolute path) and
# failed in CI. PYTHONPATH="$BUILD_DIR/python" reproduces the CI environment.
echo "=== Python test suite ==="
PYTHONPATH="$BUILD_DIR/python" "$PY" -m pytest python/tests -q

# This script builds the binding, so a skip for a missing one means the build
# it just did is not importable. LRVX_MCP_REQUIRE_DEPS turns that into a
# failure instead of a summary line that reads the same as a healthy run.
echo "=== lrvx-mcp test suite ==="
LRVX_MCP_REQUIRE_DEPS=1 PYTHONPATH="$BUILD_DIR/python" "$PY" -m pytest mcp/tests -q

echo "=== Node.js addon + test suite ==="
if command -v npm >/dev/null 2>&1; then
    (cd node && npm install --silent && npm run build --silent && npm run typecheck)
    (cd node
     fail=0
     for f in test/test_*.js; do
       echo "── $f"
       node "$f" || fail=1
     done
     exit $fail)
else
    echo "note: npm not found — skipping the Node addon and its suite (CI runs them)"
fi

echo "=== Demo ==="
./scripts/run-with-sanitizer-scan.sh "$BUILD_DIR/demo/lrvx_demo"

echo "=== Benchmarks ==="
./scripts/run-benchmarks.sh "$BUILD_DIR/benchmarks"

# Same set as the verify-docs-current job in .github/workflows/ci.yml, so a
# green local run means green docs in CI. See docs/contributors/doc-gates.md.
echo "=== Documentation gates ==="
"$PY" scripts/gen_indicator_docs.py
git diff --exit-code -- docs/ >/dev/null || {
  echo "error: docs/ out of sync with include/lrvx/indicator/registry.def" >&2
  echo "Run: $PY scripts/gen_indicator_docs.py" >&2
  exit 1
}
"$PY" scripts/gen_llms_txt.py --check
"$PY" scripts/gen_api_index.py --check
"$PY" scripts/check_dts_exports.py
"$PY" scripts/check_dts_class_members.py
"$PY" scripts/check_binding_parity.py --require-quickjs
"$PY" scripts/check_error_codes.py
"$PY" scripts/check_test_gating.py
"$PY" scripts/check_suite_discovery.py
"$PY" scripts/check_binding_smoke.py
"$PY" scripts/check_quickjs_registration.py
"$PY" scripts/check_quickjs_lrvx_typings.py
"$PY" -m pytest scripts/tests -q
"$PY" scripts/scan_sanitizer_reports.py --self-test
"$PY" scripts/check_sanitizer_scan.py
"$PY" scripts/check_doc_snippets.py --min-includes 24
"$PY" scripts/check_doc_symbols.py
"$PY" scripts/check_doc_nav.py
"$PY" scripts/check_doc_links.py
"$PY" scripts/check_doc_conventions.py
"$PY" scripts/check_doc_examples.py
"$PY" scripts/sync_mcp_data.py --check

echo "=== All CI checks passed ==="
