#!/usr/bin/env python3
"""Every member of the QuickJS `lrvx` object has a typing, and vice versa.

The `lrvx` object a script sees is one object literal inside a C++ string
(`var lrvx = { ... }` in src/quickjs/js_strategy.cpp). Its typing is one
`declare const lrvx: { ... }` block in quickjs/types/lrvx.d.ts. Nothing ties
the two: `lrvx.DeltaBookEncoder`, `lrvx.DeltaBookReplayer`,
`lrvx.BarDispatchRecorder`, `lrvx.tapeDiff` and a dozen more shipped with no
typing, so a typed script that used them did not compile, and a typing for a
member that had been removed would have gone on compiling.

This compares the two sets of names. It does not compare signatures: a
member's parameters live in C++ wrappers the typing cannot be derived from.

Run:
    python3 scripts/check_quickjs_lrvx_typings.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PRELUDE = REPO / "src" / "quickjs" / "js_strategy.cpp"
TYPINGS = REPO / "quickjs" / "types" / "lrvx.d.ts"

OBJECT_OPEN = re.compile(r"^(\s*)var lrvx = \{\s*$")
TYPING_OPEN = re.compile(r"^(\s*)declare const lrvx: \{\s*$")
MEMBER = re.compile(r"^(\w+)\s*[:(?]")


def members(path: Path, opener: re.Pattern[str]) -> set[str]:
    """Names declared one level inside the block `opener` starts."""
    lines = path.read_text(encoding="utf-8").split("\n")
    for start, line in enumerate(lines):
        m = opener.match(line)
        if m:
            break
    else:
        raise SystemExit(f"error: {path.relative_to(REPO)}: no line matches {opener.pattern!r}")
    outer = m.group(1)
    names: set[str] = set()
    inner: str | None = None
    for line in lines[start + 1:]:
        if line.startswith(outer + "}"):
            return names
        if not line.strip():
            continue
        indent = line[: len(line) - len(line.lstrip())]
        if inner is None:
            inner = indent
        if indent != inner:
            continue
        m = MEMBER.match(line.strip())
        if m:
            names.add(m.group(1))
    raise SystemExit(f"error: {path.relative_to(REPO)}: the block {opener.pattern!r} never closes")


def main() -> int:
    registered = members(PRELUDE, OBJECT_OPEN)
    typed = members(TYPINGS, TYPING_OPEN)
    print(f"QuickJS lrvx object: {len(registered)} members registered, {len(typed)} typed.")

    problems = [
        f"lrvx.{name} is registered in {PRELUDE.relative_to(REPO)} and has no typing in "
        f"{TYPINGS.relative_to(REPO)}"
        for name in sorted(registered - typed)
    ] + [
        f"lrvx.{name} is typed in {TYPINGS.relative_to(REPO)} and is not a member of the "
        f"object in {PRELUDE.relative_to(REPO)}"
        for name in sorted(typed - registered)
    ]
    if problems:
        print(f"\n{len(problems)} problem(s):", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
