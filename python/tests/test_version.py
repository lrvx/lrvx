"""``lrvx.__version__`` is the release the package was built from.

0.12.0 had none: ``hasattr(lrvx, "__version__")`` was False on the installed
wheel, and ``lrvx.bundle`` recorded every bundle's ``lrvx_version`` as
"unknown". The value has to be the version this tree declares in
pyproject.toml, and, where lrvx is installed as a distribution, the version
pip reports for it. The compiled library reports the same release through
the C API (``lrvx_version()``), so the two cannot drift apart.
"""
from __future__ import annotations

import importlib.metadata
import re
import sys
import tomllib
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
for cand in ("build/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        sys.path.insert(0, str(p))
        break

import lrvx  # noqa: E402


def _pyproject_version() -> str:
    return tomllib.loads((REPO_ROOT / "pyproject.toml").read_text())["project"]["version"]


def test_version_attribute_exists() -> None:
    assert hasattr(lrvx, "__version__"), "lrvx has no __version__"
    assert isinstance(lrvx.__version__, str)


def test_version_is_the_one_pyproject_declares() -> None:
    assert lrvx.__version__ == _pyproject_version()


def test_version_is_the_installed_distribution_version() -> None:
    try:
        dist = importlib.metadata.distribution("lrvx")
    except importlib.metadata.PackageNotFoundError:
        pytest.skip("lrvx is not installed as a distribution here (PYTHONPATH build)")
    installed_init = Path(str(dist.locate_file("lrvx/__init__.py"))).resolve()
    if installed_init != Path(lrvx.__file__).resolve():
        pytest.skip("the imported lrvx is not the installed distribution")
    assert lrvx.__version__ == importlib.metadata.version("lrvx")


def test_version_matches_the_compiled_library() -> None:
    """lrvx.version() is the compiled library's lrvx_version(); a stale
    extension next to newer Python sources shows up here."""
    assert lrvx.version() == lrvx.__version__


def test_version_is_a_release_string() -> None:
    assert re.fullmatch(r"\d+\.\d+\.\d+([.-]?[A-Za-z0-9.]+)?", lrvx.__version__), lrvx.__version__


def test_bundle_records_the_version() -> None:
    from lrvx import bundle

    assert bundle._lrvx_version() == lrvx.__version__


# ── Where the number lives ───────────────────────────────────────────

# The code that reports the version at run time. Every one of them reads it
# from the compiled library (lrvx_version(), from CMake's PROJECT_VERSION),
# never from a literal of its own: scripts/set-version.sh rewrites the
# release in CMakeLists.txt, pyproject.toml, node/package.json and the mcp /
# compat manifests only, so a literal here would survive the next bump and
# report the old release while every check above still passed on the day it
# was written.
_VERSION_REPORTERS = (
    "src/version.cpp",
    "include/lrvx/version.h",
    "src/capi/lrvx_capi.cpp",
    "python/lrvx.cpp",
    "python/lrvx/__init__.py",
    "node/src/abi.h",
    "src/quickjs/js_bindings.cpp",
    "codon/lrvx/abi.codon",
)

_QUOTED_RELEASE = re.compile(r"""["']v?\d+\.\d+\.\d+([.-][A-Za-z0-9.]+)?["']""")


def test_the_release_is_written_only_where_set_version_writes_it() -> None:
    release = _pyproject_version()
    set_version = (REPO_ROOT / "scripts" / "set-version.sh").read_text()
    for target in ("CMakeLists.txt", "pyproject.toml", "node/package.json"):
        assert target in set_version, f"scripts/set-version.sh no longer writes {target}"
    assert re.search(rf"^project\(lrvx VERSION {re.escape(release)}[ )]",
                     (REPO_ROOT / "CMakeLists.txt").read_text(), re.M), \
        "CMakeLists.txt and pyproject.toml declare different releases"

    exact = re.compile(rf"(?<![\d.]){re.escape(release)}(?![\d])")
    for rel in _VERSION_REPORTERS:
        path = REPO_ROOT / rel
        assert path.is_file(), f"{rel} moved; update _VERSION_REPORTERS"
        text = path.read_text()
        assert not exact.search(text), f"{rel} spells out the release {release}"
        literal = _QUOTED_RELEASE.search(text)
        assert literal is None, f"{rel} carries a version literal {literal.group(0)}"


# ── Without the compiled extension ───────────────────────────────────

# A source-tree import (python/ on the path, no _lrvx extension next to it)
# takes the version from the installed distribution's metadata, and reports
# "0+unknown" when there is none.
_FALLBACK_PROBE = r"""
import importlib.metadata as md
import json
import sys

mode = sys.argv[1]
asked = []

def fake_version(name):
    asked.append(name)
    if mode == "installed" and name == "lrvx":
        return "7.8.9"
    raise md.PackageNotFoundError(name)

md.version = fake_version
import lrvx
print(json.dumps({"version": lrvx.__version__, "asked": asked,
                  "native": getattr(sys.modules.get("lrvx._lrvx"), "__file__", None)
                            is not None}))
"""


def _source_tree_version(mode: str) -> dict:
    import json
    import subprocess

    r = subprocess.run([sys.executable, "-I", "-c",
                        "import sys; sys.path.insert(0, sys.argv.pop(1)); "
                        "exec(sys.argv.pop(1))",
                        str(REPO_ROOT / "python"), _FALLBACK_PROBE, mode],
                       capture_output=True, text=True, timeout=120, cwd=REPO_ROOT)
    assert r.returncode == 0, r.stdout + r.stderr
    out = json.loads(r.stdout.strip().splitlines()[-1])
    assert not out["native"], "python/ holds a compiled extension; the probe needs a bare tree"
    return out


def test_a_source_tree_import_reports_the_installed_distribution() -> None:
    out = _source_tree_version("installed")
    assert out["asked"] == ["lrvx"], out
    assert out["version"] == "7.8.9", out


def test_a_source_tree_import_without_a_distribution_is_unknown() -> None:
    out = _source_tree_version("missing")
    assert out["asked"] == ["lrvx"], out
    assert out["version"] == "0+unknown", out
