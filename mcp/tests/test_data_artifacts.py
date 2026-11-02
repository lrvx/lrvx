"""Schema + content validation for the bundled MCP data artifacts.

These tests run against the committed copies under
``mcp/lrvx_mcp/data/`` — they catch:

* Schema regressions in the IR snapshot, binding manifest, and
  examples index (key set + value shapes).
* Missing or empty surfaces — every binding must contribute *some*
  symbols, every IDL group must appear in the manifest.
* Docs FTS5 index integrity: opens read-only, returns hits for known
  doc terms, and excludes denied paths (``.notes/``, ``CLAUDE.md``).

The ``--check`` byte-equality gate in ``scripts/sync_mcp_data.py``
is the source of truth for "is the bundle in sync"; these tests
focus on "is the bundle *useful*" — schema + content shape that
downstream MCP tools (sub-PR B / C) will read.
"""
from __future__ import annotations

import json
import sqlite3
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
DATA = REPO_ROOT / "mcp" / "lrvx_mcp" / "data"

# Make ``lrvx_mcp`` importable when running pytest from the repo root.
sys.path.insert(0, str(REPO_ROOT / "mcp"))


# ── ir.snapshot.json ──────────────────────────────────────────────────


@pytest.fixture(scope="module")
def ir_snapshot() -> dict:
    path = DATA / "ir.snapshot.json"
    assert path.exists(), (
        f"missing {path}; run `python3 scripts/sync_mcp_data.py`")
    return json.loads(path.read_text())


def test_ir_snapshot_schema(ir_snapshot: dict) -> None:
    assert ir_snapshot["version"] == 1
    for key in ("functions", "structs", "enums", "typedefs", "function_pointers"):
        assert key in ir_snapshot, f"missing top-level key {key!r}"
        assert isinstance(ir_snapshot[key], list), key


def test_ir_snapshot_function_shape(ir_snapshot: dict) -> None:
    assert len(ir_snapshot["functions"]) > 0, "expected non-empty functions"
    for fn in ir_snapshot["functions"][:5]:
        assert "name" in fn
        assert fn["name"].startswith("lrvx_"), fn["name"]
        assert "return_type" in fn
        assert isinstance(fn["params"], list)
        assert "group" in fn
        for p in fn["params"]:
            assert {"name", "type"} <= p.keys()


def test_ir_snapshot_struct_shape(ir_snapshot: dict) -> None:
    assert len(ir_snapshot["structs"]) > 0
    for st in ir_snapshot["structs"][:5]:
        assert st["name"].startswith("Lrvx"), st["name"]
        assert isinstance(st["fields"], list)
        for f in st["fields"]:
            assert {"name", "type"} <= f.keys()


def test_ir_snapshot_known_symbols(ir_snapshot: dict) -> None:
    fn_names = {f["name"] for f in ir_snapshot["functions"]}
    # Sanity-check well-known C-API entries.
    assert "lrvx_indicator_ema" in fn_names
    assert "lrvx_aggregate_time_bars" in fn_names
    assert "lrvx_backtest_runner_run_csv" in fn_names

    struct_names = {s["name"] for s in ir_snapshot["structs"]}
    assert "LrvxBarData" in struct_names
    assert "LrvxTradeData" in struct_names


def test_ir_snapshot_is_byte_stable() -> None:
    """Two consecutive loads must round-trip to the same JSON bytes
    when re-dumped via the canonical writer — guards against any
    accidental dependence on dict-iteration order."""
    sys.path.insert(0, str(REPO_ROOT / "tools" / "codegen"))
    from lrvx_codegen import manifest

    raw = (DATA / "ir.snapshot.json").read_bytes()
    payload = json.loads(raw.decode("utf-8"))
    redump = manifest.dumps_canonical(payload).encode("utf-8")
    assert redump == raw


# ── binding_manifest.json ─────────────────────────────────────────────


@pytest.fixture(scope="module")
def binding_manifest() -> dict:
    path = DATA / "binding_manifest.json"
    assert path.exists(), (
        f"missing {path}; run `python3 scripts/sync_mcp_data.py`")
    return json.loads(path.read_text())


def test_binding_manifest_schema(binding_manifest: dict) -> None:
    assert binding_manifest["version"] == 1
    assert "groups" in binding_manifest
    assert "bindings" in binding_manifest
    for b in ("capi", "pybind11", "napi", "codon", "quickjs"):
        assert b in binding_manifest["bindings"], f"missing binding {b!r}"


def test_binding_manifest_groups_non_empty(binding_manifest: dict) -> None:
    assert len(binding_manifest["groups"]) > 0
    for grp_name, grp in binding_manifest["groups"].items():
        assert "capi_functions" in grp, grp_name
        for binding in ("pybind11", "napi", "codon", "quickjs"):
            assert binding in grp, f"{grp_name} missing {binding}"
            assert "status" in grp[binding]


def test_binding_manifest_quickjs_group_matches_parity_yaml(
        binding_manifest: dict) -> None:
    """The per-group `quickjs` entry must carry the same coverage as the
    other three bindings, not silently drop out of the manifest the way it
    did before -- a group where QuickJS is declared `required` with real
    functions must show that, not just a bare `status`."""
    composite_book = binding_manifest["groups"]["composite_book"]["quickjs"]
    assert composite_book["status"] == "required"
    assert "__lrvx_cb_apply_snapshot" in composite_book["functions"]
    assert "__lrvx_cb_apply_delta" in composite_book["functions"]


def test_binding_manifest_capi_symbols_have_kinds(binding_manifest: dict) -> None:
    capi = binding_manifest["bindings"]["capi"]["symbols"]
    assert len(capi) > 50, "expected a non-trivial capi surface"
    kinds = {s["kind"] for s in capi}
    assert {"function", "struct"} <= kinds


def test_binding_manifest_codon_groups_present(binding_manifest: dict) -> None:
    """The codon golden file is auto-generated from the same IDL spec
    that drives the IR. Every *required* IDL group must show up in
    ``codon.groups_present``."""
    codon_groups = set(binding_manifest["bindings"]["codon"]["groups_present"])
    for grp_name, grp in binding_manifest["groups"].items():
        if grp.get("codon", {}).get("status") == "required":
            assert grp_name in codon_groups, (
                f"required codon group {grp_name!r} missing from golden")


# ── examples_index.json ───────────────────────────────────────────────


@pytest.fixture(scope="module")
def examples_index() -> dict:
    path = DATA / "examples_index.json"
    assert path.exists()
    return json.loads(path.read_text())


def test_examples_index_schema(examples_index: dict) -> None:
    assert examples_index["version"] == 1
    assert isinstance(examples_index["examples"], list)
    assert len(examples_index["examples"]) > 0


def test_examples_index_entries(examples_index: dict) -> None:
    valid_topics = {
        "strategy", "connector", "indicator",
        "event-handler", "risk", "backtest",
    }
    valid_languages = {"python", "node", "codon", "cpp"}
    for ex in examples_index["examples"]:
        assert {"path", "language", "topic", "size_bytes", "sha256"} <= ex.keys()
        assert ex["language"] in valid_languages, ex
        assert ex["topic"] in valid_topics, ex


# ── gotchas.json ──────────────────────────────────────────────────────


@pytest.fixture(scope="module")
def gotchas() -> dict:
    path = DATA / "gotchas.json"
    assert path.exists(), (
        f"missing {path}; gotchas are hand-curated, see "
        f"mcp/lrvx_mcp/data/gotchas.json")
    return json.loads(path.read_text())


def test_gotchas_schema(gotchas: dict) -> None:
    """Every non-schema entry is a list of dicts with the required
    fields; ids are unique within a key."""
    required = {"id", "summary", "context", "fix"}
    for key, entries in gotchas.items():
        if key.startswith("$"):
            continue
        assert isinstance(entries, list), key
        assert entries, f"{key}: empty entry list"
        seen_ids = set()
        for entry in entries:
            assert required <= entry.keys(), (key, entry)
            assert entry["id"] not in seen_ids, f"{key}: duplicate id {entry['id']!r}"
            seen_ids.add(entry["id"])


def test_gotchas_keys_resolve(gotchas: dict, ir_snapshot: dict,
                              binding_manifest: dict) -> None:
    """Every gotcha key must resolve through the same loose match the
    drift check uses. A rename that orphans a key fails this test."""
    universe: set = set()
    for kind in ("functions", "structs", "enums", "typedefs"):
        for item in ir_snapshot.get(kind, []):
            n = item.get("name")
            if n:
                universe.add(n)
    for bind in binding_manifest.get("bindings", {}).values():
        for s in bind.get("symbols", []):
            n = s.get("name")
            if n:
                universe.add(n)

    for key in gotchas:
        if key.startswith("$"):
            continue
        if key in universe:
            continue
        tail = key.rsplit(".", 1)[-1]
        if tail in universe:
            continue
        if any(n.endswith("_" + tail) or n.endswith(tail) for n in universe):
            continue
        pytest.fail(f"gotcha key {key!r} resolves to no symbol")
        assert ex["path"].startswith("docs/examples/"), ex["path"]
        assert (REPO_ROOT / ex["path"]).exists(), (
            f"index points at missing file: {ex['path']}")


def test_examples_index_languages_covered(examples_index: dict) -> None:
    langs = {ex["language"] for ex in examples_index["examples"]}
    # Multi-language coverage so PR-C's get_example tool returns
    # results across bindings, not just one.
    assert {"python", "node", "codon"} <= langs


# ── docs.fts.sqlite ───────────────────────────────────────────────────


@pytest.fixture(scope="module")
def docs_db():
    path = DATA / "docs.fts.sqlite"
    # Generated at package time, not committed; a fresh checkout does
    # not have it until the sync script runs.
    assert path.exists(), (
        "docs.fts.sqlite missing -- run scripts/sync_mcp_data.py first")
    # Open read-only + immutable so the test path mirrors how MCP
    # opens it at request time.
    uri = f"file:{path}?mode=ro&immutable=1"
    conn = sqlite3.connect(uri, uri=True)
    yield conn
    conn.close()


def test_docs_db_has_rows(docs_db) -> None:
    n = docs_db.execute("SELECT COUNT(*) FROM docs").fetchone()[0]
    assert n > 50, f"expected a non-trivial docs index, got {n} rows"


def test_docs_db_known_query_hits(docs_db) -> None:
    # `walk-forward` is split on the dash by the tokenizer, so quote
    # the bigram. This mirrors how MCP's docs_search will phrase queries.
    rows = docs_db.execute(
        'SELECT path FROM docs WHERE docs MATCH ? LIMIT 5',
        ('"walk forward"',),
    ).fetchall()
    assert len(rows) > 0, "expected hits for 'walk forward'"


def test_docs_db_excludes_internal_paths(docs_db) -> None:
    # Allowlist enforcement — denied roots and CLAUDE.md must never
    # appear in the index.
    n_internal = docs_db.execute(
        "SELECT COUNT(*) FROM docs "
        "WHERE path LIKE '%/.notes/%' "
        "   OR path LIKE '.notes/%' "
        "   OR path LIKE '%/CLAUDE.md' "
        "   OR path LIKE 'CLAUDE.md'"
    ).fetchone()[0]
    assert n_internal == 0, "internal/private docs leaked into FTS index"


def test_docs_db_only_allowed_roots(docs_db) -> None:
    paths = [r[0] for r in docs_db.execute("SELECT path FROM docs").fetchall()]
    allowed_roots = {
        "docs/bindings", "docs/how-to", "docs/tutorials",
        "docs/reference", "docs/explanation", "docs/errors",
        "docs/venue",
    }
    for p in paths:
        prefix = "/".join(p.split("/")[:2])
        assert prefix in allowed_roots, f"unexpected root: {p}"


def test_docs_db_includes_venue_pages(docs_db) -> None:
    """Regression test: `docs/venue/` was missing from `ALLOWED_DOC_ROOTS`, so
    `docs_search` could not surface a single page of the venue module --
    the matching engine, the ledger, the multi-agent demo, none of it
    existed as far as an agent using MCP could tell. Measured cost of
    including it: +192,512 bytes to docs.fts.sqlite (+7.76%), landing
    the whole `data/` bundle at ~4.3 MB against a 10 MB soft cap.
    """
    rows = docs_db.execute(
        "SELECT COUNT(*) FROM docs WHERE path LIKE 'docs/venue/%'"
    ).fetchone()[0]
    assert rows >= 5, f"expected several venue pages indexed, got {rows}"
