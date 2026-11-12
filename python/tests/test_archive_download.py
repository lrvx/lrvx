"""Tests for the download, cache and range plumbing shared by the
archive importers (``lrvx.archives``), without the network.

A local HTTP server plays the exchange: each test scripts what it
answers per path (a status, a good file, a body cut short, a stall past
the client timeout) and counts the requests.

Covers:

  * One retry policy for all four importers: a 404 (or the 403 a CDN
    answers for a missing object) is asked once and never retried; a
    5xx, a timeout, a body shorter than its Content-Length and a body
    that does not open as zip / gzip are retried, and a later good
    answer succeeds.
  * A failed download leaves nothing in the mirror: no file at the
    destination and no temporary file next to it.
  * A cached file that does not open is removed from the mirror.
  * Every importer and product uses the shared cache by default
    (``LRVX_ARCHIVE_CACHE`` / ``~/.lrvx/archive-cache/<exchange>``), so
    a second run downloads nothing.
  * Symbol shapes are checked per exchange and market before any
    request, with an example of the expected form in the error.
  * A range in which every day is missing fails, naming the range and
    the first URL tried, with or without ``skip_missing``; the CLI
    exits 1.
  * The CLI summary carries the first and last trade written, in UTC,
    and says that OKX and Bitget days are UTC+8.
  * With LRVX_ARCHIVE_NETWORK_TESTS=1, a day no exchange has published
    yet fails on every real archive with the first URL tried.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_archive_download.py
"""
from __future__ import annotations

import gzip
import io
import os
import sys
import tempfile
import threading
import time
import unittest
import zipfile
from contextlib import redirect_stderr, redirect_stdout
from datetime import date
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
for cand in ("build/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        sys.path.insert(0, str(p))
        break
sys.path.insert(0, str(Path(__file__).resolve().parent))

from support import archive_network as net  # noqa: E402
from lrvx import cli  # noqa: E402
from lrvx.archives import binance, bitget, bybit, okx  # noqa: E402


# ── fixtures ────────────────────────────────────────────────────────


def _zip_bytes(name: str = "x.csv", text: str = "a,b\n1,2\n") -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(name, text)
    return buf.getvalue()


def _gz_bytes(text: str = "a,b\n1,2\n") -> bytes:
    return gzip.compress(text.encode())


class _Server:
    """A local HTTP server answering each path from a script.

    ``script[path]`` is a list of answers, one per request, the last
    repeated: ``("ok", body)``, ``("status", code)``,
    ``("short", body, declared_length)`` (sends fewer bytes than its
    Content-Length and closes), ``("stall", seconds, body)`` (waits
    before answering), ``("raw", body)`` (200 without Content-Length,
    connection closed at the end). A path with no script answers 404.
    ``hits[path]`` counts the requests."""

    def __init__(self) -> None:
        self.script: dict = {}
        self.hits: dict = {}
        self.order: list = []
        server = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_args) -> None:
                pass

            def do_GET(self) -> None:
                path = self.path
                with server.lock:
                    n = server.hits.get(path, 0)
                    server.hits[path] = n + 1
                    server.order.append(path)
                answers = server.script.get(path) or [("status", 404)]
                kind, *rest = answers[min(n, len(answers) - 1)]
                if kind == "status":
                    self.send_response(rest[0])
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                elif kind == "ok":
                    self.send_response(200)
                    self.send_header("Content-Length", str(len(rest[0])))
                    self.end_headers()
                    self.wfile.write(rest[0])
                elif kind == "short":
                    body, declared = rest
                    self.send_response(200)
                    self.send_header("Content-Length", str(declared))
                    self.send_header("Connection", "close")
                    self.end_headers()
                    self.wfile.write(body)
                    self.wfile.flush()
                    self.close_connection = True
                elif kind == "stall":
                    seconds, body = rest
                    time.sleep(seconds)
                    try:
                        self.send_response(200)
                        self.send_header("Content-Length", str(len(body)))
                        self.end_headers()
                        self.wfile.write(body)
                    except OSError:
                        pass
                elif kind == "raw":
                    self.send_response(200)
                    self.send_header("Connection", "close")
                    self.end_headers()
                    self.wfile.write(rest[0])
                    self.wfile.flush()
                    self.close_connection = True

        self.lock = threading.Lock()
        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.httpd.daemon_threads = True
        self.base = f"http://127.0.0.1:{self.httpd.server_address[1]}"
        self.thread = threading.Thread(target=self.httpd.serve_forever,
                                       daemon=True)
        self.thread.start()

    def close(self) -> None:
        self.httpd.shutdown()
        self.httpd.server_close()


class _ServerCase(unittest.TestCase):
    def setUp(self) -> None:
        self.server = _Server()
        self.addCleanup(self.server.close)
        self._tmp = tempfile.TemporaryDirectory(prefix="archive-dl-")
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)

    def url(self, path: str) -> str:
        return self.server.base + path


# Each importer's download entry point, called the way its range form
# calls it. Bitget reports a missing file as False; the others raise
# FileNotFoundError.
def _binance_get(url, dest):
    binance._download(url, dest)
    return True


def _bybit_get(url, dest):
    bybit._download(url, dest)
    return True


def _okx_get(url, dest):
    okx._download(url, dest)
    return True


def _bitget_get(url, dest):
    return bitget._fetch(url, dest)


_IMPORTERS = (("binance", _binance_get), ("bybit", _bybit_get),
              ("okx", _okx_get), ("bitget", _bitget_get))


def _no_sleep():
    """Patch the backoff sleep so retried attempts run immediately."""
    from lrvx.archives import _download

    return mock.patch.object(_download, "_sleep", lambda _s: None)


class NotPublishedIsAskedOnceTests(_ServerCase):
    """A missing day costs one request on every importer."""

    def test_404_is_not_retried(self) -> None:
        for name, get in _IMPORTERS:
            with self.subTest(importer=name):
                path = f"/{name}/missing.zip"
                dest = self.tmp / name / "missing.zip"
                started = time.monotonic()
                try:
                    found = get(self.url(path), dest)
                except FileNotFoundError as exc:
                    found = False
                    self.assertIn(self.url(path), str(exc))
                elapsed = time.monotonic() - started
                self.assertFalse(found)
                self.assertEqual(self.server.hits.get(path), 1,
                                 f"{name} asked a 404 more than once")
                self.assertLess(elapsed, 1.0)
                self.assertFalse(dest.exists())

    def test_403_is_not_retried(self) -> None:
        for name, get in _IMPORTERS:
            with self.subTest(importer=name):
                path = f"/{name}/forbidden.zip"
                self.server.script[path] = [("status", 403)]
                dest = self.tmp / name / "forbidden.zip"
                try:
                    found = get(self.url(path), dest)
                except FileNotFoundError:
                    found = False
                self.assertFalse(found)
                self.assertEqual(self.server.hits.get(path), 1)


class TransientFailureTests(_ServerCase):
    """Failures that may clear are retried, with the same policy on
    every importer, and a later good answer lands in the mirror."""

    def _retried_then_ok(self, first) -> None:
        for name, get in _IMPORTERS:
            with self.subTest(importer=name):
                path = f"/{name}/day.zip"
                good = _zip_bytes()
                self.server.script[path] = [first, ("ok", good)]
                dest = self.tmp / name / "day.zip"
                with _no_sleep():
                    self.assertTrue(get(self.url(path), dest))
                self.assertEqual(self.server.hits[path], 2)
                self.assertEqual(dest.read_bytes(), good)
                self.assertEqual(sorted(p.name for p in dest.parent.iterdir()),
                                 ["day.zip"])

    def test_500_is_retried(self) -> None:
        self._retried_then_ok(("status", 500))

    def test_503_is_retried(self) -> None:
        self._retried_then_ok(("status", 503))

    def test_body_shorter_than_content_length_is_retried(self) -> None:
        good = _zip_bytes()
        self._retried_then_ok(("short", good[: len(good) // 2], len(good)))

    def test_body_that_is_not_a_zip_is_retried(self) -> None:
        good = _zip_bytes()
        # A complete HTTP answer whose body is a zip cut in half: the
        # length matches what was sent, the archive does not open.
        self._retried_then_ok(("raw", good[: len(good) // 2]))

    def test_timeout_is_retried(self) -> None:
        from lrvx.archives import _download

        path = "/slow/day.zip"
        good = _zip_bytes()
        self.server.script[path] = [("stall", 1.0, good), ("ok", good)]
        dest = self.tmp / "slow" / "day.zip"
        with _no_sleep():
            _download.fetch(self.url(path), dest, timeout=0.2)
        self.assertEqual(self.server.hits[path], 2)
        self.assertEqual(dest.read_bytes(), good)

    def test_gzip_cut_short_is_retried(self) -> None:
        from lrvx.archives import _download

        path = "/bybit/day.csv.gz"
        good = _gz_bytes("timestamp,symbol\n" + "1,BTCUSDT\n" * 5000)
        self.server.script[path] = [("raw", good[:-20]), ("ok", good)]
        dest = self.tmp / "bybit" / "day.csv.gz"
        with _no_sleep():
            _download.fetch(self.url(path), dest)
        self.assertEqual(self.server.hits[path], 2)
        self.assertEqual(dest.read_bytes(), good)

    def test_persistent_failure_leaves_nothing_behind(self) -> None:
        from lrvx.archives import _download

        for first in (("status", 500), ("raw", b"not a zip at all")):
            with self.subTest(answer=first[0]):
                path = f"/bad/{first[0]}.zip"
                self.server.script[path] = [first]
                dest = self.tmp / "bad" / f"{first[0]}.zip"
                with _no_sleep(), self.assertRaises(_download.DownloadError) as cm:
                    _download.fetch(self.url(path), dest, retries=3)
                self.assertNotIsInstance(cm.exception, FileNotFoundError)
                self.assertIn(self.url(path), str(cm.exception))
                self.assertEqual(self.server.hits[path], 3)
                self.assertFalse(dest.exists())
                self.assertEqual(list(dest.parent.iterdir()), [])

    def test_success_on_first_attempt_is_one_request(self) -> None:
        from lrvx.archives import _download

        path = "/fine/day.zip"
        self.server.script[path] = [("ok", _zip_bytes())]
        dest = self.tmp / "fine" / "day.zip"
        _download.fetch(self.url(path), dest)
        self.assertEqual(self.server.hits[path], 1)
        with zipfile.ZipFile(dest) as zf:
            self.assertEqual(zf.namelist(), ["x.csv"])


class InjectedOpenerTests(unittest.TestCase):
    """The opener is injectable, so the policy is testable without a
    socket too."""

    def test_opener_sees_the_url_and_timeout(self) -> None:
        from lrvx.archives import _download

        seen = []
        body = _zip_bytes()

        def opener(url, timeout):
            seen.append((url, timeout))
            return io.BytesIO(body)

        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "a.zip"
            _download.fetch("http://example.invalid/a.zip", dest,
                            opener=opener, timeout=7)
            self.assertEqual(dest.read_bytes(), body)
        self.assertEqual(seen, [("http://example.invalid/a.zip", 7)])


# ── corrupt cache entries ───────────────────────────────────────────


_AGG_HEADER = ("agg_trade_id,price,quantity,first_trade_id,last_trade_id,"
               "transact_time,is_buyer_maker")
_DAY_MS = 86_400_000
_JAN15_MS = 1_705_276_800_000  # 2024-01-15T00:00:00Z


def _agg_zip(dest: Path, day_index: int = 0, rows: int = 3) -> Path:
    base = _JAN15_MS + day_index * _DAY_MS
    lines = [_AGG_HEADER] + [
        f"{1000 + day_index * 100 + i},42000.5,0.01,1,1,{base + i * 1000 + 5},false"
        for i in range(rows)]
    dest.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(dest.with_suffix(".csv").name, "\n".join(lines) + "\n")
    return dest


class CorruptCacheEntryTests(unittest.TestCase):
    def test_cached_file_that_does_not_open_is_removed(self) -> None:
        from lrvx.archives import _download

        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            mirror = tmp / "mirror"
            day = date(2024, 1, 15)
            cached = binance._mirror_path(mirror, "BTCUSDT", "um-futures", day)
            cached.parent.mkdir(parents=True, exist_ok=True)
            cached.write_bytes(b"PK\x03\x04 truncated")
            with mock.patch.object(binance, "_download") as dl:
                with self.assertRaises(_download.CorruptArchiveError) as cm:
                    binance.range_to_lrvx("BTCUSDT", "um-futures", day, day,
                                          tmp / "tape", mirror=mirror)
            dl.assert_not_called()
            self.assertIn(str(cached), str(cm.exception))
            self.assertFalse(cached.exists())

    def test_user_file_is_never_removed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp) / "BTCUSDT-aggTrades-2024-01-15.zip"
            bad.write_bytes(b"PK\x03\x04 truncated")
            with self.assertRaises(Exception):
                binance.aggtrades_to_lrvx(bad, Path(tmp) / "tape")
            self.assertTrue(bad.exists())


# ── shared cache by default ─────────────────────────────────────────


class SharedCacheTests(unittest.TestCase):
    """Without ``mirror``, every importer and product downloads into
    ``<cache root>/<exchange>`` and a second run asks for nothing."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="archive-cache-")
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)
        self.root = self.tmp / "cache"
        env = mock.patch.dict(os.environ, {"LRVX_ARCHIVE_CACHE": str(self.root)})
        env.start()
        self.addCleanup(env.stop)

    def _count_downloads(self, module, make):
        calls = []

        def fake(url, dest, **_kw):
            calls.append((url, Path(dest)))
            make(Path(dest))

        return mock.patch.object(module, "_download", fake), calls

    def test_binance_aggtrades_uses_the_shared_cache(self) -> None:
        patcher, calls = self._count_downloads(binance, _agg_zip)
        for _ in range(2):
            with patcher:
                binance.range_to_lrvx("BTCUSDT", "um-futures", "2024-01-15",
                                      "2024-01-15", self.tmp / "tape")
        self.assertEqual(len(calls), 1)
        self.assertEqual(
            calls[0][1],
            binance._mirror_path(self.root / "binance", "BTCUSDT",
                                 "um-futures", date(2024, 1, 15)))
        self.assertTrue(calls[0][1].is_relative_to(self.root / "binance"))

    def test_binance_book_uses_the_shared_cache(self) -> None:
        def make(dest: Path) -> None:
            dest.parent.mkdir(parents=True, exist_ok=True)
            with zipfile.ZipFile(dest, "w") as zf:
                zf.writestr("x.csv",
                            "update_id,best_bid_price,best_bid_qty,best_ask_price,"
                            "best_ask_qty,transaction_time,event_time\n"
                            f"1,100.0,1.0,101.0,1.0,{_JAN15_MS + 1},{_JAN15_MS + 1}\n")

        patcher, calls = self._count_downloads(binance, make)
        for _ in range(2):
            with patcher:
                binance.range_book_to_lrvx("BTCUSDT", "um-futures", "t1",
                                           "2024-01-15", "2024-01-15",
                                           self.tmp / "tape")
        self.assertEqual(len(calls), 1)
        self.assertTrue(calls[0][1].is_relative_to(self.root / "binance"))

    def test_every_exchange_caches_under_market_and_symbol(self) -> None:
        day = date(2024, 1, 15)
        cases = (
            (binance._mirror_path(Path("m"), "BTCUSDT", "um-futures", day),
             "m/um-futures/BTCUSDT/BTCUSDT-aggTrades-2024-01-15.zip"),
            (binance._book_mirror_path(Path("m"), "BTCUSDT", "um-futures", "t1", day),
             "m/um-futures/BTCUSDT/BTCUSDT-bookTicker-2024-01-15.zip"),
            (bybit._mirror_path(Path("m"), "BTCUSDT", "linear", day),
             "m/linear/BTCUSDT/BTCUSDT2024-01-15.csv.gz"),
            (okx._mirror_path(Path("m"), "BTC-USDT-SWAP", "swap", day),
             "m/swap/BTC-USDT-SWAP/BTC-USDT-SWAP-trades-2024-01-15.zip"),
            (okx._mirror_path(Path("m"), "BTC-USD-240329", "futures", day),
             "m/futures/BTC-USD/BTC-USD-futureschain-trades-2024-01-15.zip"),
            (bitget._mirror_path(Path("m"), "BTCUSDT", "umcbl", day),
             "m/umcbl/BTCUSDT/BTCUSDT_UMCBL_20240115_001.zip"),
        )
        for got, want in cases:
            self.assertEqual(got.as_posix(), want)


# ── symbol shape ────────────────────────────────────────────────────


class SymbolShapeTests(unittest.TestCase):
    """A symbol of the wrong shape for the market fails before any
    request, with an example of the right form."""

    BAD = (
        (binance, "cm-futures", "BTCUSDT", "BTCUSD_PERP"),
        (binance, "cm-futures", "BTCUSD", "BTCUSD_PERP"),
        (binance, "um-futures", "btcusdt", "BTCUSDT"),
        (binance, "um-futures", "BTC-USDT", "BTCUSDT"),
        (binance, "spot", "BTC/USDT", "BTCUSDT"),
        (binance, "spot", "BTCUSD_PERP", "BTCUSDT"),
        (bybit, "linear", "BTCUSD", "BTCUSDT"),
        (bybit, "linear", "BTC-USDT", "BTCUSDT"),
        (bybit, "inverse", "BTCUSDT", "BTCUSD"),
        (bybit, "spot", "btcusdt", "BTCUSDT"),
        (okx, "swap", "BTC-USDT", "BTC-USDT-SWAP"),
        (okx, "spot", "BTCUSDT", "BTC-USDT"),
        (okx, "spot", "btc-usdt", "BTC-USDT"),
        (okx, "futures", "BTC-USD-SWAP", "BTC-USD-240329"),
        (okx, "option", "BTC-USD-240329", "BTC-USD-240329-50000-C"),
        (bitget, "umcbl", "BTCUSDT_UMCBL", "BTCUSDT"),
        (bitget, "umcbl", "BTCUSD", "BTCUSDT"),
        (bitget, "cmcbl", "BTCUSDT", "BTCPERP"),
        (bitget, "dmcbl", "BTCUSDT", "BTCUSD"),
        (bitget, "spot", "BTC-USDT", "BTCUSDT"),
    )

    GOOD = (
        (binance, "cm-futures", "BTCUSD_PERP"),
        (binance, "cm-futures", "ETHUSD_240329"),
        (binance, "um-futures", "BTCUSDT"),
        (binance, "um-futures", "1000PEPEUSDT"),
        (binance, "um-futures", "BTCUSDT_240329"),
        (binance, "spot", "ETHBTC"),
        (bybit, "linear", "BTCUSDT"),
        (bybit, "linear", "BTCPERP"),
        (bybit, "linear", "BTC-27DEC24"),
        (bybit, "inverse", "BTCUSD"),
        (bybit, "inverse", "BTCUSDZ24"),
        (bybit, "spot", "MNTUSDT"),
        (okx, "swap", "BTC-USDT-SWAP"),
        (okx, "spot", "BTC-USDT"),
        (okx, "futures", "BTC-USD-240329"),
        (okx, "option", "BTC-USD-240126-42000-P"),
        (bitget, "umcbl", "DOGEUSDT"),
        (bitget, "cmcbl", "BTCPERP"),
        (bitget, "dmcbl", "BTCUSD"),
        (bitget, "spot", "BTCUSDT"),
    )

    def _range(self, module, symbol, market, tmp: Path):
        with mock.patch.object(module, "_download" if module is not bitget
                               else "_fetch") as dl:
            try:
                module.range_to_lrvx(symbol, market, "2024-01-15", "2024-01-15",
                                     tmp / "tape", mirror=tmp / "mirror")
            finally:
                dl.assert_not_called()

    def test_bad_symbols_fail_before_any_request(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            for module, market, symbol, example in self.BAD:
                with self.subTest(exchange=module.__name__, market=market,
                                  symbol=symbol):
                    with self.assertRaises(ValueError) as cm:
                        self._range(module, symbol, market, Path(tmp))
                    msg = str(cm.exception)
                    self.assertIn(symbol, msg)
                    self.assertIn(example, msg)
                    self.assertFalse((Path(tmp) / "mirror").exists())

    def test_good_symbols_pass_the_check(self) -> None:
        for module, market, symbol in self.GOOD:
            with self.subTest(exchange=module.__name__, market=market,
                              symbol=symbol):
                module.check_symbol(symbol, market)

    def test_binance_book_checks_the_symbol(self) -> None:
        with tempfile.TemporaryDirectory() as tmp, \
                mock.patch.object(binance, "_download") as dl:
            with self.assertRaises(ValueError) as cm:
                binance.range_book_to_lrvx("BTCUSDT", "cm-futures", "t1",
                                           "2024-01-15", "2024-01-15",
                                           Path(tmp) / "tape")
            dl.assert_not_called()
        self.assertIn("BTCUSD_PERP", str(cm.exception))

    def test_cli_reports_the_shape_error(self) -> None:
        with tempfile.TemporaryDirectory() as tmp, \
                mock.patch.object(binance, "_download") as dl:
            err = io.StringIO()
            with redirect_stderr(err):
                code = cli.main(["archive", "binance", "--market", "cm-futures",
                                 "--symbol", "BTCUSDT", "--from", "2024-01-15",
                                 "--to", "2024-01-15", "--out", f"{tmp}/tape",
                                 "--mirror", f"{tmp}/mirror"])
            dl.assert_not_called()
        self.assertEqual(code, 1)
        self.assertIn("BTCUSD_PERP", err.getvalue())


# ── a range that finds nothing ──────────────────────────────────────


class EmptyRangeTests(_ServerCase):
    """Every day missing is an error that names the range and the
    first URL tried; with ``skip_missing`` too."""

    def _point_at_server(self, module, attr="_BASE_URL"):
        return mock.patch.object(module, attr, self.server.base)

    def test_every_day_missing_fails(self) -> None:
        cases = (
            (binance, "BTCUSDT", "um-futures",
             "/data/futures/um/daily/aggTrades/BTCUSDT/"
             "BTCUSDT-aggTrades-2024-01-15.zip"),
            (bybit, "BTCUSDT", "linear", "/trading/BTCUSDT/BTCUSDT2024-01-15.csv.gz"),
            (okx, "BTC-USDT-SWAP", "swap",
             "/20240115/BTC-USDT-SWAP-trades-2024-01-15.zip"),
            (bitget, "BTCUSDT", "umcbl", "/UMCBL/BTCUSDT/BTCUSDT_UMCBL_20240115_001.zip"),
        )
        for module, symbol, market, first_path in cases:
            for skip in (False, True):
                with self.subTest(exchange=module.__name__, skip_missing=skip):
                    with self._point_at_server(module), \
                            self.assertRaises(FileNotFoundError) as cm:
                        module.range_to_lrvx(symbol, market, "2024-01-15",
                                             "2024-01-17", self.tmp / "tape",
                                             mirror=self.tmp / "mirror",
                                             parallel=1, skip_missing=skip)
                    msg = str(cm.exception)
                    self.assertIn("produced no files", msg)
                    self.assertIn(symbol, msg)
                    self.assertIn(market, msg)
                    self.assertIn("2024-01-15", msg)
                    self.assertIn("2024-01-17", msg)
                    self.assertIn(self.server.base + first_path, msg)
                    self.assertFalse((self.tmp / "tape").exists())

    def test_every_day_missing_is_asked_once_per_day(self) -> None:
        with self._point_at_server(binance), self.assertRaises(FileNotFoundError):
            binance.range_to_lrvx("BTCUSDT", "um-futures", "2024-01-15",
                                  "2024-01-17", self.tmp / "tape",
                                  mirror=self.tmp / "mirror", parallel=1)
        self.assertEqual(sum(self.server.hits.values()), 3)

    def test_one_missing_day_names_its_url(self) -> None:
        present = ("/data/futures/um/daily/aggTrades/BTCUSDT/"
                   "BTCUSDT-aggTrades-2024-01-15.zip")
        self.server.script[present] = [("ok", _agg_zip(self.tmp / "src.zip")
                                        .read_bytes())]
        with self._point_at_server(binance), \
                self.assertRaises(FileNotFoundError) as cm:
            binance.range_to_lrvx("BTCUSDT", "um-futures", "2024-01-15",
                                  "2024-01-16", self.tmp / "tape",
                                  mirror=self.tmp / "mirror", parallel=1)
        self.assertIn("BTCUSDT-aggTrades-2024-01-16.zip", str(cm.exception))
        self.assertIn("skip", str(cm.exception))
        with self._point_at_server(binance):
            stats = binance.range_to_lrvx("BTCUSDT", "um-futures", "2024-01-15",
                                          "2024-01-16", self.tmp / "tape",
                                          mirror=self.tmp / "mirror",
                                          parallel=1, skip_missing=True)
        self.assertEqual((stats.files_processed, stats.trades_written), (1, 3))

    def test_skip_missing_does_not_skip_a_failing_download(self) -> None:
        path = ("/data/futures/um/daily/aggTrades/BTCUSDT/"
                "BTCUSDT-aggTrades-2024-01-15.zip")
        self.server.script[path] = [("status", 500)]
        with self._point_at_server(binance), _no_sleep(), \
                self.assertRaises(RuntimeError):
            binance.range_to_lrvx("BTCUSDT", "um-futures", "2024-01-15",
                                  "2024-01-15", self.tmp / "tape",
                                  mirror=self.tmp / "mirror", parallel=1,
                                  skip_missing=True)

    def test_cli_exits_1_and_names_the_first_url(self) -> None:
        err = io.StringIO()
        with self._point_at_server(bybit), redirect_stderr(err):
            code = cli.main(["archive", "bybit", "--symbol", "BTCUSDT",
                             "--market", "linear", "--from", "2024-01-15",
                             "--to", "2024-01-16", "--skip-missing",
                             "--out", str(self.tmp / "tape"),
                             "--mirror", str(self.tmp / "mirror")])
        self.assertEqual(code, 1)
        self.assertIn("produced no files", err.getvalue())
        self.assertIn(self.server.base + "/trading/BTCUSDT/BTCUSDT2024-01-15.csv.gz",
                      err.getvalue())


# ── CLI summary ─────────────────────────────────────────────────────


class SummaryLineTests(unittest.TestCase):
    """The summary line names the first and last trade written, in
    UTC, and says when the exchange cuts its days on UTC+8."""

    def _run(self, argv):
        out = io.StringIO()
        with redirect_stdout(out):
            code = cli.main(argv)
        self.assertEqual(code, 0)
        return out.getvalue()

    def test_binance_summary_has_first_and_last_trade(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            z = _agg_zip(Path(tmp) / "BTCUSDT-aggTrades-2024-01-15.zip")
            out = self._run(["archive", "binance", "--csv", str(z),
                             "--out", f"{tmp}/tape"])
            self.assertIn("first_trade=2024-01-15T00:00:00.005Z", out)
            self.assertIn("last_trade=2024-01-15T00:00:02.005Z", out)
            again = self._run(["archive", "binance", "--csv", str(z),
                               "--out", f"{tmp}/tape"])
            self.assertIn("trades_written=0", again)
            self.assertIn("first_trade=- last_trade=-", again)

    def test_okx_range_summary_says_utc_plus_8(self) -> None:
        # One OKX day (UTC+8) 2024-01-15: 2024-01-14T16:00Z .. 2024-01-15T16:00Z.
        start_ms = 1_705_248_000_000
        text = ("instrument_name,trade_id,side,price,size,created_time\n"
                f"BTC-USDT-SWAP,1,buy,42000,1,{start_ms + 1}\n"
                f"BTC-USDT-SWAP,2,sell,42001,1,{start_ms + _DAY_MS - 1}\n")
        with tempfile.TemporaryDirectory() as tmp:
            mirror = Path(tmp) / "mirror"
            dest = okx._mirror_path(mirror, "BTC-USDT-SWAP", "swap",
                                    date(2024, 1, 15))
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(_zip_bytes("BTC-USDT-SWAP-trades-2024-01-15.csv", text))
            out = self._run(["archive", "okx", "--symbol", "BTC-USDT-SWAP",
                             "--market", "swap", "--from", "2024-01-15",
                             "--to", "2024-01-15", "--out", f"{tmp}/tape",
                             "--mirror", str(mirror)])
        self.assertIn("first_trade=2024-01-14T16:00:00.001Z", out)
        self.assertIn("last_trade=2024-01-15T15:59:59.999Z", out)
        self.assertIn("UTC+8", out)
        self.assertIn("2024-01-14T16:00:00Z", out)
        self.assertEqual(len(out.strip().splitlines()), 1)

    def test_help_states_the_day_boundary(self) -> None:
        for exchange, zone in (("binance", "UTC"), ("bybit", "UTC"),
                               ("okx", "UTC+8"), ("bitget", "UTC+8")):
            with self.subTest(exchange=exchange):
                out = io.StringIO()
                with redirect_stdout(out), self.assertRaises(SystemExit):
                    cli.main(["archive", exchange, "--help"])
                text = " ".join(out.getvalue().split())
                self.assertIn(f"{zone} day", text)
                self.assertIn("archive-cache", text)


@unittest.skipUnless(net.ENABLED, net.SKIP_REASON)
class RealMissingDayTests(unittest.TestCase):
    """Against the real archives: a day no exchange has published yet
    is answered as not published, asked once per URL, and the range
    fails with the first URL tried."""

    CASES = (
        (binance, "BTCUSDT", "um-futures"),
        (bybit, "BTCUSDT", "linear"),
        (okx, "BTC-USDT-SWAP", "swap"),
        (bitget, "BTCUSDT", "umcbl"),
    )

    def test_future_day_produces_no_files(self) -> None:
        for module, symbol, market in self.CASES:
            with self.subTest(exchange=module.__name__):
                with tempfile.TemporaryDirectory() as tmp:
                    started = time.monotonic()
                    with self.assertRaises(FileNotFoundError) as cm:
                        module.range_to_lrvx(symbol, market, "2099-01-01",
                                             "2099-01-01", Path(tmp) / "tape",
                                             mirror=Path(tmp) / "mirror",
                                             parallel=1)
                    self.assertLess(time.monotonic() - started, 30.0)
                    self.assertIn("produced no files", str(cm.exception))
                    self.assertIn("https://", str(cm.exception))
                    leftovers = [p for p in (Path(tmp) / "mirror").rglob("*")
                                 if p.is_file()]
                    self.assertEqual(leftovers, [])


if __name__ == "__main__":
    unittest.main()
