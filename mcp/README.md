# lrvx-mcp — Model Context Protocol server for lrvx

Gives AI agents (Cursor, Claude Code, Cline) **grounded** access to the
lrvx C-API surface, error catalog, and indicator library — so they can
write code against real signatures instead of guessing.

The server runs locally on the developer's machine; the IDE spawns it
as a child process and talks to it over stdio. There is no public
hosting; nothing leaves the machine.

## Tools

| Tool | What it does |
|---|---|
| `list_indicators` | Every indicator in `lrvx` with class signature, batch fn (if any), and shape. Filter by substring. |
| `lookup_error_code` | `E_SYM_001` → full Markdown page with fix recipe, common causes, diagnostics. |
| `list_capi_functions` | Search the lrvx C-API surface from the committed ABI snapshot. Returns `name + return type + parameter types`. |
| `validate_strategy` | Static-analysis check on Python strategy code: AST parses, expected hooks present, no `eval`/`exec`. |
| `explain_event` | Describe the fields of a lrvx event struct (`LrvxTradeData`, `LrvxBookData`, `LrvxBarData`, `LrvxSymbolContext`, `LrvxSignal`). Accepts a struct name or a raw event dict. |
| `lookup_symbol` | Take any binding-local spelling (`LrvxBarData`, `BarData`, `lrvx_indicator_ema`, `ema`) and return what the symbol is called in C-API, Python, Node, and Codon. |
| `list_bindings` | Enumerate the exports of one binding (capi, python, node, codon, quickjs). Substring filter and limit. |
| `get_example` | Code from `docs/examples/` matching a topic (strategy, connector, indicator, event-handler, risk, backtest), optionally filtered by language. |
| `scaffold_strategy` | Starter strategy class for Python or Node. Three kinds: bar-driven, trade-driven, hybrid. The Python output goes through `ast.parse` + `validate_strategy`; the Node output goes through `node --check`. CI fails if either breaks, so templates can't quietly rot. |
| `docs_search` | Top-k FTS5 search over the docs. The index is built from an allowlist of roots; private trackers and `CLAUDE.md` are not in that allowlist. |
| `run_backtest` | Run a Python strategy against a CSV dataset in a sandboxed subprocess. Caps CPU, memory, and output size; wall-clock timeout. **MVP sandbox** — caps resources but does not isolate filesystem or network. Treat the same way as any untrusted Python; use nsjail / firejail / Docker for production. |
| `compute_indicator` | Run one lrvx indicator over a list of floats. Accepts class-form (`EMA`) or function-form (`ema`) names and forwards extra kwargs to the constructor. Input capped at 1 MiB. Needs `lrvx` installed (`pip install "lrvx-mcp[lrvx]"`). |
| `suggest_indicator` | Map an English description ('trend filter', 'momentum oscillator', 'volatility band', 'mean revert', 'regime test') to a ranked shortlist of lrvx indicators. Pure keyword heuristic — no LLM call. Always confirm the chosen indicator with `list_indicators` before using it. |

## Quickstart

```bash
pip install lrvx-mcp           # MCP server + lrvx (indicators, backtest, engine CLI)
lrvx-mcp init                  # writes ./.mcp.json for the current project
# restart your MCP client (Claude Code / Cursor / Cline) → done
```

You get the whole surface in one install: docs / lookup tools,
indicator introspection, backtest, and the engine CLI
(`lrvx engine sim`, `lrvx tape record`). For tier-5/6 (live state,
`place_order`, `flatten_positions`), also start a paper engine and
rerun `init` with `--overwrite` and the printed token:

```bash
lrvx engine sim --strategy s.py --tape ./tape         # prints engine URL + token
lrvx-mcp init --overwrite --engine-url URL --token T  # rewrites the lrvx entry with the engine env
```

`lrvx engine sim` boots a `Runner` + `SimulatedExecutor` +
`ControlServer` and writes a runtime snapshot the read tools poll.
See [`python/lrvx/engine_cli.py`](../python/lrvx/engine_cli.py)
for the full set of flags.

### `lrvx-mcp init` flags

| Flag | What it does |
|---|---|
| *(default)* | Writes `./.mcp.json` in the current directory. Merges with an existing file. |
| `--global` | Writes `~/.config/claude/.mcp.json` instead. |
| `--overwrite` | Replace an existing `mcpServers.lrvx` entry instead of refusing. |
| `--print` | Print the merged config to stdout, don't write. |
| `--engine-url URL` | Sets `LRVX_CONTROL_URL` (tier-5/6 wiring). |
| `--token T` | Sets `LRVX_CONTROL_TOKEN` (printed by `lrvx engine sim`). |

An existing `mcpServers.flox` entry that launches `flox-mcp`, the server's
FLOX-era name, is replaced by the `lrvx` entry instead of kept next to it.
The `FLOX_CONTROL_URL` and `FLOX_CONTROL_TOKEN` it set move over as
`LRVX_CONTROL_URL` and `LRVX_CONTROL_TOKEN` unless `--engine-url` or
`--token` sets them.

### Manual override

If you'd rather hand-edit, the resulting `.mcp.json` is shaped like:

```json
{
  "mcpServers": {
    "lrvx": {
      "command": "lrvx-mcp",
      "args": ["serve"],
      "env": {
        "LRVX_RUNTIME_STATE": "$HOME/.lrvx/runtime.json"
      }
    }
  }
}
```

Same shape works for Claude Code, Cursor, and Cline — they all read
`mcpServers.<name>` from a project-local `.mcp.json` (or the
client-specific global path).

## Develop

```bash
cd mcp
pip install -e ".[dev]"

# Run the server manually (stdio):
lrvx-mcp

# Sync bundled data from canonical sources (.api/, docs/errors/) and
# generate docs.fts.sqlite -- the tests below expect the index to exist:
python ../scripts/sync_mcp_data.py

# Run unit tests:
pytest tests/
```

## Bundled data

Read-only copies in the wheel:

- `lrvx_mcp/data/c-api.snapshot` — copy of `.api/c-api.snapshot`.
- `lrvx_mcp/data/errors/E_*.md` — copies of `docs/errors/E_*.md`.
- `lrvx_mcp/data/ir.snapshot.json` — IR (functions, structs, enums,
  typedefs, function pointers) pulled from the IDL spec via libclang.
  Versioned schema.
- `lrvx_mcp/data/binding_manifest.json` — per-binding symbol map keyed
  by IDL group. Built from `binding_parity.yaml` plus scans of
  `lrvx/_lrvx/__init__.pyi`, `node/index.d.ts`, and the codon
  golden file.
- `lrvx_mcp/data/examples_index.json` — index of `docs/examples/`
  (path, language, topic, sha256).
- `lrvx_mcp/data/docs.fts.sqlite` — SQLite FTS5 index over seven doc
  roots: `bindings`, `how-to`, `tutorials`, `reference`, `explanation`,
  `errors`, `venue`. Anything outside that list is skipped at index
  build time. Private trackers and `CLAUDE.md` are not in the
  allowlist. Unlike the rest of this list the index is not committed:
  it is generated by `sync_mcp_data.py` at package time (and locally
  before the tests), because its bytes depend on the libsqlite that
  wrote them.
- `lrvx_mcp/data/templates/strategy/{python,node}/{bar,trade,hybrid}-driven.tmpl`
  — strategy scaffolds rendered by `scaffold_strategy`.

`scripts/sync_mcp_data.py --check` runs in CI. Any drift between a
committed bundled copy and its canonical source fails the build, and
the FTS index has to build cleanly from `docs/`. To update after
editing a canonical source: run the script without `--check` and
commit the diff.

The `lrvx-mcp` package version is bumped in lockstep with `lrvx`
and `lrvx` by `scripts/set-version.sh`. An installed
`lrvx-mcp x.y.z` was built from the same source tree as the matching
lrvx / npm release, so what the agent sees lines up with what the
developer has installed.

## Scope notes

- The server is **local**. No network calls. AI clients spawn it as a
  child process and talk via stdio.
- Most tools are read-only data lookups. The two that execute code —
  `compute_indicator` and `run_backtest` — run in-process / in a
  resource-limited subprocess respectively. `run_backtest` is an MVP
  sandbox: it caps CPU, memory, and output size, but does NOT isolate
  filesystem or network. Wrap with nsjail / firejail / Docker for any
  deployment that takes untrusted input.
- The snapshot used by `list_capi_functions` is the same
  `.api/c-api.snapshot` enforced by the codegen ABI gate; the IR
  snapshot used by `lookup_symbol` / `list_bindings` is regenerated
  from the same IDL spec on every release. So the agent's view of
  the surface tracks what lrvx actually ships.

## License

MIT — same as lrvx.
