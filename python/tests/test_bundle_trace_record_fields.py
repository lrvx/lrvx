"""A bundle packed now is validated record by record.

Validation compared the expected trace with the replay's on the number of
signals, order events and fills only: a fill at another price, an order on
the other side or a signal under another name passed as long as the counts
agreed. A bundle whose manifest has ``lrvxrun_records_comparable`` is now
compared on every field of every record but ``run_ts_ns``; one without it
keeps the count comparison.
"""
from __future__ import annotations

import io
import json
import os
import subprocess
import sys
import tarfile
import warnings
from pathlib import Path
from typing import Any, Callable, Dict, List

sys.path.insert(0, os.path.dirname(__file__))

import lrvx  # noqa: E402
from lrvx import bundle  # noqa: E402
from lrvx._lrvx import FillLiquidity, OrderEventKind  # noqa: E402
from support.bundle_traces import expected_trace as _expected_trace  # noqa: E402
from support.bundle_traces import two_fill_trace as _two_fill_trace  # noqa: E402

_RECORDS = "signal, order event and fill records"


def _validate(path: Path):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", bundle.BundleTrustWarning)
        return bundle.validate_bundle(path)


def _read(trace: Path) -> Dict[str, List[Dict[str, Any]]]:
    reader = lrvx.TraceReader(str(trace))
    return {
        "signals": reader.read_all_signals(),
        "orders": reader.read_all_order_events(),
        "fills": reader.read_all_fills(),
    }


def _write(trace: Path, recs: Dict[str, List[Dict[str, Any]]]) -> None:
    rec = lrvx.TraceRecorder(str(trace), strategy_id="strategy")
    for s in recs["signals"]:
        rec.write_signal(run_ts_ns=s["run_ts_ns"], feed_ts_ns=s["feed_ts_ns"],
                         signal_id=s["signal_id64"], flags=s["flags"],
                         strength_raw=s["strength_raw"], name=s["name"],
                         symbol_ids=s["symbol_ids"], payload=s["payload"])
    for o in recs["orders"]:
        rec.write_order_event(run_ts_ns=o["run_ts_ns"], feed_ts_ns=o["feed_ts_ns"],
                              order_id=o["order_id"], parent_signal_id=o["parent_signal_id"],
                              price_raw=o["price_raw"], qty_raw=o["qty_raw"],
                              symbol_id=o["symbol_id"], event_kind=OrderEventKind(o["event_kind"]),
                              side=o["side"], order_type=o["order_type"], flags=o["flags"],
                              reason=o["reason"])
    for f in recs["fills"]:
        rec.write_fill(run_ts_ns=f["run_ts_ns"], feed_ts_ns=f["feed_ts_ns"],
                       order_id=f["order_id"], fill_id=f["fill_id"], price_raw=f["price_raw"],
                       qty_raw=f["qty_raw"], fee_raw=f["fee_raw"], symbol_id=f["symbol_id"],
                       side=f["side"], liquidity=FillLiquidity(f["liquidity"]))
    rec.close()


def _with_trace(path: Path, work: Path, edit: Callable[[Dict[str, List[Dict[str, Any]]]], None],
                *, drop_marker: bool = False) -> Path:
    """The bundle at ``path`` with its expected trace rewritten by ``edit``,
    and without the records marker when ``drop_marker``."""
    recs = _read(_expected_trace(path, work))
    edit(recs)
    trace = work / "edited.lrvxrun"
    _write(trace, recs)
    out = work / "edited.tar"
    with tarfile.open(path, "r") as src, tarfile.open(out, "w") as dst:
        for member in src.getmembers():
            if member.name.startswith("expected.lrvxrun"):
                continue
            data = src.extractfile(member).read() if member.isfile() else None
            if member.name == "manifest.json" and drop_marker:
                manifest = json.loads(data)
                del manifest["lrvxrun_records_comparable"]
                data = json.dumps(manifest, indent=2, sort_keys=True).encode()
                member.size = len(data)
            dst.addfile(member, io.BytesIO(data) if data is not None else None)
        dst.add(str(trace), arcname="expected.lrvxrun")
    return out


def test_the_manifest_says_the_records_are_comparable(tmp_path):
    path, *_ = _two_fill_trace(tmp_path)
    with tarfile.open(path, "r") as tf:
        manifest = json.loads(tf.extractfile("manifest.json").read())
    assert manifest["lrvxrun_records_comparable"] is True


def test_a_new_bundle_is_compared_record_by_record(tmp_path):
    path, signals, orders, fills = _two_fill_trace(tmp_path)
    res = _validate(path)
    assert res.matches, res.diff
    assert res.expected["lrvxrun_compared"] == _RECORDS
    assert len(res.expected["lrvxrun_fills"]) == 2
    assert res.actual["lrvxrun_fills"] == res.expected["lrvxrun_fills"]
    for key in ("lrvxrun_signals", "lrvxrun_order_events", "lrvxrun_fills"):
        assert all("run_ts_ns" not in r for r in res.expected[key]), key
    assert res.expected["lrvxrun_fills"][1]["price_raw"] == fills[1]["price_raw"]


def test_a_fill_at_another_price_fails(tmp_path):
    path, *_ = _two_fill_trace(tmp_path)

    def edit(recs):
        recs["fills"][1]["price_raw"] += 1

    res = _validate(_with_trace(path, tmp_path, edit))
    assert not res.matches
    assert any(d.startswith("lrvxrun_fills[1]: price_raw") for d in res.diff), res.diff
    assert not any(d.startswith("lrvxrun_fill_count") for d in res.diff), res.diff


def test_an_order_on_the_other_side_and_a_renamed_signal_fail(tmp_path):
    path, *_ = _two_fill_trace(tmp_path)

    def edit(recs):
        recs["orders"][0]["side"] = 1 - recs["orders"][0]["side"]
        recs["signals"][1]["name"] = "limit"

    res = _validate(_with_trace(path, tmp_path, edit))
    assert not res.matches
    assert any(d.startswith("lrvxrun_order_events[0]: side") for d in res.diff), res.diff
    assert any(d.startswith("lrvxrun_signals[1]: name") for d in res.diff), res.diff


def test_another_order_id_fails(tmp_path):
    path, *_ = _two_fill_trace(tmp_path)

    def edit(recs):
        recs["fills"][0]["order_id"] += 1

    res = _validate(_with_trace(path, tmp_path, edit))
    assert any(d.startswith("lrvxrun_fills[0]: order_id") for d in res.diff), res.diff


def test_the_wall_clock_is_not_compared(tmp_path):
    path, *_ = _two_fill_trace(tmp_path)

    def edit(recs):
        for kind in recs.values():
            for r in kind:
                r["run_ts_ns"] += 12_345

    res = _validate(_with_trace(path, tmp_path, edit))
    assert res.matches, res.diff


def test_a_missing_record_is_named(tmp_path):
    path, *_ = _two_fill_trace(tmp_path)

    def edit(recs):
        recs["fills"].pop()

    res = _validate(_with_trace(path, tmp_path, edit))
    assert any(d.startswith("lrvxrun_fills[1]: missing from expected") for d in res.diff), res.diff


def test_a_bundle_packed_before_keeps_the_count_comparison(tmp_path):
    path, *_ = _two_fill_trace(tmp_path)

    def edit(recs):
        recs["fills"][1]["price_raw"] += 1

    res = _validate(_with_trace(path, tmp_path, edit, drop_marker=True))
    assert res.matches, res.diff
    assert res.expected["lrvxrun_compared"] == "signals, order events, fills"
    assert "lrvxrun_fills" not in res.expected


def test_a_bundle_packed_in_another_process_validates(tmp_path):
    """Order ids, fill ids and feed times come out the same in a separate
    process: only run_ts_ns is left out of the comparison."""
    work = tmp_path / "packed"
    work.mkdir()
    here = Path(__file__).resolve().parent
    code = (
        "import sys; from pathlib import Path\n"
        f"sys.path.insert(0, {str(here)!r})\n"
        "from support.bundle_traces import two_fill_trace\n"
        f"two_fill_trace(Path({str(work)!r}))\n"
    )
    subprocess.run([sys.executable, "-c", code], check=True)
    res = _validate(work / "b.tar")
    assert res.matches, res.diff
    assert res.expected["lrvxrun_compared"] == _RECORDS
