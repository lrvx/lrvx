#!/usr/bin/env python3
"""Check that the Node binding names arguments the way node/index.d.ts does.

The checked reads in node/src (argNumber, argString, toInt64Ns, ...) take the
name an error message gives the argument. They were handed "advanceClock:
argument 1", which tells the caller a position, not which parameter; each now
passes "<method>: <parameter>" with the parameter name index.d.ts declares
("advanceClock: timestampNs"). This fails on any label that names an argument
by its position again.

Usage:
    python3 scripts/check_node_argument_names.py
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
NODE_SRC = REPO_ROOT / "node" / "src"
POSITIONAL = re.compile(r'"[^"\n]*\bargument [0-9]+[^"\n]*"')


def main() -> int:
    failures = []
    for src in sorted(NODE_SRC.glob("*.h")) + sorted(NODE_SRC.glob("*.cpp")):
        for n, line in enumerate(src.read_text(encoding="utf-8").splitlines(), 1):
            for m in POSITIONAL.finditer(line):
                failures.append(f"{src.relative_to(REPO_ROOT)}:{n}: {m.group(0)}")
    if failures:
        print("::error::node/src names arguments by position; use the parameter name from "
              "node/index.d.ts (\"<method>: <parameter>\"):")
        for f in failures:
            print(f"  {f}")
        return 1
    print("check_node_argument_names: every checked argument is named")
    return 0


if __name__ == "__main__":
    sys.exit(main())
