"""The funding schedule every binding's funding test settles, and the line
format they print the result in.

A payment is printed as ``FUNDING <timestamp_ns> <symbol> <rate> <mark>
<position> <amount>`` with each float as the integer its IEEE-754 bits spell,
so two bindings agree exactly or the lines differ.
"""
from __future__ import annotations

import struct

T1 = 1_700_000_000_123_456_789
T2 = 1_700_000_028_800_000_001

# (timestamp_ns, symbol, rate); symbol 0 applies to every symbol.
TAPE = [(T1, 1, 0.0001), (T1, 2, -0.0002), (T2, 0, 0.0003)]
SYMBOLS = [1, 2]
POSITIONS = [2.0, -3.0]
MARKS = [50000.0, 2000.0]


def float_bits(x: float) -> int:
    return struct.unpack("<q", struct.pack("<d", x))[0]


def line(timestamp_ns: int, symbol: int, rate: float, mark: float, position: float,
         amount: float) -> str:
    return (f"FUNDING {timestamp_ns} {symbol} {float_bits(rate)} {float_bits(mark)} "
            f"{float_bits(position)} {float_bits(amount)}")


def funding_lines(output: str) -> list[str]:
    return [ln.strip() for ln in output.splitlines() if ln.startswith("FUNDING ")]
