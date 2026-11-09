"""validate_strategy — static-analysis sanity check on a lrvx strategy.

The check is intentionally **static** (no exec): AST-parse the source,
verify it imports `lrvx` (or a known alias), defines at least one
class/function that looks like a strategy hook (on_trade / on_bar /
on_book), and contains no obvious unsafe constructs. AI agents call
this before suggesting code; a passing check is necessary but not
sufficient for the strategy to actually work — `run_backtest` (future
tool) is the runtime check.
"""
from __future__ import annotations

import ast
from typing import Iterable


_FORBIDDEN_NAMES = {"eval", "exec", "compile", "__import__", "globals", "open"}
_KNOWN_HOOKS = {
    "on_trade", "on_bar", "on_book", "on_book_update", "on_touch", "on_start", "on_stop",
}


def _flatten_attrs(node: ast.AST) -> list[str]:
    out: list[str] = []
    for n in ast.walk(node):
        if isinstance(n, ast.Name):
            out.append(n.id)
        elif isinstance(n, ast.Attribute):
            out.append(n.attr)
    return out


def _imports_lrvx(tree: ast.Module) -> bool:
    for n in ast.walk(tree):
        if isinstance(n, ast.Import):
            for a in n.names:
                if a.name.split(".")[0] in {"lrvx", "lrvx", "lrvx_mcp"}:
                    return True
        elif isinstance(n, ast.ImportFrom):
            if n.module and n.module.split(".")[0] in {"lrvx", "lrvx", "lrvx_mcp"}:
                return True
    return False


def _hooks_defined(tree: ast.Module) -> set[str]:
    found: set[str] = set()
    for n in ast.walk(tree):
        if isinstance(n, ast.FunctionDef) and n.name in _KNOWN_HOOKS:
            found.add(n.name)
    return found


def _forbidden_calls(tree: ast.Module) -> Iterable[tuple[int, str]]:
    for n in ast.walk(tree):
        if isinstance(n, ast.Call):
            target = n.func
            if isinstance(target, ast.Name) and target.id in _FORBIDDEN_NAMES:
                yield n.lineno, target.id
            elif isinstance(target, ast.Attribute) and target.attr in _FORBIDDEN_NAMES:
                yield n.lineno, target.attr


def validate_strategy(code: str) -> str:
    findings: list[str] = []

    try:
        tree = ast.parse(code)
    except SyntaxError as e:
        return f"SyntaxError at line {e.lineno}, col {e.offset}: {e.msg}"

    if not _imports_lrvx(tree):
        findings.append(
            "WARN: no `import lrvx` (or `from lrvx import ...`) "
            "detected — strategies normally need bindings."
        )

    hooks = _hooks_defined(tree)
    if not hooks:
        findings.append(
            "WARN: no recognized hook (on_trade / on_bar / on_book / "
            "on_book_update / on_touch / on_start / on_stop) defined. Strategies "
            "need at least one entry point."
        )
    else:
        findings.append(f"hooks defined: {', '.join(sorted(hooks))}")

    forbidden = list(_forbidden_calls(tree))
    if forbidden:
        for lineno, name in forbidden:
            findings.append(
                f"DENY at line {lineno}: forbidden call `{name}()` — "
                "strategies must not eval/exec/compile/import dynamically."
            )

    n_classes = sum(1 for n in tree.body if isinstance(n, ast.ClassDef))
    n_funcs = sum(1 for n in tree.body if isinstance(n, ast.FunctionDef))
    findings.append(
        f"top-level: {n_classes} classes, {n_funcs} functions, "
        f"{len(code.splitlines())} lines."
    )

    has_blocking = any(line.startswith("DENY") for line in findings)
    status = "REJECT" if has_blocking else (
        "REVIEW" if any(line.startswith("WARN") for line in findings) else "OK"
    )

    return f"validate_strategy: {status}\n\n" + "\n".join(f"- {f}" for f in findings)
