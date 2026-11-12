#!/usr/bin/env python3
"""Check that the binding type files declare every member a class exposes.

Node: node/index.d.ts against the NAPI wraps in node/src/*.h.
  - Every DefineClass(env, "<JsName>", {...}) block is read for its
    InstanceMethod and InstanceAccessor names. The JS name is the one the
    block registers (a wrap named FooWrap can register "FooAggregator").
  - Wraps defined by a macro (`#define WRAP_X(Name, ...)` whose body is
    `class Name##Wrap ... DefineClass(env, #Name, ...)`) are expanded once
    per invocation `WRAP_X(SMA, ...)`, so a macro-built class is checked
    like a hand-written one.
  - The `export class <JsName>` body in index.d.ts, together with every
    interface it implements and class it extends, must declare each name.

QuickJS: quickjs/types/lrvx.d.ts against the prelude classes in
quickjs/lrvx/ (the files in QJS_FILES). Each public method and getter of a class (its own and
those inherited through `extends`, names without a leading underscore)
must appear in the matching `declare class` body (with its own `extends`
and `implements`).

A class the binding registers but the type file does not declare at all is
a failure too, unless listed in SKIP below with the reason. The previous
version of this check read InstanceMethod only, never expanded the macros
and skipped a d.ts class written `export class X implements Y {` as "no d.ts
class", so a missing accessor (`count`) on the macro-built bar-input
indicators passed it.

Extra members in the type files are fine (aliases, overloads).
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
NODE_SRC = REPO_ROOT / "node" / "src"
NODE_DTS = REPO_ROOT / "node" / "index.d.ts"
QJS_SRC = REPO_ROOT / "quickjs" / "lrvx"
# The prelude files whose classes lrvx.d.ts declares. The others (latency,
# feed clock, order group, and the Strategy hooks such as onBar) are not
# declared there yet; add a file here once its classes are.
QJS_FILES = ["indicators.js"]
QJS_DTS = REPO_ROOT / "quickjs" / "types" / "lrvx.d.ts"

# JS class name -> reason it has no type declaration.
SKIP: dict[str, str] = {}

MEMBER_RE = re.compile(r'Instance(?:Method|Accessor)\(\s*"([A-Za-z0-9_]+)"')
DEFINE_CLASS_RE = re.compile(r'DefineClass\(\s*env\s*,\s*("([A-Za-z0-9_]+)"|#(\w+))')
MACRO_RE = re.compile(r"^#define\s+(\w+)\((\w+)(?:\s*,\s*\w+)*\)")


def _block_after(text: str, start: int) -> str:
    """The text of the `{...}` member list that follows DefineClass at start."""
    i = text.index("{", start)
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return text[i:j + 1]
    return text[i:]


def node_classes() -> dict[str, tuple[str, set[str]]]:
    """{JsName -> (source file, member names)} for every NAPI class."""
    out: dict[str, tuple[str, set[str]]] = {}
    for src in sorted(NODE_SRC.glob("*.h")):
        text = src.read_text(encoding="utf-8")
        # Macro bodies: joined continuation lines.
        macros: dict[str, tuple[str, set[str]]] = {}
        lines = text.splitlines()
        i = 0
        while i < len(lines):
            m = MACRO_RE.match(lines[i])
            if m:
                body = [lines[i]]
                while body[-1].rstrip().endswith("\\") and i + 1 < len(lines):
                    i += 1
                    body.append(lines[i])
                joined = "\n".join(l.rstrip().rstrip("\\") for l in body)
                dm = DEFINE_CLASS_RE.search(joined)
                if dm and dm.group(3) == m.group(2):
                    macros[m.group(1)] = (m.group(2), set(MEMBER_RE.findall(_block_after(joined, dm.end()))))
            i += 1
        for name, (param, members) in macros.items():
            for inv in re.finditer(r"^%s\((\w+)[,)]" % re.escape(name), text, re.M):
                out[inv.group(1)] = (src.name, set(members))
        # Hand-written classes.
        for dm in DEFINE_CLASS_RE.finditer(text):
            if dm.group(2) is None:
                continue
            members = set(MEMBER_RE.findall(_block_after(text, dm.end())))
            if members:
                out.setdefault(dm.group(2), (src.name, set()))[1].update(members)
    return out


def _ts_bodies(text: str, head_re: re.Pattern) -> dict[str, tuple[set[str], list[str]]]:
    """{name -> (members, parents)} for every class/interface `head_re` matches."""
    out: dict[str, tuple[set[str], list[str]]] = {}
    member_re = re.compile(r"^\s*(?:readonly\s+|static\s+|get\s+|set\s+)*([A-Za-z0-9_]+)\s*[?]?\s*[(:<]")
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        m = head_re.match(lines[i])
        if not m:
            i += 1
            continue
        name = m.group(1)
        parents = re.findall(r"[A-Za-z0-9_]+", (m.group(2) or "").replace("extends", " ").replace("implements", " "))
        members: set[str] = set()
        depth = lines[i].count("{") - lines[i].count("}")
        i += 1
        while i < len(lines) and depth > 0:
            line = lines[i]
            if depth == 1:
                mm = member_re.match(line)
                if mm and mm.group(1) != "constructor":
                    members.add(mm.group(1))
            depth += line.count("{") - line.count("}")
            i += 1
        prev = out.get(name)
        if prev:
            prev[0].update(members)
            prev[1].extend(parents)
        else:
            out[name] = (members, parents)
    return out


def _resolved(bodies: dict[str, tuple[set[str], list[str]]], name: str, seen=None) -> set[str] | None:
    if name not in bodies:
        return None
    seen = seen or set()
    if name in seen:
        return set()
    seen.add(name)
    members, parents = bodies[name]
    out = set(members)
    for p in parents:
        out |= _resolved(bodies, p, seen) or set()
    return out


def check_node(failures: list[str], coverage: list[str]) -> None:
    head = re.compile(r"^(?:export\s+)?(?:declare\s+)?(?:abstract\s+)?(?:class|interface)\s+([A-Za-z0-9_]+)(?:<[^>{]*>)?\s*((?:extends|implements)[^{]*)?\{")
    bodies = _ts_bodies(NODE_DTS.read_text(encoding="utf-8"), head)
    for js, (src, members) in sorted(node_classes().items()):
        if js in SKIP:
            continue
        declared = _resolved(bodies, js)
        if declared is None:
            failures.append(f"::error::{src}: class {js} is registered but node/index.d.ts declares no class {js}")
            continue
        missing = members - declared
        coverage.append(f"  node    {js:<34} {len(members):>3} members / {len(missing)} missing")
        if missing:
            failures.append(f"::error::node/index.d.ts class {js} ({src}) is missing: {', '.join(sorted(missing))}")


def quickjs_classes() -> dict[str, tuple[set[str], str | None, str]]:
    """{class -> (public members, base class, file)} for the prelude."""
    out: dict[str, tuple[set[str], str | None, str]] = {}
    head = re.compile(r"^class\s+([A-Za-z0-9_]+)(?:\s+extends\s+([A-Za-z0-9_.]+))?\s*\{")
    member = re.compile(r"^\s{4}(?:static\s+)?(?:get\s+|set\s+)?([A-Za-z_$][A-Za-z0-9_$]*)\s*\(")
    for src in (QJS_SRC / f for f in QJS_FILES):
        lines = src.read_text(encoding="utf-8").splitlines()
        i = 0
        while i < len(lines):
            m = head.match(lines[i])
            if not m:
                i += 1
                continue
            name, base = m.group(1), m.group(2)
            members: set[str] = set()
            depth = lines[i].count("{") - lines[i].count("}")
            i += 1
            while i < len(lines) and depth > 0:
                line = lines[i]
                if depth == 1:
                    mm = member.match(line)
                    if mm and mm.group(1) not in ("constructor", "if", "for", "while", "switch", "return"):
                        members.add(mm.group(1))
                depth += line.count("{") - line.count("}")
                i += 1
            out[name] = (members, base, src.name)
    return out


def check_quickjs(failures: list[str], coverage: list[str]) -> None:
    head = re.compile(r"^(?:export\s+)?(?:declare\s+)?(?:abstract\s+)?(?:class|interface)\s+([A-Za-z0-9_]+)(?:<[^>{]*>)?\s*((?:extends|implements)[^{]*)?\{")
    bodies = _ts_bodies(QJS_DTS.read_text(encoding="utf-8"), head)
    classes = quickjs_classes()

    def public(name: str, seen=None) -> set[str]:
        seen = seen or set()
        if name not in classes or name in seen:
            return set()
        seen.add(name)
        members, base, _ = classes[name]
        out = {m for m in members if not m.startswith("_")}
        return out | (public(base, seen) if base else set())

    for name, (_, _, src) in sorted(classes.items()):
        if name.startswith("_") or name in SKIP:
            continue
        declared = _resolved(bodies, name)
        if declared is None:
            failures.append(f"::error::quickjs/lrvx/{src}: class {name} has no `declare class {name}` in quickjs/types/lrvx.d.ts")
            continue
        members = public(name)
        missing = members - declared
        coverage.append(f"  quickjs {name:<34} {len(members):>3} members / {len(missing)} missing")
        if missing:
            failures.append(f"::error::quickjs/types/lrvx.d.ts class {name} (quickjs/lrvx/{src}) is missing: {', '.join(sorted(missing))}")


def main() -> int:
    for p in (NODE_DTS, QJS_DTS):
        if not p.exists():
            print(f"::error::{p} not found", file=sys.stderr)
            return 2
    failures: list[str] = []
    coverage: list[str] = []
    check_node(failures, coverage)
    check_quickjs(failures, coverage)

    print("binding class -> type file member coverage:")
    for line in coverage:
        print(line)
    print()
    if failures:
        for f in failures:
            print(f, file=sys.stderr)
        return 1
    print("OK — every NAPI and QuickJS class member is declared in its type file.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
