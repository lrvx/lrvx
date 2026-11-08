"""What the Python binding owes a callback that raises.

Three separate contracts live in this file, all of them about a Python
exception meeting the C ABI.

1. A pre-trade gate (RiskManager / KillSwitch / OrderValidator) whose
   Python callable raises must fail *closed*.

   `include/lrvx/capi/lrvx_capi_spec.hpp:2306` fixes what the two return
   values mean:

       "Returning 0 (deny) drops the signal entirely; returning 1 (allow)
        lets it propagate."

   and `include/lrvx/capi/lrvx_capi.h:47` fixes what a call that could
   not complete has to do:

       "Exceptions. None escape. The engine underneath is C++ and throws;
        unwinding out of a frame with C linkage is undefined behaviour, so
        every function on this page catches. A call that was stopped this
        way returns its failure value and leaves a description behind:
        lrvx_last_error_code() and lrvx_last_error_message() report it for
        the calling thread."

   A gate that raised did not complete, so the two halves of that
   sentence apply to it: it returns *its failure value*, which for a gate
   is 0 = deny, and it leaves a description behind. The Python binding
   exposes no `lrvx_last_error_*` accessor (`grep -n last_error
   python/*.h python/*.cpp` is empty), so "leaves a description behind"
   can only reach a Python caller two ways: the exception is re-raised
   out of the call that drove the gate, or it is reported through the
   binding's own error path, `lrvx.set_log_callback` (the C API's
   "Logger -- process-wide log redirection callback"). This file accepts
   either, and requires one of them.

   Today neither half holds: `python/hook_bindings.h:364` (also `:420`,
   `:475`) pre-initialises `uint8_t result = 1` and `invokeUnderGil`
   (`:220`) swallows the exception, so a raising gate returns ALLOW and
   says nothing.

2. A strategy callback that raises must not take the process or the
   engine with it. `python/hook_bindings.h:216` states the rule the
   sibling header does not follow:

       "Exceptions from Python are caught and printed -- propagating them
        across the C ABI boundary is undefined behaviour."

   `python/strategy_bindings.h` has no `catch` at all, so the nine
   callbacks registered at `:774-:782` let a Python exception unwind
   through a C function pointer.

3. `Runner.on_trade` / `on_book_snapshot` / `on_bar`
   (`python/strategy_bindings.h:2991-:2995`) publish into an EventBus
   that busy-waits on consumer gating once the ring is full
   (`include/lrvx/util/eventing/event_bus.h:747`), and they do it holding
   the GIL. The consumer thread needs the GIL to finish the Python
   callback that would drain the ring, so the two threads wedge.

Every subprocess case runs under a wall-clock timeout so a wedge is
reported as a failure instead of hanging the suite; a timeout is also the
only safe way to observe an aborted process.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import textwrap

import pytest

import lrvx

# The directory that has to be on sys.path for `import lrvx` to find
# this build -- the parent of the package directory, which is what CI
# passes as PYTHONPATH=build/python. Derived from the loaded module so a
# subprocess started from any cwd resolves the same build.
BUILD_DIR = os.path.dirname(os.path.dirname(os.path.abspath(lrvx.__file__)))

DEFAULT_TIMEOUT = 90
MARKER_TEXT = "CALLBACK_BLEW_UP"

# docs/bindings/python.md, "When a callback raises": "level: 0=info,
# 1=warn, 2=error". callback_guard.h reports a callback error at error.
LOG_LEVEL_ERROR = 2


class Outcome:
    """What a driver subprocess reported back."""

    def __init__(self, returncode, payload, output):
        self.returncode = returncode
        self.payload = payload
        self.output = output

    @property
    def timed_out(self):
        return self.returncode is None

    def surfaced(self, marker):
        """True when the exception text reached Python at all.

        Either re-raised out of the call that drove the callback, or
        reported through the binding's error path (set_log_callback).
        """
        if marker in self.payload.get("raised", ""):
            return True
        return any(marker in message for _level, message in self.log)

    @property
    def log(self):
        """The (level, message) pairs the sink collected."""
        return [tuple(entry) for entry in self.payload.get("log", [])]

    @property
    def extra(self):
        return self.payload.get("extra", {})

    def reports(self, marker):
        """Log entries carrying `marker`, as (level, message) pairs."""
        return [(level, message) for level, message in self.log if marker in message]

    def describe(self):
        return (f"returncode={self.returncode} payload={self.payload}\n"
                f"--- subprocess output ---\n{self.output}")


def drive(body, timeout=DEFAULT_TIMEOUT, marker=MARKER_TEXT):
    """Run `body` against this build in a fresh interpreter.

    Out of process on purpose: an exception crossing the C ABI can abort
    the process or kill a bus consumer, and neither is observable -- nor
    survivable -- from inside the pytest process.

    The body gets `lrvx`, `MARKER`, `fired`, `alive`, `raised` and
    `LOG` in scope and ends by calling `report()`.
    """
    prelude = textwrap.dedent(f"""
        import json, sys, time
        sys.path.insert(0, {BUILD_DIR!r})
        import lrvx

        MARKER = {marker!r}
        LOG = []
        # The level is half the contract: callback_guard.h reports at
        # error level, and docs/bindings/python.md spells the mapping
        # out -- "level: 0=info, 1=warn, 2=error". A sink that drops the
        # level would make an Error->Info regression invisible.
        lrvx.set_log_callback(lambda level, msg: LOG.append([int(level), str(msg)]))

        fired = [0]     # times the callback under test ran
        alive = [0]     # times the engine reached the strategy afterwards
        raised = [""]   # what came back out of the driving call
        extra = {{}}     # whatever a single driver needs to report back

        def report():
            # Detach before the report: a Python log callback still
            # installed at interpreter shutdown segfaults this build, and
            # that would masquerade as the crash this file is looking for.
            lrvx.set_log_callback(None)
            sys.stdout.write("RESULT " + json.dumps({{
                "fired": fired[0],
                "alive": alive[0],
                "raised": raised[0],
                "log": LOG,
                "extra": extra,
            }}) + "\\n")
            sys.stdout.flush()
    """)
    script = prelude + textwrap.dedent(body)
    try:
        done = subprocess.run([sys.executable, "-c", script],
                              capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as expired:
        out = (expired.stdout or b"").decode(errors="replace") if isinstance(
            expired.stdout, bytes) else (expired.stdout or "")
        err = (expired.stderr or b"").decode(errors="replace") if isinstance(
            expired.stderr, bytes) else (expired.stderr or "")
        return Outcome(None, {}, f"timed out after {timeout}s\n{out}{err}")

    output = done.stdout + done.stderr
    payload = {}
    for line in done.stdout.splitlines():
        if line.startswith("RESULT "):
            payload = json.loads(line[len("RESULT "):])
    return Outcome(done.returncode, payload, output)


# ── 1. Pre-trade gates fail closed ───────────────────────────────────

# Indented to match the inline bodies it is concatenated with: drive()
# dedents the whole script by its common prefix.
_SYNC_RUNNER_SETUP = """
        reg = lrvx.SymbolRegistry()
        sym = reg.add_symbol("test", "BTC", 0.01)
        signals = []
        runner = lrvx.Runner(reg, lambda s: signals.append(s), threaded=False)

        class S(lrvx.Strategy):
            def on_trade(self, ctx, trade):
                self.emit_market_buy(sym, 1.0)

        runner.add_strategy(S(symbols=[sym]))
"""

GATES = [
    # (gate id, base class, method, Runner attach, BacktestRunner attach)
    ("risk_manager", "RiskManager", "allow", "set_risk_manager", "set_risk_manager"),
    ("kill_switch", "KillSwitch", "check", "set_kill_switch", "set_kill_switch"),
    ("order_validator", "OrderValidator", "validate", "set_order_validator",
     "set_order_validator"),
]


@pytest.mark.parametrize("gate,base,method,attach,_bt_attach", GATES,
                         ids=[g[0] for g in GATES])
def test_gate_denies_when_the_python_callable_raises(gate, base, method, attach,
                                                     _bt_attach):
    """A gate that raised did not answer, so the signal must be dropped.

    lrvx_capi.h:47 -- "A call that was stopped this way returns its
    failure value"; lrvx_capi_spec.hpp:2306 -- "Returning 0 (deny) drops
    the signal entirely". The failure value of a gate is deny.

    Red today: hook_bindings.h:364/:420/:475 start from `uint8_t result =
    1` and invokeUnderGil swallows the raise, so the order goes through.
    """
    outcome = drive(_SYNC_RUNNER_SETUP + f"""
        class Gate(lrvx.{base}):
            def {method}(self, sig):
                fired[0] += 1
                raise RuntimeError(MARKER)

        runner.{attach}(Gate())
        runner.start()
        try:
            runner.on_trade(sym, 100.0, 1.0, True, 1_000)
        except BaseException as exc:
            raised[0] = repr(exc)
        runner.stop()
        alive[0] = len(signals)
        report()
    """)

    assert not outcome.timed_out, outcome.describe()
    assert outcome.returncode == 0, (
        f"the {gate} raise must not abort the process\n{outcome.describe()}")
    assert outcome.payload.get("fired", 0) >= 1, (
        f"the {gate} callable never ran, so this case proves nothing\n"
        f"{outcome.describe()}")
    assert outcome.payload.get("alive") == 0, (
        f"a {gate} that raises must deny; "
        f"{outcome.payload.get('alive')} signal(s) reached on_signal\n"
        f"{outcome.describe()}")


@pytest.mark.parametrize("gate,base,method,attach,_bt_attach", GATES,
                         ids=[g[0] for g in GATES])
def test_gate_exception_is_surfaced(gate, base, method, attach, _bt_attach):
    """Denying silently is half the contract; the description must survive.

    lrvx_capi.h:47 -- the stopped call "leaves a description behind:
    lrvx_last_error_code() and lrvx_last_error_message() report it for
    the calling thread." The Python binding exposes neither accessor, so
    the description has to reach Python either as a re-raise out of the
    driving call or through set_log_callback, the binding's error path.

    Red today: invokeUnderGil (hook_bindings.h:220) catches
    py::error_already_set and calls PyErr_Print(), which finds no error
    set on the thread state -- pybind11 has already stashed it in the
    exception object -- so nothing is printed, logged or raised.
    """
    outcome = drive(_SYNC_RUNNER_SETUP + f"""
        class Gate(lrvx.{base}):
            def {method}(self, sig):
                fired[0] += 1
                raise RuntimeError(MARKER)

        runner.{attach}(Gate())
        runner.start()
        try:
            runner.on_trade(sym, 100.0, 1.0, True, 1_000)
        except BaseException as exc:
            raised[0] = repr(exc)
        runner.stop()
        alive[0] = len(signals)
        report()
    """)

    assert not outcome.timed_out, outcome.describe()
    assert outcome.payload.get("fired", 0) >= 1, outcome.describe()
    assert outcome.surfaced(MARKER_TEXT), (
        f"the {gate} exception vanished: it was neither re-raised out of "
        f"Runner.on_trade nor reported through set_log_callback\n"
        f"{outcome.describe()}")


_BACKTEST_GATE_SETUP = """
        import numpy as np

        reg = lrvx.SymbolRegistry()
        sym = reg.add_symbol("backtest", "BTCUSDT", tick_size=0.01)
        bt = lrvx.BacktestRunner(reg, fee_rate=0.0, initial_capital=100_000.0)

        fills = []

        class L(lrvx.ExecutionListener):
            def on_filled(self, order):
                fills.append(order)

        bt.add_execution_listener(L())

        class S(lrvx.Strategy):
            def on_bar(self, ctx, bar):
                self.market_buy(0.5)
"""

_BACKTEST_GATE_RUN = """
        bt.set_strategy(S([sym]))
        n = 4
        start = np.array([i * 60_000_000_000 for i in range(n)], dtype=np.int64)
        try:
            bt.run_bars(start, start + 60_000_000_000,
                        np.full(n, 100.0), np.full(n, 101.0),
                        np.full(n, 99.0), np.full(n, 100.5),
                        np.full(n, 10.0), symbol="BTCUSDT")
        except BaseException as exc:
            raised[0] = repr(exc)
        alive[0] = len(fills)
        report()
"""


@pytest.mark.parametrize("gate,base,method,_attach,bt_attach", GATES,
                         ids=[g[0] for g in GATES])
def test_backtest_gate_denies_when_the_python_callable_raises(gate, base, method,
                                                              _attach, bt_attach):
    """The same gate contract on the BacktestRunner attach points.

    BacktestRunner does not go through the C ABI: it wraps
    lrvx::BacktestRunner directly, so each gate arrives through its own
    C++ adapter -- PyRiskManagerCxxAdapter, PyKillSwitchCxxAdapter,
    PyOrderValidatorCxxAdapter (hook_bindings.h). Three adapters, three
    `= false` defaults, so all three are pinned here and not just the
    risk manager: the kill switch and the validator have their own
    copy of the rule and nothing else in the suite reads them.
    """
    outcome = drive(_BACKTEST_GATE_SETUP + f"""
        class Gate(lrvx.{base}):
            def {method}(self, sig):
                fired[0] += 1
                raise RuntimeError(MARKER)

        bt.{bt_attach}(Gate())
    """ + _BACKTEST_GATE_RUN)

    assert not outcome.timed_out, outcome.describe()
    assert outcome.returncode == 0, outcome.describe()
    assert outcome.payload.get("fired", 0) >= 1, (
        f"the backtest {gate} never ran\n{outcome.describe()}")
    assert outcome.payload.get("alive") == 0, (
        f"a backtest {gate} that raises must deny; "
        f"{outcome.payload.get('alive')} fill(s) happened\n{outcome.describe()}")
    assert outcome.reports(MARKER_TEXT), (
        f"the backtest {gate} exception was not reported\n{outcome.describe()}")


@pytest.mark.parametrize("gate,base,method,_attach,bt_attach", GATES,
                         ids=[g[0] for g in GATES])
def test_backtest_gate_that_returns_true_allows(gate, base, method, _attach,
                                                bt_attach):
    """The other half of each backtest adapter: an allow must allow.

    Without this, an adapter that answers deny unconditionally passes
    every deny test in the file and nothing notices.
    """
    outcome = drive(_BACKTEST_GATE_SETUP + f"""
        class Gate(lrvx.{base}):
            def {method}(self, sig):
                fired[0] += 1
                return True

        bt.{bt_attach}(Gate())
    """ + _BACKTEST_GATE_RUN)

    assert not outcome.timed_out, outcome.describe()
    assert outcome.returncode == 0, outcome.describe()
    assert outcome.payload.get("fired", 0) >= 1, (
        f"the backtest {gate} never ran\n{outcome.describe()}")
    assert outcome.payload.get("alive", 0) >= 1, (
        f"a backtest {gate} that returns True must let the order through; "
        f"{outcome.payload.get('alive')} fill(s) happened\n{outcome.describe()}")


@pytest.mark.parametrize("gate,base,method,attach,_bt_attach", GATES,
                         ids=[g[0] for g in GATES])
def test_gate_that_returns_false_still_denies(gate, base, method, attach,
                                              _bt_attach):
    """Green control for the gate area, on all three bridges.

    Passes on the untouched tree. If this one goes red, plain deny
    broke, not the exception path.
    """
    outcome = drive(_SYNC_RUNNER_SETUP + f"""
        class Gate(lrvx.{base}):
            def {method}(self, sig):
                fired[0] += 1
                return False

        runner.{attach}(Gate())
        runner.start()
        runner.on_trade(sym, 100.0, 1.0, True, 1_000)
        runner.stop()
        alive[0] = len(signals)
        report()
    """)

    assert not outcome.timed_out, outcome.describe()
    assert outcome.returncode == 0, outcome.describe()
    assert outcome.payload.get("fired", 0) == 1, outcome.describe()
    assert outcome.payload.get("alive") == 0, outcome.describe()


@pytest.mark.parametrize("gate,base,method,attach,_bt_attach", GATES,
                         ids=[g[0] for g in GATES])
def test_gate_that_returns_true_allows(gate, base, method, attach, _bt_attach):
    """Each bridge must carry the callable's answer, not a constant.

    A bridge that returns deny whatever the callable said satisfies
    every deny case in this file. Three signals go in and all three
    must come out, so a bridge that drops the answer is visible on each
    of riskAllowBridge, killCheckBridge and orderValidateBridge.
    """
    outcome = drive(_SYNC_RUNNER_SETUP + f"""
        class Gate(lrvx.{base}):
            def {method}(self, sig):
                fired[0] += 1
                return True

        runner.{attach}(Gate())
        runner.start()
        for i in range(3):
            runner.on_trade(sym, 100.0, 1.0, True, 1_000 + i)
        runner.stop()
        alive[0] = len(signals)
        report()
    """)

    assert not outcome.timed_out, outcome.describe()
    assert outcome.returncode == 0, outcome.describe()
    assert outcome.payload.get("fired", 0) == 3, outcome.describe()
    assert outcome.payload.get("alive") == 3, (
        f"a {gate} that returns True must let every signal through; "
        f"{outcome.payload.get('alive')} of 3 arrived\n{outcome.describe()}")


# A gate's answer has to be a bool. docs/bindings/python.md, "When a
# callback raises": the three gates "drop the signal when the callable
# raises, or returns something that is not a bool".
#
# Measured carve-out: pybind11's bool caster accepts a Python int, so 0
# denies as False and 1 allows as True rather than both denying. Every
# other non-bool -- None, a str, a list, a bare object -- fails the cast,
# which raises, which denies by rule 2. Only the deny side is pinned
# here; `1` allowing is noted so the next reader knows it was measured
# and not overlooked.
NON_BOOL_DENIES = [
    ("none", "None"),
    ("zero", "0"),
    ("empty_str", '""'),
    ("nonempty_str", '"yes"'),
    ("object", "object()"),
    ("empty_list", "[]"),
    ("nonempty_list", "[1]"),
]


@pytest.mark.parametrize("value_id,value", NON_BOOL_DENIES,
                         ids=[v[0] for v in NON_BOOL_DENIES])
@pytest.mark.parametrize("gate,base,method,attach,_bt_attach", GATES,
                         ids=[g[0] for g in GATES])
def test_gate_denies_when_the_callable_returns_a_non_bool(gate, base, method,
                                                          attach, _bt_attach,
                                                          value_id, value):
    """An answer that is not a bool is not an answer.

    The trampolines declare a `bool` return, so pybind11 casts strictly
    and a value it cannot convert raises -- which lands on rule 2 and
    denies. A bridge that instead treated "anything that is not literally
    False" as allow would let None, "" and a bare object through.
    """
    outcome = drive(_SYNC_RUNNER_SETUP + f"""
        class Gate(lrvx.{base}):
            def {method}(self, sig):
                fired[0] += 1
                return {value}

        runner.{attach}(Gate())
        runner.start()
        runner.on_trade(sym, 100.0, 1.0, True, 1_000)
        runner.stop()
        alive[0] = len(signals)
        report()
    """)

    assert not outcome.timed_out, outcome.describe()
    assert outcome.returncode == 0, outcome.describe()
    assert outcome.payload.get("fired", 0) == 1, outcome.describe()
    assert outcome.payload.get("alive") == 0, (
        f"a {gate} answering {value} is not answering a bool and must deny; "
        f"{outcome.payload.get('alive')} signal(s) got through\n"
        f"{outcome.describe()}")


@pytest.mark.parametrize("gate,base,method,attach,_bt_attach", GATES,
                         ids=[g[0] for g in GATES])
def test_gate_answers_again_after_it_has_raised(gate, base, method, attach,
                                                _bt_attach):
    """A raise costs that one signal, not the gate.

    docs/bindings/python.md: "a callback that raises costs you that
    event, not the run". So the bridge must not latch a deny after the
    first exception, and it must keep calling the callable -- the third
    signal, whose call returns True, has to get through.

    The report is pinned per raise as well: two distinct exceptions from
    the same source have to produce two distinct descriptions, or a
    once-per-source dedup would hide every repeat failure of a gate that
    breaks intermittently.
    """
    outcome = drive(_SYNC_RUNNER_SETUP + f"""
        class Gate(lrvx.{base}):
            def {method}(self, sig):
                fired[0] += 1
                if fired[0] <= 2:
                    raise RuntimeError(MARKER + "_" + str(fired[0]))
                return True

        runner.{attach}(Gate())
        runner.start()
        for i in range(3):
            runner.on_trade(sym, 100.0, 1.0, True, 1_000 + i)
        runner.stop()
        alive[0] = len(signals)
        report()
    """)

    assert not outcome.timed_out, outcome.describe()
    assert outcome.returncode == 0, outcome.describe()
    assert outcome.payload.get("fired", 0) == 3, (
        f"the {gate} callable stopped being called after it raised: "
        f"{outcome.payload.get('fired')} of 3 calls\n{outcome.describe()}")
    assert outcome.payload.get("alive") == 1, (
        f"the {gate} did not answer again after raising; "
        f"{outcome.payload.get('alive')} of 1 signal got through\n"
        f"{outcome.describe()}")
    assert outcome.reports(MARKER_TEXT + "_1"), (
        f"the first {gate} raise was not reported\n{outcome.describe()}")
    assert outcome.reports(MARKER_TEXT + "_2"), (
        f"the second {gate} raise from the same source was dropped\n"
        f"{outcome.describe()}")


@pytest.mark.parametrize("gate,base,method,attach,_bt_attach", GATES,
                         ids=[g[0] for g in GATES])
def test_gate_error_is_logged_at_error_level(gate, base, method, attach,
                                             _bt_attach):
    """The description is reported at error level, naming the callback.

    docs/bindings/python.md, "When a callback raises": "The exception
    text, with type and traceback, goes to the engine log at error
    level." A report that arrives at info level is a report a user
    filtering on errors never sees.
    """
    outcome = drive(_SYNC_RUNNER_SETUP + f"""
        class Gate(lrvx.{base}):
            def {method}(self, sig):
                fired[0] += 1
                raise RuntimeError(MARKER)

        runner.{attach}(Gate())
        runner.start()
        runner.on_trade(sym, 100.0, 1.0, True, 1_000)
        runner.stop()
        report()
    """)

    assert not outcome.timed_out, outcome.describe()
    reports = outcome.reports(MARKER_TEXT)
    assert reports, (
        f"the {gate} exception never reached the log sink\n{outcome.describe()}")
    assert all(level == LOG_LEVEL_ERROR for level, _ in reports), (
        f"the {gate} exception was reported at level(s) "
        f"{sorted({level for level, _ in reports})}, expected "
        f"{LOG_LEVEL_ERROR} (error)\n{outcome.describe()}")
    assert any(f"{base}.{method}" in message for _level, message in reports), (
        f"the report does not name the callback that raised; expected "
        f"{base}.{method} in it\n{outcome.describe()}")


# ── 2. Strategy callbacks: ten C function pointers ───────────────────
#
# PyStrategyHost in python/strategy_bindings.h registers ten callbacks on
# the bridge. Each one is a C function pointer; a Python exception raised
# inside any of them unwinds through C linkage today. The driver per
# callback raises on the first invocation, then keeps the engine running
# so `alive` can show whether the engine survived.

_RUNNER_DRIVER = """
    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("test", "BTC", 0.01)
    runner = lrvx.Runner(reg, lambda s: None, threaded=%(threaded)s)

    class S(lrvx.Strategy):
        def %(cb)s(self, %(args)s):
            fired[0] += 1
            if fired[0] == 1:
                raise ValueError(MARKER)
        def on_bar(self, ctx, bar):
            alive[0] += 1

    runner.add_strategy(S(symbols=[sym]))
    runner.start()
    try:
        %(drive)s
    except BaseException as exc:
        raised[0] = repr(exc)
    %(settle)s
    runner.on_bar(sym, 100.0, 101.0, 99.0, 100.5, 10.0, 5.0, 9_000, 9_999, 0, 0, 0)
    %(settle)s
    runner.stop()
    report()
"""


def _runner_case(cb, args, drive_call, threaded=False):
    settle = "time.sleep(0.4)" if threaded else ""
    return _RUNNER_DRIVER % {
        "threaded": "True" if threaded else "False",
        "cb": cb,
        "args": args,
        "drive": drive_call,
        "settle": settle,
    }


_ON_BAR_DRIVER = """
    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("test", "BTC", 0.01)
    runner = lrvx.Runner(reg, lambda s: None, threaded=False)

    class S(lrvx.Strategy):
        def on_bar(self, ctx, bar):
            fired[0] += 1
            if fired[0] == 1:
                raise ValueError(MARKER)
        def on_trade(self, ctx, trade):
            alive[0] += 1

    runner.add_strategy(S(symbols=[sym]))
    runner.start()
    try:
        runner.on_bar(sym, 100.0, 101.0, 99.0, 100.5, 10.0, 5.0, 1_000, 1_999, 0, 0, 0)
    except BaseException as exc:
        raised[0] = repr(exc)
    runner.on_trade(sym, 100.0, 1.0, True, 2_000)
    runner.stop()
    report()
"""

_ON_START_DRIVER = """
    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("test", "BTC", 0.01)
    runner = lrvx.Runner(reg, lambda s: None, threaded=False)

    class S(lrvx.Strategy):
        def on_start(self):
            fired[0] += 1
            raise ValueError(MARKER)
        def on_trade(self, ctx, trade):
            alive[0] += 1

    runner.add_strategy(S(symbols=[sym]))
    try:
        runner.start()
    except BaseException as exc:
        raised[0] = repr(exc)
    runner.on_trade(sym, 100.0, 1.0, True, 1_000)
    runner.stop()
    report()
"""

_ON_STOP_DRIVER = """
    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("test", "BTC", 0.01)
    runner = lrvx.Runner(reg, lambda s: None, threaded=False)

    class S(lrvx.Strategy):
        def on_stop(self):
            fired[0] += 1
            raise ValueError(MARKER)
        def on_trade(self, ctx, trade):
            alive[0] += 1

    runner.add_strategy(S(symbols=[sym]))
    runner.start()
    runner.on_trade(sym, 100.0, 1.0, True, 1_000)
    try:
        runner.stop()
    except BaseException as exc:
        raised[0] = repr(exc)
    report()
"""

_BACKTEST_DRIVER = """
    import numpy as np

    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("backtest", "BTCUSDT", tick_size=0.01)
    bt = lrvx.BacktestRunner(reg, fee_rate=0.0, initial_capital=100_000.0)

    class S(lrvx.Strategy):
        def __init__(self, syms):
            super().__init__(syms)
            self.bars = 0
        def on_bar(self, ctx, bar):
            self.bars += 1
            if self.bars == 2:
                self.market_buy(0.5)
            if fired[0]:
                alive[0] += 1
        def %(cb)s(self, ctx, ev):
            fired[0] += 1
            if fired[0] == 1:
                raise ValueError(MARKER)

    bt.set_strategy(S([sym]))
    n = 6
    start = np.array([i * 60_000_000_000 for i in range(n)], dtype=np.int64)
    price = np.array([100.0 + i for i in range(n)])
    try:
        bt.run_bars(start, start + 60_000_000_000,
                    price, price + 0.5, price - 0.5, price + 0.25,
                    np.full(n, 100.0), symbol="BTCUSDT")
    except BaseException as exc:
        raised[0] = repr(exc)
    report()
"""

# on_fill and on_order_update, with no other guarded callback on the
# stack.
#
# The obvious driver -- market_buy() from inside on_bar -- fills
# synchronously, so the fill is delivered while the bridge is still
# inside on_bar's own guard. An exception raised there is contained by
# that outer guard whether or not on_fill has one of its own, and the
# test cannot tell the two apart. Both drivers below arrange for the
# event to arrive from the executor's own market-data handling, which
# BacktestRunner::processEvent runs before it calls the strategy
# (src/backtest/backtest_runner.cpp:352), so the guard under test is the
# outermost one. `depth` proves it: the strategy raises the depth
# counter around every other callback it implements and the driver
# records what it read when it raised.

# A limit order placed on bar 0 below the market, filled by a later bar
# from processPendingOrders inside sim().onBar().
_ISOLATED_FILL_DRIVER = """
    import numpy as np

    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("backtest", "BTCUSDT", tick_size=0.01)
    bt = lrvx.BacktestRunner(reg, fee_rate=0.0, initial_capital=100_000.0)
    depth = [0]

    class S(lrvx.Strategy):
        def __init__(self, syms):
            super().__init__(syms)
            self.bars = 0
        def on_bar(self, ctx, bar):
            depth[0] += 1
            if self.bars == 0:
                self.limit_buy(98.0, 0.5)
            self.bars += 1
            if fired[0]:
                alive[0] += 1
            depth[0] -= 1
        def on_fill(self, ctx, ev):
            fired[0] += 1
            if fired[0] == 1:
                extra["depth_at_raise"] = depth[0]
                raise ValueError(MARKER)

    bt.set_strategy(S([sym]))
    n = 6
    start = np.array([i * 60_000_000_000 for i in range(n)], dtype=np.int64)
    # Flat until bar 2, then the low dips under the resting limit.
    low = np.array([99.0, 99.0, 97.0, 97.0, 97.0, 97.0])
    close = np.array([100.0, 100.0, 98.0, 98.0, 98.0, 98.0])
    try:
        bt.run_bars(start, start + 60_000_000_000,
                    np.full(n, 100.0), np.full(n, 101.0), low, close,
                    np.full(n, 10.0), symbol="BTCUSDT")
    except BaseException as exc:
        raised[0] = repr(exc)
    report()
"""

# A trailing stop ratcheting up on a rising series. updateTrailingStops()
# runs at the end of SimulatedExecutor::onBar, so TRAILING_UPDATED is
# delivered with nothing else of ours on the stack -- unlike the
# SUBMITTED / ACCEPTED / PENDING_TRIGGER events, which arrive inside the
# on_bar that emitted the order and are skipped here for exactly that
# reason.
_ISOLATED_ORDER_UPDATE_DRIVER = """
    import numpy as np

    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("backtest", "BTCUSDT", tick_size=0.01)
    sym_id = int(sym)
    bt = lrvx.BacktestRunner(reg, fee_rate=0.0, initial_capital=100_000.0)
    depth = [0]

    class S(lrvx.Strategy):
        def __init__(self, syms):
            super().__init__(syms)
            self.bars = 0
        def on_bar(self, ctx, bar):
            depth[0] += 1
            if self.bars == 0:
                self.emit_trailing_stop(sym_id, "sell", 5.0, 1.0)
            self.bars += 1
            if fired[0]:
                alive[0] += 1
            depth[0] -= 1
        def on_order_update(self, ctx, ev):
            if ev.status != "TRAILING_UPDATED":
                return
            fired[0] += 1
            if fired[0] == 1:
                extra["depth_at_raise"] = depth[0]
                raise ValueError(MARKER)

    bt.set_strategy(S([sym]))
    n = 6
    start = np.array([i * 60_000_000_000 for i in range(n)], dtype=np.int64)
    price = np.array([100.0 + 5 * i for i in range(n)])
    try:
        bt.run_bars(start, start + 60_000_000_000,
                    price, price + 0.5, price - 0.5, price + 0.25,
                    np.full(n, 10.0), symbol="BTCUSDT")
    except BaseException as exc:
        raised[0] = repr(exc)
    report()
"""


_VENUE_STACK_DRIVER = """
    import pathlib

    CSV = pathlib.Path(%(csv)r)

    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("binance", "BTCUSDT", tick_size=0.01)
    bt = lrvx.BacktestRunner(reg, fee_rate=0.0004, initial_capital=10_000.0)

    class S(lrvx.Strategy):
        def __init__(self, syms):
            super().__init__(syms)
            self.n = 0
        def on_trade(self, ctx, trade):
            self.n += 1
            if self.n == 5:
                self.limit_buy(trade.price * 0.999, 0.01)
            if self.n == 50:
                self.market_buy(0.01)
            if fired[0]:
                alive[0] += 1
        def %(cb)s(self, ctx, ev):
            fired[0] += 1
            if fired[0] == 1:
                raise ValueError(MARKER)

    bt.set_strategy(S([sym]))
    bt.set_venue_stack(lrvx.VenueStack.binance_um_futures(account_id=42,
                                                          equity=10_000.0))
    try:
        bt.run_csv(str(CSV), "BTCUSDT")
    except BaseException as exc:
        raised[0] = repr(exc)
    report()
"""

# on_queue_position_change is the one callback no OHLCV run can reach:
# SimulatedExecutor::maybeEmitQueuePositionChanges() only fires from
# onBookUpdate and onTrade, and a bar feed carries neither a book nor a
# trade quantity. The public Python surface does have a path to both:
# lrvx.BinaryLogRecorderHook records a `.lrvx` tape of book snapshots
# and sized trades, BacktestRunner.run_tape replays them into the
# executor, and VenueStack.binance_um_futures arrives with
# QueueModel::FULL already set (src/backtest/venue_stack.cpp:100) --
# VenueExecutor exposes no set_queue_model of its own. That is the same
# scenario as tests/test_backtest_queue.cpp:131, built out of public
# calls: rest a limit at the touch behind 30 units, then print trades at
# that price so the queue ahead of it shrinks.
#
# Trades are emitted at both the bid and the ask with both aggressor
# flags, so the case does not depend on which side the tape round-trip
# calls the aggressor -- one of the four prints moves each resting
# order's queue whichever way the convention runs.
_QUEUE_POSITION_DRIVER = """
    import os
    import tempfile

    SEC = 1_000_000_000
    BASE = 1_700_000_000_000_000_000

    def write_tape(out_dir):
        reg = lrvx.SymbolRegistry()
        sym = reg.add_symbol("binance", "BTCUSDT", tick_size=0.01)
        hook = lrvx.BinaryLogRecorderHook(
            out_dir, max_segment_mb=4, exchange_id=0, compression="none",
            exchange_name="binance", instrument_type="perpetual")
        hook.add_symbol(sym, "BTCUSDT", "", "", 2, 6)
        rec = lrvx.Runner(reg, on_signal=lambda _s: None)
        rec.set_market_data_recorder(hook)
        rec.start()
        ts = [BASE]
        def book():
            rec.on_book_snapshot(sym, [100.0, 99.0], [30.0, 40.0],
                                 [101.0, 102.0], [30.0, 40.0], ts[0])
            ts[0] += SEC
        for _ in range(3):
            book()
        for _ in range(10):
            for price in (100.0, 101.0):
                for is_buy in (False, True):
                    rec.on_trade(sym, price=price, qty=1.0, is_buy=is_buy,
                                 ts_ns=ts[0])
                    ts[0] += SEC
            book()
        rec.stop()
        hook.close()

    tape = os.path.join(tempfile.mkdtemp(), "tape")
    write_tape(tape)

    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("binance", "BTCUSDT", tick_size=0.01)
    bt = lrvx.BacktestRunner(reg, fee_rate=0.0, initial_capital=100_000.0)

    class S(lrvx.Strategy):
        def __init__(self, syms):
            super().__init__(syms)
            self.placed = False
        def on_book_update(self, ctx):
            if not self.placed:
                self.placed = True
                self.limit_buy(100.0, 1.0)
                self.limit_sell(101.0, 1.0)
            if fired[0]:
                alive[0] += 1
        def on_trade(self, ctx, trade):
            if fired[0]:
                alive[0] += 1
        def on_queue_position_change(self, ctx, ev):
            fired[0] += 1
            if fired[0] == 1:
                raise ValueError(MARKER)

    bt.set_strategy(S([sym]))
    bt.set_venue_stack(lrvx.VenueStack.binance_um_futures(account_id=42,
                                                          equity=100_000.0))
    try:
        bt.run_tape(tape)
    except BaseException as exc:
        raised[0] = repr(exc)
    report()
"""


_CSV = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "lrvx", "templates", "research", "data", "btcusdt_sample.csv")

# One entry per C callback slot PyStrategyHost registers.
STRATEGY_CALLBACKS = [
    ("on_trade", _runner_case(
        "on_trade", "ctx, trade",
        "runner.on_trade(sym, 100.0, 1.0, True, 1_000)")),
    ("on_book_update", _runner_case(
        "on_book_update", "ctx",
        "runner.on_book_snapshot(sym, [100.0, 99.0], [1.0, 2.0], "
        "[101.0, 102.0], [1.0, 2.0], 1_000)")),
    ("on_touch", _runner_case(
        "on_touch", "ctx",
        "runner.on_book_top(sym, (100.0, 1.0), (101.0, 1.0), 1_000)")),
    ("on_bar", _ON_BAR_DRIVER),
    ("on_start", _ON_START_DRIVER),
    ("on_stop", _ON_STOP_DRIVER),
    ("on_fill", _ISOLATED_FILL_DRIVER),
    ("on_order_update", _ISOLATED_ORDER_UPDATE_DRIVER),
    ("on_queue_position_change", _QUEUE_POSITION_DRIVER),
    ("on_market_position_change",
     _VENUE_STACK_DRIVER % {"cb": "on_market_position_change", "csv": _CSV}),
]

assert len(STRATEGY_CALLBACKS) == 10, "one case per callback slot"


@pytest.mark.parametrize("callback,body", STRATEGY_CALLBACKS,
                         ids=[c[0] for c in STRATEGY_CALLBACKS])
def test_strategy_callback_that_raises_does_not_cross_the_c_boundary(callback, body):
    """Each of the ten bridge callbacks must contain its own exception.

    hook_bindings.h:216 states the rule for the sibling header:
    "Exceptions from Python are caught and printed -- propagating them
    across the C ABI boundary is undefined behaviour."
    strategy_bindings.h has no catch (`grep -c catch` is 0).

    Three things are pinned per callback:
      * the process survives (returncode 0, no abort, no wedge);
      * the engine survives -- the bridge contains the exception at the
        callback, so the run is not torn down and the strategy keeps
        getting events. This is the observable proof that nothing
        unwound through C linkage: an engine that stops mid-run is
        indistinguishable from one whose C++ frames were unwound past;
      * the exception is carried out, not dropped: it reaches Python
        either as a re-raise out of the driving call or through
        set_log_callback.

    `fired` is asserted first so a case that never reached the callback
    fails loudly instead of passing vacuously.
    """
    outcome = drive(body)

    assert not outcome.timed_out, (
        f"{callback} wedged the process\n{outcome.describe()}")
    assert outcome.returncode == 0, (
        f"a raise in {callback} must not abort the process\n{outcome.describe()}")
    assert outcome.payload.get("fired", 0) >= 1, (
        f"{callback} never fired, so this case proves nothing about it\n"
        f"{outcome.describe()}")
    assert outcome.payload.get("alive", 0) >= 1, (
        f"the engine stopped reaching the strategy after {callback} raised: "
        f"the exception tore the run down instead of being contained at the "
        f"bridge and reported out of band\n{outcome.describe()}")
    assert outcome.surfaced(MARKER_TEXT), (
        f"the exception from {callback} was swallowed: not re-raised out of "
        f"the driving call and not reported through set_log_callback\n"
        f"{outcome.describe()}")
    if "depth_at_raise" in outcome.extra:
        # Only the drivers that can arrange it report this. A non-zero
        # depth means another guarded callback was on the stack and would
        # have contained the exception on this one's behalf, which makes
        # the case blind to a missing guard here.
        assert outcome.extra["depth_at_raise"] == 0, (
            f"{callback} raised {outcome.extra['depth_at_raise']} frame(s) "
            f"inside another guarded callback, so this case does not test "
            f"its own guard\n{outcome.describe()}")


def test_threaded_consumer_survives_a_raising_callback():
    """The live engine path, where the raise unwinds on a bus thread.

    Red today: the EventBus consumer logs "handler threw, consumer is
    dead" and stops, so the strategy never sees a second event.
    """
    outcome = drive("""
        reg = lrvx.SymbolRegistry()
        sym = reg.add_symbol("test", "BTC", 0.01)
        runner = lrvx.Runner(reg, lambda s: None, threaded=True)

        class S(lrvx.Strategy):
            def on_trade(self, ctx, trade):
                fired[0] += 1
                if fired[0] == 1:
                    raise ValueError(MARKER)
                alive[0] += 1

        runner.add_strategy(S(symbols=[sym]))
        runner.start()
        runner.on_trade(sym, 100.0, 1.0, True, 1_000)
        time.sleep(0.4)
        runner.on_trade(sym, 101.0, 1.0, True, 2_000)
        time.sleep(0.4)
        runner.stop()
        report()
    """)

    assert not outcome.timed_out, outcome.describe()
    assert outcome.returncode == 0, outcome.describe()
    assert outcome.payload.get("fired", 0) >= 1, outcome.describe()
    assert outcome.payload.get("alive", 0) >= 1, (
        "the bus consumer died on the first raise; no later event reached "
        f"the strategy\n{outcome.describe()}")
    assert outcome.surfaced(MARKER_TEXT), outcome.describe()


def test_threaded_callback_runs_holding_the_gil():
    """The live path stays correct with several threads in Python at once.

    Runner(threaded=True) runs the callback on an EventBus consumer
    thread while the feeding thread publishes -- and publish now
    releases the GIL, so the two really do overlap. This drives that
    overlap hard: the strategy allocates on every event, two more
    Python threads churn objects, and 8k trades go through. A thread
    touching CPython without the GIL corrupts refcounts here; the
    process aborts, or wedges and the timeout reports it.

    What this does *not* prove on its own is that
    PyStrategyHost::dispatch is the frame doing the acquiring. Every one
    of the ten callbacks reaches Python through PYBIND11_OVERRIDE,
    which opens with `pybind11::gil_scoped_acquire gil;`
    (pybind11/pybind11.h:3847), and error_already_set acquires in its
    own destructor, so removing dispatch's acquire leaves no observable
    trace from Python. The assertion here is the outcome -- every event
    delivered, interpreter intact -- which is what a reader of this file
    can actually check.
    """
    outcome = drive("""
        import threading

        reg = lrvx.SymbolRegistry()
        sym = reg.add_symbol("test", "BTC", 0.01)
        runner = lrvx.Runner(reg, lambda s: None, threaded=True)

        class S(lrvx.Strategy):
            def on_trade(self, ctx, trade):
                d = {}
                for i in range(8):
                    d[str(i)] = [object(), (trade.price, trade.quantity),
                                 "x" * (i + 1)]
                fired[0] += 1

        runner.add_strategy(S(symbols=[sym]))
        runner.start()

        stop = threading.Event()

        def churn():
            while not stop.is_set():
                acc = [{"k": str(i), "v": [object()] * 4} for i in range(200)]
                del acc

        threads = [threading.Thread(target=churn, daemon=True) for _ in range(2)]
        for t in threads:
            t.start()

        N = 8_000
        for i in range(N):
            runner.on_trade(sym, 100.0 + (i %% 7), 1.0, True, i + 1)
        for _ in range(200):
            if fired[0] >= N:
                break
            time.sleep(0.05)
        stop.set()
        for t in threads:
            t.join(timeout=5)
        runner.stop()
        alive[0] = N
        report()
    """ % (), timeout=180)

    assert not outcome.timed_out, (
        f"the threaded run wedged under GIL contention\n{outcome.describe()}")
    assert outcome.returncode == 0, (
        f"the interpreter did not survive a callback running on the bus "
        f"consumer thread while other threads ran Python\n{outcome.describe()}")
    assert outcome.payload.get("fired", 0) == outcome.payload.get("alive"), (
        f"{outcome.payload.get('fired')} of {outcome.payload.get('alive')} "
        f"events reached the strategy\n{outcome.describe()}")


def test_strategy_callback_that_does_not_raise_is_delivered():
    """Green control for the callback area."""
    outcome = drive("""
        reg = lrvx.SymbolRegistry()
        sym = reg.add_symbol("test", "BTC", 0.01)
        runner = lrvx.Runner(reg, lambda s: None, threaded=False)

        class S(lrvx.Strategy):
            def on_trade(self, ctx, trade):
                fired[0] += 1
            def on_bar(self, ctx, bar):
                alive[0] += 1

        runner.add_strategy(S(symbols=[sym]))
        runner.start()
        runner.on_trade(sym, 100.0, 1.0, True, 1_000)
        runner.on_bar(sym, 100.0, 101.0, 99.0, 100.5, 10.0, 5.0, 2_000, 2_999, 0, 0, 0)
        runner.stop()
        report()
    """)

    assert not outcome.timed_out, outcome.describe()
    assert outcome.returncode == 0, outcome.describe()
    assert outcome.payload.get("fired") == 1, outcome.describe()
    assert outcome.payload.get("alive") == 1, outcome.describe()


# ── 3. Publishing must not hold the GIL ──────────────────────────────

_RING = 4096  # config::DEFAULT_EVENTBUS_CAPACITY, engine_config.h:45


_PUBLISH_DRIVER = """
    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("test", "BTC", 0.01)
    runner = lrvx.Runner(reg, lambda s: None, threaded=True)

    def block_once():
        fired[0] += 1
        if fired[0] == 1:
            # Releases the GIL and needs it back to return. A publisher
            # that spins on ring gating with the GIL held never lets it
            # have the GIL again, so the ring is never drained.
            time.sleep(1.0)

    class S(lrvx.Strategy):
        def on_trade(self, ctx, trade):
            block_once()
        def on_book_update(self, ctx):
            block_once()
        def on_bar(self, ctx, bar):
            block_once()

    runner.add_strategy(S(symbols=[sym]))
    runner.start()
    for i in range(%(n)d):
        %(call)s
    for _ in range(100):
        if fired[0] >= %(n)d:
            break
        time.sleep(0.05)
    alive[0] = fired[0]
    runner.stop()
    report()
"""

FEEDS = [
    ("on_trade", "runner.on_trade(sym, 100.0, 1.0, True, i + 1)"),
    ("on_book_snapshot",
     "runner.on_book_snapshot(sym, [100.0], [1.0], [101.0], [1.0], i + 1)"),
    ("on_bar",
     "runner.on_bar(sym, 100.0, 101.0, 99.0, 100.5, 10.0, 5.0, "
     "i + 1, i + 2, 0, 0, 0)"),
]


@pytest.mark.parametrize("feed,call", FEEDS, ids=[f[0] for f in FEEDS])
def test_publish_releases_the_gil(feed, call):
    """Runner feed methods must not gate on the bus while holding the GIL.

    The ring holds 4096 events (engine_config.h:45); past that,
    EventBus::publish busy-waits on consumer gating (event_bus.h:747).
    None of the three feed methods (strategy_bindings.h:2991-:2995)
    carries py::call_guard<py::gil_scoped_release>, while stop() at
    :1487 does -- so the hazard was already known in this class and
    applied to exactly one method.

    The consumer blocks in a Python callback on the first event, which
    is enough: it can only finish by taking the GIL back, and the
    publisher is holding it while spinning.

    Red today: the case never finishes, and the timeout reports it
    instead of hanging the suite.
    """
    n = _RING * 3
    outcome = drive(_PUBLISH_DRIVER % {"n": n, "call": call}, timeout=60)

    assert not outcome.timed_out, (
        f"Runner.{feed} wedged: the publisher spun on ring gating while "
        f"holding the GIL, so the consumer could not return from its "
        f"callback and drain the ring\n{outcome.describe()}")
    assert outcome.returncode == 0, outcome.describe()
    assert outcome.payload.get("alive", 0) >= n, (
        f"the consumer drained {outcome.payload.get('alive')} of {n} "
        f"published events\n{outcome.describe()}")


@pytest.mark.parametrize("feed,call", FEEDS, ids=[f[0] for f in FEEDS])
def test_publish_below_the_ring_completes(feed, call):
    """Green control for the GIL area.

    Fewer events than the ring holds, so publish never reaches the
    gating loop and the GIL is never held across a wait. Passes on the
    untouched tree; if it goes red, the harness broke, not the fix.
    """
    n = _RING // 2
    outcome = drive(_PUBLISH_DRIVER % {"n": n, "call": call}, timeout=60)

    assert not outcome.timed_out, outcome.describe()
    assert outcome.returncode == 0, outcome.describe()
    assert outcome.payload.get("alive", 0) >= n, outcome.describe()
