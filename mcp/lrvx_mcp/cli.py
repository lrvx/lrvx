"""``lrvx-mcp`` console-script entry point with subcommands.

Two verbs:

* ``lrvx-mcp serve`` — start the MCP server over stdio. This is the
  same behaviour the bare ``lrvx-mcp`` invocation has always had,
  preserved for back-compat (and because that's what MCP clients
  invoke directly).
* ``lrvx-mcp init`` — write a working ``.mcp.json`` for the current
  shell environment so users do not hand-craft the JSON.

Bare ``lrvx-mcp`` (no args) keeps invoking ``serve`` for back-compat
with existing MCP client configurations.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional


# ── init ──────────────────────────────────────────────────────────────


_DEFAULT_RUNTIME_STATE = "$HOME/.lrvx/runtime.json"


def _resolve_command() -> List[str]:
    """Pick the most portable form of 'launch the lrvx-mcp server'.

    Prefer the installed console script (`lrvx-mcp serve`) — it
    works from any active Python regardless of which interpreter the
    MCP client launches. Fall back to `python -m lrvx_mcp.server`
    when the script is not yet on PATH (e.g. an editable install in
    a venv whose ``bin/`` is not exported)."""
    bin_path = shutil.which("lrvx-mcp")
    if bin_path:
        return [bin_path, "serve"]
    return [sys.executable, "-m", "lrvx_mcp.server"]


def _build_lrvx_entry(engine_url: Optional[str],
                      token: Optional[str]) -> Dict[str, Any]:
    cmd = _resolve_command()
    env: Dict[str, str] = {
        "LRVX_RUNTIME_STATE": _DEFAULT_RUNTIME_STATE,
    }
    if engine_url:
        env["LRVX_CONTROL_URL"] = engine_url
    if token:
        env["LRVX_CONTROL_TOKEN"] = token
    return {
        "command": cmd[0],
        "args": cmd[1:],
        "env": env,
    }


# The entry `init` wrote, and the README showed, under the FLOX name: the
# server keyed `flox`, launched as `flox-mcp` (bare or `serve`) or as
# `python -m flox_mcp.server`.
_LEGACY_KEY = "flox"
_LEGACY_SCRIPTS = ("flox-mcp", "flox-mcp.exe")
_LEGACY_MODULES = ("flox_mcp", "flox_mcp.server")


def _is_legacy_entry(entry: Any) -> bool:
    """True when ``entry`` launches this server under its FLOX name."""
    if not isinstance(entry, dict):
        return False
    args = [str(a) for a in entry.get("args") or []]
    if Path(str(entry.get("command", ""))).name in _LEGACY_SCRIPTS:
        return args in ([], ["serve"])
    return len(args) >= 2 and args[0] == "-m" and args[1] in _LEGACY_MODULES


def _global_config_path() -> Path:
    """Where MCP clients look for a global server config.

    Claude Code reads ``~/.config/claude/.mcp.json`` on POSIX and
    Claude Desktop reads a platform-specific path. We standardise on
    the POSIX location; users with the Desktop client can pass an
    explicit path via the upcoming ``--out`` flag (out of scope for
    the v1 init).
    """
    return Path(os.environ.get("XDG_CONFIG_HOME", "~/.config")).expanduser() \
        / "claude" / ".mcp.json"


def _local_config_path() -> Path:
    return Path.cwd() / ".mcp.json"


def _read_existing(path: Path) -> Dict[str, Any]:
    if not path.exists():
        return {}
    try:
        return json.loads(path.read_text())
    except json.JSONDecodeError as exc:
        raise SystemExit(
            f"lrvx-mcp init: existing {path} is not valid JSON ({exc}). "
            f"Fix or remove it before running init."
        )


def _render(config: Dict[str, Any]) -> str:
    return json.dumps(config, indent=2, sort_keys=True) + "\n"


def cmd_init(args: argparse.Namespace) -> int:
    target = _global_config_path() if args.global_ else _local_config_path()

    lrvx_entry = _build_lrvx_entry(args.engine_url, args.token)
    new_config: Dict[str, Any] = _read_existing(target) if not args.print else {}
    new_config.setdefault("mcpServers", {})

    if "lrvx" in new_config["mcpServers"] and not args.overwrite \
            and not args.print:
        print(
            f"lrvx-mcp init: {target} already has an `mcpServers.lrvx` "
            f"entry. Re-run with --overwrite to replace it.",
            file=sys.stderr,
        )
        return 2

    # An entry for the same server under its FLOX name is replaced, not
    # kept next to the new one; the engine wiring it carried moves over
    # unless the command line sets it.
    legacy = new_config["mcpServers"].get(_LEGACY_KEY)
    replaced = _is_legacy_entry(legacy)
    if replaced:
        del new_config["mcpServers"][_LEGACY_KEY]
        legacy_env = legacy.get("env") or {}
        for name, given in (("CONTROL_URL", args.engine_url),
                            ("CONTROL_TOKEN", args.token)):
            if not given and legacy_env.get("FLOX_" + name):
                lrvx_entry["env"]["LRVX_" + name] = legacy_env["FLOX_" + name]
    new_config["mcpServers"]["lrvx"] = lrvx_entry

    rendered = _render(new_config)
    if args.print:
        sys.stdout.write(rendered)
        return 0

    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(rendered)
    if replaced:
        print(f"replaced the mcpServers.{_LEGACY_KEY} entry (flox-mcp) "
              f"with mcpServers.lrvx")
    print(f"wrote {target}")
    print("restart your MCP client to pick up the lrvx server.")
    return 0


# ── dispatcher ────────────────────────────────────────────────────────


def main(argv: Optional[List[str]] = None) -> int:
    """``lrvx-mcp`` console-script entry point.

    Bare ``lrvx-mcp`` (no subcommand) still launches the server, so
    existing MCP-client configurations keep working. ``init`` is the
    new bootstrap path.
    """
    raw = sys.argv[1:] if argv is None else list(argv)

    # Back-compat: bare invocation = serve. That's what every MCP
    # client config in the wild does.
    p = argparse.ArgumentParser(prog="lrvx-mcp")
    sub = p.add_subparsers(dest="cmd", required=True)

    init_p = sub.add_parser(
        "init",
        help="Write a working .mcp.json for the current environment.",
    )
    init_p.add_argument(
        "--global", dest="global_", action="store_true",
        help="Write to the user-global config (~/.config/claude/.mcp.json) "
             "instead of ./.mcp.json.",
    )
    init_p.add_argument(
        "--overwrite", action="store_true",
        help="Replace an existing mcpServers.lrvx entry instead of "
             "refusing.",
    )
    init_p.add_argument(
        "--print", action="store_true",
        help="Print the merged config to stdout, write nothing.",
    )
    init_p.add_argument(
        "--engine-url",
        help="Wire tier-5/6 control tools at this engine URL "
             "(sets LRVX_CONTROL_URL). Pair with --token.",
    )
    init_p.add_argument(
        "--token",
        help="ControlServer token (sets LRVX_CONTROL_TOKEN). The "
             "token is printed by `lrvx engine sim`.",
    )

    sub.add_parser("serve", help="Start the MCP server over stdio.")

    # Help variants print usage and return 0 (a plain return value for
    # embedders, instead of SystemExit from inside argparse). Must run
    # before the bare-invocation fallback, which would otherwise launch
    # the stdio server for `-h`/`help` too.
    if raw and raw[0] in ("--help", "-h", "help"):
        p.print_help()
        return 0

    # Back-compat: bare invocation = serve. That's what every MCP client
    # config in the wild does.
    if not raw or raw[0] not in ("init", "serve"):
        from lrvx_mcp.server import main as serve_main
        return serve_main() or 0

    args = p.parse_args(raw)
    if args.cmd == "init":
        return cmd_init(args)
    if args.cmd == "serve":
        from lrvx_mcp.server import main as serve_main
        return serve_main() or 0
    if args.cmd is None:  # only reachable via --help
        p.print_help()
        return 0
    return 0  # pragma: no cover


if __name__ == "__main__":
    raise SystemExit(main())
