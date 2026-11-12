"""One funding schedule, settled through every binding, gives the same payments.

Node, QuickJS and Codon asked the C API for a count before they asked for the
events, and the count call settled them: the events came back as zeros. Each
of those bindings has a test of its own that prints what tick() returned; this
one runs them and compares the lines with what Python returns.

A binding that is not built in this tree is skipped, not failed.
"""
from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

import pytest

import lrvx
from support import funding_scenario as fx

REPO = Path(__file__).resolve().parents[2]
# build/python/lrvx/__init__.py -> build
BUILD = Path(lrvx.__file__).resolve().parents[2]


def _python_events():
    entries = []
    for ts, symbol, rate in fx.TAPE:
        e = lrvx.FundingTapeEntry()
        e.timestamp_ns = ts
        e.symbol = symbol
        e.rate = rate
        entries.append(e)
    schedule = lrvx.FundingSchedule.tape_by_symbol(entries)
    return schedule.tick(fx.T2, fx.SYMBOLS, fx.POSITIONS, fx.MARKS)


def _python_lines():
    return [fx.line(e.timestamp_ns, e.symbol, e.rate, e.mark_price, e.position_signed, e.amount)
            for e in _python_events()]


def test_python_returns_the_four_payments():
    events = _python_events()
    got = [(e.timestamp_ns, e.symbol, e.rate, e.mark_price, e.position_signed, e.amount)
           for e in events]
    assert got == [
        (fx.T1, 1, 0.0001, 50000.0, 2.0, -2.0 * 50000.0 * 0.0001),
        (fx.T1, 2, -0.0002, 2000.0, -3.0, 3.0 * 2000.0 * -0.0002),
        (fx.T2, 1, 0.0003, 50000.0, 2.0, -2.0 * 50000.0 * 0.0003),
        (fx.T2, 2, 0.0003, 2000.0, -3.0, 3.0 * 2000.0 * 0.0003),
    ]


def _run(cmd, cwd, env=None):
    res = subprocess.run(cmd, cwd=str(cwd), capture_output=True, text=True, timeout=120, env=env)
    assert res.returncode == 0, f"{cmd} failed:\n{res.stdout}\n{res.stderr}"
    return fx.funding_lines(res.stdout)


def _node_lines():
    addon = REPO / "node" / "build" / "Release" / "lrvx_node.node"
    if shutil.which("node") is None or not addon.exists():
        pytest.skip("the Node addon is not built in this tree")
    return _run(["node", "test/test_funding_tick.js"], REPO / "node")


def _quickjs_lines():
    runner = BUILD / "src" / "quickjs" / "lrvx_js_runner"
    if not runner.exists():
        pytest.skip("lrvx_js_runner is not built in this tree")
    return _run([str(runner), "quickjs/examples/funding_schedule_smoke.js"], REPO)


def _codon_lines():
    binary = BUILD / "codon" / "codon_funding_schedule_smoke"
    if not binary.exists():
        pytest.skip("the Codon examples are not built in this tree")
    env = dict(os.environ)
    capi = str(BUILD / "src" / "capi")
    codon_libs = [str(Path.home() / ".codon" / "lib" / "codon"),
                  str(Path.home() / ".local" / "lib" / "codon")]
    for var in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH"):
        env[var] = os.pathsep.join([capi, *codon_libs, env.get(var, "")])
    return _run([str(binary)], REPO, env)


@pytest.mark.parametrize("binding,lines", [
    pytest.param("node", _node_lines, id="node"),
    pytest.param("quickjs", _quickjs_lines, id="quickjs"),
    pytest.param("codon", _codon_lines, id="codon"),
])
def test_binding_returns_what_python_returns(binding, lines):
    want = _python_lines()
    assert len(want) == 4
    assert lines() == want, f"{binding} and Python disagree on the settled payments"
