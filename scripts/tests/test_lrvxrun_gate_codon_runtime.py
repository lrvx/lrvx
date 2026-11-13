"""The .lrvxrun parity gate finds the Codon runtime libraries.

A Codon executable loads libomp and libcodonrt at run time. The gate put
only the C API's directory on the loader path, so on macOS its Codon leg
died in dyld before the fixture ran.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def _gate():
    spec = importlib.util.spec_from_file_location(
        "lrvxrun_byte_identical_gate", REPO / "scripts" / "lrvxrun_byte_identical_gate.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _install(root: Path) -> Path:
    (root / "bin").mkdir(parents=True)
    (root / "lib" / "codon").mkdir(parents=True)
    (root / "bin" / "codon").write_text("#!/bin/sh\n")
    (root / "bin" / "codon").chmod(0o755)
    return root / "lib" / "codon"


def test_the_runtime_is_found_beside_codon_home(tmp_path):
    lib = _install(tmp_path / "codon")
    env = {"CODON_HOME": str(tmp_path / "codon"), "HOME": str(tmp_path / "home"), "PATH": ""}
    assert _gate().codon_runtime_dirs(env) == [lib]


def test_the_runtime_is_found_under_the_home_directory(tmp_path):
    lib = _install(tmp_path / "home" / ".codon")
    env = {"HOME": str(tmp_path / "home"), "PATH": ""}
    assert _gate().codon_runtime_dirs(env) == [lib]


def test_a_symlinked_compiler_leads_to_the_real_installation(tmp_path):
    lib = _install(tmp_path / "real")
    link_dir = tmp_path / "home" / ".codon" / "bin"
    link_dir.mkdir(parents=True)
    (link_dir / "codon").symlink_to(tmp_path / "real" / "bin" / "codon")
    env = {"HOME": str(tmp_path / "home"), "PATH": ""}
    assert _gate().codon_runtime_dirs(env) == [lib.resolve()]


def test_the_runtime_is_found_through_path(tmp_path):
    lib = _install(tmp_path / "opt")
    env = {"HOME": str(tmp_path / "home"), "PATH": str(tmp_path / "opt" / "bin")}
    assert _gate().codon_runtime_dirs(env) == [lib]


def test_no_compiler_means_no_extra_directories(tmp_path):
    env = {"HOME": str(tmp_path / "home"), "PATH": ""}
    assert _gate().codon_runtime_dirs(env) == []
