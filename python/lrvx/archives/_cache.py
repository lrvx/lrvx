"""Shared on-disk cache for downloaded exchange archives.

`~/.lrvx/archive-cache/` (or `LRVX_ARCHIVE_CACHE`) is the cache root
for every importer and product. Each exchange keeps its files under
`<cache_root>/<exchange>/<market>/<symbol>/<file name as published>`,
where `<symbol>` is the symbol in the file's URL (for OKX futures and
options, the instrument family). A range import passed `mirror` uses
that directory in place of `<cache_root>/<exchange>`.

Files land in the cache only complete and readable: a download is
written to a temporary name next to its destination and renamed once
it has arrived in full and opens (see `_download`). The cache is never
evicted automatically; deleting the directory, or any file in it, at
any time is safe, and the next import downloads what is missing.
"""
from __future__ import annotations

import os
from pathlib import Path
from typing import Optional


_DEFAULT_CACHE_ENV = "LRVX_ARCHIVE_CACHE"


def cache_root(override: Optional[Path] = None) -> Path:
    """Resolve the shared archive cache root.

    Precedence: explicit ``override`` > ``LRVX_ARCHIVE_CACHE`` env var
    > ``~/.lrvx/archive-cache``. The directory is created on demand."""
    if override is not None:
        root = Path(override).expanduser()
    else:
        env = os.environ.get(_DEFAULT_CACHE_ENV)
        if env:
            root = Path(env).expanduser()
        else:
            root = Path.home() / ".lrvx" / "archive-cache"
    root.mkdir(parents=True, exist_ok=True)
    return root


def exchange_dir(exchange: str, mirror=None) -> Path:
    """The directory an importer caches ``exchange``'s files in:
    ``mirror`` when given, else ``<cache_root>/<exchange>``."""
    if mirror is not None:
        path = Path(mirror).expanduser()
    else:
        path = cache_root() / exchange
    path.mkdir(parents=True, exist_ok=True)
    return path


__all__ = ["cache_root", "exchange_dir"]
