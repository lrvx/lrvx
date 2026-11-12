"""SimulatedExecutor.set_queue_model refuses a model name it does not know.

A typo used to read as "none": queue simulation silently off, every resting
limit filling as if it stood at the front. The valid names come from the one
table the bindings share, and the message lists them.
"""
from __future__ import annotations

import pytest

import lrvx

NAMES = ("none", "tob", "full", "pro_rata", "pro_rata_with_fifo", "top_pro_lmm",
         "pro_rata_with_priority")


@pytest.mark.parametrize("bad", ["bogus", "FULL", "pro-rata", ""])
def test_unknown_queue_model_is_refused_with_the_valid_names(bad):
    ex = lrvx.SimulatedExecutor()
    with pytest.raises(lrvx.LrvxError) as err:
        ex.set_queue_model(bad, 1)
    msg = str(err.value)
    assert f"'{bad}'" in msg
    for name in NAMES:
        assert f"'{name}'" in msg, name


@pytest.mark.parametrize("name", NAMES)
def test_every_valid_queue_model_is_accepted(name):
    lrvx.SimulatedExecutor().set_queue_model(name, 1)
