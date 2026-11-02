#!/usr/bin/env python3
"""scripts/sync_mcp_data.py

Bundle canonical artifacts into the lrvx-mcp package data directory so
the installable package works offline. Six artifacts:

* ``c-api.snapshot``        copy of ``.api/c-api.snapshot``
* ``errors/E_*.md``         copy of ``docs/errors/E_*.md``
* ``ir.snapshot.json``      minimal IR (functions / structs / enums /
                            typedefs / function pointers) extracted
                            from ``include/lrvx/capi/lrvx_capi_spec.hpp``
                            via libclang and projected into the v1
                            schema in ``lrvx_codegen.manifest``.
* ``binding_manifest.json`` per-binding symbol map joined by IDL group
                            (``binding_parity.yaml`` + scans of
                            ``lrvx/_lrvx/__init__.pyi``,
                            ``node/index.d.ts``, and the codon golden).
* ``examples_index.json``   index of ``docs/examples/`` runnable corpus
                            (path, language, topic, sha256).
* ``docs.fts.sqlite``       SQLite FTS5 full-text index over an
                            allowlisted set of doc roots (NEVER
                            ``.notes/``, NEVER ``CLAUDE.md``).
                            Generated, never committed: it is a pure
                            function of ``docs/`` whose bytes vary with
                            the libsqlite that wrote them, so a
                            committed copy conflicted on every pair of
                            branches touching docs. The wheel job builds
                            it at package time.

CI gate: ``--check`` rebuilds every committed artifact in memory and
exits 1 on any byte-level drift from the committed copies; for the FTS
index it is a build step instead — it proves the index still builds
from docs/. Total bundle size is clamped to 10 MB so the wheel stays
light.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import sqlite3
import sys
import tempfile
from pathlib import Path
from typing import List, Tuple

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "tools" / "codegen"))

try:
    import yaml
except ImportError:
    print("ERROR: PyYAML required. pip install pyyaml", file=sys.stderr)
    sys.exit(2)

from lrvx_codegen import extractor, manifest  # noqa: E402

# ── Paths ─────────────────────────────────────────────────────────────

SRC_SNAPSHOT = REPO_ROOT / ".api" / "c-api.snapshot"
SRC_ERRORS_DIR = REPO_ROOT / "docs" / "errors"
SRC_SPEC = REPO_ROOT / "include" / "lrvx" / "capi" / "lrvx_capi_spec.hpp"
SRC_PYI = REPO_ROOT / "python" / "lrvx" / "_lrvx" / "__init__.pyi"
SRC_DTS = REPO_ROOT / "node" / "index.d.ts"
SRC_CODON = REPO_ROOT / "tools" / "codegen" / "golden" / "lrvx_capi.codon"
# The surfaces the manifest used to omit or inventory at the wrong layer.
SRC_CODON_API = REPO_ROOT / "codon" / "lrvx"
SRC_CPP_HEADERS = REPO_ROOT / "include" / "lrvx"
SRC_QUICKJS_BINDINGS = REPO_ROOT / "src" / "quickjs" / "js_bindings.cpp"
SRC_QUICKJS_PRELUDE = REPO_ROOT / "src" / "quickjs" / "js_strategy.cpp"
SRC_PARITY = REPO_ROOT / "tools" / "codegen" / "binding_parity.yaml"
SRC_EXAMPLES = REPO_ROOT / "docs" / "examples"
SRC_DOCS = REPO_ROOT / "docs"

DST_DATA = REPO_ROOT / "mcp" / "lrvx_mcp" / "data"
DST_IR = DST_DATA / "ir.snapshot.json"
DST_MANIFEST = DST_DATA / "binding_manifest.json"
DST_EXAMPLES = DST_DATA / "examples_index.json"
DST_FTS = DST_DATA / "docs.fts.sqlite"
DST_GOTCHAS = DST_DATA / "gotchas.json"

# Total bundle budget. The IR snapshot + binding manifest + examples
# index are tiny (kBs); the FTS index dominates. 10 MB is a soft cap
# choice — re-evaluate if the docs corpus grows.
BUNDLE_BUDGET_BYTES = 10 * 1024 * 1024


# ── Docs FTS allowlist ────────────────────────────────────────────────

# Hard allowlist of doc roots indexed by ``docs_search``. Listing is
# additive — every directory the FTS may index appears here. Anything
# outside is silently skipped. Never index ``.notes/`` (private
# tracker / strategy / roadmap), never index CLAUDE.md (author-only
# instructions).
ALLOWED_DOC_ROOTS = (
    "bindings",
    "how-to",
    "tutorials",
    "reference",
    "explanation",
    "errors",
    "venue",
)
DENIED_NAMES = {"CLAUDE.md", "claude.md"}


def _is_allowed_doc(rel_to_docs: Path) -> bool:
    if not rel_to_docs.parts:
        return False
    if rel_to_docs.parts[0] not in ALLOWED_DOC_ROOTS:
        return False
    if rel_to_docs.name in DENIED_NAMES:
        return False
    if rel_to_docs.suffix.lower() != ".md":
        return False
    return True


# ── Builders ──────────────────────────────────────────────────────────


def _build_ir_json() -> str:
    module = extractor.parse_spec(SRC_SPEC)
    payload = manifest.ir_to_snapshot(module)
    return manifest.dumps_canonical(payload)


def _build_binding_manifest_json() -> str:
    module = extractor.parse_spec(SRC_SPEC)
    parity = yaml.safe_load(SRC_PARITY.read_text()) or {}
    pyi_text = SRC_PYI.read_text() if SRC_PYI.exists() else ""
    dts_text = SRC_DTS.read_text() if SRC_DTS.exists() else ""
    codon_text = SRC_CODON.read_text() if SRC_CODON.exists() else ""
    codon_sources = {
        str(f.relative_to(REPO_ROOT)): f.read_text()
        for f in sorted(SRC_CODON_API.glob("*.codon"))
    } if SRC_CODON_API.is_dir() else {}
    cpp_headers = {
        str(f.relative_to(REPO_ROOT)): f.read_text(errors="ignore")
        for f in sorted(SRC_CPP_HEADERS.rglob("*.h"))
    } if SRC_CPP_HEADERS.is_dir() else {}
    payload = manifest.build_binding_manifest(
        module=module,
        parity_yaml=parity,
        pyi_text=pyi_text,
        dts_text=dts_text,
        codon_text=codon_text,
        codon_sources=codon_sources,
        cpp_headers=cpp_headers,
        quickjs_bindings_text=(SRC_QUICKJS_BINDINGS.read_text(errors="ignore")
                               if SRC_QUICKJS_BINDINGS.exists() else ""),
        quickjs_prelude_text=(SRC_QUICKJS_PRELUDE.read_text(errors="ignore")
                              if SRC_QUICKJS_PRELUDE.exists() else ""),
    )
    return manifest.dumps_canonical(payload)


def _build_examples_json() -> str:
    payload = manifest.build_examples_index(SRC_EXAMPLES)
    return manifest.dumps_canonical(payload)


def _build_docs_fts(out_path: Path) -> None:
    """Write a SQLite FTS5 index of allowlisted docs to ``out_path``.

    The output file is overwritten. The schema is deliberately small so
    the bundle stays light:

        CREATE VIRTUAL TABLE docs USING fts5(path, title, body,
                                              tokenize='porter');

    ``path`` is the repo-relative path. ``title`` is the first ``# ``
    heading or the filename if absent. ``body`` is the full file
    contents.
    """
    if out_path.exists():
        out_path.unlink()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(out_path)
    try:
        conn.execute(
            "CREATE VIRTUAL TABLE docs USING fts5("
            "path UNINDEXED, title, body, tokenize='porter unicode61')"
        )
        # Files are inserted in deterministic order so the resulting
        # sqlite file is byte-stable across runs.
        rows: List[Tuple[str, str, str]] = []
        for md in sorted(SRC_DOCS.rglob("*.md")):
            rel = md.relative_to(SRC_DOCS)
            if not _is_allowed_doc(rel):
                continue
            text = md.read_text(errors="replace")
            title = ""
            for line in text.splitlines():
                stripped = line.strip()
                if stripped.startswith("# "):
                    title = stripped[2:].strip()
                    break
            if not title:
                title = md.stem
            rows.append((str("docs" / rel), title, text))
        # Single transaction for determinism + speed.
        conn.execute("BEGIN")
        conn.executemany(
            "INSERT INTO docs(path, title, body) VALUES (?, ?, ?)", rows
        )
        conn.execute("COMMIT")
        # FTS5 maintains internal aux tables; an `optimize` pass merges
        # them into a single segment so the on-disk layout is canonical
        # and equality-checkable.
        conn.execute("INSERT INTO docs(docs) VALUES('optimize')")
        conn.commit()
    finally:
        conn.close()


# ── Sync helpers ──────────────────────────────────────────────────────


def _sync_text(name: str, dst: Path, builder, drifts: List[str], *, check_only: bool) -> None:
    text = builder()
    new = text.encode("utf-8")
    if dst.exists() and dst.read_bytes() == new:
        return
    if check_only:
        drifts.append(str(dst.relative_to(REPO_ROOT)))
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(new)


def _sync_example_sources(drifts: List[str], *, check_only: bool) -> None:
    """Copy the runnable example SOURCES (not just the index) into
    data/docs/examples/ so `get_example` can serve them from an installed
    wheel -- previously only the index shipped and every source read
    'unavailable' off-checkout."""
    dst_dir = DST_DATA / "docs" / "examples"
    exts = (".py", ".js", ".codon", ".cpp")
    srcs = [p for p in sorted(SRC_EXAMPLES.glob("*")) if p.suffix in exts]
    for src in srcs:
        dst = dst_dir / src.name
        new = src.read_bytes()
        if dst.exists() and dst.read_bytes() == new:
            continue
        if check_only:
            drifts.append(str(dst.relative_to(REPO_ROOT)))
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(new)
    # Drop stale bundled copies whose source was removed.
    if dst_dir.exists():
        keep = {s.name for s in srcs}
        for old in dst_dir.glob("*"):
            if old.name not in keep:
                if check_only:
                    drifts.append(str(old.relative_to(REPO_ROOT)) + " (stale)")
                else:
                    old.unlink()


def _generate_docs_fts(dst: Path, *, check_only: bool) -> int:
    """Build the docs FTS index; return its size in bytes.

    The one artifact here that is generated rather than committed: on
    ``--check`` it is built to a temp path — the same build the wheel
    job runs at package time, so a doc that breaks indexing fails the
    gate — and nothing is compared, because there is no committed copy
    to compare against.
    """
    if check_only:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td) / dst.name
            _build_docs_fts(tmp)
            return tmp.stat().st_size
    _build_docs_fts(dst)
    return dst.stat().st_size


def _validate_gotchas(gotchas_path: Path, ir_json: str,
                      manifest_json: str) -> List[str]:
    """Return a list of error strings for any gotcha key that fails to
    resolve against the IR or any binding manifest.

    The check is intentionally permissive — same loose matching the
    ``lookup_symbol`` tool does — so an entry keyed on a Python member
    (``Strategy.market_buy``) does not need to exist verbatim in the
    C-API IR. The key resolves if any of:

    - exact key in IR functions / structs / enums / typedefs
    - any key segment matches a binding-local symbol name
    - last dot-segment matches an IR or binding name (snake-case
      tolerant: ``market_buy`` matches ``lrvx_strategy_market_buy``)
    """
    if not gotchas_path.exists():
        return []
    try:
        data = json.loads(gotchas_path.read_text())
    except Exception as exc:
        return [f"{gotchas_path.relative_to(REPO_ROOT)}: invalid JSON: {exc}"]

    ir = json.loads(ir_json)
    mf = json.loads(manifest_json)

    ir_names = set()
    for kind in ("functions", "structs", "enums", "typedefs"):
        for item in ir.get(kind, []):
            n = item.get("name")
            if n:
                ir_names.add(n)

    binding_names = set()
    for bind in mf.get("bindings", {}).values():
        for s in bind.get("symbols", []):
            n = s.get("name")
            if n:
                binding_names.add(n)

    universe = ir_names | binding_names
    errors: List[str] = []

    for key, entries in data.items():
        if key.startswith("$"):
            continue  # schema/comment slot
        if not isinstance(entries, list):
            errors.append(f"gotchas.json: {key!r} value must be a list")
            continue
        if not _gotcha_key_resolves(key, universe):
            errors.append(
                f"gotchas.json: {key!r} resolves to no symbol in IR / any "
                f"binding manifest. Did the symbol get renamed?"
            )
        for i, entry in enumerate(entries):
            for required in ("id", "summary", "context", "fix"):
                if required not in entry:
                    errors.append(
                        f"gotchas.json: {key!r}[{i}] missing field "
                        f"{required!r}"
                    )
    return errors


def _gotcha_key_resolves(key: str, universe: set) -> bool:
    if key in universe:
        return True
    tail = key.rsplit(".", 1)[-1]
    if tail in universe:
        return True
    for n in universe:
        if n.endswith("_" + tail) or n.endswith(tail):
            return True
    return False


def _copy_if_different(src: Path, dst: Path, *, check_only: bool) -> bool:
    if not src.exists():
        return True
    src_bytes = src.read_bytes()
    if dst.exists() and dst.read_bytes() == src_bytes:
        return True
    if check_only:
        return False
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(src_bytes)
    return True


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--check", action="store_true",
                   help="don't write; exit 1 on drift")
    args = p.parse_args(argv)

    drifts: list[str] = []

    # 1. ABI snapshot.
    if not _copy_if_different(SRC_SNAPSHOT, DST_DATA / "c-api.snapshot",
                              check_only=args.check):
        drifts.append(str((DST_DATA / "c-api.snapshot").relative_to(REPO_ROOT)))

    # 2. Error catalog. Sync set of E_*.md files exactly; report extras
    # under mcp/lrvx_mcp/data/errors/ that aren't in the canonical list.
    dst_errors_dir = DST_DATA / "errors"
    src_pages = sorted(SRC_ERRORS_DIR.glob("E_*.md"))
    src_names = {p.name for p in src_pages}
    if dst_errors_dir.is_dir():
        for existing in dst_errors_dir.glob("*.md"):
            if existing.name not in src_names:
                if args.check:
                    drifts.append(
                        str(existing.relative_to(REPO_ROOT)) + " (stale)"
                    )
                else:
                    existing.unlink()
    for src in src_pages:
        dst = dst_errors_dir / src.name
        if not _copy_if_different(src, dst, check_only=args.check):
            drifts.append(str(dst.relative_to(REPO_ROOT)))

    # 3. IR snapshot, binding manifest, examples index — JSON.
    _sync_text("ir.snapshot.json", DST_IR, _build_ir_json,
               drifts, check_only=args.check)
    _sync_text("binding_manifest.json", DST_MANIFEST,
               _build_binding_manifest_json, drifts, check_only=args.check)
    _sync_text("examples_index.json", DST_EXAMPLES, _build_examples_json,
               drifts, check_only=args.check)
    _sync_example_sources(drifts, check_only=args.check)

    # 4. Docs FTS5 index — generated, not committed; --check only
    # proves it builds.
    fts_bytes = _generate_docs_fts(DST_FTS, check_only=args.check)

    # 4b. gotchas.json — hand-curated, not regenerated. Validate that
    # every key still resolves to a real symbol; otherwise a rename
    # silently orphans the entry.
    gotcha_errors = _validate_gotchas(
        DST_GOTCHAS,
        _build_ir_json(),
        _build_binding_manifest_json(),
    )
    drifts.extend(gotcha_errors)

    # 5. Bundle budget. Reported (not enforced) on --check; enforced on
    # write so an over-budget commit fails locally before it lands.
    # The FTS index may only exist in a temp dir under --check, so its
    # size is carried over from the build instead of read off disk.
    if DST_DATA.exists():
        total = fts_bytes + sum(
            p.stat().st_size for p in DST_DATA.rglob("*")
            if p.is_file() and p != DST_FTS)
        if total > BUNDLE_BUDGET_BYTES:
            print(
                f"::error::lrvx-mcp bundled data is "
                f"{total / 1024 / 1024:.2f} MB, over the "
                f"{BUNDLE_BUDGET_BYTES / 1024 / 1024:.0f} MB budget. "
                f"Trim docs FTS scope or move it to an extra.",
                file=sys.stderr,
            )
            if not args.check:
                return 2

    if drifts:
        print("::error::lrvx-mcp bundled data is out of sync:", file=sys.stderr)
        for d in drifts:
            print(f"  {d}", file=sys.stderr)
        print("Run: python3 scripts/sync_mcp_data.py", file=sys.stderr)
        return 1

    if args.check:
        print(f"OK: lrvx-mcp bundled data in sync.")
    else:
        n_pages = len(src_pages)
        n_gotchas = sum(
            len(v) for k, v in
            (json.loads(DST_GOTCHAS.read_text()) if DST_GOTCHAS.exists() else {}).items()
            if not k.startswith("$") and isinstance(v, list)
        )
        print(
            f"synced: c-api.snapshot + {n_pages} error page(s) + "
            f"ir.snapshot.json + binding_manifest.json + "
            f"examples_index.json + docs.fts.sqlite + "
            f"{n_gotchas} gotcha(s) → "
            f"{DST_DATA.relative_to(REPO_ROOT)}"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
