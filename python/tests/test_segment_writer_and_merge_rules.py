"""A 0 MB segment size is the default, a partial merge is not a success,
and a validation issue says which kind it is.
"""
from __future__ import annotations

import lrvx

SEC = 1_000_000_000


def _write(writer, n=5):
    for i in range(n):
        ts = (10 + i) * SEC
        writer.write_trade(ts, ts, 100.0 + i, 1.0, i + 1, 1, True)
    writer.close()


def _segments(directory):
    return sorted(p for p in directory.iterdir() if p.suffix == ".lrvx")


def test_a_data_writer_with_zero_segment_size_writes_one_segment(tmp_path):
    tape = tmp_path / "zero"
    _write(lrvx.DataWriter(str(tape), max_segment_mb=0))
    assert len(_segments(tape)) == 1, "0 is the 256 MB default, not a bound of 0 bytes"
    assert lrvx.validate_dataset(str(tape))["total_events"] == 5


def test_a_compressed_data_writer_with_zero_segment_size_writes_one_segment(tmp_path):
    tape = tmp_path / "zero_lz4"
    _write(lrvx.DataWriter(str(tape), max_segment_mb=0, compression="lz4"), n=200)
    assert len(_segments(tape)) == 1
    assert lrvx.validate_dataset(str(tape))["total_events"] == 200


def test_a_recorder_hook_with_zero_segment_size_writes_one_segment(tmp_path):
    tape = tmp_path / "hook"
    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("test", "BTCUSDT", tick_size=0.01)
    hook = lrvx.BinaryLogRecorderHook(str(tape), max_segment_mb=0)
    runner = lrvx.Runner(reg, on_signal=lambda _: None)
    runner.set_market_data_recorder(hook)
    runner.start()
    for i in range(5):
        runner.on_trade(sym, 100.0 + i, 1.0, True, (10 + i) * SEC)
    runner.stop()
    hook.flush()
    del runner, hook
    assert len(_segments(tape)) == 1


def _tape_with_a_bad_input(tmp_path):
    tape = tmp_path / "tape"
    _write(lrvx.DataWriter(str(tape)))
    bad = tape / "zz_bad.lrvx"
    bad.write_bytes(b"x" * 256)
    return tape, bad


def test_a_partial_merge_dir_is_not_a_success(tmp_path):
    tape, bad = _tape_with_a_bad_input(tmp_path)
    result = lrvx.merge_dir(str(tape), str(tmp_path / "merged"))
    assert result["success"] is False
    assert result["segments_merged"] == 1 and result["events_written"] == 5
    assert result["errors"] == ["Cannot open: " + str(bad)]


def test_a_partial_merge_is_not_a_success(tmp_path):
    tape, bad = _tape_with_a_bad_input(tmp_path)
    result = lrvx.merge([str(p) for p in _segments(tape)], str(tmp_path / "merged"))
    assert result["success"] is False
    assert result["segments_merged"] == 1 and result["events_written"] == 5
    assert result["errors"] == ["Cannot open: " + str(bad)]


def test_a_whole_merge_is_a_success(tmp_path):
    tape = tmp_path / "tape"
    _write(lrvx.DataWriter(str(tape)))
    result = lrvx.merge_dir(str(tape), str(tmp_path / "merged"))
    assert result["success"] is True and result["errors"] == []


def test_a_segment_issue_carries_its_type(tmp_path):
    missing = tmp_path / "no_such.lrvx"
    result = lrvx.validate(str(missing))
    (issue,) = result["issues"]
    assert issue["type"] == lrvx.ISSUE_FILE_NOT_FOUND
    assert issue["message"] == "File not found: " + str(missing)

    garbage = tmp_path / "garbage.lrvx"
    garbage.write_bytes(b"x" * 256)
    types = [i["type"] for i in lrvx.validate(str(garbage))["issues"]]
    assert lrvx.ISSUE_INVALID_MAGIC in types


def test_a_dataset_issue_carries_its_type(tmp_path):
    result = lrvx.validate_dataset(str(tmp_path / "no_such_directory"))
    (issue,) = result["issues"]
    assert issue["type"] == lrvx.ISSUE_FILE_NOT_FOUND


def test_the_issue_type_constants_are_the_enum_values():
    assert lrvx.ISSUE_INVALID_MAGIC == 0
    assert lrvx.ISSUE_FILE_NOT_FOUND != lrvx.ISSUE_FILE_READ_ERROR
    assert lrvx.ISSUE_SEGMENT_RANGE_OVERLAP > lrvx.ISSUE_FILE_READ_ERROR
