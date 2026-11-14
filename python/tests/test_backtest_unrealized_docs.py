"""The pages that describe a position open at the end of a backtest say how
it is valued.

0.12.0 documented the gap: docs/reference/python/strategy.md ("A position
open at the end": "nothing values the open position, so its unrealized P&L
is in none of the numbers") and docs/explanation/reference-backtest.md ("its
unrealized P&L is not counted"). The stats now carry ``unrealized_pnl`` and
``final_equity`` and the runner lists ``open_positions()``; the pages, and
the typings every binding publishes, have to say so. ``net_pnl`` keeps its
meaning, so its row stays.
"""
from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
STRATEGY_MD = REPO / "docs" / "reference" / "python" / "strategy.md"
REFERENCE_MD = REPO / "docs" / "explanation" / "reference-backtest.md"


def _section(text: str, heading: str) -> str:
    m = re.search(rf"^(#+) {re.escape(heading)}\s*$", text, re.M)
    assert m, f"heading {heading!r} not found"
    level = len(m.group(1))
    rest = text[m.end():]
    end = re.search(rf"^#{{1,{level}}} ", rest, re.M)
    return rest[: end.start()] if end else rest


def _table_keys(section: str) -> set[str]:
    return set(re.findall(r"^\|\s*`([a-z_]+)`\s*\|", section, re.M))


def test_stats_keys_table_lists_the_new_keys():
    keys = _table_keys(_section(STRATEGY_MD.read_text(), "Stats dict keys"))
    assert {"unrealized_pnl", "final_equity"} <= keys, keys
    assert {"net_pnl", "total_fees", "final_capital"} <= keys, "existing rows must stay"


def test_open_position_section_says_how_it_is_valued():
    section = _section(STRATEGY_MD.read_text(), "A position open at the end")
    assert "in none of the numbers" not in section
    assert "nothing values the open position" not in section
    for needle in ("unrealized_pnl", "final_equity", "open_positions()"):
        assert needle in section, needle
    # The mark rule, per replay mode.
    assert re.search(r"bar replay[^.]*close", section, re.I | re.S), \
        "say that bar replay marks at the bar close"
    assert re.search(r"tick replay[^.]*last trade", section, re.I | re.S), \
        "say that tick replay marks at the last trade"


def test_reference_backtest_rule_no_longer_says_it_is_not_counted():
    text = REFERENCE_MD.read_text()
    assert "its unrealized P&L is not counted" not in text
    assert "unrealized_pnl" in text


def test_node_typings_carry_the_new_stats_and_accessor():
    dts = (REPO / "node" / "index.d.ts").read_text()
    runner_stats = _interface(dts, "RunnerStats")
    stats = _interface(dts, "BacktestStats")
    for body in (runner_stats, stats):
        assert re.search(r"\bunrealizedPnl\s*:\s*number", body)
        assert re.search(r"\bfinalEquity\s*:\s*number", body)
    assert re.search(r"\bopenPositions\(\)\s*:", _class(dts, "BacktestRunner"))
    assert re.search(r"\bopenPositions\(\)\s*:", _class(dts, "BacktestResult"))
    assert re.search(r"\bmarkPrice\(", _class(dts, "BacktestResult"))


def test_quickjs_typings_carry_the_new_stats_and_accessor():
    dts = (REPO / "quickjs" / "types" / "lrvx.d.ts").read_text()
    stats = _interface(dts, "BacktestStats")
    assert re.search(r"\bunrealizedPnl\s*:\s*number", stats)
    assert re.search(r"\bfinalEquity\s*:\s*number", stats)
    result = _class(dts, "BacktestResult")
    assert re.search(r"\bmarkPrice\(", result)
    assert re.search(r"\bopenPositions\(\)\s*:", result)


def _block(text: str, pattern: str) -> str:
    m = re.search(pattern, text)
    assert m, pattern
    depth, i = 0, text.index("{", m.end() - 1)
    start = i
    while True:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i]
        i += 1


def _interface(text: str, name: str) -> str:
    return _block(text, rf"(?:export |declare )?interface {name}\b[^{{]*\{{")


def _class(text: str, name: str) -> str:
    return _block(text, rf"(?:export |declare )?class {name}\b[^{{]*\{{")
