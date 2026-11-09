"""Lookahead-bias detector for Python strategy code.

Lookahead bias is the most common backtest bug in algorithmic
trading: a strategy reads tomorrow's price by accident, claims a
profit it could never have realized live, and quietly fails the
moment it ships. This module catches the obvious patterns by walking
the AST. It is a heuristic, not a proof; *What it does not catch* in
docs/how-to/lookahead-detector.md lists the gap.

Patterns that fire (N is always a positive integer literal):

* ``shift_negative``: ``x.shift(-N)``, with the amount given
  positionally, as ``periods=-N`` or as ``**{"periods": -N}``, and
  ``np.roll(a, -N)`` / ``np.roll(a, shift=-N)``.
* ``forward_index_add``: a subscript read whose index, or one element
  of a tuple index, is ``i + N`` or ``N + i`` (``df.iloc[i + 1]``,
  ``arr[i + 5]``, ``df.iloc[i + 1, 0]``). Writes are not flagged.
* ``forward_slice``: a slice whose upper bound is ``i + N``
  (``close[i:i + 3]``), anywhere in the file.
* ``open_upper_slice_in_callback``: ``seq[i:]`` with an open upper
  bound and a bare name as the lower bound, inside a function named
  like a per-bar callback (``_PER_BAR_CALLBACKS``). A literal lower
  bound (``buf[1:]``, ``buf[-20:]``) is not flagged.
* ``future_attr_name``: any attribute named ``next_*``, ``future_*``
  or ``lookahead_*``.

Offsets held in variables (``shift(-k)``, ``arr[i + n]``,
``df.loc[t + delta]``) are not flagged: only literals are resolved.

The detector returns a list of ``Finding`` objects; ``lrvx lint
lookahead`` prints them, and ``validate_strategy_no_lookahead``
exposes them as an MCP tool.
"""
from __future__ import annotations

import ast
import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterator, List, Optional, Sequence


# Function names that are entered when scanning hot-path callbacks.
# Any reference to a future row inside one of these is a finding.
_PER_BAR_CALLBACKS = frozenset({
    "on_trade", "on_bar", "on_book", "on_book_update", "on_touch",
    "on_tick", "on_quote", "signal", "should_enter", "should_exit",
    "compute", "update",
})


@dataclass
class Finding:
    """One detected lookahead pattern."""

    rule: str
    message: str
    line: int
    col: int
    snippet: str = ""

    def to_dict(self) -> dict:
        return {
            "rule": self.rule,
            "message": self.message,
            "line": self.line,
            "col": self.col,
            "snippet": self.snippet,
        }


@dataclass
class Report:
    """The full result of analyzing a source file."""

    path: Optional[str]
    findings: List[Finding] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return not self.findings

    def to_dict(self) -> dict:
        return {
            "path": self.path,
            "ok": self.ok,
            "findings": [f.to_dict() for f in self.findings],
        }


def _is_positive_int_literal(node: ast.AST) -> Optional[int]:
    if isinstance(node, ast.Constant) and isinstance(node.value, int):
        if node.value > 0:
            return node.value
    return None


def _is_negative_int_literal(node: ast.AST) -> Optional[int]:
    if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.USub):
        if isinstance(node.operand, ast.Constant) and isinstance(node.operand.value, int):
            if node.operand.value > 0:
                return -node.operand.value
    return None


def _dict_literal_int(node: ast.AST, key: str) -> Optional[ast.AST]:
    """If `node` is a literal ``{...}`` with a literal string key `key`,
    return the value expression for that key. Used to look inside
    ``foo(**{"periods": -1})`` -- a call whose only keyword argument is
    a starred literal dict -- since that is still statically resolvable
    even though it never shows up as an ``ast.keyword`` with a name."""
    if not isinstance(node, ast.Dict):
        return None
    for k, v in zip(node.keys, node.values):
        if isinstance(k, ast.Constant) and k.value == key:
            return v
    return None


class _Visitor(ast.NodeVisitor):
    def __init__(self, source_lines: Sequence[str]) -> None:
        self.findings: List[Finding] = []
        self._lines = list(source_lines)
        self._in_callback_depth = 0

    def _snippet(self, lineno: int) -> str:
        if 1 <= lineno <= len(self._lines):
            return self._lines[lineno - 1].strip()
        return ""

    def _add(self, rule: str, message: str, node: ast.AST) -> None:
        self.findings.append(Finding(
            rule=rule,
            message=message,
            line=getattr(node, "lineno", 0),
            col=getattr(node, "col_offset", 0),
            snippet=self._snippet(getattr(node, "lineno", 0)),
        ))

    # ── Function entry / exit (track callback depth) ─────────────

    def visit_FunctionDef(self, node: ast.FunctionDef) -> None:  # noqa: N802
        is_callback = node.name in _PER_BAR_CALLBACKS
        if is_callback:
            self._in_callback_depth += 1
        try:
            self.generic_visit(node)
        finally:
            if is_callback:
                self._in_callback_depth -= 1

    def visit_AsyncFunctionDef(self, node: ast.AsyncFunctionDef) -> None:  # noqa: N802
        # Treat the same as the sync form.
        self.visit_FunctionDef(node)  # type: ignore[arg-type]

    # ── shift(-N) on any object, and its numpy cousin np.roll(a, -N) ──

    def _shift_arg(self, node: ast.Call) -> Optional[ast.AST]:
        """The expression carrying the shift amount, from whichever
        form the call used: positional, a `periods=` keyword (pandas),
        or a `**{"periods": -1}` starred literal dict. Returns None if
        none of those apply -- including the genuinely dynamic
        `shift(-k)` / `**kwargs` forms, which are not statically
        resolvable and are left alone rather than guessed at."""
        if node.args:
            return node.args[0]
        for kw in node.keywords:
            if kw.arg == "periods":
                return kw.value
            if kw.arg is None:  # **something
                val = _dict_literal_int(kw.value, "periods")
                if val is not None:
                    return val
        return None

    def visit_Call(self, node: ast.Call) -> None:  # noqa: N802
        if isinstance(node.func, ast.Attribute) and node.func.attr == "shift":
            arg = self._shift_arg(node)
            if arg is not None:
                neg = _is_negative_int_literal(arg)
                if neg is not None:
                    self._add(
                        "shift_negative",
                        f"`.shift({neg})` reads from the future. "
                        "Pandas / numpy shift by a negative offset "
                        "is the canonical lookahead bug.",
                        node,
                    )
        elif isinstance(node.func, ast.Attribute) and node.func.attr == "roll":
            # numpy.roll(a, shift, axis=None): a negative shift moves
            # future elements into the current position, same idea as
            # pandas' negative .shift(). Positional arg 1 or the
            # `shift=` keyword.
            arg = node.args[1] if len(node.args) > 1 else None
            if arg is None:
                for kw in node.keywords:
                    if kw.arg == "shift":
                        arg = kw.value
                        break
            if arg is not None:
                neg = _is_negative_int_literal(arg)
                if neg is not None:
                    self._add(
                        "shift_negative",
                        f"`np.roll(..., {neg})` reads from the future "
                        "the same way `.shift()` with a negative offset "
                        "does: elements from ahead of the current "
                        "position are rolled backward into it.",
                        node,
                    )
        self.generic_visit(node)

    # ── df.iloc[i + N], arr[i + N], df.loc[i + N] (forward index) ─

    def _forward_index_candidates(self, slc: ast.AST) -> Iterator[ast.BinOp]:
        """Yield every BinOp that is a direct index expression under
        `slc` -- either `slc` itself, or one of its elements when the
        subscript is a tuple index (`df.iloc[i + 1, 0]`)."""
        if isinstance(slc, ast.BinOp):
            yield slc
        elif isinstance(slc, ast.Tuple):
            for elt in slc.elts:
                if isinstance(elt, ast.BinOp):
                    yield elt

    def visit_Subscript(self, node: ast.Subscript) -> None:  # noqa: N802
        # A write (`out[i + 1] = v`) or delete is not a lookahead read;
        # only flag subscripts actually being read.
        is_read = isinstance(node.ctx, ast.Load)
        slc = node.slice

        if is_read and isinstance(slc, ast.Slice):
            # Open-upper slice like `bar.history[i:]` in a per-bar
            # callback spans every future row -- but only when the
            # lower bound is itself a variable that plausibly tracks
            # "the current position" (`i`, `idx`, ...). A literal lower
            # bound (`buf[1:]`, a ring-buffer eviction; `buf[-20:]`, a
            # fixed tail window) has no relationship to "the current
            # bar" at all and is not a lookahead pattern -- treating it
            # as one was the detector's worst false-positive source.
            if (
                self._in_callback_depth > 0
                and slc.upper is None
                and isinstance(slc.lower, ast.Name)
            ):
                self._add(
                    "open_upper_slice_in_callback",
                    "Open-upper slice inside a per-bar callback "
                    "includes future rows. Cap the upper bound at "
                    "the current bar index.",
                    node,
                )
            # Forward slice like `close[i:i+3]`: the upper bound
            # itself walks past the current index, so this is a
            # lookahead read regardless of callback depth (the tape
            # replay convention that makes `future_data[i:]` dangerous
            # inside a callback does not need to hold for this to be a
            # bug -- `i + N` as an explicit upper bound always is one).
            elif slc.upper is not None:
                for cand in self._forward_index_candidates(slc.upper):
                    if isinstance(cand.op, ast.Add):
                        n = _is_positive_int_literal(cand.right) or _is_positive_int_literal(cand.left)
                        if n is not None:
                            self._add(
                                "forward_slice",
                                f"Slice upper bound `... + {n}` walks "
                                "forward from the current index; this "
                                "reads future rows.",
                                node,
                            )
                            break

        # Detect i + N (or N + i) where N is a positive int literal as
        # the index expression. Includes plain subscripts, BinOp Adds,
        # and tuple indices (`df.iloc[i + 1, 0]`).
        if is_read:
            for cand in self._forward_index_candidates(slc):
                if isinstance(cand.op, ast.Add):
                    n = _is_positive_int_literal(cand.right)
                    if n is None:
                        n = _is_positive_int_literal(cand.left)
                    if n is not None:
                        self._add(
                            "forward_index_add",
                            f"Index `... + {n}` walks forward from the "
                            "current bar; this reads a future row.",
                            node,
                        )
        self.generic_visit(node)

    # ── trade.next_*, bar.future_*, ctx.lookahead_* ──────────────

    def visit_Attribute(self, node: ast.Attribute) -> None:  # noqa: N802
        suspicious = (
            node.attr.startswith("next_")
            or node.attr.startswith("future_")
            or node.attr.startswith("lookahead_")
        )
        if suspicious:
            self._add(
                "future_attr_name",
                f"Attribute name `{node.attr}` looks like it points "
                "at a future-dated field. Verify it does not read "
                "ahead of the current bar.",
                node,
            )
        self.generic_visit(node)


def analyze_source(source: str, *, path: Optional[str] = None) -> Report:
    """Parse ``source`` and return a :class:`Report`. Syntax errors
    surface as a single ``syntax_error`` finding rather than an
    exception so the lint pipeline does not abort on a bad file."""
    try:
        tree = ast.parse(source, filename=path or "<string>")
    except SyntaxError as exc:
        return Report(
            path=path,
            findings=[Finding(
                rule="syntax_error",
                message=f"could not parse: {exc.msg}",
                line=exc.lineno or 0,
                col=exc.offset or 0,
                snippet="",
            )],
        )
    lines = source.splitlines()
    v = _Visitor(lines)
    v.visit(tree)
    return Report(path=path, findings=v.findings)


def analyze_path(path: str | Path) -> Report:
    """Read and analyze the file at ``path``. Mirrors
    :func:`analyze_source`'s "never raise, report instead" contract:
    a file that cannot be decoded as UTF-8, that no longer exists by
    the time this reads it, or that this process lacks permission for
    surfaces as a single ``read_error`` finding rather than an
    exception. The documented CI recipe (``set -e``; lint every file
    in ``strategies/*.py``) needs this -- one strategy file saved in
    the author's editor's default Latin-1 encoding, or briefly
    unreadable mid-deploy, used to take the whole lint job down with
    it, the exact failure mode ``analyze_source`` already guards
    against for a bad *parse*.
    """
    p = Path(path).expanduser()
    try:
        source = p.read_text()
    except (OSError, UnicodeDecodeError) as exc:
        return Report(
            path=str(p),
            findings=[Finding(
                rule="read_error",
                message=f"could not read {p}: {exc}",
                line=0,
                col=0,
                snippet="",
            )],
        )
    return analyze_source(source, path=str(p))


def validate_strategy_no_lookahead(code: str) -> str:
    """MCP-friendly entry point. Takes raw Python source, returns a
    JSON string with the findings. Same shape as the rest of the
    lrvx-mcp lint tools."""
    report = analyze_source(code)
    return json.dumps(report.to_dict(), indent=2, sort_keys=True)


__all__ = [
    "Finding",
    "Report",
    "analyze_source",
    "analyze_path",
    "validate_strategy_no_lookahead",
]
