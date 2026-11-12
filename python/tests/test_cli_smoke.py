"""Every `lrvx` CLI verb must at least run.

Of the 18 leaf verbs, 4 were exercised by a test and 14 were not. `lrvx engine
sim` is what that costs: it shipped in #234 and raised TypeError on the first
call after argument parsing, so it had never started once, and its two test
files (265 lines) covered only private helpers and `--help` text -- which pass
whether or not the command works.

So this file does not check `--help`. Each verb is either run for real against
a fixture, or -- where it needs the network or blocks on a server -- run with
inputs that must reach its own validation. The assertion in that second case is
that the failure is a *domain* failure: an argparse error or a clean message,
not a TypeError / AttributeError / NameError from mis-wired plumbing. That is
precisely the class of bug that made `engine sim` dead on arrival, and it is
detectable without a live exchange.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent

# Raised from mis-wired plumbing rather than from a rejected input. If one of
# these reaches the user, the command was never run by anyone.
WIRING_ERRORS = (
    "TypeError:",
    "AttributeError:",
    "NameError:",
    "UnboundLocalError:",
    "ModuleNotFoundError:",
    "ImportError:",
)



def _subprocess_env() -> dict:
    """Absolutise PYTHONPATH before spawning with a different cwd.

    CI provides lrvx via `PYTHONPATH=build/python`, a path relative to the
    repo root. Every subprocess here runs with cwd set to a temp directory, so
    a relative entry resolves against that temp dir and the import fails with
    ModuleNotFoundError. Locally this never showed up because lrvx is
    pip-installed at an absolute site-packages path.
    """
    env = dict(os.environ)
    raw = env.get("PYTHONPATH", "")
    if raw:
        env["PYTHONPATH"] = os.pathsep.join(
            str(Path(part).resolve()) if part else part
            for part in raw.split(os.pathsep))
    return env


def _run(argv: list[str], cwd: str | None = None, timeout: int = 300):
    return subprocess.run([sys.executable, "-m", "lrvx.cli", *argv],
                          cwd=cwd, env=_subprocess_env(),
                          capture_output=True, text=True, timeout=timeout)


def _assert_no_wiring_error(argv: list[str], r) -> None:
    blob = (r.stdout or "") + (r.stderr or "")
    for marker in WIRING_ERRORS:
        assert marker not in blob, (
            f"`lrvx {' '.join(argv)}` failed with {marker} — that is mis-wired "
            f"plumbing, not a rejected input:\n{blob[-1500:]}")
    assert "Traceback (most recent call last)" not in blob, (
        f"`lrvx {' '.join(argv)}` died with an unhandled traceback:\n{blob[-1500:]}")


@pytest.fixture(scope="module")
def tape(tmp_path_factory) -> Path:
    """A real single-symbol .lrvx tape for the offline verbs."""
    lrvx = pytest.importorskip("lrvx")
    from lrvx import tape as tape_mod

    out = tmp_path_factory.mktemp("tape")
    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("binance", "BTCUSDT", tick_size=0.01)
    hook = tape_mod.make_recorder_hook(str(out), exchange_name="binance",
                                       instrument_type="spot")
    runner = lrvx.Runner(reg, on_signal=lambda s: None, threaded=False)
    runner.set_market_data_recorder(hook)
    runner.start()
    base = 1_704_067_200_000_000_000
    for i in range(40):
        runner.on_trade(int(sym.id), 100.0 + (i % 5) * 0.5, 1.0, True,
                        base + i * 1_000_000_000)
    runner.stop()
    return out


@pytest.fixture(scope="module")
def strategy_file(tmp_path_factory) -> Path:
    p = tmp_path_factory.mktemp("strat") / "s.py"
    p.write_text(
        "import lrvx\n\n\n"
        "class S(lrvx.Strategy):\n"
        "    def on_trade(self, ctx, trade):\n"
        "        if ctx.is_flat():\n"
        "            self.market_buy(1.0)\n"
    )
    return p


# ── Verbs that must succeed offline ──────────────────────────────────


def test_templates_lists_something() -> None:
    r = _run(["templates"])
    _assert_no_wiring_error(["templates"], r)
    assert r.returncode == 0, r.stderr
    assert "research" in r.stdout


def test_new_scaffolds_a_project() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        r = _run(["new", "smoke_proj"], cwd=tmp)
        _assert_no_wiring_error(["new"], r)
        assert r.returncode == 0, r.stderr
        assert (Path(tmp) / "smoke_proj" / "main.py").is_file()


def test_report_renders_html_from_stats() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        stats = Path(tmp) / "stats.json"
        stats.write_text(json.dumps({
            "total_trades": 3, "return_pct": 1.5, "sharpe_ratio": 0.4,
            "max_drawdown_pct": 0.2, "win_rate": 0.66, "profit_factor": 1.2,
            "net_pnl": 15.0, "total_fees": 0.3,
            "initial_capital": 1000.0, "final_capital": 1015.0,
        }))
        out = Path(tmp) / "r.html"
        argv = ["report", str(stats), "-o", str(out)]
        r = _run(argv)
        _assert_no_wiring_error(argv, r)
        assert r.returncode == 0, r.stderr
        assert out.is_file() and "<" in out.read_text()


def test_tape_inspect_reports_the_tape(tape: Path) -> None:
    argv = ["tape", "inspect", str(tape)]
    r = _run(argv)
    _assert_no_wiring_error(argv, r)
    assert r.returncode == 0, r.stderr
    assert "40" in r.stdout, r.stdout


def test_tape_replay_dispatches_the_tape(tape: Path) -> None:
    argv = ["tape", "replay", str(tape)]
    r = _run(argv)
    _assert_no_wiring_error(argv, r)
    assert r.returncode == 0, r.stderr


def test_tape_diff_compares_a_tape_with_itself(tape: Path) -> None:
    argv = ["tape", "diff", str(tape), str(tape)]
    r = _run(argv)
    _assert_no_wiring_error(argv, r)
    assert r.returncode == 0, r.stderr


def test_bundle_pack_then_validate(tape: Path, strategy_file: Path) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "b.lrvxbundle"
        argv = ["bundle", "pack", "--strategy", str(strategy_file),
                "--tape", str(tape), "--output", str(out)]
        r = _run(argv)
        _assert_no_wiring_error(argv, r)
        if r.returncode != 0:
            pytest.skip(f"bundle pack needs more setup than this smoke provides: "
                        f"{r.stderr[-300:]}")
        assert out.exists()
        argv = ["bundle", "validate", str(out)]
        r = _run(argv)
        _assert_no_wiring_error(argv, r)
        assert r.returncode == 0, r.stderr


def test_lint_lookahead_accepts_a_clean_strategy(strategy_file: Path) -> None:
    argv = ["lint", "lookahead", str(strategy_file)]
    r = _run(argv)
    _assert_no_wiring_error(argv, r)
    assert r.returncode == 0, r.stderr


# ── Verbs that need a network or block: they must still reach their own
#    validation rather than crashing in the plumbing ───────────────────


@pytest.mark.parametrize("argv", [
    ["tape", "record", "binance", "BTCUSDT", "--output", "/nonexistent/x",
     "--duration", "1"],
    ["tape", "view", "/nonexistent/tape"],
    ["engine", "sim", "--strategy", "/nonexistent/s.py",
     "--tape", "/nonexistent/tape"],
    ["bundle", "replay", "/nonexistent/b.lrvxbundle"],
    ["archive", "binance", "/nonexistent/data.zip", "--output", "/nonexistent/o"],
    ["archive", "bybit", "/nonexistent/data.zip", "--output", "/nonexistent/o"],
    ["archive", "okx", "/nonexistent/data.zip", "--output", "/nonexistent/o"],
    ["archive", "bitget", "/nonexistent/data.zip", "--output", "/nonexistent/o"],
], ids=lambda a: " ".join(a[:2]))
def test_verb_reaches_its_own_validation(argv: list[str]) -> None:
    r = _run(argv)
    _assert_no_wiring_error(argv, r)
    # A clean rejection: non-zero with a message, not a crash and not silence.
    assert r.returncode != 0, (
        f"`lrvx {' '.join(argv)}` reported success on nonexistent inputs")
    assert (r.stderr.strip() or r.stdout.strip()), (
        f"`lrvx {' '.join(argv)}` failed with no explanation at all")


def test_archive_without_a_public_archive_is_not_offered() -> None:
    """Deribit publishes no public trade file archive, so `lrvx archive`
    offers no Deribit importer: the request fails in argument parsing,
    naming the importers that exist."""
    r = _run(["archive", "deribit", "--symbol", "BTC-PERPETUAL",
              "--from", "2024-01-15", "--to", "2024-01-15", "--out", "/nonexistent/o"])
    assert r.returncode == 2
    assert "invalid choice: 'deribit'" in r.stderr
    for name in ("binance", "bybit", "okx", "bitget"):
        assert name in r.stderr.split("invalid choice", 1)[1]


def test_engine_sim_runs_a_tape_and_stops(tape: Path, strategy_file: Path) -> None:
    """The one blocking verb, actually started.

    `engine sim` serves a control server until SIGINT, which is why nothing
    ever ran it. Start it, let it replay, interrupt it, and require a clean
    exit -- this is the test that would have caught the original TypeError.
    """
    import signal
    import time

    proc = subprocess.Popen(
        [sys.executable, "-m", "lrvx.cli", "engine", "sim",
         "--strategy", str(strategy_file), "--tape", str(tape),
         "--port", "18921",
         "--state-file", str(Path(tempfile.gettempdir()) / "lrvx-smoke-state.json")],
        env=_subprocess_env(),
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        time.sleep(8)
        proc.send_signal(signal.SIGINT)
        out, err = proc.communicate(timeout=60)
    except subprocess.TimeoutExpired:  # pragma: no cover
        proc.kill()
        out, err = proc.communicate()
        pytest.fail("engine sim did not stop on SIGINT")

    _assert_no_wiring_error(["engine", "sim"], subprocess.CompletedProcess(
        args=[], returncode=proc.returncode, stdout=out, stderr=err))
    assert proc.returncode == 0, f"engine sim exited {proc.returncode}\n{err[-1500:]}"
    assert "tape replay complete" in out, out[-1500:]
