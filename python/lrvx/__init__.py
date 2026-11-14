"""lrvx Python bindings.

The compiled pybind11 extension lives at `lrvx._lrvx` and is
re-exported here so users can `import lrvx` and access the full
public surface (indicators, engine, backtest, replay, etc.) at the
top level. Type information is shipped via `lrvx/__init__.pyi`
and the PEP 561 marker `lrvx/py.typed`.
"""
# The `_lrvx/` directory next to this file holds only typing stubs
# (*.pyi, no __init__.py). When the compiled extension is absent — a
# source-tree import, or a build made for a different Python version
# whose ABI tag does not match — the import system falls back to that
# stub directory as an empty namespace package. Pure-Python surfaces
# (lrvx.cli, lrvx.lookahead, ...) are expected to work in that
# degraded mode, so the package still imports; but any access to a
# native name must raise an ImportError naming the actual cause rather
# than a bare AttributeError.
from . import _lrvx as _native
_NATIVE_AVAILABLE = getattr(_native, "__file__", None) is not None
del _native

if _NATIVE_AVAILABLE:
    from ._lrvx import *  # noqa: F401,F403  # also brings in the `targets` submodule
else:
    import sys as _sys
    _NATIVE_IMPORT_ERROR = (
        "lrvx: the compiled extension `lrvx._lrvx` is not "
        f"importable under Python {_sys.version_info.major}."
        f"{_sys.version_info.minor}; the typing-stub directory was "
        "resolved instead. Either this is a source-tree import (only "
        "pure-Python submodules like lrvx.cli work here), or the "
        "native module was built for a different Python version. "
        "Rebuild the bindings with this interpreter, or run with the "
        "interpreter they were built for."
    )
    del _sys

    def __getattr__(name):  # PEP 562: fires only for names not found above
        # `from lrvx import cli` resolves through this hook before
        # the import machinery falls back to a submodule import, so
        # pure-Python submodules must be served here; only names with
        # no submodule behind them are genuinely native and get the
        # explanatory error.
        import importlib
        try:
            return importlib.import_module(f".{name}", __name__)
        except ModuleNotFoundError:
            raise ImportError(
                f"{_NATIVE_IMPORT_ERROR} (while accessing lrvx.{name})"
            ) from None

# The release this package was built from. The compiled library carries it
# (lrvx_version(), from the version CMakeLists.txt declares), so a stale
# extension next to newer Python sources cannot report the newer number. A
# source-tree import without the extension falls back to the installed
# distribution's metadata.
if _NATIVE_AVAILABLE:
    __version__: str = version()
else:
    def _installed_version() -> str:
        try:
            from importlib.metadata import version as _dist_version
            return _dist_version("lrvx")
        except Exception:  # noqa: BLE001 -- no distribution either
            return "0+unknown"

    __version__ = _installed_version()
    del _installed_version

del _NATIVE_AVAILABLE  # branch flag only; keep it out of the module surface

# Pure-Python research surfaces that sit on top of the compiled bindings.
from .live_queue_calibrator import (  # noqa: F401
    CalibrationResult,
    LiveQueueCalibrator,
    TestOrderHelper,
)
from .custom_venue import (  # noqa: F401
    CustomVenue,
    assemble_custom_venue,
)
