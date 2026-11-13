"""Validation says why it could not read its input.

A dataset directory that is not there validated as "not valid" with nothing
else, and a path that is a file raised whatever the directory walk threw. A
segment that cannot be opened was reported without its path.
"""
from __future__ import annotations

import os

import pytest

import lrvx

SEC = 1_000_000_000


def _tape(tmp_path):
    tape = tmp_path / "tape"
    writer = lrvx.DataWriter(str(tape))
    for i in range(5):
        ts = (10 + i) * SEC
        writer.write_trade(ts, ts, 100.0 + i, 1.0, i + 1, 1, True)
    writer.close()
    return tape


def test_a_missing_dataset_directory_is_an_issue_that_names_it(tmp_path):
    missing = str(tmp_path / "no_such_directory")
    result = lrvx.validate_dataset(missing)
    assert result["valid"] is False
    assert result["total_segments"] == 0
    assert result["total_errors"] >= 1
    assert [i["message"] for i in result["issues"]] == ["Directory not found: " + missing]


def test_a_dataset_path_that_is_a_file_is_an_issue_not_an_exception(tmp_path):
    a_file = tmp_path / "a_file"
    a_file.write_text("not a directory")
    result = lrvx.validate_dataset(str(a_file))
    assert result["valid"] is False
    (issue,) = result["issues"]
    assert issue["message"].startswith("Cannot read directory: " + str(a_file))


def test_a_readable_dataset_has_no_dataset_issue(tmp_path):
    result = lrvx.validate_dataset(str(_tape(tmp_path)))
    assert result["valid"] is True
    assert result["issues"] == []


def test_a_segment_that_cannot_be_opened_is_named(tmp_path):
    tape = _tape(tmp_path)
    (segment,) = [p for p in tape.iterdir() if p.suffix == ".lrvx"]
    os.chmod(segment, 0)
    try:
        try:
            open(segment, "rb").close()
        except PermissionError:
            pass
        else:
            pytest.skip("this user can read a file with no permission bits")
        result = lrvx.validate(str(segment))
    finally:
        os.chmod(segment, 0o600)
    assert result["valid"] is False
    assert "Cannot open file for reading: " + str(segment) in [
        i["message"] for i in result["issues"]]


def test_merge_dir_lists_the_inputs_it_skipped(tmp_path):
    tape = _tape(tmp_path)
    (segment,) = [p for p in tape.iterdir() if p.suffix == ".lrvx"]
    bad = tape / "zz_bad.lrvx"
    bad.write_bytes(b"x" * 256)
    result = lrvx.merge_dir(str(tape), str(tmp_path / "merged"))
    assert result["segments_merged"] == 1
    assert result["errors"] == ["Cannot open: " + str(bad)]
    assert result["output_path"].startswith(str(tmp_path / "merged"))
    assert str(segment) not in " ".join(result["errors"])
