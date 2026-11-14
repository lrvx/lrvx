"""The wheel matrix covers every CPython the package claims to support.

0.12.0 shipped no wheel for Python 3.14: `CIBW_BUILD` stopped at cp313 and
the pinned cibuildwheel (2.22.0) could not build 3.14, so pip on 3.14 fell
back to compiling the sdist. `scripts/check_wheel_matrix.py` ties the claim
(pyproject's requires-python and classifiers) to the build (python-wheels.yml)
and to the minimum build-tool versions. These tests run it against the real
tree, and against a miniature tree to show it goes red on each kind of drift.
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_wheel_matrix.py"
CI = REPO / ".github" / "workflows" / "ci.yml"


def _run(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(SCRIPT), *args],
                          capture_output=True, text=True, cwd=REPO)


# ── The real tree ─────────────────────────────────────────────────────


def test_the_real_tree_builds_a_wheel_for_every_claimed_python():
    r = _run()
    assert r.returncode == 0, r.stdout + r.stderr


def test_the_wheel_matrix_includes_python_3_14():
    import re
    text = (REPO / ".github" / "workflows" / "python-wheels.yml").read_text()
    build = re.search(r"CIBW_BUILD:\s*\"([^\"]*)\"", text)
    assert build, "CIBW_BUILD not found"
    assert "cp314-*" in build.group(1).split(), build.group(1)


def test_ci_runs_the_wheel_matrix_check():
    assert "scripts/check_wheel_matrix.py" in CI.read_text(), \
        "add a step running `python3 scripts/check_wheel_matrix.py` next to check_dep_pins.py"


# ── A miniature tree ──────────────────────────────────────────────────

GOOD_PYPROJECT = """\
[build-system]
requires = ["scikit-build-core>=0.10", "pybind11>=3.1.0"]
build-backend = "scikit_build_core.build"

[project]
name = "lrvx"
version = "0.0.0"
requires-python = ">=3.10"
classifiers = [
    "Programming Language :: Python :: 3",
    "Programming Language :: Python :: 3.10",
    "Programming Language :: Python :: 3.11",
    "Programming Language :: Python :: 3.12",
    "Programming Language :: Python :: 3.13",
    "Programming Language :: Python :: 3.14",
    "Programming Language :: Python :: 3.15",
]
"""

GOOD_WORKFLOW = """\
jobs:
  build-wheels:
    steps:
      - name: Install cibuildwheel
        run: pip install cibuildwheel==4.2.0
      - name: Build wheels
        run: cibuildwheel --output-dir wheelhouse .
        env:
          CIBW_BUILD: "cp310-* cp311-* cp312-* cp313-* cp314-* cp315-*"
          CIBW_SKIP: "*-musllinux_* pp*"
"""


class Tree:
    def __init__(self, root: Path):
        self.root = root
        self.write("pyproject.toml", GOOD_PYPROJECT)
        self.write(".github/workflows/python-wheels.yml", GOOD_WORKFLOW)

    def write(self, rel: str, text: str) -> None:
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)

    def edit(self, rel: str, old: str, new: str) -> None:
        text = (self.root / rel).read_text()
        assert old in text, f"{old!r} not in {rel}; the fixture drifted"
        (self.root / rel).write_text(text.replace(old, new))

    def run(self) -> subprocess.CompletedProcess:
        return _run("--root", str(self.root))


@pytest.fixture
def tree(tmp_path: Path) -> Tree:
    return Tree(tmp_path / "tree")


def test_a_complete_tree_is_green(tree: Tree):
    r = tree.run()
    assert r.returncode == 0, r.stdout + r.stderr


@pytest.mark.parametrize("old,new,named", [
    # The 0.12.0 shape: no cp314 / cp315 wheel.
    ("cp313-* cp314-* cp315-*", "cp313-*", "cp314"),
    ("cp314-* cp315-*", "cp314-*", "cp315"),
    # A version dropped from the middle of the range.
    ("cp311-* ", "", "cp311"),
    # The 0.12.0 cibuildwheel pin cannot build 3.14.
    ("cibuildwheel==4.2.0", "cibuildwheel==2.22.0", "cibuildwheel>=3.1.0"),
    # 3.0.0 has 3.14 only behind enable=cpython-prerelease.
    ("cibuildwheel==4.2.0", "cibuildwheel==3.0.0", "cibuildwheel>=3.1.0"),
    # 4.1.x builds 3.15 only behind enable=cpython-prerelease.
    ("cibuildwheel==4.2.0", "cibuildwheel==4.1.0", "cibuildwheel>=4.2.0"),
    # The 0.12.0 pybind11 floor admits releases without 3.14 support.
    ('"pybind11>=3.1.0"', '"pybind11>=2.12"', "pybind11>=3.0.0"),
    ('"pybind11>=3.1.0"', '"pybind11>=3.0.0"', "pybind11>=3.1.0"),
    ('"pybind11>=3.1.0"', '"pybind11"', "pybind11>=3.0.0"),
])
def test_drift_in_the_workflow_or_pins_is_red(tree: Tree, old, new, named):
    rel = ".github/workflows/python-wheels.yml" if "cp3" in old or "cibuildwheel" in old \
        else "pyproject.toml"
    tree.edit(rel, old, new)
    r = tree.run()
    assert r.returncode == 1, r.stdout + r.stderr
    assert named in r.stdout, r.stdout


def test_a_missing_classifier_is_red(tree: Tree):
    tree.edit("pyproject.toml", '    "Programming Language :: Python :: 3.14",\n', "")
    r = tree.run()
    assert r.returncode == 1
    assert "Python :: 3.14" in r.stdout, r.stdout


def test_a_wheel_outside_requires_python_is_red(tree: Tree):
    tree.edit("pyproject.toml", 'requires-python = ">=3.10"', 'requires-python = ">=3.11"')
    tree.edit("pyproject.toml", '    "Programming Language :: Python :: 3.10",\n', "")
    r = tree.run()
    assert r.returncode == 1
    assert "cp310" in r.stdout, r.stdout


def test_an_upper_bound_below_a_released_python_is_red(tree: Tree):
    tree.edit("pyproject.toml", 'requires-python = ">=3.10"', 'requires-python = ">=3.10,<3.14"')
    r = tree.run()
    assert r.returncode == 1
    assert "3.14" in r.stdout, r.stdout


def test_the_0_12_0_configuration_is_red_and_names_every_gap(tree: Tree):
    tree.edit("pyproject.toml", '"pybind11>=3.1.0"', '"pybind11>=2.12"')
    tree.edit("pyproject.toml", '    "Programming Language :: Python :: 3.13",\n'
              '    "Programming Language :: Python :: 3.14",\n'
              '    "Programming Language :: Python :: 3.15",\n', "")
    tree.edit(".github/workflows/python-wheels.yml", "cibuildwheel==4.2.0", "cibuildwheel==2.22.0")
    tree.edit(".github/workflows/python-wheels.yml", " cp314-* cp315-*", "")
    r = tree.run()
    assert r.returncode == 1
    for needle in ("cp314", "cp315", "Python :: 3.13", "Python :: 3.14",
                   "cibuildwheel==2.22.0", "pybind11>=2.12"):
        assert needle in r.stdout, (needle, r.stdout)


def test_a_classifier_above_the_newest_released_python_is_red(tree: Tree):
    tree.edit("pyproject.toml", '    "Programming Language :: Python :: 3.15",\n',
              '    "Programming Language :: Python :: 3.15",\n'
              '    "Programming Language :: Python :: 3.16",\n')
    r = tree.run()
    assert r.returncode == 1, r.stdout + r.stderr
    assert "classifier for Python 3.16" in r.stdout, r.stdout


@pytest.mark.parametrize("bound,red", [
    # Excludes 3.15, the newest released CPython.
    ("<3.15", True),
    # Admits every released CPython; 3.16 is not out.
    ("<3.16", False),
])
def test_an_upper_bound_at_the_newest_released_python(tree: Tree, bound, red):
    tree.edit("pyproject.toml", 'requires-python = ">=3.10"',
              f'requires-python = ">=3.10,{bound}"')
    r = tree.run()
    assert r.returncode == (1 if red else 0), r.stdout + r.stderr
    if red:
        assert "excludes CPython 3.15" in r.stdout, r.stdout


def test_a_free_threaded_selector_is_not_the_regular_wheel(tree: Tree):
    # cp314t-* builds the free-threaded 3.14 wheel only; the regular cp314
    # wheel is still missing.
    tree.edit(".github/workflows/python-wheels.yml", "cp314-*", "cp314t-*")
    r = tree.run()
    assert r.returncode == 1, r.stdout + r.stderr
    assert "builds no cp314 wheel" in r.stdout, r.stdout


@pytest.mark.parametrize("pin,red", [
    ("pybind11==3.1.0", False),
    ("pybind11~=3.1.0", False),
    ("pybind11==3.0.1", True),
    ("pybind11~=3.0", True),
])
def test_exact_and_compatible_pybind11_pins_are_judged_by_their_version(tree: Tree, pin, red):
    tree.edit("pyproject.toml", '"pybind11>=3.1.0"', f'"{pin}"')
    r = tree.run()
    assert r.returncode == (1 if red else 0), r.stdout + r.stderr
    if red:
        assert "pybind11>=3.1.0" in r.stdout, r.stdout


def test_the_cibuildwheel_pin_is_read_from_the_pip_install_line(tree: Tree):
    # An older version mentioned before the install line (a comment, a
    # changelog note) is not the pin.
    tree.edit(".github/workflows/python-wheels.yml", "jobs:\n",
              "# Moved off cibuildwheel==2.22.0, which cannot build 3.14.\njobs:\n")
    r = tree.run()
    assert r.returncode == 0, r.stdout + r.stderr


def test_a_cibuildwheel_version_outside_a_pip_install_line_is_no_pin(tree: Tree):
    tree.edit(".github/workflows/python-wheels.yml",
              "run: pip install cibuildwheel==4.2.0",
              "run: pip install cibuildwheel\n        # cibuildwheel==4.2.0")
    r = tree.run()
    assert r.returncode == 1, r.stdout + r.stderr
    assert "cibuildwheel is not pinned" in r.stdout, r.stdout
