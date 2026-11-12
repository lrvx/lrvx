"""lrvx.last_error() says why a call that returns False or 0 failed.

FundingSchedule.load_tape, recompress, extract_symbols and extract_time_range
report failure through their return value, and 0 is also what an extract that
matches nothing returns.
"""
from __future__ import annotations

import lrvx

SEC = 1_000_000_000


def _segment(tmp_path):
    tape = tmp_path / "tape"
    writer = lrvx.DataWriter(str(tape))
    for i in range(5):
        ts = (10 + i) * SEC
        writer.write_trade(ts, ts, 100.0 + i, 1.0, i + 1, 1, True)
    writer.close()
    (segment,) = [p for p in tape.iterdir() if p.suffix == ".lrvx"]
    return str(segment)


def test_load_tape_names_the_file_it_could_not_open(tmp_path):
    missing = str(tmp_path / "no_such_file")
    schedule = lrvx.FundingSchedule()
    assert schedule.load_tape(missing) is False
    assert missing in lrvx.last_error()

    tape = tmp_path / "funding.csv"
    tape.write_text("timestamp_ns,symbol,funding_rate\n1000,1,0.0001\n")
    assert schedule.load_tape(str(tape)) is True
    assert lrvx.last_error() == ""
    assert schedule.settlement_timestamps() == [1000]


def test_segment_ops_name_the_input_they_could_not_open(tmp_path):
    missing = str(tmp_path / "no_such_file")
    out = str(tmp_path / "out.lrvx")

    assert lrvx.recompress(missing, out) is False
    assert missing in lrvx.last_error()
    assert "lrvx_segment_recompress" in lrvx.last_error()

    assert lrvx.extract_symbols(missing, out, [1]) == 0
    assert missing in lrvx.last_error()
    assert "lrvx_segment_extract_symbols" in lrvx.last_error()

    assert lrvx.extract_time_range(missing, out, 0, SEC) == 0
    assert missing in lrvx.last_error()


def test_an_extract_that_matches_nothing_leaves_no_error(tmp_path):
    segment = _segment(tmp_path)
    missing = str(tmp_path / "no_such_file")

    lrvx.recompress(missing, str(tmp_path / "x.lrvx"))
    assert lrvx.extract_symbols(segment, str(tmp_path / "none.lrvx"), [99]) == 0
    assert lrvx.last_error() == ""

    lrvx.recompress(missing, str(tmp_path / "x.lrvx"))
    assert lrvx.extract_time_range(segment, str(tmp_path / "none2.lrvx"), 500 * SEC,
                                   600 * SEC) == 0
    assert lrvx.last_error() == ""


def test_the_operations_still_do_their_work(tmp_path):
    segment = _segment(tmp_path)
    assert lrvx.extract_symbols(segment, str(tmp_path / "all.lrvx"), [1]) == 5
    assert lrvx.extract_time_range(segment, str(tmp_path / "some.lrvx"), 11 * SEC, 12 * SEC) == 2
    assert lrvx.recompress(segment, str(tmp_path / "lz4.lrvx"), "lz4") is True
    assert lrvx.recompress(segment, str(tmp_path / "raw.lrvx"), "none") is True
    assert lrvx.last_error() == ""
    assert lrvx.extract_symbols(str(tmp_path / "lz4.lrvx"), str(tmp_path / "back.lrvx"), [1]) == 5
