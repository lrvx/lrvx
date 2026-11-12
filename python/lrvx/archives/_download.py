"""One download policy for every archive importer.

``fetch(url, dest)`` puts one archive file into the mirror cache:

  * 403, 404 and 410 mean the exchange does not publish the file (a
    CDN in front of a bucket answers 403 for a missing object). The URL
    is asked once and ``NotPublished`` (a ``FileNotFoundError``) is
    raised; nothing is retried.
  * 408, 429, any 5xx, a timeout, a dropped connection, a body shorter
    than its ``Content-Length`` and a body that does not open as the
    zip or gzip its name says are transient: the request is retried,
    ``retries`` attempts in all, waiting ``backoff * 2**attempt``
    seconds between them. When every attempt fails, ``DownloadError``
    (a ``RuntimeError``) is raised.
  * Any other status fails at once with ``DownloadError``.

The body is written to a temporary name in the destination directory
and renamed onto ``dest`` only once it is complete and opens, so the
cache never holds a partial or unreadable file under its real name,
and a failed attempt leaves nothing behind.

``evict_if_corrupt(path)`` guards the conversion of a cached file: if
it turns out not to open, it is removed from the cache before the
error is raised, so the next run downloads it again.
"""
from __future__ import annotations

import gzip
import http.client
import os
import tempfile
import time
import urllib.error
import urllib.request
import zipfile
import zlib
from contextlib import contextmanager
from pathlib import Path
from typing import Callable, Iterator, Optional

NOT_PUBLISHED = frozenset({403, 404, 410})
_TRANSIENT_4XX = frozenset({408, 429})

DEFAULT_RETRIES = 3
DEFAULT_BACKOFF = 2.0
DEFAULT_TIMEOUT = 60.0

_CHUNK = 1 << 20
_sleep = time.sleep


class NotPublished(FileNotFoundError):
    """The exchange does not publish the file at ``url``."""

    def __init__(self, url: str, status: Optional[int] = None,
                 message: Optional[str] = None) -> None:
        self.url = url
        self.status = status
        why = f"HTTP {status}" if status else "not found"
        super().__init__(message or f"not published ({why}): {url}")


class DownloadError(RuntimeError):
    """Every attempt to download ``url`` failed."""

    def __init__(self, url: str, reason: str) -> None:
        self.url = url
        super().__init__(f"failed to download {url}: {reason}")


class CorruptArchiveError(ValueError):
    """A file does not open as the zip or gzip archive its name says."""


def check_archive(path: Path, name: Optional[str] = None) -> None:
    """Raise CorruptArchiveError unless ``path`` opens and reads to the
    end as a zip (every member's CRC checked) or a gzip stream, chosen
    by ``name`` (default: the file's own name). Any other name passes
    unchecked."""
    name = (name or Path(path).name).lower()
    try:
        if name.endswith(".zip"):
            with zipfile.ZipFile(path) as zf:
                if not zf.namelist():
                    raise CorruptArchiveError(f"{path}: empty zip archive")
                bad = zf.testzip()
                if bad is not None:
                    raise CorruptArchiveError(f"{path}: bad CRC in member {bad}")
        elif name.endswith(".gz"):
            with gzip.open(path, "rb") as f:
                while f.read(_CHUNK):
                    pass
    except CorruptArchiveError:
        raise
    except (zipfile.BadZipFile, EOFError, zlib.error, OSError) as exc:
        raise CorruptArchiveError(f"{path}: does not open as an archive: {exc}") from exc


_OPEN_ERRORS = (zipfile.BadZipFile, gzip.BadGzipFile, EOFError, zlib.error)


@contextmanager
def evict_if_corrupt(path: Path) -> Iterator[None]:
    """Remove the cached ``path`` and raise CorruptArchiveError when the
    body of the ``with`` fails because the file does not open."""
    try:
        yield
    except _OPEN_ERRORS as exc:
        try:
            Path(path).unlink()
        except OSError:
            pass
        raise CorruptArchiveError(
            f"{path}: cached archive does not open ({exc}); removed it from "
            f"the cache, run the import again to download it afresh") from exc


def _urlopen(url: str, timeout: float):
    return urllib.request.urlopen(url, timeout=timeout)


def _content_length(resp) -> Optional[int]:
    headers = getattr(resp, "headers", None)
    if headers is None:
        return None
    value = headers.get("Content-Length")
    try:
        return int(value) if value is not None else None
    except ValueError:
        return None


def _attempt(url: str, tmp: Path, name: str, opener, timeout: float) -> None:
    """One request: the body into ``tmp``, checked for length and that
    it opens. Raises on any failure."""
    resp = opener(url, timeout)
    try:
        expected = _content_length(resp)
        got = 0
        with open(tmp, "wb") as f:
            while True:
                chunk = resp.read(_CHUNK)
                if not chunk:
                    break
                f.write(chunk)
                got += len(chunk)
    finally:
        close = getattr(resp, "close", None)
        if close is not None:
            close()
    if expected is not None and got != expected:
        raise DownloadError(url, f"body is {got} bytes, Content-Length {expected}")
    check_archive(tmp, name)


def fetch(url: str, dest: Path, *, retries: Optional[int] = None,
          backoff: Optional[float] = None, timeout: Optional[float] = None,
          opener: Optional[Callable] = None) -> Path:
    """Download ``url`` to ``dest`` under the policy in the module
    docstring. ``opener(url, timeout)`` returns a readable response; the
    default is ``urllib.request.urlopen``."""
    retries = DEFAULT_RETRIES if retries is None else max(1, int(retries))
    backoff = DEFAULT_BACKOFF if backoff is None else float(backoff)
    timeout = DEFAULT_TIMEOUT if timeout is None else float(timeout)
    opener = opener or _urlopen
    dest = Path(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    last = "no attempt made"
    for attempt in range(retries):
        fd, tmp_name = tempfile.mkstemp(dir=dest.parent, prefix=f".{dest.name}.",
                                        suffix=".part")
        os.close(fd)
        tmp = Path(tmp_name)
        try:
            _attempt(url, tmp, dest.name, opener, timeout)
            os.replace(tmp, dest)
            return dest
        except urllib.error.HTTPError as exc:
            if exc.code in NOT_PUBLISHED:
                raise NotPublished(url, exc.code) from None
            last = f"HTTP {exc.code} {exc.reason}"
            if exc.code < 500 and exc.code not in _TRANSIENT_4XX:
                raise DownloadError(url, last) from exc
        except (DownloadError, CorruptArchiveError) as exc:
            last = str(exc)
        except (urllib.error.URLError, http.client.HTTPException, OSError) as exc:
            last = f"{type(exc).__name__}: {exc}"
        finally:
            try:
                tmp.unlink()
            except FileNotFoundError:
                pass
        if attempt < retries - 1:
            _sleep(backoff * (2 ** attempt))
    raise DownloadError(url, f"{retries} attempts, last: {last}")


__all__ = [
    "CorruptArchiveError",
    "DownloadError",
    "NotPublished",
    "check_archive",
    "evict_if_corrupt",
    "fetch",
]
