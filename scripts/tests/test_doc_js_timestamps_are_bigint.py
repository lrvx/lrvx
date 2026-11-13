"""The JavaScript examples of the .lrvxrun how-to write timestamps as BigInt.

The QuickJS example passed `runStartedNs: 1700000000000000000`, a Number
past 2^53, where a Number no longer holds every integer: the example worked
only because that one value happens to be a multiple of 256.
"""

from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DOC = REPO / "docs" / "how-to" / "lrvxrun.md"


def lossy_literals(text: str) -> list[str]:
    """Integer literals above 2^53, with no BigInt suffix, in JS blocks."""
    blocks = re.findall(r"```(?:javascript|typescript|js|ts)\n(.*?)```", text, re.S)
    assert blocks, "the how-to has JavaScript examples"
    return [lit for block in blocks
            for lit in re.findall(r"(?<![\w.])(\d{16,})(?![\dn_])", block)
            if int(lit) > 2 ** 53]


def test_no_integer_past_2_53_is_written_as_a_number():
    assert lossy_literals(DOC.read_text(encoding="utf-8")) == []
