#!/usr/bin/env python3
"""Cross-binding `.lrvxrun` parity gate.

Each binding (pybind11 / NAPI / Codon) runs its fixture, which writes the
same hand-authored records (2 signals, 1 order event, 1 fill, explicit
timestamps) through that binding's `TraceRecorder`. No strategy runs and
no market data is replayed: the gate checks recorder serialization, not
strategy behaviour.

Every binding's `.lrvxrun` directory is read back through the pybind11
`TraceReader`, and the decoded records (signals, order events, fills) are
compared field by field. Every field must be equal, timestamps to the
nanosecond: the fixtures stamp their records with odd nanosecond counts,
which no float64 holds, so a binding that routed a timestamp through a
double would show here. The segment
files themselves are not compared: each segment header carries
`created_ns`, the wall clock at the moment the segment was opened
(src/run/trace_recorder.cpp), so two runs never produce the same bytes,
even through one binding.

A binding whose artefact is missing (no `lrvx` package in the build tree,
no `node` on PATH, no Node addon, no Codon fixture binary) is skipped, so
the gate runs on machines that build only a subset; it still needs two
bindings to compare. A binding whose fixture exists but exits non-zero
fails the gate with exit 1 and the fixture's stderr. Every leg reads from
one build tree, ``build`` by default; ``LRVX_BUILD_DIR=build-ci`` points
all of them at another tree.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

ROOT = Path(__file__).resolve().parent.parent
# The build tree every leg reads from: the pybind11 package, the Codon
# fixture binary and the C API library. LRVX_BUILD_DIR points the gate at
# another tree (build-ci, build-codon, ...). The Node addon is built by
# cmake-js under node/build and does not move.
BUILD_DIR = Path(os.environ.get("LRVX_BUILD_DIR", ROOT / "build")).resolve()
PYTHON_FIXTURE = ROOT / "scripts" / "lrvxrun_byte_identical_fixture.py"
NODE_FIXTURE = ROOT / "node" / "test" / "byte_identical_fixture.js"
CODON_FIXTURE_BIN = BUILD_DIR / "codon" / "codon_byte_identical_fixture"
CODON_LIB_DIR = BUILD_DIR / "src" / "capi"


def codon_runtime_dirs(env: Dict[str, str]) -> List[Path]:
    """Where the Codon runtime libraries (libcodonrt, libomp) live.

    A Codon executable loads them at run time and macOS does not find
    them by itself. They sit in ``lib/codon`` beside the compiler's
    ``bin``, so this looks the compiler up where codon/CMakeLists.txt
    does (``$CODON_HOME/bin``, ``~/.codon/bin``, then PATH) and follows a
    symlinked ``bin/codon`` to the real installation."""
    compilers: List[Path] = []
    if env.get("CODON_HOME"):
        compilers.append(Path(env["CODON_HOME"]) / "bin" / "codon")
    if env.get("HOME"):
        compilers.append(Path(env["HOME"]) / ".codon" / "bin" / "codon")
    on_path = shutil.which("codon", path=env.get("PATH"))
    if on_path:
        compilers.append(Path(on_path))

    dirs: List[Path] = []
    for compiler in compilers:
        if not compiler.exists():
            continue
        for binary in (compiler, compiler.resolve()):
            lib = binary.parent.parent / "lib" / "codon"
            if lib.is_dir() and lib not in dirs:
                dirs.append(lib)
    return dirs


def _read_via_pybind(out_dir: Path) -> Dict[str, List[Dict[str, Any]]]:
    """Read records back through pybind11 TraceReader. Used as the
    canonical reader for every binding's output — the goal is to compare
    the records themselves, not the storage layout."""
    sys.path.insert(0, str(BUILD_DIR / "python"))
    import lrvx  # type: ignore  # noqa: PLC0415

    reader = lrvx.TraceReader(str(out_dir))
    return {
        "signals": [_normalize(s) for s in reader.read_all_signals()],
        "orders": [_normalize(o) for o in reader.read_all_order_events()],
        "fills": [_normalize(f) for f in reader.read_all_fills()],
    }


def _normalize(record: Any) -> Dict[str, Any]:
    """Convert a record (dict or attribute object) to a plain dict, stripping
    fields that move with each run (run_ts_ns isn't expected to differ in
    this fixture but we drop it defensively)."""
    if isinstance(record, dict):
        out = dict(record)
    else:
        out = {k: getattr(record, k) for k in dir(record) if not k.startswith("_")}
    # bytes -> b64 so dict equality works.
    for k, v in list(out.items()):
        if isinstance(v, bytes):
            out[k] = ("b64", v.hex())
    return out


class FixtureFailed(Exception):
    """A binding's fixture exists but exited non-zero."""

    def __init__(self, res: subprocess.CompletedProcess) -> None:
        output = (res.stderr or "").strip() or (res.stdout or "").strip()
        how = (f"killed by signal {-res.returncode}" if res.returncode < 0
               else f"exit {res.returncode}")
        super().__init__(f"{how}: {output or '(no output)'}")


def _check(res: subprocess.CompletedProcess) -> None:
    if res.returncode != 0:
        raise FixtureFailed(res)


def run_python(out_dir: Path) -> Optional[Dict[str, List[Dict[str, Any]]]]:
    if not (BUILD_DIR / "python" / "lrvx").exists():
        print(f"[skip] pybind11: {BUILD_DIR / 'python' / 'lrvx'} missing")
        return None
    env = dict(os.environ)
    env["PYTHONPATH"] = str(BUILD_DIR / "python")
    _check(subprocess.run([sys.executable, str(PYTHON_FIXTURE), str(out_dir)],
                          env=env, capture_output=True, text=True))
    return _read_via_pybind(out_dir)


def run_node(out_dir: Path) -> Optional[Dict[str, List[Dict[str, Any]]]]:
    if shutil.which("node") is None:
        print("[skip] node: node binary not found")
        return None
    if not (ROOT / "node" / "build" / "Release" / "lrvx_node.node").exists():
        print("[skip] node: build/Release/lrvx_node.node missing")
        return None
    _check(subprocess.run(["node", str(NODE_FIXTURE), str(out_dir)],
                          capture_output=True, text=True))
    return _read_via_pybind(out_dir)


def run_codon(out_dir: Path) -> Optional[Dict[str, List[Dict[str, Any]]]]:
    if not CODON_FIXTURE_BIN.exists():
        print(f"[skip] codon: {CODON_FIXTURE_BIN} missing")
        return None
    env = dict(os.environ)
    libs = ":".join(str(d) for d in [CODON_LIB_DIR, *codon_runtime_dirs(env)])
    env["DYLD_LIBRARY_PATH"] = libs + ":" + env.get("DYLD_LIBRARY_PATH", "")
    env["LD_LIBRARY_PATH"] = libs + ":" + env.get("LD_LIBRARY_PATH", "")
    _check(subprocess.run([str(CODON_FIXTURE_BIN), str(out_dir)],
                          env=env, capture_output=True, text=True))
    return _read_via_pybind(out_dir)


def _diff_records(name_a: str, recs_a: Dict[str, List[Dict[str, Any]]],
                   name_b: str, recs_b: Dict[str, List[Dict[str, Any]]]) -> List[str]:
    issues: List[str] = []
    for kind in ("signals", "orders", "fills"):
        la, lb = recs_a[kind], recs_b[kind]
        if len(la) != len(lb):
            issues.append(f"{name_a} vs {name_b}: {kind} count {len(la)} vs {len(lb)}")
            continue
        for i, (a, b) in enumerate(zip(la, lb)):
            for k in sorted(set(a) | set(b)):
                va, vb = a.get(k), b.get(k)
                if va == vb:
                    continue
                issues.append(
                    f"{name_a} vs {name_b}: {kind}[{i}].{k}  "
                    f"{va!r}  vs  {vb!r}")
    return issues


def main() -> int:
    # [ok]/[skip] go to stdout and [fail] to stderr; keep them in order.
    sys.stdout.reconfigure(line_buffering=True)
    runners: List[Tuple[str, Any]] = [
        ("pybind11", run_python),
        ("napi", run_node),
        ("codon", run_codon),
    ]
    results: Dict[str, Dict[str, List[Dict[str, Any]]]] = {}
    failed: List[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        for name, runner in runners:
            out = Path(tmp) / name / "run.lrvxrun"
            out.parent.mkdir(parents=True, exist_ok=True)
            try:
                recs = runner(out)
            except FixtureFailed as exc:
                failed.append(name)
                print(f"[fail] {name}: {exc}", file=sys.stderr)
                continue
            if recs is None:
                continue
            results[name] = recs
            print(f"[ok]   {name}: "
                  f"{len(recs['signals'])} signal(s), "
                  f"{len(recs['orders'])} order(s), "
                  f"{len(recs['fills'])} fill(s)")

    if failed:
        print("\n::error::cross-binding .lrvxrun parity gate FAILED: the fixture "
              f"for {', '.join(failed)} exists but exited non-zero (see [fail] "
              "above). A missing artefact is a skip; a failing fixture is not.",
              file=sys.stderr)
        return 1

    if len(results) < 2:
        print("\nfewer than 2 bindings produced output — nothing to compare; "
              "build the missing artefacts and re-run.", file=sys.stderr)
        return 1

    bindings = list(results.keys())
    reference = results[bindings[0]]
    diverged: List[str] = []
    for other in bindings[1:]:
        diverged.extend(_diff_records(bindings[0], reference, other, results[other]))

    if diverged:
        print("\n::error::cross-binding .lrvxrun parity gate FAILED",
              file=sys.stderr)
        for d in diverged:
            print(f"  {d}", file=sys.stderr)
        return 1

    print(f"\n.lrvxrun parity gate OK across {len(results)} binding(s): "
          f"{', '.join(bindings)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
