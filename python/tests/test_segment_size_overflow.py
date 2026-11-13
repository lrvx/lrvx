"""A segment size in megabytes whose byte count does not fit in 64 bits.

``max_segment_mb * 2**20`` wrapped: ``2**44 + 1`` megabytes became a
segment size of one megabyte, and a recording meant to stay in one file
rotated every megabyte. The value is refused instead, and the largest
that fits is still taken.
"""
from __future__ import annotations

import pytest

import lrvx

LARGEST = (2**64 - 1) >> 20


@pytest.mark.parametrize("make", [
    lambda d, mb: lrvx.DataWriter(str(d), max_segment_mb=mb),
    lambda d, mb: lrvx.BinaryLogRecorderHook(str(d), max_segment_mb=mb),
], ids=["DataWriter", "BinaryLogRecorderHook"])
def test_a_megabyte_count_that_overflows_is_refused(tmp_path, make):
    with pytest.raises(ValueError) as excinfo:
        make(tmp_path / "over", LARGEST + 2)
    assert "max_segment_mb %d" % (LARGEST + 2) in str(excinfo.value)
    assert str(LARGEST) in str(excinfo.value)

    make(tmp_path / "fits", LARGEST)
