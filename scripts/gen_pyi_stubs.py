#!/usr/bin/env python3
"""
scripts/gen_pyi_stubs.py

Regenerate `python/lrvx/__init__.pyi` and `python/lrvx/targets.pyi`
from the built `lrvx` extension via pybind11-stubgen.

Without committed stubs, type checkers (mypy, pyright) and AI agents
(Cursor, Claude Code, Copilot) treat the pybind11 module as opaque and
hallucinate method names. Committed stubs + a CI sync gate close that
gap; the generated `.pyi` files ship inside the wheel via
`python/lrvx/`, so PyPI users get types automatically (PEP 561 marker
is `python/lrvx/py.typed`).

The script imports `lrvx` from `build/python` (set by
`PYTHONPATH=build/python` if not already importable) — lrvx must be
built first, e.g. `cmake --build build --target _lrvx`.

Usage:
    python3 scripts/gen_pyi_stubs.py            # write
    python3 scripts/gen_pyi_stubs.py --check    # exit 1 if stubs drift
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PKG_SRC = REPO / "python" / "lrvx"
# Override with LRVX_PY_BUILD_DIR=path/to/python when the .so lives
# outside the default `build/python` (e.g. a per-Python-version build).
BUILD_PYTHON = Path(os.environ.get("LRVX_PY_BUILD_DIR", REPO / "build" / "python"))

# Stub layout produced by stubgen for our package:
#   lrvx/__init__.pyi               wrapper that re-exports symbols
#   lrvx/_lrvx/__init__.pyi      typed signatures of the C extension
#   lrvx/_lrvx/targets.pyi       typed signatures of the C submodule
# Even though `_lrvx` is a `.so` at runtime (not a directory), type
# checkers happily resolve `lrvx._lrvx.X` against
# `lrvx/_lrvx/__init__.pyi` when the parent package has `py.typed`.
STUB_FILES = (
    Path("__init__.pyi"),
    Path("_lrvx") / "__init__.pyi",
    Path("_lrvx") / "targets.pyi",
    # `_heatmap` is a second bound submodule of the C extension (same
    # shape as `targets`, just for the heatmap-export helpers) and gets
    # its own stub from stubgen. It was missing from this list, so
    # `_lrvx/__init__.pyi`'s own `from . import _heatmap` pointed at
    # a stub that was never written to the committed package: mypy
    # reported "Module lrvx._lrvx does not explicitly export
    # attribute _heatmap" because, as far as the committed tree was
    # concerned, there was nothing there to export.
    Path("_lrvx") / "_heatmap.pyi",
)

# `(anonymous namespace)::PyExtBar` shows up as a return-type annotation
# for `aggregate_*_bars` because PyExtBar is a translation-unit-local
# numpy dtype; stubgen replaces the whole annotation with `...` (i.e.
# Any), which is fine — the runtime return is a structured numpy array.
IGNORE_INVALID_RE = r"anonymous namespace"


def ensure_module_importable() -> None:
    if not (BUILD_PYTHON / "lrvx" / "__init__.py").exists():
        sys.exit(
            "error: lrvx package not found in build/python. Build it first:\n"
            "    cmake --build build --target _lrvx"
        )
    sys.path.insert(0, str(BUILD_PYTHON))
    try:
        __import__("lrvx")
    except ImportError as e:
        sys.exit(f"error: cannot import lrvx from {BUILD_PYTHON}: {e}")


def sort_import_lines(text: str) -> str:
    """Sort consecutive `from ... import ...` lines alphabetically.

    pybind11 module-attribute insertion order is not stable across
    platforms (macOS vs Linux) — `dir()` reflects that, and stubgen's
    output reflects `dir()`. Sorting the import block makes the stub
    deterministic so the CI sync gate doesn't fire on platform drift.
    """
    lines = text.splitlines(keepends=True)
    out: list[str] = []
    block: list[str] = []

    def flush() -> None:
        if block:
            out.extend(sorted(block, key=str.lower))
            block.clear()

    for line in lines:
        if line.startswith("from ") and " import " in line:
            block.append(line)
        else:
            flush()
            out.append(line)
    flush()
    return "".join(out)


# `numpy.typing.NDArray[X]` annotations where `X` is either `...`
# (stubgen's placeholder for a type it could not resolve) or `PyExtBar`
# (a numpy structured-dtype struct that has no Python class) confuse
# mypy: it reports `Unexpected "..."` / undefined name and silently
# degrades the surrounding class to `Any`. Rewrite both shapes to a
# generic structured-array annotation that mypy parses cleanly.
_NDARRAY_VOID = "numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]"
_NDARRAY_REWRITES = (
    ("numpy.typing.NDArray[...]", _NDARRAY_VOID),
    ("numpy.typing.NDArray[PyExtBar]", _NDARRAY_VOID),
)

# A parameter stubgen could not resolve a type for is emitted as a bare
# `...` (the same placeholder used elsewhere for an unresolved return
# type, e.g. `set_rate_limit_policy(self, policy: ...)` for an argument
# bound as a raw `py::object`/pointer). As a VALUE `...` is the Ellipsis
# object mypy is happy with; as the annotation of a parameter that sits
# next to typed siblings (`self` counts once its own type is inferred),
# mypy rejects the whole signature with "Ellipses cannot accompany other
# argument types in function type signature" [syntax] and skips the
# function entirely. `typing.Any` says the same thing stubgen meant
# ("no idea what this type is") in a form mypy actually parses. Matches
# only `: ...` immediately followed by `,` or `)` -- a parameter
# position -- so it does not touch a stub function's own `...` body
# (followed by a newline into the next `def`/`class`, never a comma or
# paren) or the `NDArray[...]` placeholder already rewritten above
# (preceded by `[`, not `:`).
_BARE_ELLIPSIS_PARAM_RE = re.compile(r"(:\s*)\.\.\.(?=\s*[,)])")

# `from . import _heatmap` (a private, underscore-prefixed submodule
# deliberately left out of `__all__`) reads to mypy as "imported but
# not re-exported": with `implicit_reexport` off it holds every import
# in a stub to the same explicit-export bar `__all__` enforces for
# everything else. Fixing it takes both halves of that bar: `import X
# as X` is the standard spelling for "yes, this really is meant to be
# visible under this name" (recognised by every mypy version this
# project has tested against), AND -- for a submodule specifically,
# empirically -- `X` itself still has to appear in `__all__`, or mypy
# reports the same "does not explicitly export" error regardless of
# the `as X` alias.
_PRIVATE_SUBMODULE_IMPORT_RE = re.compile(r"^from \. import (_\w+)$", re.MULTILINE)
_ALL_LIST_RE = re.compile(r"^__all__: list\[str\] = \[", re.MULTILINE)


def fix_invalid_annotations(text: str, *, rel: Path | None = None) -> str:
    for old, new in _NDARRAY_REWRITES:
        text = text.replace(old, new)
    # pybind11 binds `__eq__(self, other: T)` literally as `T`, but
    # Python's data model requires `object` (Liskov). mypy flags this as
    # an override violation. Every dunder eq we've seen is a single-arg
    # comparator, so widening the parameter to `object` is safe.
    text = re.sub(
        r"def __eq__\(self, arg0: \w+\) -> bool:",
        "def __eq__(self, arg0: object) -> bool:",
        text,
    )
    text = _BARE_ELLIPSIS_PARAM_RE.sub(r"\1typing.Any", text)
    # stubgen writes the module's __version__ with its value. The value
    # is the build's release, so a stub carrying it would drift on every
    # version bump (scripts/set-version.sh) with no change in the surface.
    text = re.sub(r"^__version__: str = .*$", "__version__: str", text, flags=re.M)
    private_submodules = _PRIVATE_SUBMODULE_IMPORT_RE.findall(text)
    text = _PRIVATE_SUBMODULE_IMPORT_RE.sub(r"from . import \1 as \1", text)
    for name in private_submodules:
        if f"'{name}'" not in text:
            text = _ALL_LIST_RE.sub(f"__all__: list[str] = ['{name}', ", text, count=1)
    if rel is not None and rel.parent.name == "_lrvx":
        # This file's own fully-qualified module name IS
        # `lrvx._lrvx` -- stubgen occasionally qualifies a type
        # this fully even for a class defined earlier in this very
        # file (seen on `ReplaySource.next() -> lrvx._lrvx.
        # ReplayEvent | None`), and nothing in the file ever imports
        # the top-level `lrvx` package to make that name resolve,
        # so mypy reports "Name lrvx is not defined". Every such
        # reference is self-referential by construction, so dropping
        # the redundant prefix is always correct here (never in the
        # sibling `lrvx/__init__.pyi`, where the same prefix is a
        # real cross-module reference).
        text = text.replace("lrvx._lrvx.", "")
    return text


def post_process(text: str, *, rel: Path | None = None) -> str:
    return fix_invalid_annotations(sort_import_lines(text), rel=rel)


def run_stubgen(out_dir: Path) -> None:
    env = os.environ.copy()
    env["PYTHONPATH"] = f"{BUILD_PYTHON}{os.pathsep}{env.get('PYTHONPATH', '')}"
    cmd = [
        sys.executable,
        "-m",
        "pybind11_stubgen",
        "lrvx",
        "--output-dir",
        str(out_dir),
        "--ignore-invalid-expressions",
        IGNORE_INVALID_RE,
    ]
    # Don't pass --exit-code: it would fail even on the recoverable
    # per-symbol errors (ignored anonymous-namespace return types are
    # reduced to `...` in the stub, which is fine).
    result = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        sys.exit(f"error: pybind11-stubgen failed (rc={result.returncode})")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="Exit non-zero if existing stubs differ from regenerated content.",
    )
    args = parser.parse_args()

    ensure_module_importable()

    with tempfile.TemporaryDirectory() as tmp:
        tmp_root = Path(tmp)
        run_stubgen(tmp_root)
        gen_pkg = tmp_root / "lrvx"

        for rel in STUB_FILES:
            src = gen_pkg / rel
            if not src.exists():
                sys.exit(f"error: stubgen did not produce {rel}")

        if args.check:
            ok = True
            for rel in STUB_FILES:
                committed = PKG_SRC / rel
                generated = post_process((gen_pkg / rel).read_text(), rel=rel)
                current = committed.read_text() if committed.exists() else ""
                if current != generated:
                    print(
                        f"::error::{committed.relative_to(REPO)} is out of sync",
                        file=sys.stderr,
                    )
                    ok = False
            if not ok:
                print(
                    "Run: cmake --build build --target _lrvx "
                    "&& python3 scripts/gen_pyi_stubs.py",
                    file=sys.stderr,
                )
                return 1
            return 0

        for rel in STUB_FILES:
            dst = PKG_SRC / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_text(post_process((gen_pkg / rel).read_text(), rel=rel))
            print(
                f"wrote {dst.relative_to(REPO)} "
                f"({dst.stat().st_size:,} bytes)"
            )
    return 0


if __name__ == "__main__":
    sys.exit(main())
