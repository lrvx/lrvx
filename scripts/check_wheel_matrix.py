#!/usr/bin/env python3
"""Fail when the wheel build does not cover every Python the package claims.

pyproject.toml says which CPython versions lrvx supports: `requires-python`
sets the floor and the `Programming Language :: Python :: 3.X` classifiers
name each minor version. `.github/workflows/python-wheels.yml` decides which
interpreters get a wheel, through `CIBW_BUILD` and the cibuildwheel pin.
When the two disagree, pip on an interpreter with no wheel falls back to the
sdist and compiles the C++ core on the user's machine -- which is what
happened on Python 3.14 with 0.12.0: `CIBW_BUILD` stopped at cp313 and the
cibuildwheel pin (2.22.0) could not build 3.14 at all.

Supported means: every minor version from the `requires-python` floor up to
the newest CPython released as stable (`NEWEST_STABLE_CPYTHON`). The check
requires that

  * `requires-python` has a lower bound and no upper bound below that range,
  * the classifiers list exactly those minor versions,
  * `CIBW_BUILD` selects exactly those minor versions (cpXY-*),
  * the pinned cibuildwheel builds each of them by default, and
  * the pybind11 floor in `[build-system].requires` supports each of them.

The minimum tool versions come from the projects' own changelogs, not from
memory; the table below names the entry each one was read from. Extend it,
and raise `NEWEST_STABLE_CPYTHON`, when a new CPython is released.

Run:
    python3 scripts/check_wheel_matrix.py
    python3 scripts/check_wheel_matrix.py --root <tree>   # tests use this
"""
from __future__ import annotations

import argparse
import re
import sys
import tomllib
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]

# The newest CPython minor version released as stable. 3.15.0 was released
# on 2026-10-09 (python.org/downloads).
NEWEST_STABLE_CPYTHON = (3, 15)

# minor version -> (first cibuildwheel that builds it by default,
#                   first pybind11 that supports it).
#
#   3.14: cibuildwheel 3.1.0 (2025-07-23), "CPython 3.14 wheels are now built
#         by default" (3.0.0 had it only behind enable=cpython-prerelease);
#         pybind11 3.0.0 (2025-07-10), "Support Python 3.14 (beta 1+)".
#   3.15: cibuildwheel 4.2.0 (2026-08-04), "CPython 3.15 wheels are now built
#         by default" (4.0.0 had it only behind enable=cpython-prerelease);
#         pybind11 3.1.0 (2026-08-06), "Test Python 3.15 in CI".
#
# Older minor versions are built by every cibuildwheel and supported by every
# pybind11 this repository could plausibly pin, so they carry no entry.
TOOL_MINIMUMS: dict[tuple[int, int], tuple[str, str]] = {
    (3, 14): ("3.1.0", "3.0.0"),
    (3, 15): ("4.2.0", "3.1.0"),
}

WORKFLOW = Path(".github/workflows/python-wheels.yml")
PYPROJECT = Path("pyproject.toml")


def _version(text: str) -> tuple[int, ...]:
    return tuple(int(p) for p in re.findall(r"\d+", text)[:3])


def _fmt(minor: tuple[int, int]) -> str:
    return f"{minor[0]}.{minor[1]}"


def _requires_python_floor(spec: str) -> tuple[int, int] | None:
    m = re.search(r">=\s*(\d+)\.(\d+)", spec)
    return (int(m.group(1)), int(m.group(2))) if m else None


def _requires_python_ceiling(spec: str) -> tuple[int, int] | None:
    m = re.search(r"<\s*(\d+)\.(\d+)", spec)
    return (int(m.group(1)), int(m.group(2))) if m else None


def _classifier_minors(classifiers: list[str]) -> set[tuple[int, int]]:
    out = set()
    for c in classifiers:
        m = re.fullmatch(r"Programming Language :: Python :: (\d+)\.(\d+)", c.strip())
        if m:
            out.add((int(m.group(1)), int(m.group(2))))
    return out


def _cibw_build(workflow: str) -> str | None:
    m = re.search(r"^\s*CIBW_BUILD:\s*[\"']?([^\"'\n]*)[\"']?\s*$", workflow, re.M)
    return m.group(1).strip() if m else None


def _cibw_build_minors(selector: str) -> set[tuple[int, int]]:
    out = set()
    for token in selector.split():
        m = re.fullmatch(r"cp(\d)(\d+)-\*", token)
        if m:
            out.add((int(m.group(1)), int(m.group(2))))
    return out


def _cibuildwheel_pin(workflow: str) -> str | None:
    m = re.search(r"pip install[^\n]*\bcibuildwheel\s*==\s*([0-9][0-9A-Za-z.]*)", workflow)
    return m.group(1) if m else None


def _pybind11_floor(requires: list[str]) -> str | None:
    for req in requires:
        m = re.fullmatch(r"\s*pybind11\s*(?:\[[^\]]*\])?\s*(.*)", req)
        if m:
            spec = m.group(1)
            floor = re.search(r"(?:>=|==|~=)\s*([0-9][0-9A-Za-z.]*)", spec)
            return floor.group(1) if floor else ""
    return None


def check(root: Path) -> list[str]:
    problems: list[str] = []
    pyproject = tomllib.loads((root / PYPROJECT).read_text())
    workflow = (root / WORKFLOW).read_text()

    project = pyproject.get("project", {})
    spec = project.get("requires-python", "")
    floor = _requires_python_floor(spec)
    if floor is None:
        return [f"{PYPROJECT}: requires-python ({spec!r}) has no >= lower bound"]
    ceiling = _requires_python_ceiling(spec)
    if ceiling is not None and ceiling <= NEWEST_STABLE_CPYTHON:
        problems.append(
            f"{PYPROJECT}: requires-python ({spec!r}) excludes CPython "
            f"{_fmt(ceiling)}, which is released; lift the bound and build wheels for it")

    supported = {(3, m) for m in range(floor[1], NEWEST_STABLE_CPYTHON[1] + 1)}
    wanted = ", ".join(_fmt(v) for v in sorted(supported))

    classified = _classifier_minors(project.get("classifiers", []))
    for v in sorted(supported - classified):
        problems.append(
            f"{PYPROJECT}: no 'Programming Language :: Python :: {_fmt(v)}' classifier; "
            f"requires-python {spec!r} supports {wanted}")
    for v in sorted(classified - supported):
        problems.append(
            f"{PYPROJECT}: classifier for Python {_fmt(v)} is outside requires-python {spec!r}")

    selector = _cibw_build(workflow)
    if selector is None:
        problems.append(f"{WORKFLOW}: no CIBW_BUILD selector found")
    else:
        built = _cibw_build_minors(selector)
        for v in sorted(supported - built):
            problems.append(
                f"{WORKFLOW}: CIBW_BUILD ({selector!r}) builds no cp{v[0]}{v[1]} wheel; "
                f"pip on Python {_fmt(v)} falls back to compiling the sdist")
        for v in sorted(built - supported):
            problems.append(
                f"{WORKFLOW}: CIBW_BUILD builds cp{v[0]}{v[1]}, which requires-python "
                f"{spec!r} does not claim")

    needed = [TOOL_MINIMUMS[v] for v in sorted(supported) if v in TOOL_MINIMUMS]
    pin = _cibuildwheel_pin(workflow)
    if pin is None:
        problems.append(f"{WORKFLOW}: cibuildwheel is not pinned with == in a pip install line")
    else:
        for v in sorted(supported):
            if v in TOOL_MINIMUMS and _version(pin) < _version(TOOL_MINIMUMS[v][0]):
                problems.append(
                    f"{WORKFLOW}: cibuildwheel=={pin} does not build CPython {_fmt(v)} "
                    f"by default; it needs cibuildwheel>={TOOL_MINIMUMS[v][0]}")

    requires = pyproject.get("build-system", {}).get("requires", [])
    pyb = _pybind11_floor(requires)
    if pyb is None:
        problems.append(f"{PYPROJECT}: [build-system].requires names no pybind11")
    elif needed:
        for v in sorted(supported):
            if v not in TOOL_MINIMUMS:
                continue
            minimum = TOOL_MINIMUMS[v][1]
            if not pyb or _version(pyb) < _version(minimum):
                problems.append(
                    f"{PYPROJECT}: build-system requires pybind11>={pyb or '(any)'}, which "
                    f"admits releases without CPython {_fmt(v)} support; it needs "
                    f"pybind11>={minimum}")
    return problems


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", type=Path, default=REPO,
                    help="repository root to check (default: this checkout)")
    args = ap.parse_args(argv)
    problems = check(args.root)
    if problems:
        print("the wheel matrix does not cover the Pythons the package claims:")
        for p in problems:
            print(f"  {p}")
        return 1
    print("OK: wheel matrix, classifiers and build-tool pins cover every claimed CPython.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
