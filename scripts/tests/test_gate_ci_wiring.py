"""The gates only gate what CI actually runs.

The parity step ran without `--require-quickjs` and said so in a comment
("turn it on here once the manifest is complete"), which left 59 of 73 IDL
groups unchecked against the QuickJS layer while the step reported success.
A flag that exists and is never passed is not a gate.
"""

from __future__ import annotations

from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CI = REPO / ".github" / "workflows" / "ci.yml"


def test_ci_runs_the_parity_gate_with_quickjs_required():
    text = CI.read_text()
    assert "check_binding_parity.py --require-quickjs" in text, \
        "the CI parity step must name the gate it runs"
    assert "--no-require-quickjs" not in text, \
        "CI must not opt out of the QuickJS half"


def test_ci_runs_the_gate_tests():
    """These tests are the only thing holding the two gates honest, so they
    have to run somewhere other than a developer's laptop."""
    assert "scripts/tests" in CI.read_text(), \
        "add a step running `pytest scripts/tests -q`"


def test_the_typecheck_ci_runs_covers_the_quickjs_typings():
    """quickjs/types/lrvx.d.ts was checked by nothing: a typing could name a
    method the runtime does not have and no job would say so."""
    import json

    scripts = json.loads((REPO / "node" / "package.json").read_text())["scripts"]
    assert "../quickjs/types/tsconfig.json" in scripts["typecheck"]
    assert "npm run typecheck" in CI.read_text()
    config = json.loads((REPO / "quickjs" / "types" / "tsconfig.json").read_text())
    assert "lrvx.d.ts" in config["include"]
