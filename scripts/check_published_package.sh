#!/usr/bin/env bash
# Rehearse, by hand, the install-and-import gate that node-publish.yml and
# python-wheels.yml now run before publishing. Takes an already-built npm
# tarball (`npm pack` output), an already-built Python wheel, and/or an
# already-built Python sdist; this script does not build any of them -- it
# only proves that what would go to the registry actually installs and
# loads on this machine.
#
# This is the check that would have caught the flat prebuilds/lrvx_node.node
# bug: every npm publish since 0.7.1 shipped a package whose prebuilds/
# directory held one file that overwrote all three platform artifacts on
# download, while node/index.js requires prebuilds/<platform>-<arch>/..., so
# `require('@lrvx/lrvx')` failed on every install. Nothing in the
# old workflow ever ran that require -- publish went straight from
# download-artifact to `npm publish`.
#
# --sdist is the slow one: it runs a real source build (cmake configures the
# whole project and compiles the core plus the extension), which is the only
# thing that can tell a self-contained sdist from one that merely looks
# populated.
#
# Usage:
#   scripts/check_published_package.sh --npm-tarball PATH [--wheel PATH]
#   scripts/check_published_package.sh --wheel PATH [--npm-tarball PATH]
#   scripts/check_published_package.sh --sdist PATH
#
# At least one of --npm-tarball / --wheel / --sdist is required. Exit status
# is nonzero if any requested check fails.
set -euo pipefail

npm_tarball=""
wheel_path=""
sdist_path=""

usage() {
  echo "usage: $0 [--npm-tarball PATH] [--wheel PATH] [--sdist PATH]" >&2
  exit 2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --npm-tarball)
      npm_tarball="$2"
      shift 2
      ;;
    --wheel)
      wheel_path="$2"
      shift 2
      ;;
    --sdist)
      sdist_path="$2"
      shift 2
      ;;
    -h|--help)
      usage
      ;;
    *)
      echo "unknown argument: $1" >&2
      usage
      ;;
  esac
done

if [[ -z "$npm_tarball" && -z "$wheel_path" && -z "$sdist_path" ]]; then
  usage
fi

fail=0

# The import smoke both Python checks run, kept in one place so the sdist
# path cannot drift into proving something weaker than the wheel path.
LRVX_PY_SMOKE='
import sys
import importlib.metadata
import lrvx
installed = importlib.metadata.version("lrvx")
assert getattr(lrvx, "__version__", None) == installed, (
    f"lrvx.__version__ is {getattr(lrvx, '__version__', None)!r}, "
    f"the installed distribution is {installed!r}")
reg = lrvx.SymbolRegistry()
sid = reg.add_symbol("bybit", "BTCUSDT", 0.01)
assert lrvx.SLIPPAGE_NONE == 0
assert sid is not None
print(sys.argv[1], "OK: import + SymbolRegistry.add_symbol() ->", sid)
'

check_npm_tarball() {
  local tarball="$1"
  if [[ ! -f "$tarball" ]]; then
    echo "::error::npm tarball not found: $tarball" >&2
    return 1
  fi
  tarball="$(cd -- "$(dirname -- "$tarball")" && pwd)/$(basename -- "$tarball")"

  local work
  work=$(mktemp -d)
  trap 'rm -rf "$work"' RETURN

  echo "[npm-check] installing $(basename "$tarball") into a clean directory..."
  (
    cd "$work"
    npm init -y >/dev/null
    npm install "$tarball" >/dev/null
  )

  echo "[npm-check] requiring the package and calling into the native addon..."
  if ! node -e "
    const lrvx = require('$work/node_modules/@lrvx/lrvx');
    const pkgVersion = require('$work/node_modules/@lrvx/lrvx/package.json').version;
    if (lrvx.version() !== pkgVersion) {
      throw new Error('lrvx.version() is ' + lrvx.version() + ', package.json says ' + pkgVersion);
    }
    const reg = new lrvx.SymbolRegistry();
    const sym = reg.addSymbol('bybit', 'BTCUSDT', 0.01);
    if (sym === undefined || sym === null) {
      throw new Error('native addon call returned nothing');
    }
    console.log('[npm-check] OK: require() + SymbolRegistry.addSymbol() ->', sym);
  "; then
    echo "::error::npm package failed to load or call the native addon" >&2
    return 1
  fi
}

check_wheel() {
  local wheel="$1"
  if [[ ! -f "$wheel" ]]; then
    echo "::error::wheel not found: $wheel" >&2
    return 1
  fi
  wheel="$(cd -- "$(dirname -- "$wheel")" && pwd)/$(basename -- "$wheel")"

  local work
  work=$(mktemp -d)
  trap 'rm -rf "$work"' RETURN

  local py="${PYTHON:-python3}"
  echo "[wheel-check] installing $(basename "$wheel") into a clean venv..."
  "$py" -m venv "$work/venv"
  "$work/venv/bin/pip" install --quiet "$wheel"

  echo "[wheel-check] importing lrvx and calling into the native extension..."
  # Run from a directory with no python/lrvx/ nearby so cwd resolution
  # cannot shadow the installed wheel with the source tree.
  if ! (cd /tmp && "$work/venv/bin/python" -c "$LRVX_PY_SMOKE" "[wheel-check]"); then
    echo "::error::wheel failed to import or call the native extension" >&2
    return 1
  fi
}

check_sdist() {
  local sdist="$1"
  if [[ ! -f "$sdist" ]]; then
    echo "::error::sdist not found: $sdist" >&2
    return 1
  fi
  sdist="$(cd -- "$(dirname -- "$sdist")" && pwd)/$(basename -- "$sdist")"

  local work
  work=$(mktemp -d)
  trap 'rm -rf "$work"' RETURN

  # Cheap structural check first. An sdist rooted below the C++ sources --
  # what a pyproject.toml under python/ produces -- looks like a normal
  # archive and only fails minutes later, deep inside a cmake configure
  # step, with nothing saying the archive was the problem. Name the missing
  # path instead.
  local members
  members=$(tar tzf "$sdist" | cut -d/ -f2- | sort -u)
  local missing=()
  local required
  for required in CMakeLists.txt cmake/ include/ src/ venue/ python/lrvx/; do
    if ! grep -q "^${required}" <<<"$members"; then
      missing+=("$required")
    fi
  done
  if [[ ${#missing[@]} -gt 0 ]]; then
    echo "::error::sdist does not carry the sources its own build configures: ${missing[*]}" >&2
    echo "the sdist root is the directory holding pyproject.toml; everything cmake reads has to live under it" >&2
    return 1
  fi

  local py="${PYTHON:-python3}"
  echo "[sdist-check] building and installing $(basename "$sdist") in a clean venv (this compiles the core; minutes)..."
  "$py" -m venv "$work/venv"
  if ! "$work/venv/bin/pip" install "$sdist"; then
    echo "::error::sdist failed to build or install from source" >&2
    return 1
  fi

  echo "[sdist-check] importing lrvx and calling into the native extension..."
  # Same reason as the wheel check: run from elsewhere so the source tree
  # cannot shadow what was installed.
  if ! (cd /tmp && "$work/venv/bin/python" -c "$LRVX_PY_SMOKE" "[sdist-check]"); then
    echo "::error::sdist installed but lrvx failed to import or call the native extension" >&2
    return 1
  fi
}

if [[ -n "$npm_tarball" ]]; then
  if ! check_npm_tarball "$npm_tarball"; then
    fail=1
  fi
fi

if [[ -n "$wheel_path" ]]; then
  if ! check_wheel "$wheel_path"; then
    fail=1
  fi
fi

if [[ -n "$sdist_path" ]]; then
  if ! check_sdist "$sdist_path"; then
    fail=1
  fi
fi

if [[ "$fail" -ne 0 ]]; then
  echo "::error::check_published_package.sh: one or more checks failed" >&2
  exit 1
fi

echo "check_published_package.sh: all requested checks passed"
