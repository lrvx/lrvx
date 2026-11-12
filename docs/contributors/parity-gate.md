# Cross-Binding Parity Gate

How `scripts/check_binding_parity.py` makes "I added a function to the C ABI but forgot the pybind11/NAPI wrapper" a CI failure instead of a silent gap that surfaces months later when a user complains.

## The problem it solves

The C ABI surface (`lrvx_capi.h`) grows whenever someone adds a `LRVX_EXPORT` to the IDL. Each binding (pybind11, NAPI, Codon, QuickJS) is supposed to expose that addition, and all four are hand-written, so all four drift.

The gate used to promise this and not deliver it. It matched **class names** in `.pyi` / `.d.ts`, so a single name satisfied a whole group however many functions the group gained; it checked Codon against `tools/codegen/golden/lrvx_capi.codon`, a file generated from the same IDL the gate had just parsed, which cannot disagree with it; and it skipped QuickJS for any group that carried no `quickjs` entry, which was most of them. All three holes are closed: parity is now checked **per C function**.

## How it works

The script:

1. **Parses the IDL** ([`include/lrvx/capi/lrvx_capi_spec.hpp`](../../include/lrvx/capi/lrvx_capi_spec.hpp)). Every `LRVX_EXPORT(group = "X")` declaration belongs to a named group.
2. **Reads the manifest** ([`tools/codegen/binding_parity.yaml`](../../tools/codegen/binding_parity.yaml)). Each group has four entries: `pybind11`, `napi`, `codon`, `quickjs`.
3. **Asks, for every function of the group, whether that binding can reach it**:
   - pybind11: a call site under `python/` (C/C++ sources, comments stripped)
   - NAPI: a call site under `node/src/`
   - Codon: a `from C import lrvx_x` in a module under `codon/lrvx/` — the code a strategy links, not the generated golden
   - QuickJS: a `lrvx_x: __lrvx_x` entry in the group's `functions:` map, whose global is registered with `addGlobalFunc` in `src/quickjs/js_bindings.cpp`
4. **Checks the declared names too**: the `classes:` of a `required` entry must appear in `.pyi` / `.d.ts` / the QuickJS prelude, and a `functions:` list under `pybind11` / `napi` still names top-level functions in those stubs. QuickJS classes are read through `lrvx_codegen.manifest.scan_quickjs`, the same extractor `scripts/sync_mcp_data.py` uses for the MCP `binding_manifest`, so the two never diverge.
5. **Fails loudly** on a function that is neither reachable nor allowlisted.

Run locally:

```bash
python3 scripts/check_binding_parity.py            # exit 1 on drift
python3 scripts/check_binding_parity.py --verbose  # show every group, not just failures
python3 scripts/check_binding_parity.py --root /path/to/tree
```

Wired into CI as the "Verify cross-binding parity" step inside the `verify-docs-current` job, with `--require-quickjs` passed explicitly, plus a step running `pytest scripts/tests -q` — the gate's own tests, which build a miniature repository, delete one wrapper, and require the gate to go red.

## The QuickJS map

A QuickJS `required` entry maps each C function to the global a strategy calls:

```yaml
composite_book:
  quickjs:
    status: required
    classes: [CompositeBook]
    functions:
      lrvx_composite_book_create: __lrvx_cb_create
      lrvx_composite_book_destroy: __lrvx_cb_destroy
```

The globals are written out rather than derived because they are not derivable: the `__` + C-name convention has real exceptions (`__lrvx_vprofile_create` wraps `lrvx_volume_profile_create`, `__lrvx_cb_*` abbreviates `lrvx_composite_book_*`). `--require-quickjs` is the default, so every group carries an entry; `--no-require-quickjs` exists for a tree where the QuickJS layer is not built.

## The per-function allowlist

A binding that has no wrapper for a function carries it in the top-level `allowlist_functions:` section, under that binding, with a one-line reason:

```yaml
allowlist_functions:
  napi:
    lrvx_walk_forward_run_csv: *no_napi_wrapper
```

The reason is mandatory — an entry with an empty reason fails the gate. Shared reasons are YAML anchors defined once in `allowlist_reasons:` above the list; a one-off gap can carry its own text. Remove the entry when the wrapper is written.

The group's status stays `required`: demoting a group to `allowlist` would hide every function in it, including the ones added tomorrow. This way a new `LRVX_EXPORT` still fails the gate in all four bindings until someone either writes the wrapper or writes down why there isn't one.

### What each binding does not reach

The gate prints the current counts itself. Its last line on a passing run names the number of IDL groups and C functions and, per binding, how many functions it checked and how many of those are allowlisted:

```bash
python3 scripts/check_binding_parity.py | tail -1
```

No count is repeated here: `allowlist_functions:` is the source of truth and that line is always computed from it. What stays true as the numbers move is the kind of gap each binding has:

| binding  | what the gap is |
|----------|-----------------|
| pybind11 | the Python binding wraps the C++ API directly rather than calling the C ABI, so most entry points have no call site under `python/` at all |
| napi     | individual wrappers never written |
| codon    | functions no module under `codon/lrvx/` imports |
| quickjs  | functions with no `addGlobalFunc` registration, and a few called only inside other wrappers, with no global of their own |

The size of the pybind11 allowlist is not a claim that Python cannot do those things: it says the Python binding is a parallel implementation over the C++ headers, so fixes made at the C boundary (NULL-safety, the exception guard, the error accessors) do not reach it.

## The struct-layout tables

The Codon binding reads event payloads out of the C structs by byte offset, because the codegen models every aggregate as an opaque `cobj`. Those offsets used to be typed in by hand in `codon/lrvx/dispatch.codon` and `codon/lrvx/strategy.codon`, with nothing connecting them to the header.

They are generated now, by `tools/codegen/lrvx_codegen/emit_layout.py`, from the same IDL spec the C header comes from:

- [`include/lrvx/capi/lrvx_capi_layout.h`](../../include/lrvx/capi/lrvx_capi_layout.h) — `kLrvxEventLayout` (struct, field, offset, width) and `kLrvxEventStructLayout` (struct, size, alignment)
- [`codon/lrvx/layout.codon`](../../codon/lrvx/layout.codon) — the `_TRADE_*` / `_BAR_*` / `_EV_*` / `_CTX_*` / `_LRVXBAR_*` constants the Codon modules import

Both are regenerated by `tools/codegen/scripts/regenerate.sh` and diffed in place by `tools/codegen/scripts/check.sh` and the `codegen` workflow, so editing either by hand is drift and fails CI. `tests/test_capi_event_layout.cpp` closes the loop at the other end: every entry is compared against `offsetof` / `sizeof` / `alignof` on the real struct, so an offset the compiler disagrees with fails the build rather than delivering a wrong number to a strategy.

## Status values

Each per-binding entry has one of three statuses:

### `required`

The binding **must** expose the listed symbols. List them under `classes` or `functions`:

```yaml
metrics:
  pybind11: { status: required, classes: [PnLTracker] }
  napi: { status: required, classes: [PnLTracker] }
  codon: { status: required }
```

This is the default for every user-facing surface. If the listed class or function disappears, CI fails — and so does a C function of the group that the binding cannot reach and that carries no `allowlist_functions:` entry.

### `not_applicable`

This group is internal helpers and **never** intended for this binding. No symbol list:

```yaml
fixed_point:
  pybind11: { status: not_applicable }   # exposed via Price.toDouble() on classes
  napi: { status: not_applicable }
  codon: { status: required }
```

Used for things like `pointer_out` (output-pointer helpers) or `validation` (input validation primitives) that wouldn't make sense as a Python class.

### `allowlist`

The **whole group** is a known gap, with a mandatory `reason`:

```yaml
your_group:
  napi:
    status: allowlist
    reason: "stats functions exposed via Stats class methods; function-style not declared."
```

Use this only when the binding exposes nothing of the group at all. For a group the binding does expose, with individual functions missing, keep `required` and list those functions under `allowlist_functions:` — a group-level allowlist also swallows every function added to the group later.

## Adding a new IDL group

If you add `LRVX_EXPORT(group = "new_thing")` and don't update the YAML, CI says:

```
FAIL  new_thing  config  missing_yaml: add an entry to binding_parity.yaml
```

Add a stanza for the new group:

```yaml
new_thing:
  pybind11: { status: required, classes: [NewThing] }
  napi: { status: required, classes: [NewThing] }
  codon: { status: required }
  quickjs:
    status: required
    functions:
      lrvx_new_thing_create: __lrvx_new_thing_create
```

Then make sure the listed classes exist on each side, and that every function of the group is either reachable from each binding or listed under `allowlist_functions:` with a reason.

## Removing a group

If you delete the last `LRVX_EXPORT(group = "old_thing")` declaration, the gate notices the YAML still mentions it:

```
FAIL  old_thing  config  missing_in_binding: group `old_thing` not found in IDL spec; remove from yaml
```

Remove the YAML stanza in the same PR.

## Why this is just a manifest, not auto-generation

Reasonable question: "if you know which classes belong to which group, why not generate the bindings from that?"

Two reasons:

1. **The manifest knows the *names*, not the *shape*.** It can say "`Executor` should exist", but it can't generate the `submit / cancel / replace / submit_oco / capabilities` method signatures — those are codegen's job (and codegen is unsafe to run for pybind11/NAPI for [perf reasons](architecture-overview.md#why-this-layout-not-c-abi-all-the-way)).
2. **Coverage ≠ correctness.** Even if `Executor` is named in `.pyi`, that doesn't mean the binding actually wires `lrvx_executor_*` correctly. Tests check correctness; the gate checks "did you remember to write the wrapper at all".

So the gate is a coarse mechanical check: "does the symbol exist?". Tests cover the semantics. Together they catch most of what auto-generation would catch, without giving up idiomatic bindings.

## Scope of the gate (what it doesn't catch)

- Argument signature drift (pybind11 method takes `int` but C ABI takes `int64_t`). That's pybind11's responsibility at runtime.
- Submodule-style exposure (`lrvx.targets.linear_slope` rather than top-level `lrvx.LinearSlope`). The gate only inspects top-level declarations for `classes:` / `functions:`. The `targets` group is `allowlist`'ed for this reason.
- Whether a call site is *correct*. The gate sees that `python/` mentions `lrvx_x`; it does not see whether the arguments are right. Tests cover that.
- Behavioral parity (Python and Node returning different values for the same input). Cross-binding parity *tests* (`scripts/cross_binding_parity.py`) cover that.

## Extending the gate

The script is small ([scripts/check_binding_parity.py](../../scripts/check_binding_parity.py)). If you need a new check, add it there. Examples:

- **QuickJS registration, not just parity.** [`check_quickjs_registration.py`](../../scripts/check_quickjs_registration.py) is a sibling gate: it asserts that every `__lrvx_*` global the JS layer *calls* has an `addGlobalFunc` registration, which is what `setQueueFifoTopN` violated. That is a different question from this gate's "does the IDL group's promised symbol exist" — this gate would pass on a registered-but-never-called global, and `check_quickjs_registration.py` doesn't know about IDL groups at all.
- **Method-level checks.** Currently we check class presence; we could check that a class has specific methods (e.g. `Executor` must have `submit`, `cancel`, `replace`).

Both would tighten the gate. Keep changes minimal — every false-positive case wastes contributor time.

## Sibling gate: behaviour, not names

This gate asserts a symbol *exists*. Every defect the binding-surface audit
found had an existing symbol — `VenueStack.clock()` raised "Unregistered type",
the five Node `VenueStack` factories threw `SyntaxError`.
[`check_binding_smoke.py`](../../scripts/check_binding_smoke.py) closes that by
walking both bindings by reflection and *calling* what it finds with no
arguments, failing only on binding-level errors and ignoring rejected inputs
(it prints, per binding, how many calls it made).

**A binding that cannot be imported fails that gate.** It used to print
`python: SKIP (cannot import lrvx: ...)` and exit 0, so a wheel that did not
load and an addon that was never built both read as "OK: no binding-level
failures" — the gate was loudest exactly when it had exercised nothing. An
import failure, a Node entry point that throws, and a missing `node/index.js`
are all reported with the binding named and a non-zero exit. `--root` points it
at another tree, the way the parity gate's `--root` does.
