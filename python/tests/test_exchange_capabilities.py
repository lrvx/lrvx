"""
python/tests/test_exchange_capabilities.py — ExchangeCapabilities mirrors
every C++ capability flag, so a Python executor can advertise what its
venue supports. The flag set is derived from exchange_capabilities.h, so
a new C++ flag with no Python mirror fails here until the binding
carries it.
"""

import os
import re
import sys

build_dir = os.path.join(os.path.dirname(__file__), '..', '..', 'build', 'python')
sys.path.insert(0, os.path.abspath(build_dir))

import lrvx

CAPS_HEADER = os.path.join(os.path.dirname(__file__), '..', '..',
                           'include', 'lrvx', 'execution', 'exchange_capabilities.h')


def cpp_flags():
    with open(CAPS_HEADER) as f:
        names = re.findall(r'bool\s+supports(\w+)\s*\{', f.read())

    def snake(name):
        # StopMarket -> stop_market, OCO -> oco, BatchSubmit -> batch_submit
        words = re.findall(r'[A-Z]+(?![a-z])|[A-Z][a-z]*', name)
        return '_'.join(w.lower() for w in words)

    return {snake(n) for n in names}


def test_batch_submit_defaults_false_and_roundtrips():
    caps = lrvx.ExchangeCapabilities()
    assert caps.batch_submit is False
    caps.batch_submit = True
    assert caps.batch_submit is True


def test_every_cpp_flag_has_a_python_mirror():
    expected = cpp_flags()
    assert len(expected) >= 15, f"suspiciously few flags parsed: {sorted(expected)}"
    have = {a for a in dir(lrvx.ExchangeCapabilities()) if not a.startswith('_')}
    missing = expected - have
    assert not missing, f"C++ flags without a Python mirror: {sorted(missing)}"
