"""The .lrvxrun parity gate compares timestamps to the nanosecond.

It allowed 1024 ns of drift on `run_ts_ns` / `feed_ts_ns`, from when the
Node recorder took timestamps as a JS Number. The Node recorder takes a
BigInt, so the allowance only hid differences. And the fixtures stamped
their records with multiples of 10^8 ns, which a float64 holds exactly, so
the allowance had never been needed and a lossy path would not have shown.
"""

from __future__ import annotations

import importlib.util
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def _gate():
    spec = importlib.util.spec_from_file_location(
        "lrvxrun_byte_identical_gate", REPO / "scripts" / "lrvxrun_byte_identical_gate.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _records(run_ts_ns):
    return {"signals": [{"run_ts_ns": run_ts_ns, "feed_ts_ns": 5, "name": "entry"}],
            "orders": [], "fills": []}


def test_one_nanosecond_of_difference_is_reported():
    diff = _gate()._diff_records
    t = 1_700_000_000_100_000_001
    assert diff("a", _records(t), "b", _records(t)) == []
    issues = diff("a", _records(t), "b", _records(t + 1))
    assert len(issues) == 1 and "run_ts_ns" in issues[0], issues
    assert len(diff("a", _records(t), "b", _records(t + 1024))) == 1


def _timestamps(path, pattern):
    return [int(m.replace("_", "")) for m in re.findall(pattern, (REPO / path).read_text())]


def test_every_fixture_timestamp_is_one_a_float64_cannot_hold():
    python = _timestamps("scripts/lrvxrun_byte_identical_fixture.py", r"\b1_700_[0-9_]+\b")
    codon = _timestamps("codon/examples/byte_identical_fixture.codon", r"\b1_700_[0-9_]+\b")
    node = _timestamps("node/test/byte_identical_fixture.js", r"\b(1700\d{15})n\b")
    assert len(python) == 10 and python == codon == node
    for t in python:
        assert int(float(t)) != t, f"{t} survives a float64"


def test_the_node_fixture_passes_no_timestamp_as_a_number():
    text = (REPO / "node/test/byte_identical_fixture.js").read_text()
    assert re.findall(r"\b1700\d{15}\b(?!n)", text) == []
