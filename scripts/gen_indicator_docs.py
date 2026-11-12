#!/usr/bin/env python3
"""
scripts/gen_indicator_docs.py

Reads include/lrvx/indicator/registry.def and rewrites the auto-generated
sections in the per-binding indicator docs. Adding an indicator to the
registry → it appears in the docs without manual edits to N markdown
files.

The registry only carries the class list, the ctor arg list of the C++
surface and the input kind. Anything that differs per binding (which
classes carry a batch `compute()`, whether it is static or on the
instance, how the runtime exposes the classes) is parsed out of that
binding's own source:

    codon/lrvx/indicators.codon    → Codon classes / methods / free functions
    quickjs/lrvx/indicators.js     → QuickJS classes / static vs instance

so the generated prose cannot drift from the binding it describes.

Auto-gen sections are bounded by:
    <!-- INDICATOR-LIST-START -->
    ...generated content...
    <!-- INDICATOR-LIST-END -->

Anything outside those markers is preserved verbatim. Files without the
markers are skipped.

CI gate: this script should be run before push; if `git diff docs/` is
non-empty after running, docs are stale.

Usage:
    python3 scripts/gen_indicator_docs.py
"""

from __future__ import annotations

import json
import re
import sys
import textwrap
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
REGISTRY = REPO / "include" / "lrvx" / "indicator" / "registry.def"
CODON_SRC = REPO / "codon" / "lrvx" / "indicators.codon"
QUICKJS_SRC = REPO / "quickjs" / "lrvx" / "indicators.js"

NODE_PACKAGE_JSON = REPO / "node" / "package.json"

START = "<!-- INDICATOR-LIST-START -->"
END = "<!-- INDICATOR-LIST-END -->"


def parse_registry() -> list[dict]:
    pattern = re.compile(
        r"^LRVX_INDICATOR\(\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*\((.*?)\)\s*\)\s*$"
    )
    indicators = []
    for raw in REGISTRY.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("//"):
            continue
        m = pattern.match(line)
        if not m:
            continue
        cls, snake, kind, args = m.groups()
        indicators.append(
            {
                "class": cls,
                "snake": snake,
                "kind": kind,
                "args": args.strip(),
            }
        )
    return indicators


def _wrap(text: str, width: int = 96) -> str:
    """Wrap a generated paragraph so the markdown stays readable in diffs."""
    return textwrap.fill(text, width=width, break_long_words=False, break_on_hyphens=False)


def node_package_name() -> str:
    """npm package name of the Node binding, from node/package.json."""
    data = json.loads(NODE_PACKAGE_JSON.read_text())
    return data["name"]


def _split_params(params: str) -> list[str]:
    """Split a parameter list on top-level commas (types may hold brackets)."""
    out: list[str] = []
    depth = 0
    current = ""
    for ch in params:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(current)
            current = ""
        else:
            current += ch
    if current.strip():
        out.append(current)
    return out


def _render_params(params: str) -> str:
    """`self, period: int, fast: int = 2` -> `period, fast=2`."""
    rendered = []
    for part in _split_params(params):
        part = part.strip()
        if not part or part == "self":
            continue
        head, _, default = part.partition("=")
        name = head.split(":")[0].strip()
        default = default.strip()
        rendered.append(f"{name}={default}" if default else name)
    return ", ".join(rendered)


def _registry_params(args: str) -> str:
    """Registry ctor args (`size_t period`) -> `period`. Fallback when the
    binding source has no parseable constructor."""
    return _render_params(args.replace("size_t ", "").replace("double ", ""))


def _codon_logical_lines(text: str):
    """Yield (indent, line) with parenthesised signatures joined onto one line."""
    buf: str | None = None
    indent = 0
    for raw in text.splitlines():
        if buf is None:
            indent = len(raw) - len(raw.lstrip())
            line = raw.strip()
        else:
            line = buf + " " + raw.strip()
        if line.count("(") > line.count(")"):
            buf = line
            continue
        buf = None
        yield indent, line


def parse_codon_module() -> tuple[dict[str, dict], dict[str, str]]:
    """Parse codon/lrvx/indicators.codon.

    Returns (classes, free_functions) where classes maps class name to
    {"ctor": rendered ctor params, "methods": {name: params},
     "statics": {name: params}} and free_functions maps function name to
    its rendered params.
    """
    classes: dict[str, dict] = {}
    functions: dict[str, str] = {}
    current: str | None = None
    is_static = False

    for indent, line in _codon_logical_lines(CODON_SRC.read_text()):
        if indent == 0:
            m = re.match(r"class (\w+):", line)
            if m:
                current = m.group(1)
                classes[current] = {"ctor": "", "methods": {}, "statics": {}}
                is_static = False
                continue
            m = re.match(r"def (\w+)\((.*?)\)\s*(?:->|:)", line)
            if m:
                current = None
                functions[m.group(1)] = _render_params(m.group(2))
            continue
        if current is None or not line:
            continue
        if line.startswith("@"):
            is_static = line == "@staticmethod"
            continue
        m = re.match(r"def (\w+)\((.*?)\)\s*(?:->|:)", line)
        if m:
            name, params = m.group(1), _render_params(m.group(2))
            if name == "__init__":
                classes[current]["ctor"] = params
            elif is_static:
                classes[current]["statics"][name] = params
            else:
                classes[current]["methods"][name] = params
            is_static = False
    return classes, functions


def parse_quickjs_module() -> dict[str, dict]:
    """Parse quickjs/lrvx/indicators.js.

    Returns {class name: {"ctor", "extends", "methods": set, "statics": {name: params}}}
    in source order. Inherited members are resolved through `extends`.
    """
    classes: dict[str, dict] = {}
    current: str | None = None

    for raw in QUICKJS_SRC.read_text().splitlines():
        m = re.match(r"class (\w+)(?:\s+extends\s+(\w+))?\s*\{", raw)
        if m:
            current = m.group(1)
            classes[current] = {
                "ctor": "",
                "extends": m.group(2),
                "methods": {},
                "statics": {},
            }
            continue
        if current is None:
            continue
        line = raw.strip()
        m = re.match(r"constructor\s*\((.*?)\)", line)
        if m:
            classes[current]["ctor"] = _render_params(m.group(1))
            continue
        m = re.match(r"(static\s+)?(?:get\s+)?(\w+)\s*\((.*?)\)", line)
        if m:
            static, name, params = m.groups()
            if name in ("if", "for", "while", "return", "super", "constructor"):
                continue
            if static:
                classes[current]["statics"][name] = _render_params(params)
            else:
                classes[current]["methods"][name] = _render_params(params)

    for info in classes.values():
        base = info["extends"]
        while base and base in classes:
            for name, params in classes[base]["methods"].items():
                info["methods"].setdefault(name, params)
            for name, params in classes[base]["statics"].items():
                info["statics"].setdefault(name, params)
            base = classes[base]["extends"]
    return classes


def py_signature(ind: dict) -> str:
    args = ind["args"]
    args = args.replace("size_t ", "").replace("double ", "")
    return f"lrvx.{ind['class']}({args})"


def py_block(indicators: list[dict]) -> str:
    lines = [
        "Every indicator below is **one Python class** with both a batch",
        "`compute()` method and streaming `update()`/`value`/`ready`/`reset()`.",
        "Same instance, two ways to use it:",
        "",
        "```python",
        "import lrvx",
        "ema = lrvx.EMA(10)",
        "out = ema.compute(prices)             # batch",
        "for v in stream:",
        "    ema.update(v)",
        "    if ema.ready: print(ema.value)    # streaming on the same instance",
        "```",
        "",
        "| Indicator | Constructor | Kind |",
        "|---|---|---|",
    ]
    for ind in indicators:
        lines.append(f"| `{ind['class']}` | `{py_signature(ind)}` | {ind['kind']} |")
    lines.append("")
    lines.append("Discovery: `lrvx.list_indicators()` returns the full list at runtime.")
    return "\n".join(lines)


def js_signature(ind: dict, ctor_kw: str = "new ") -> str:
    args = ind["args"]
    args = args.replace("size_t ", "").replace("double ", "")
    return f"{ctor_kw}lrvx.{ind['class']}({args})"


def node_block(indicators: list[dict]) -> str:
    """Node binding: every class carries an instance compute() (node/index.d.ts)."""
    lines = [
        "Every indicator below is **one Node.js class** with both a batch",
        "`compute()` method and streaming `update()` / `value` / `ready` / `reset()`.",
        "Same instance, two ways to use it:",
        "",
        "```js",
        f"const lrvx = require('{node_package_name()}');",
        "const ema = new lrvx.EMA(10);",
        "const out = ema.compute(prices);            // batch",
        "for (const v of stream) {",
        "  ema.update(v);",
        "  if (ema.ready) console.log(ema.value);    // streaming on the same instance",
        "}",
        "```",
        "",
        "| Indicator | Constructor | Kind |",
        "|---|---|---|",
    ]
    for ind in indicators:
        lines.append(
            f"| `{ind['class']}` | `{js_signature(ind)}` | {ind['kind']} |"
        )
    lines.append("")
    return "\n".join(lines)


def _quickjs_batch_cell(ind: dict, info: dict | None) -> str:
    if info is None:
        return "not exposed in `lrvx/indicators.js`"
    cells = []
    if "compute" in info["statics"]:
        cells.append(f"static `{ind['class']}.compute({info['statics']['compute']})`")
    if "compute" in info["methods"]:
        cells.append(f"instance `compute({info['methods']['compute']})`")
    return ", ".join(cells) if cells else "no `compute()`"


def quickjs_block(indicators: list[dict]) -> str:
    """QuickJS binding: globals (no require), static compute() plus a few instance ones."""
    classes = parse_quickjs_module()
    known = {ind["class"] for ind in indicators}

    instance_compute = [
        ind["class"]
        for ind in indicators
        if "compute" in classes.get(ind["class"], {}).get("methods", {})
    ]
    static_only = [
        ind["class"] for ind in indicators if ind["class"] not in instance_compute
    ]

    lines = [
        "Every indicator below is a **global class**: the embedded runtime evaluates",
        "`quickjs/lrvx/indicators.js` at global scope, so there is no `require()` and no",
        "namespace prefix. Streaming is `update()` / `value` / `ready` / `count` /",
        "`reset()` on the instance, one kernel step of the C++ class per update, and",
        "`destroy()` releases the instance's C handle. Batch is a **static** `compute()`.",
        "Only some classes also carry an instance `compute()` — see the `Batch` column.",
        "",
        "```js",
        "const ema = new EMA(10);                    // global class, no require()",
        "const out = EMA.compute(prices, 10);        // batch: static compute()",
        "for (const v of stream) {",
        "  ema.update(v);",
        "  if (ema.ready) console.log(ema.value);    // streaming on the instance",
        "}",
        "```",
        "",
        "| Indicator | Constructor | Kind | Batch |",
        "|---|---|---|---|",
    ]
    for ind in indicators:
        info = classes.get(ind["class"])
        ctor_args = info["ctor"] if info and info["ctor"] else _registry_params(ind["args"])
        lines.append(
            f"| `{ind['class']}` | `new {ind['class']}({ctor_args})` | {ind['kind']} "
            f"| {_quickjs_batch_cell(ind, info)} |"
        )
    lines.append("")

    if instance_compute and static_only:
        names = ", ".join(f"`{c}`" for c in instance_compute)
        lines.append(
            _wrap(
                f"An instance `compute()` exists only on {names}. On every other class "
                "above `new EMA(20).compute(prices)` is `undefined` — call "
                "`EMA.compute(prices, 20)`."
            )
        )
        lines.append("")

    extras = [
        (name, info)
        for name, info in classes.items()
        if not name.startswith("_") and name not in known
    ]
    if extras:
        streaming_extras = [n for n, i in extras if "update" in i["methods"]]
        batch_only_extras = [
            n for n, i in extras if "update" not in i["methods"] and "compute" in i["statics"]
        ]
        parts = []
        if streaming_extras:
            parts.append(
                ", ".join(f"`{n}`" for n in streaming_extras)
                + " (streaming plus a static `compute()`)"
            )
        if batch_only_extras:
            parts.append(
                ", ".join(f"`{n}`" for n in batch_only_extras)
                + " (batch-only static `compute()`, no `update()`)"
            )
        lines.append(
            _wrap(
                "Also defined in `lrvx/indicators.js` but not in the shared registry: "
                + "; ".join(parts)
                + "."
            )
        )
        lines.append("")
    return "\n".join(lines)


def cpp_block(indicators: list[dict]) -> str:
    lines = [
        "Every indicator below is **one C++ class** with both a batch",
        "`compute()` method and streaming `update()` / `value()` / `ready()` /",
        "`reset()`. Same instance, two ways to use it:",
        "",
        "```cpp",
        '#include "lrvx/indicator/ema.h"',
        "lrvx::indicator::EMA ema(10);",
        "auto out = ema.compute(prices);            // batch",
        "for (auto v : stream) {",
        "    ema.update(v);",
        "    if (ema.ready()) {",
        "        std::cout << ema.value();          // streaming on same instance",
        "    }",
        "}",
        "```",
        "",
        "| Indicator | Header | Constructor | Kind |",
        "|---|---|---|---|",
    ]
    for ind in indicators:
        header = f"lrvx/indicator/{ind['snake']}.h"
        if ind["snake"] == "tema":
            header = "lrvx/indicator/dema.h"
        lines.append(
            f"| `{ind['class']}` | `<{header}>` | `{ind['class']}({ind['args']})` | {ind['kind']} |"
        )
    lines.append("")
    return "\n".join(lines)


def _codon_batch_cell(ind: dict, info: dict | None, functions: dict[str, str]) -> str:
    if info is None:
        return "not exposed in `lrvx/indicators.codon`"
    cells = []
    if "compute" in info["methods"]:
        cells.append(f"instance `compute({info['methods']['compute']})`")
    for name, params in info["statics"].items():
        if name.startswith("compute"):
            cells.append(f"static `{name}({params})`")
    if cells:
        return ", ".join(cells)
    snake = ind["snake"]
    if snake in functions:
        return f"none — free function `{snake}({functions[snake]})`"
    return "none"


def codon_block(indicators: list[dict]) -> str:
    """Codon module: streaming on every class, compute() only on some."""
    classes, functions = parse_codon_module()

    no_compute = [
        ind["class"]
        for ind in indicators
        if "compute" not in classes.get(ind["class"], {}).get("methods", {})
    ]
    statics = [
        f"`{ind['class']}.{name}({params})`"
        for ind in indicators
        for name, params in classes.get(ind["class"], {}).get("statics", {}).items()
        if name.startswith("compute")
    ]

    lines = [
        "Every indicator below is **one Codon class** with streaming `update()` / `value` /",
        "`ready` / `count` / `reset()`, one kernel step of the C++ class per update, and",
        "`close()` to release its C handle. A batch `compute()` is **not** on every class — the `Batch`",
        "column says what each class actually exposes. Classes and the batch free functions",
        "both come from `lrvx.indicators`.",
        "",
        "```codon",
        "from lrvx.indicators import EMA, CCI, cci",
        "",
        "ema = EMA(20)",
        "out = ema.compute(prices)                  # EMA has an instance compute()",
        "for v in stream:",
        "    ema.update(v)",
        "    if ema.ready: print(ema.value)         # streaming on the same instance",
        "",
        "c = CCI(20)                                # CCI has no compute()",
        "series = cci(highs, lows, closes, 20)      # batch via the free function",
        "```",
        "",
        "| Indicator | Constructor | Kind | Batch |",
        "|---|---|---|---|",
    ]
    for ind in indicators:
        info = classes.get(ind["class"])
        ctor_args = info["ctor"] if info and info["ctor"] else _registry_params(ind["args"])
        lines.append(
            f"| `{ind['class']}` | `{ind['class']}({ctor_args})` | {ind['kind']} "
            f"| {_codon_batch_cell(ind, info, functions)} |"
        )
    lines.append("")

    if no_compute:
        names = ", ".join(f"`{c}`" for c in no_compute)
        lines.append(
            _wrap(
                "Streaming only, with no `compute()` at all — call the matching free "
                f"function for batch: {names}."
            )
        )
        lines.append("")
    if len(statics) == 1:
        lines.append(_wrap(f"{statics[0]} is the only static batch method in the module."))
        lines.append("")
    elif statics:
        lines.append(_wrap("Static batch methods: " + ", ".join(statics) + "."))
        lines.append("")
    return "\n".join(lines)


def replace_block(path: Path, content: str) -> bool:
    if not path.exists():
        return False
    text = path.read_text()
    if START not in text or END not in text:
        return False
    # Match the entire block including blank lines around the marker, then
    # rewrite with a stable canonical form: blank line after START, blank
    # line before END.
    pattern = re.compile(
        rf"{re.escape(START)}.*?{re.escape(END)}", re.DOTALL
    )
    canonical = f"{START}\n\n{content}\n\n{END}"
    new = pattern.sub(canonical, text, count=1)
    if new != text:
        path.write_text(new)
        return True
    return False


def warn_missing(indicators: list[dict]) -> None:
    """Registry entries a binding source does not expose show up in the tables
    as 'not exposed'; say so loudly too, it is usually a binding bug."""
    codon_classes, _ = parse_codon_module()
    quickjs_classes = parse_quickjs_module()
    for label, names in (
        ("codon/lrvx/indicators.codon", set(codon_classes)),
        ("quickjs/lrvx/indicators.js", set(quickjs_classes)),
    ):
        missing = [ind["class"] for ind in indicators if ind["class"] not in names]
        if missing:
            print(
                f"  warn   {label} has no class for: {', '.join(missing)}",
                file=sys.stderr,
            )


def main() -> int:
    indicators = parse_registry()
    if not indicators:
        print("error: no indicators found in registry.def", file=sys.stderr)
        return 1

    warn_missing(indicators)

    targets = [
        (REPO / "docs" / "reference" / "python" / "indicators.md", py_block(indicators)),
        (REPO / "docs" / "reference" / "node" / "indicators.md", node_block(indicators)),
        (REPO / "docs" / "reference" / "quickjs" / "indicators.md", quickjs_block(indicators)),
        (REPO / "docs" / "reference" / "codon" / "indicators.md", codon_block(indicators)),
        # No docs/reference/cpp/ today; the C++ surface lives under
        # docs/reference/api/ — keep generating once added.
    ]

    changed_any = False
    for path, block in targets:
        if not path.exists():
            print(f"  skip   {path.relative_to(REPO)} (does not exist)")
            continue
        text = path.read_text()
        if START not in text:
            # Inject markers near the top so the user can re-run and the
            # auto-gen section will populate.
            inject = f"\n## Indicator catalog\n\n{START}\n\n{block}\n\n{END}\n"
            path.write_text(text.rstrip() + "\n" + inject)
            changed_any = True
            print(f"  inject {path.relative_to(REPO)}")
        else:
            if replace_block(path, block):
                changed_any = True
                print(f"  update {path.relative_to(REPO)}")
            else:
                print(f"  ok     {path.relative_to(REPO)}")

    if changed_any:
        print(f"\nUpdated {sum(1 for _ in targets)} files for {len(indicators)} indicators.")
    else:
        print(f"\nAll {len(targets)} doc files are already in sync with the {len(indicators)} registry entries.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
