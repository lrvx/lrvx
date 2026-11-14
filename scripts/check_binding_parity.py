#!/usr/bin/env python3
"""Cross-binding parity gate.

Reads the IDL spec (`include/lrvx/capi/lrvx_capi_spec.hpp`) and verifies, for
every user-facing C ABI group, that each language binding -- pybind11 Python,
NAPI Node, Codon, QuickJS -- can actually reach the functions in it.

The gate used to promise that and not deliver it. It matched *class names* in
`.pyi` / `.d.ts`, so one name satisfied a whole group no matter how many
functions the group gained; it checked Codon against
`tools/codegen/golden/lrvx_capi.codon`, a file generated from the same IDL it
had just read, which cannot disagree with it; and it skipped QuickJS entirely
for any group that carried no `quickjs` entry.

What is checked now, per group and per binding:

    pybind11  every C function of the group has a call site under `python/`
    napi      every C function of the group has a call site under `node/src/`
    codon     every C function of the group is `from C import`-ed by a module
              under `codon/lrvx/` -- the code a strategy actually links, not
              the generated golden
    quickjs   every C function of the group is mapped, in this file, to the
              global name a strategy calls, and that global is registered
              with `addGlobalFunc` in `src/quickjs/js_bindings.cpp`

Class declarations are still checked on top of that: the `classes:` list of a
`required` entry must appear in `.pyi` / `.d.ts` / the QuickJS prelude, and a
`functions:` list under pybind11 / napi still names top-level binding
functions in those stubs. The QuickJS layer is read through
`lrvx_codegen.manifest.scan_quickjs`, the extractor `scripts/sync_mcp_data.py`
uses for the MCP `list_bindings` / `lookup_symbol` tools, so the two never
drift apart on what counts as a registered QuickJS class.

Configuration lives in `tools/codegen/binding_parity.yaml`. Each group
declared in IDL must be listed there with a per-binding status:

    required          the binding exposes this group; classes/functions as
                      declared, and every C function reachable or allowlisted
    not_applicable    this group is internal / never exposed to this binding
    allowlist         the whole group is a known gap, with a `reason`

A binding that is missing an individual wrapper carries the function in the
top-level `allowlist_functions:` section, under that binding, with a one-line
reason -- rather than the group being demoted to `allowlist`. That section is
the inventory of what each binding does not reach; see
docs/contributors/parity-gate.md.

The script fails CI when:
    - A group exists in IDL but isn't in the YAML
    - A `required` binding is missing a declared class or function
    - A C function is reachable from neither the binding nor the allowlist
    - A QuickJS mapping names a global that is not registered
    - An allowlist entry is missing a reason

Run as:
    python3 scripts/check_binding_parity.py

`--verbose` prints every group, `--no-require-quickjs` drops the demand for a
`quickjs` entry on every group, and `--root` points the gate at a tree other
than the one it lives in (the gate's own tests build a miniature repository
and point it there).
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

try:
    import yaml
except ImportError:
    print("ERROR: PyYAML required. pip install pyyaml", file=sys.stderr)
    sys.exit(2)

SCRIPT_ROOT = Path(__file__).resolve().parent.parent

# The codegen package always comes from the checkout this script lives in:
# a tree passed with --root carries binding sources, not the toolchain.
sys.path.insert(0, str(SCRIPT_ROOT / "tools" / "codegen"))
from lrvx_codegen import manifest as codegen_manifest  # noqa: E402

BINDINGS = ("pybind11", "napi", "codon", "quickjs")

# Sources scanned for a call site, per binding. C++ trees are scanned with
# comments stripped, so a function named in a comment does not count as
# reachable.
CPP_SUFFIXES = (".h", ".hpp", ".hh", ".cpp", ".cc", ".cxx", ".inl")


@dataclass
class Paths:
    root: Path

    @property
    def idl(self) -> Path:
        return self.root / "include" / "lrvx" / "capi" / "lrvx_capi_spec.hpp"

    @property
    def config(self) -> Path:
        return self.root / "tools" / "codegen" / "binding_parity.yaml"

    @property
    def pyi(self) -> Path:
        return self.root / "python" / "lrvx" / "_lrvx" / "__init__.pyi"

    @property
    def dts(self) -> Path:
        return self.root / "node" / "index.d.ts"

    @property
    def pybind_sources(self) -> Path:
        return self.root / "python"

    @property
    def napi_sources(self) -> Path:
        return self.root / "node" / "src"

    @property
    def codon_sources(self) -> Path:
        return self.root / "codon" / "lrvx"

    @property
    def quickjs_bindings(self) -> Path:
        return self.root / "src" / "quickjs" / "js_bindings.cpp"

    @property
    def quickjs_prelude(self) -> Path:
        return self.root / "src" / "quickjs" / "js_strategy.cpp"

    @property
    def quickjs_stdlib(self) -> Path:
        return self.root / "quickjs" / "lrvx"


# ── IDL parsing ────────────────────────────────────────────────────────


@dataclass
class IdlGroup:
    name: str
    functions: list[str]


def parse_idl_groups(text: str) -> list[IdlGroup]:
    """Walk the IDL spec, gathering function declarations per group.

    Each LRVX_EXPORT(group = "X") attaches to the next function. Functions
    span multiple lines (parameters often broken). We track the current
    group via the most recently seen LRVX_EXPORT, then capture the
    function name from the line that follows."""
    groups: dict[str, list[str]] = {}
    current_group: str | None = None
    fn_pattern = re.compile(r"\b(lrvx_[a-z_0-9]+)\s*\(")
    group_pattern = re.compile(r'group\s*=\s*"([^"]+)"')
    for line in text.splitlines():
        s = line.strip()
        if "LRVX_EXPORT" in s:
            m = group_pattern.search(s)
            if m:
                current_group = m.group(1)
            continue
        if current_group is not None:
            m = fn_pattern.search(s)
            if m:
                groups.setdefault(current_group, []).append(m.group(1))
                current_group = None  # consumed
    return [IdlGroup(name=k, functions=sorted(set(v))) for k, v in sorted(groups.items())]


# ── Binding scanners ───────────────────────────────────────────────────


def scan_pyi(path: Path) -> tuple[set[str], set[str]]:
    """Return (classes, top-level functions) declared in the .pyi file."""
    if not path.exists():
        return set(), set()
    classes: set[str] = set()
    funcs: set[str] = set()
    for line in path.read_text().splitlines():
        m = re.match(r"^class\s+([A-Za-z_][A-Za-z_0-9]*)", line)
        if m:
            classes.add(m.group(1))
            continue
        m = re.match(r"^def\s+([A-Za-z_][A-Za-z_0-9]*)", line)
        if m:
            funcs.add(m.group(1))
    return classes, funcs


def scan_dts(path: Path) -> tuple[set[str], set[str]]:
    """Return (classes-and-interfaces, top-level functions) declared in
    the .d.ts file. NAPI hooks are typically modelled as `interface` (a
    plain object with named method properties) rather than `class`, so
    we accept both for the parity gate."""
    if not path.exists():
        return set(), set()
    classes, funcs = set(), set()
    for line in path.read_text().splitlines():
        m = re.match(r"^export\s+(class|interface)\s+([A-Za-z_][A-Za-z_0-9]*)", line)
        if m:
            classes.add(m.group(2))
            continue
        m = re.match(r"^export\s+function\s+([A-Za-z_][A-Za-z_0-9]*)", line)
        if m:
            funcs.add(m.group(1))
    return classes, funcs


_BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)
_LINE_COMMENT = re.compile(r"//[^\n]*")
_C_FUNCTION = re.compile(r"\blrvx_[a-z_0-9]+")


def scan_c_call_sites(tree: Path) -> set[str]:
    """C API functions named in the C/C++ sources under `tree`.

    Comments are stripped first: a function that only appears in a comment
    is not reachable from the binding, and a gate that accepted one would
    be back to matching text instead of code.
    """
    found: set[str] = set()
    if not tree.is_dir():
        return found
    for path in sorted(tree.rglob("*")):
        if not path.is_file() or path.suffix not in CPP_SUFFIXES:
            continue
        text = path.read_text(encoding="utf-8", errors="ignore")
        text = _BLOCK_COMMENT.sub(" ", text)
        text = _LINE_COMMENT.sub(" ", text)
        found |= set(_C_FUNCTION.findall(text))
    return found


def scan_codon_imports(tree: Path) -> set[str]:
    """C API functions imported by the shipped Codon modules.

    `from C import lrvx_x(...)` is the only way Codon can call into the C
    ABI, so it is what "Codon reaches this function" means. Read from
    `codon/lrvx/`, deliberately not from the IDL-generated golden: the
    golden is generated from the same spec this gate parses, so a check
    against it can never fail.
    """
    found: set[str] = set()
    if not tree.is_dir():
        return found
    for path in sorted(tree.rglob("*.codon")):
        _groups, imports = codegen_manifest.scan_codon(
            path.read_text(encoding="utf-8", errors="ignore"))
        found |= set(imports)
    return found


def scan_quickjs_globals(path: Path) -> set[str]:
    """Every global name registered with addGlobalFunc in the QuickJS layer.

    Only the string literal is taken, not the wrapper identifier: ~21
    registrations pass an inline lambda and have no named wrapper to capture.
    The name is what a strategy calls, so the name is what parity means here.

    Names are not derivable from the C API function names -- the `__` + name
    convention has real exceptions (`__lrvx_vprofile_create` wraps
    `lrvx_volume_profile_create`) -- which is why the manifest maps each C
    function to its global explicitly rather than the gate guessing them.
    """
    if not path.exists():
        return set()
    text = path.read_text(encoding="utf-8")
    return set(re.findall(r'addGlobalFunc\(\s*ctx\s*,\s*"([^"]+)"', text))


def scan_quickjs_classes(bindings_path: Path, prelude_path: Path,
                         stdlib_dir: Path | None = None) -> set[str]:
    """Classes declared in the QuickJS embedded JS standard library.

    Delegates to `lrvx_codegen.manifest.scan_quickjs`, the extractor
    `scripts/sync_mcp_data.py` already uses to build the `binding_manifest`
    MCP tools query -- reusing it instead of a second regex kept in sync by
    hand. It reads the raw-string JS literal in `js_strategy.cpp`
    (`quickjs_prelude`), not `js_bindings.cpp`, since that is where the
    classes -- as opposed to the C++ registration calls -- are written, and
    the top-level classes of the files under quickjs/lrvx/ (`stdlib_dir`),
    which the engine evaluates before a script the same way.
    """
    if not bindings_path.exists() or not prelude_path.exists():
        return set()
    stdlib = sorted(stdlib_dir.glob("*.js")) if stdlib_dir and stdlib_dir.is_dir() else []
    _globals, classes, _funcs = codegen_manifest.scan_quickjs(
        bindings_path.read_text(encoding="utf-8", errors="ignore"),
        prelude_path.read_text(encoding="utf-8", errors="ignore"),
        [p.read_text(encoding="utf-8", errors="ignore") for p in stdlib])
    return set(classes)


@dataclass
class BindingSurface:
    """What each binding exposes, as read off the tree under test."""

    classes: dict[str, set[str]] = field(default_factory=dict)
    stub_functions: dict[str, set[str]] = field(default_factory=dict)
    reachable: dict[str, set[str]] = field(default_factory=dict)


def read_surface(paths: Paths) -> BindingSurface:
    pyi_classes, pyi_funcs = scan_pyi(paths.pyi)
    dts_classes, dts_funcs = scan_dts(paths.dts)
    return BindingSurface(
        classes={
            "pybind11": pyi_classes,
            "napi": dts_classes,
            "codon": set(),
            "quickjs": scan_quickjs_classes(paths.quickjs_bindings,
                                            paths.quickjs_prelude,
                                            paths.quickjs_stdlib),
        },
        stub_functions={
            "pybind11": pyi_funcs,
            "napi": dts_funcs,
            "codon": set(),
            "quickjs": set(),
        },
        reachable={
            "pybind11": scan_c_call_sites(paths.pybind_sources),
            "napi": scan_c_call_sites(paths.napi_sources),
            "codon": scan_codon_imports(paths.codon_sources),
            "quickjs": scan_quickjs_globals(paths.quickjs_bindings),
        },
    )


# ── Verification ───────────────────────────────────────────────────────


@dataclass
class GroupReport:
    group: str
    binding: str
    status: str  # ok | missing_yaml | missing_in_binding | allowlist_no_reason
    detail: str = ""


def _allowlisted(allowlist: dict, fn: str) -> tuple[bool, str]:
    """Is `fn` in this binding's allowlist, and with what reason? An entry
    with a blank reason is not an allowlist entry: the reason is the point."""
    if not isinstance(allowlist, dict) or fn not in allowlist:
        return False, ""
    return True, str(allowlist.get(fn) or "").strip()


def verify_functions(group: str, binding: str, expect: dict,
                     idl_functions: list[str],
                     reachable: set[str],
                     allowlist: dict) -> list[GroupReport]:
    """Every C function of the group is reachable from this binding, mapped
    to a registered QuickJS global, or allowlisted with a reason."""
    missing: list[str] = []
    unregistered: list[str] = []
    no_reason: list[str] = []
    mapping = expect.get("functions") if binding == "quickjs" else None
    if not isinstance(mapping, dict):
        mapping = {}

    for fn in idl_functions:
        if binding == "quickjs":
            if fn in mapping:
                global_name = str(mapping[fn])
                if global_name not in reachable:
                    unregistered.append(f"{fn} -> {global_name}")
                continue
        elif fn in reachable:
            continue
        listed, reason = _allowlisted(allowlist, fn)
        if not listed:
            missing.append(fn)
        elif not reason:
            no_reason.append(fn)

    reports: list[GroupReport] = []
    if missing:
        reports.append(GroupReport(
            group, binding, "missing_in_binding",
            f"no wrapper and no allowlist entry for {sorted(missing)}"))
    if unregistered:
        reports.append(GroupReport(
            group, binding, "missing_in_binding",
            f"mapped to a global that is not registered: {sorted(unregistered)}"))
    if no_reason:
        reports.append(GroupReport(
            group, binding, "allowlist_no_reason",
            f"allowlist_functions entries without a reason: {sorted(no_reason)}"))
    return reports


def verify_binding(group: str, binding: str, expect: dict,
                   idl_functions: list[str],
                   surface: BindingSurface,
                   allowlist: dict) -> list[GroupReport]:
    status = expect.get("status")
    if status == "not_applicable":
        return [GroupReport(group, binding, "ok", "n/a")]
    if status == "allowlist":
        reason = (expect.get("reason") or "").strip()
        if not reason:
            return [GroupReport(group, binding, "allowlist_no_reason",
                                "allowlist entry must include a `reason`")]
        return [GroupReport(group, binding, "ok", f"allowlist: {reason}")]
    if status != "required":
        return [GroupReport(group, binding, "missing_yaml",
                            f"unknown status `{status}`; expected required/not_applicable/allowlist")]

    reports: list[GroupReport] = []
    expected_classes = expect.get("classes") or []
    missing_c = [c for c in expected_classes if c not in surface.classes[binding]]

    # `functions:` under pybind11 / napi names top-level functions in the
    # stub; under quickjs it is the C-function-to-global map, checked by
    # verify_functions.
    expected_funcs = []
    if binding in ("pybind11", "napi"):
        declared = expect.get("functions") or []
        if isinstance(declared, list):
            expected_funcs = declared
    missing_f = [f for f in expected_funcs
                 if f not in surface.stub_functions[binding]]

    if missing_c or missing_f:
        bits = []
        if missing_c:
            bits.append(f"classes={missing_c}")
        if missing_f:
            bits.append(f"functions={missing_f}")
        reports.append(GroupReport(group, binding, "missing_in_binding",
                                   "missing " + ", ".join(bits)))

    reports += verify_functions(group, binding, expect, idl_functions,
                                surface.reachable[binding], allowlist)
    if not reports:
        detail = f"{len(idl_functions)} C functions"
        allowed = len([f for f in idl_functions if f in allowlist])
        if allowed:
            detail += f", {allowed} allowlisted"
        if expected_classes:
            detail += f", classes={expected_classes}"
        reports.append(GroupReport(group, binding, "ok", detail))
    return reports


# ── Main ──────────────────────────────────────────────────────────────


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--verbose", action="store_true",
                    help="print every group, not only failures")
    ap.add_argument("--require-quickjs", action=argparse.BooleanOptionalAction,
                    default=True,
                    help="fail on a group with no `quickjs` entry (default: on)")
    ap.add_argument("--root", default=str(SCRIPT_ROOT),
                    help="repository to check (default: the one this script lives in)")
    args = ap.parse_args(argv)

    paths = Paths(Path(args.root).resolve())

    if not paths.idl.exists():
        print(f"ERROR: IDL spec not found: {paths.idl}", file=sys.stderr)
        return 2
    if not paths.config.exists():
        print(f"ERROR: parity config not found: {paths.config}", file=sys.stderr)
        return 2

    idl_groups = parse_idl_groups(paths.idl.read_text())
    idl_names = {g.name for g in idl_groups}
    functions_by_group = {g.name: g.functions for g in idl_groups}

    config = yaml.safe_load(paths.config.read_text()) or {}
    cfg_groups = config.get("groups", {})

    allowlists = config.get("allowlist_functions") or {}

    surface = read_surface(paths)
    if not surface.reachable["quickjs"]:
        print(f"ERROR: no QuickJS registrations found in {paths.quickjs_bindings}",
              file=sys.stderr)
        return 2
    if not surface.classes["quickjs"]:
        print(f"ERROR: no QuickJS classes found in {paths.quickjs_prelude}",
              file=sys.stderr)
        return 2

    reports: list[GroupReport] = []

    # 1. Every IDL group must appear in the config.
    for g in sorted(idl_names - set(cfg_groups.keys())):
        reports.append(GroupReport(g, "config", "missing_yaml",
                                   f"add an entry to {paths.config.name}"))

    # 2. Config entries for groups that don't exist in IDL.
    for g in sorted(set(cfg_groups.keys()) - idl_names):
        reports.append(GroupReport(g, "config", "missing_in_binding",
                                   f"group `{g}` not found in IDL spec; remove from yaml"))

    # 3. Per-group, per-binding verification.
    for group_name in sorted(idl_names & set(cfg_groups.keys())):
        entry = cfg_groups[group_name] or {}
        functions = functions_by_group.get(group_name, [])
        for binding in BINDINGS:
            if binding not in entry:
                if binding == "quickjs" and not args.require_quickjs:
                    continue
                reports.append(GroupReport(
                    group_name, binding, "missing_yaml",
                    f"no `{binding}` entry in {paths.config.name}"))
                continue
            reports += verify_binding(group_name, binding, entry[binding] or {},
                                      functions, surface,
                                      allowlists.get(binding) or {})

    # ── Output ─────────────────────────────────────────────────────────

    failures = [r for r in reports if r.status != "ok"]
    if args.verbose or failures:
        print("Cross-binding parity report")
        print("─" * 60)
        if args.verbose:
            for r in reports:
                tag = "OK   " if r.status == "ok" else "FAIL "
                print(f"{tag} {r.group:30s} {r.binding:10s} {r.detail}")
        else:
            for r in failures:
                print(f"FAIL  {r.group:30s} {r.binding:10s} {r.status}: {r.detail}")

    if failures:
        print()
        print(f"{len(failures)} parity issue(s) found.")
        print(f"Edit {paths.config.name} to declare expected coverage,")
        print("or add bindings for the missing functions.")
        return 1

    # Say what was checked, with the count. "All bindings in parity" is what
    # this line used to claim while one of the four was never opened.
    checked = {b: 0 for b in BINDINGS}
    allowlisted = {b: 0 for b in BINDINGS}
    for group_name in sorted(idl_names & set(cfg_groups.keys())):
        functions = functions_by_group.get(group_name, [])
        for binding in BINDINGS:
            allowed = allowlists.get(binding) or {}
            allowlisted[binding] += len([f for f in functions if f in allowed])
            checked[binding] += len(functions)
    print(f"OK — {len(idl_names)} IDL groups, "
          f"{sum(len(v) for v in functions_by_group.values())} C functions. "
          "Functions checked per binding: " +
          ", ".join(f"{name} {checked[name]} ({allowlisted[name]} allowlisted)"
                    for name in BINDINGS))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
