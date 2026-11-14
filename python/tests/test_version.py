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
