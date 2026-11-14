"""Bundled-data builders for `lrvx-mcp`.

This module turns the canonical lrvx surface (IDL spec + per-binding
artifacts) into the structured JSON / SQLite snapshots that
``lrvx-mcp`` ships in its wheel for offline use. Three artifacts:

* ``ir.snapshot.json`` — minimal cross-language IR (functions /
  structs / enums / typedefs / function pointers / macro constants)
  with a versioned schema. The build is deterministic — order is
  fixed alphabetically so re-runs are byte-identical.
* ``binding_manifest.json`` — per-binding symbol inventory, joined
  by IDL group, so ``lookup_symbol`` / ``list_bindings`` MCP tools can
  resolve a name across languages.
* ``examples_index.json`` — index of runnable corpora under
  ``docs/examples/`` (path / language / topic / sha256), so
  ``get_example`` can return a topic-filtered list without scanning
  the docs tree at request time.

All three are pure-Python derivations from canonical sources — no
network access, no extra dependencies beyond what ``lrvx_codegen``
already needs (libclang for the IR extractor, PyYAML for the parity
manifest). The fourth artifact, the SQLite FTS5 docs index, has its
own builder in ``scripts/sync_mcp_data.py`` because it depends only
on the standard-library ``sqlite3`` module.
"""
from __future__ import annotations

import dataclasses
import hashlib
import json
import re
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple

from . import ir


SCHEMA_VERSION = 1


# ── IR snapshot ────────────────────────────────────────────────────────


def ir_to_snapshot(module: ir.Module) -> Dict[str, Any]:
    """Project an `ir.Module` into the bundled v1 schema.

    The schema strips emitter-internal fields (source locations are
    omitted; they belong in error messages, not in the bundle). All
    lists are sorted by name so the JSON dump is byte-stable.
    """
    fn_groups = module.functions_by_group()
    fn_to_group: Dict[str, str] = {}
    for grp, fns in fn_groups.items():
        for fn in fns:
            fn_to_group[fn.name] = grp

    functions = [
        {
            "name": fn.name,
            "return_type": fn.return_type,
            "params": [{"name": p.name, "type": p.type} for p in fn.params],
            "group": fn_to_group.get(fn.name, "_ungrouped"),
        }
        for fn in sorted(module.functions, key=lambda f: f.name)
    ]
    structs = [
        {
            "name": st.name,
            "fields": [
                {
                    "name": f.name,
                    "type": f.type,
                    **({"array_size": f.array_size} if f.array_size else {}),
                }
                for f in st.fields
            ],
        }
        for st in sorted(module.structs, key=lambda s: s.name)
    ]
    enums = [
        {
            "name": en.name,
            "values": [
                {"name": v.name, **({"value": v.value} if v.value is not None else {})}
                for v in en.values
            ],
        }
        for en in sorted(module.enums, key=lambda e: e.name)
    ]
    handles = [
        {
            "name": h.name,
            **({"alias_of": h.alias_of} if h.alias_of else {"target": "void*"}),
        }
        for h in sorted(module.handles, key=lambda x: x.name)
    ]
    function_pointers = [
        {
            "name": fp.name,
            "return_type": fp.return_type,
            "params": [{"name": p.name, "type": p.type} for p in fp.params],
        }
        for fp in sorted(module.function_pointers, key=lambda x: x.name)
    ]
    macros = [
        {"name": m.name, "value": m.value, "group": m.group}
        for m in sorted(module.macros, key=lambda x: x.name)
    ]
    return {
        "version": SCHEMA_VERSION,
        "functions": functions,
        "structs": structs,
        "enums": enums,
        "typedefs": handles,
        "function_pointers": function_pointers,
        "macros": macros,
    }


# ── Per-binding scanners ──────────────────────────────────────────────


_PYI_CLASS = re.compile(r"^class\s+([A-Za-z_][A-Za-z_0-9]*)")
_PYI_FUNC = re.compile(r"^def\s+([A-Za-z_][A-Za-z_0-9]*)")
_DTS_CLASS = re.compile(r"^export\s+(?:class|interface)\s+([A-Za-z_][A-Za-z_0-9]*)")
_DTS_FUNC = re.compile(r"^export\s+function\s+([A-Za-z_][A-Za-z_0-9]*)")
_CODON_GROUP = re.compile(r"^#\s*──\s*([A-Za-z_0-9]+)\s*──\s*$")
_CODON_IMPORT = re.compile(r"^from\s+C\s+import\s+([A-Za-z_][A-Za-z_0-9]*)")
# Methods, so a lookup can answer "what is this called in my language" rather
# than only "does this class exist".
_PYI_METHOD = re.compile(r"^\s{4}def\s+([A-Za-z_][A-Za-z_0-9]*)")
_DTS_MEMBER = re.compile(r"^\s{2}(?:readonly\s+)?(?:static\s+)?([A-Za-z_][A-Za-z_0-9]*)\s*[(?:]")
# The real Codon API: what a Codon user types, not the FFI declarations.
_CODON_CLASS = re.compile(r"^class\s+([A-Za-z_][A-Za-z_0-9]*)")
_CODON_DEF = re.compile(r"^def\s+([A-Za-z_][A-Za-z_0-9]*)")
_CODON_METHOD = re.compile(r"^\s{4}def\s+([A-Za-z_][A-Za-z_0-9]*)")
# C++ is the engine itself; every other surface wraps it.
_CPP_TYPE = re.compile(r"^\s*(?:class|struct)\s+([A-Z][A-Za-z_0-9]*)\s*(?:final\s*)?(?::|\{|$)")
_CPP_METHOD = re.compile(
    r"^\s{2}(?:\[\[[^\]]*\]\]\s*)?(?:virtual\s+|static\s+|explicit\s+|constexpr\s+|inline\s+)*"
    r"[A-Za-z_][\w:<>,&*\s]*?\b([a-z][A-Za-z_0-9]*)\s*\(")
# QuickJS: the globals the addon registers, and the lrvx.* prelude built on them.
_QJS_GLOBAL = re.compile(r'addGlobalFunc\(\s*ctx\s*,\s*"([^"]+)"')
_QJS_PRELUDE_FN = re.compile(r"^\s{6}([a-zA-Z_][A-Za-z_0-9]*)\s*:\s*function")
_QJS_PRELUDE_CLASS = re.compile(r"^\s{4}class\s+([A-Za-z_][A-Za-z_0-9]*)")
# A top-level class of the embedded JS standard library (quickjs/lrvx/*.js).
# A leading underscore marks an internal base, which is not a binding class.
_QJS_STDLIB_CLASS = re.compile(r"^class\s+([A-Za-z][A-Za-z_0-9]*)")


def scan_pyi(text: str) -> Tuple[List[str], List[str]]:
    classes: List[str] = []
    funcs: List[str] = []
    for line in text.splitlines():
        m = _PYI_CLASS.match(line)
        if m:
            classes.append(m.group(1))
            continue
        m = _PYI_FUNC.match(line)
        if m:
            funcs.append(m.group(1))
    return sorted(set(classes)), sorted(set(funcs))


def scan_dts(text: str) -> Tuple[List[str], List[str]]:
    classes: List[str] = []
    funcs: List[str] = []
    for line in text.splitlines():
        m = _DTS_CLASS.match(line)
        if m:
            classes.append(m.group(1))
            continue
        m = _DTS_FUNC.match(line)
        if m:
            funcs.append(m.group(1))
    return sorted(set(classes)), sorted(set(funcs))


def scan_pyi_methods(text: str) -> List[Tuple[str, str]]:
    """(class, method) pairs from the .pyi. Class-only inventories cannot
    answer the question an agent actually asks -- what is this *method* called
    in my language -- so `run_csv` used to resolve nowhere."""
    out: List[Tuple[str, str]] = []
    current = None
    for line in text.splitlines():
        m = _PYI_CLASS.match(line)
        if m:
            current = m.group(1)
            continue
        if current:
            m = _PYI_METHOD.match(line)
            if m and not m.group(1).startswith("__"):
                out.append((current, m.group(1)))
    return sorted(set(out))


def scan_dts_members(text: str) -> List[Tuple[str, str]]:
    """(class, member) pairs from index.d.ts."""
    out: List[Tuple[str, str]] = []
    current = None
    for line in text.splitlines():
        m = _DTS_CLASS.match(line)
        if m:
            current = m.group(1)
            continue
        if line.startswith("}"):
            current = None
            continue
        if current:
            m = _DTS_MEMBER.match(line)
            if m and m.group(1) not in {"constructor", "new"}:
                out.append((current, m.group(1)))
    return sorted(set(out))


def scan_codon_api(sources: Dict[str, str]) -> Tuple[List[str], List[str], List[Tuple[str, str]]]:
    """The Codon surface a user types: (classes, module functions, methods).

    The manifest used to inventory Codon from the generated FFI golden, so it
    listed `lrvx_*` C names -- a Codon user writing `BacktestRunner` found
    nothing. These come from codon/lrvx/*.codon instead.
    """
    classes: List[str] = []
    funcs: List[str] = []
    methods: List[Tuple[str, str]] = []
    for _, text in sorted(sources.items()):
        current = None
        for line in text.splitlines():
            m = _CODON_CLASS.match(line)
            if m:
                current = m.group(1)
                classes.append(current)
                continue
            m = _CODON_DEF.match(line)
            if m:
                current = None
                if not m.group(1).startswith("_"):
                    funcs.append(m.group(1))
                continue
            if current:
                m = _CODON_METHOD.match(line)
                if m and not m.group(1).startswith("__"):
                    methods.append((current, m.group(1)))
    return sorted(set(classes)), sorted(set(funcs)), sorted(set(methods))


def scan_cpp(headers: Dict[str, str]) -> Tuple[List[Tuple[str, str]], List[Tuple[str, str]]]:
    """The C++ engine surface: (types with header, (type, method) pairs).

    C++ was absent from the manifest entirely, so `lookup_symbol` reported
    "Tried C-API, Python, Node, Codon" and missed the surface every other
    binding wraps -- `SimulatedExecutor` resolved to its Python and Node
    wrappers while the class itself was invisible.

    A class/struct declared at column 0 is a top-level engine type and
    becomes the method-attribution owner (matching `_CPP_METHOD`, which only
    recognizes members indented exactly two spaces -- direct members of a
    column-0 class). A class/struct declared with any leading indentation is
    a private nested helper (RAII guards, hash functors, ...) living inside
    the type whose body is already open; it must not become the new owner,
    or every method the enclosing type declares afterwards -- textually,
    not just the ones inside the helper -- gets attributed to the helper
    instead. It is also not part of the public surface this manifest
    indexes, so it is not recorded as its own type.
    """
    types: List[Tuple[str, str]] = []
    methods: List[Tuple[str, str]] = []
    for path, text in sorted(headers.items()):
        current = None
        for line in text.splitlines():
            m = _CPP_TYPE.match(line)
            if m:
                if not line[:1].isspace():
                    current = m.group(1)
                    types.append((current, path))
                continue
            if current:
                m = _CPP_METHOD.match(line)
                if m and m.group(1) not in {"if", "for", "while", "return", "switch"}:
                    methods.append((current, m.group(1)))
    return sorted(set(types)), sorted(set(methods))


def scan_quickjs(bindings_text: str, prelude_text: str,
                 stdlib_texts: Iterable[str] = ()) -> Tuple[List[str], List[str], List[str]]:
    """(registered globals, prelude classes, prelude functions).

    QuickJS recorded zero symbols, so list_bindings answered "No QuickJS
    exports recorded yet" for a surface with ~500 registered globals.

    `stdlib_texts` are the files of quickjs/lrvx/. They are compiled into the
    engine and evaluated at global scope before a script loads, so their
    top-level classes (the indicators, the exit rules, History) are QuickJS
    classes as much as the prelude's.
    """
    globals_ = sorted(set(_QJS_GLOBAL.findall(bindings_text)))
    classes: List[str] = []
    funcs: List[str] = []
    for line in prelude_text.splitlines():
        m = _QJS_PRELUDE_CLASS.match(line)
        if m:
            classes.append(m.group(1))
            continue
        m = _QJS_PRELUDE_FN.match(line)
        if m:
            funcs.append(m.group(1))
    for text in stdlib_texts:
        for line in text.splitlines():
            m = _QJS_STDLIB_CLASS.match(line)
            if m:
                classes.append(m.group(1))
    return globals_, sorted(set(classes)), sorted(set(funcs))


def scan_codon(text: str) -> Tuple[List[str], List[str]]:
    """Return (groups_present, c_function_imports) found in the codon golden."""
    groups: List[str] = []
    imports: List[str] = []
    for line in text.splitlines():
        m = _CODON_GROUP.match(line)
        if m:
            groups.append(m.group(1))
            continue
        m = _CODON_IMPORT.match(line.lstrip())
        if m:
            imports.append(m.group(1))
    return sorted(set(groups)), sorted(set(imports))


# ── Binding manifest ──────────────────────────────────────────────────


def build_binding_manifest(
    *,
    module: ir.Module,
    parity_yaml: Dict[str, Any],
    pyi_text: str,
    dts_text: str,
    codon_text: str,
    codon_sources: Dict[str, str] | None = None,
    cpp_headers: Dict[str, str] | None = None,
    quickjs_bindings_text: str = "",
    quickjs_prelude_text: str = "",
    quickjs_stdlib_texts: Iterable[str] = (),
) -> Dict[str, Any]:
    """Compose the cross-binding symbol manifest.

    The structure has two views:

    * ``groups`` — keyed by IDL group; for each group, lists the
      C-API functions plus the per-binding classes/functions known to
      satisfy that group (sourced from ``binding_parity.yaml``).
    * ``bindings`` — keyed by binding name; for each, a flat
      enumeration of the symbols it exports (with the IDL group they
      belong to when known).
    """
    cfg_groups = parity_yaml.get("groups", {}) or {}
    fn_groups = module.functions_by_group()
    pyi_classes, pyi_funcs = scan_pyi(pyi_text)
    dts_classes, dts_funcs = scan_dts(dts_text)
    codon_groups, codon_imports = scan_codon(codon_text)

    groups_out: Dict[str, Any] = {}
    for grp_name in sorted(cfg_groups.keys()):
        entry = cfg_groups[grp_name] or {}
        capi_fns = sorted({f.name for f in fn_groups.get(grp_name, [])})

        def _binding(key: str) -> Dict[str, Any]:
            b = entry.get(key) or {}
            status = b.get("status", "missing_yaml")
            declared = b.get("functions") or []
            # The QuickJS entry is a C-function-to-global map; the symbol a
            # strategy calls is the global, so that is what the manifest
            # lists -- the same content the list form carried before.
            functions = (sorted(set(declared.values()))
                         if isinstance(declared, dict) else list(declared))
            return {
                "status": status,
                "classes": list(b.get("classes") or []),
                "functions": functions,
                **({"reason": b["reason"]} if "reason" in b else {}),
            }

        codon_entry = entry.get("codon") or {}
        groups_out[grp_name] = {
            "capi_functions": capi_fns,
            "pybind11": _binding("pybind11"),
            "napi": _binding("napi"),
            "codon": {
                "status": codon_entry.get("status", "missing_yaml"),
                "present": grp_name in codon_groups,
                **({"reason": codon_entry["reason"]} if "reason" in codon_entry else {}),
            },
            "quickjs": _binding("quickjs"),
        }

    # Reverse-index: per-binding symbol enumeration.
    capi_symbols = []
    for fn in sorted(module.functions, key=lambda f: f.name):
        capi_symbols.append({
            "name": fn.name,
            "kind": "function",
            "group": fn.annotations.get("group", "_ungrouped"),
        })
    for st in sorted(module.structs, key=lambda s: s.name):
        capi_symbols.append({"name": st.name, "kind": "struct"})
    for en in sorted(module.enums, key=lambda e: e.name):
        capi_symbols.append({"name": en.name, "kind": "enum"})
    for h in sorted(module.handles, key=lambda x: x.name):
        capi_symbols.append({"name": h.name, "kind": "handle"})

    pybind_symbols = (
        [{"name": c, "kind": "class"} for c in pyi_classes]
        + [{"name": f, "kind": "function"} for f in pyi_funcs]
        + [{"name": m, "kind": "method", "parent": c}
           for c, m in scan_pyi_methods(pyi_text)]
    )
    napi_symbols = (
        [{"name": c, "kind": "class"} for c in dts_classes]
        + [{"name": f, "kind": "function"} for f in dts_funcs]
        + [{"name": m, "kind": "method", "parent": c}
           for c, m in scan_dts_members(dts_text)]
    )

    # Codon: the API a user writes. The FFI imports from the golden are kept
    # under a separate kind so the C names stay findable without being the
    # only thing Codon appears to expose.
    cd_classes, cd_funcs, cd_methods = scan_codon_api(codon_sources or {})
    codon_symbols = (
        [{"name": c, "kind": "class"} for c in cd_classes]
        + [{"name": f, "kind": "function"} for f in cd_funcs]
        + [{"name": m, "kind": "method", "parent": c} for c, m in cd_methods]
        + [{"name": n, "kind": "c_import"} for n in codon_imports]
    )

    cpp_types, cpp_methods = scan_cpp(cpp_headers or {})
    cpp_symbols = (
        [{"name": n, "kind": "type", "header": h} for n, h in cpp_types]
        + [{"name": m, "kind": "method", "parent": c} for c, m in cpp_methods]
    )

    qjs_globals, qjs_classes, qjs_funcs = scan_quickjs(
        quickjs_bindings_text, quickjs_prelude_text, quickjs_stdlib_texts)
    quickjs_symbols = (
        [{"name": c, "kind": "class"} for c in qjs_classes]
        + [{"name": f, "kind": "function"} for f in qjs_funcs]
        + [{"name": g, "kind": "global"} for g in qjs_globals]
    )

    return {
        "version": SCHEMA_VERSION,
        "groups": groups_out,
        "bindings": {
            "capi": {"symbols": capi_symbols},
            "pybind11": {"symbols": pybind_symbols},
            "napi": {"symbols": napi_symbols},
            "codon": {"symbols": codon_symbols, "groups_present": codon_groups},
            "quickjs": {"symbols": quickjs_symbols},
            "cpp": {"symbols": cpp_symbols},
        },
    }


# ── Examples index ────────────────────────────────────────────────────


_LANG_BY_EXT = {
    ".py": "python",
    ".js": "node",
    ".ts": "node",
    ".mjs": "node",
    ".codon": "codon",
    ".cpp": "cpp",
    ".cc": "cpp",
    ".cxx": "cpp",
}


def _topic_for(name: str) -> str:
    """Heuristic topic inference from filename. The taxonomy mirrors the
    ``get_example`` tool's accepted topics. Order matters —
    ``backtest`` wins over ``connector`` for files like
    ``*_backtest_vs_live.*`` where both keywords appear.
    """
    n = name.lower()
    if "indicator" in n:
        return "indicator"
    if "risk" in n:
        return "risk"
    if "event" in n:
        return "event-handler"
    if "backtest" in n:
        return "backtest"
    if "ccxt" in n:
        return "connector"
    if "strategy" in n or "multi_symbol" in n or "sma" in n:
        return "strategy"
    return "strategy"


def build_examples_index(examples_root: Path) -> Dict[str, Any]:
    if not examples_root.is_dir():
        return {"version": SCHEMA_VERSION, "examples": []}
    out: List[Dict[str, Any]] = []
    for path in sorted(examples_root.rglob("*")):
        if not path.is_file():
            continue
        ext = path.suffix.lower()
        if ext not in _LANG_BY_EXT:
            continue
        rel = path.relative_to(examples_root.parent.parent)
        text = path.read_bytes()
        out.append({
            "path": str(rel),
            "language": _LANG_BY_EXT[ext],
            "topic": _topic_for(path.name),
            "size_bytes": len(text),
            "sha256": hashlib.sha256(text).hexdigest(),
        })
    return {"version": SCHEMA_VERSION, "examples": out}


# ── Bundle helpers ────────────────────────────────────────────────────


def dumps_canonical(payload: Dict[str, Any]) -> str:
    """JSON dump with stable key order + 2-space indent. The output is
    byte-identical for byte-identical inputs, so the ``--check`` mode
    in ``sync_mcp_data.py`` can rely on plain bytes-equality."""
    return json.dumps(payload, indent=2, sort_keys=True) + "\n"
