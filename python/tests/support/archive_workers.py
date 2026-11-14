"""Helpers for the parallel archive conversion tests.

The functions at module level are handed to worker processes, so they
must be importable by name from a fresh interpreter (the ``spawn``
start method pickles a function as its module and qualified name).

Run as a script, this module performs one range import in its own
process and prints the outcome as JSON, so a test can put a timeout on
an import that might hang:

    python archive_workers.py '<spec json>'

where the spec is ``{"module": ..., "func": ..., "args": [...],
"kwargs": {...}}``.
"""
from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path


class DayFailed(RuntimeError):
    """Raised by ``fail_on`` for the item it is told to fail on."""


def pid_of(item):
    """The id of the process that converted ``item``."""
    return os.getpid()


def tagged(item):
    """A transform of ``item`` that shows which item it came from."""
    return ("done", item, item * 3)


def fail_on(item):
    """Fail on item 3, after a short pause on the others so the failure
    lands while other items are in flight."""
    if item == 3:
        raise DayFailed(f"day {item} does not convert")
    time.sleep(0.05)
    return item


class NeedsTwoArgs(Exception):
    """An exception that does not survive pickling: unpickling calls
    ``NeedsTwoArgs(message)``, which is one argument short."""

    def __init__(self, day, why):
        super().__init__(f"day {day}: {why}")
        self.day = day
        self.why = why


def fail_unpicklable_on(item):
    """Like ``fail_on``, but item 3 raises an exception that cannot make
    the trip back from a worker process."""
    if item == 3:
        raise NeedsTwoArgs(item, "bad header")
    time.sleep(0.05)
    return item


def mark_started(item):
    """``item`` is ``(directory, index)``: leaves a file named ``index`` in
    ``directory``, so the caller can count the items a worker has begun."""
    directory, index = item
    (Path(directory) / str(index)).touch()
    return index


def _run(spec: dict) -> dict:
    import importlib

    module = importlib.import_module(spec["module"])
    fn = getattr(module, spec["func"])
    try:
        stats = fn(*spec["args"], **spec["kwargs"])
    except BaseException as exc:  # reported, not raised: the parent decides
        return {"ok": False,
                "type": type(exc).__name__,
                "mro": [c.__name__ for c in type(exc).__mro__],
                "message": str(exc)}
    return {"ok": True, "stats": dict(vars(stats))}


def main(argv) -> int:
    repo = Path(__file__).resolve().parents[3]
    for cand in ("build/python", "build-py312/python"):
        p = repo / cand
        if p.is_dir():
            sys.path.insert(0, str(p))
            break
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    print(json.dumps(_run(json.loads(argv[1]))))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
