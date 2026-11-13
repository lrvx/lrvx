"""SymbolRegistry reads back the tick and the lot an entry holds.

add_symbol returned them once, on the Symbol; nothing read the entry again, so
a caller holding a registry (or only an id) could not ask what a symbol's
orders are checked against.
"""
from __future__ import annotations

import pytest

import lrvx


def test_getters_read_what_was_registered():
    reg = lrvx.SymbolRegistry()
    plain = reg.add_symbol("test", "PLAIN")
    ticked = reg.add_symbol("test", "TICKED", tick_size=0.5)
    both = reg.add_symbol("test", "BOTH", tick_size=0.01, lot_size=0.001)

    assert reg.tick_size(plain) == pytest.approx(0.01), "the book's default"
    assert reg.tick_size_declared(plain) is False
    assert reg.lot_size(plain) is None

    assert reg.tick_size(ticked) == pytest.approx(0.5)
    assert reg.tick_size_declared(ticked) is True
    assert reg.lot_size(ticked) is None

    # An explicit 0.01 is declared, though it equals the default.
    assert reg.tick_size(both) == pytest.approx(0.01)
    assert reg.tick_size_declared(both) is True
    assert reg.lot_size(both) == pytest.approx(0.001)

    # A Symbol and its id are the same question.
    assert reg.lot_size(both.id) == reg.lot_size(both)
    assert reg.tick_size(int(ticked)) == reg.tick_size(ticked)


def test_an_unregistered_id_is_refused():
    reg = lrvx.SymbolRegistry()
    reg.add_symbol("test", "BTC", tick_size=0.5)
    for getter in (reg.tick_size, reg.tick_size_declared, reg.lot_size):
        with pytest.raises(ValueError, match="9999"):
            getter(9999)
