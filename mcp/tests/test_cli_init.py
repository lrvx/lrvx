"""Tests for ``lrvx-mcp init`` — the .mcp.json bootstrap path."""
from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "mcp"))

from lrvx_mcp import cli  # noqa: E402


def _ns(**kwargs) -> argparse.Namespace:
    """Build the argparse Namespace cli.cmd_init expects."""
    defaults = dict(
        global_=False, overwrite=False, print=False,
        engine_url=None, token=None,
    )
    defaults.update(kwargs)
    return argparse.Namespace(**defaults)


class CliInitTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="lrvx-mcp-init-")
        self.cwd_before = os.getcwd()
        os.chdir(self.tmp)

    def tearDown(self) -> None:
        os.chdir(self.cwd_before)
        # Best-effort cleanup; tmp dirs from mkdtemp are not auto-removed.
        for child in Path(self.tmp).rglob("*"):
            if child.is_file():
                child.unlink()
        for child in sorted(Path(self.tmp).rglob("*"), reverse=True):
            if child.is_dir():
                child.rmdir()
        Path(self.tmp).rmdir()

    def test_writes_local_mcp_json(self) -> None:
        rc = cli.cmd_init(_ns())
        self.assertEqual(rc, 0)

        path = Path(self.tmp) / ".mcp.json"
        self.assertTrue(path.exists())
        cfg = json.loads(path.read_text())
        self.assertIn("lrvx", cfg["mcpServers"])
        lrvx = cfg["mcpServers"]["lrvx"]
        self.assertIn("command", lrvx)
        self.assertIn("args", lrvx)
        # Default env var so tier-4 read tools find a snapshot path.
        self.assertEqual(
            lrvx["env"]["LRVX_RUNTIME_STATE"],
            "$HOME/.lrvx/runtime.json",
        )
        # No engine wiring unless explicitly requested.
        self.assertNotIn("LRVX_CONTROL_URL", lrvx["env"])
        self.assertNotIn("LRVX_CONTROL_TOKEN", lrvx["env"])

    def test_engine_url_and_token_wire_tier56(self) -> None:
        rc = cli.cmd_init(_ns(
            engine_url="http://127.0.0.1:8765", token="secret"))
        self.assertEqual(rc, 0)
        lrvx = json.loads(
            (Path(self.tmp) / ".mcp.json").read_text())["mcpServers"]["lrvx"]
        self.assertEqual(lrvx["env"]["LRVX_CONTROL_URL"],
                         "http://127.0.0.1:8765")
        self.assertEqual(lrvx["env"]["LRVX_CONTROL_TOKEN"], "secret")

    def test_merges_into_existing_other_servers(self) -> None:
        existing = {
            "mcpServers": {
                "other": {"command": "/usr/bin/other", "args": []},
            },
        }
        target = Path(self.tmp) / ".mcp.json"
        target.write_text(json.dumps(existing))

        rc = cli.cmd_init(_ns())
        self.assertEqual(rc, 0)
        merged = json.loads(target.read_text())["mcpServers"]
        # Existing entry preserved untouched.
        self.assertEqual(merged["other"], existing["mcpServers"]["other"])
        # New lrvx entry added.
        self.assertIn("lrvx", merged)

    def test_refuses_to_overwrite_existing_lrvx_entry(self) -> None:
        existing = {"mcpServers": {"lrvx": {"command": "old", "args": []}}}
        target = Path(self.tmp) / ".mcp.json"
        target.write_text(json.dumps(existing))

        rc = cli.cmd_init(_ns())
        self.assertEqual(rc, 2)
        # File untouched.
        self.assertEqual(json.loads(target.read_text()), existing)

    def test_overwrite_flag_replaces_existing_entry(self) -> None:
        existing = {"mcpServers": {"lrvx": {"command": "old", "args": []}}}
        target = Path(self.tmp) / ".mcp.json"
        target.write_text(json.dumps(existing))

        rc = cli.cmd_init(_ns(overwrite=True))
        self.assertEqual(rc, 0)
        cfg = json.loads(target.read_text())
        self.assertNotEqual(cfg["mcpServers"]["lrvx"]["command"], "old")

    def test_replaces_flox_entry_that_launches_flox_mcp(self) -> None:
        other = {"command": "/usr/bin/other", "args": []}
        existing = {
            "mcpServers": {
                "flox": {
                    "command": "/usr/local/bin/flox-mcp",
                    "args": ["serve"],
                    "env": {
                        "FLOX_RUNTIME_STATE": "$HOME/.flox/runtime.json",
                        "FLOX_CONTROL_URL": "http://127.0.0.1:1111",
                        "FLOX_CONTROL_TOKEN": "old-token",
                    },
                },
                "other": other,
            },
        }
        target = Path(self.tmp) / ".mcp.json"
        target.write_text(json.dumps(existing))

        rc = cli.cmd_init(_ns(engine_url="http://127.0.0.1:8765"))
        self.assertEqual(rc, 0)
        servers = json.loads(target.read_text())["mcpServers"]
        self.assertEqual(sorted(servers), ["lrvx", "other"])
        self.assertEqual(servers["other"], other)
        env = servers["lrvx"]["env"]
        # The command line wins; what it does not set moves over.
        self.assertEqual(env["LRVX_CONTROL_URL"], "http://127.0.0.1:8765")
        self.assertEqual(env["LRVX_CONTROL_TOKEN"], "old-token")
        self.assertEqual(env["LRVX_RUNTIME_STATE"], "$HOME/.lrvx/runtime.json")
        self.assertFalse(any(k.startswith("FLOX_") for k in env))

    def test_replaces_flox_entry_that_runs_the_flox_mcp_module(self) -> None:
        existing = {"mcpServers": {"flox": {
            "command": "/usr/bin/python3", "args": ["-m", "flox_mcp.server"]}}}
        target = Path(self.tmp) / ".mcp.json"
        target.write_text(json.dumps(existing))

        rc = cli.cmd_init(_ns())
        self.assertEqual(rc, 0)
        servers = json.loads(target.read_text())["mcpServers"]
        self.assertEqual(sorted(servers), ["lrvx"])

    def test_keeps_a_flox_entry_for_another_server(self) -> None:
        unrelated = {"command": "/opt/flox/bin/flox", "args": ["mcp"]}
        target = Path(self.tmp) / ".mcp.json"
        target.write_text(json.dumps({"mcpServers": {"flox": unrelated}}))

        rc = cli.cmd_init(_ns())
        self.assertEqual(rc, 0)
        servers = json.loads(target.read_text())["mcpServers"]
        self.assertEqual(servers["flox"], unrelated)
        self.assertIn("lrvx", servers)

    def test_print_does_not_write(self) -> None:
        target = Path(self.tmp) / ".mcp.json"
        # Capture stdout.
        from io import StringIO
        buf = StringIO()
        with patch.object(sys, "stdout", buf):
            rc = cli.cmd_init(_ns(print=True))
        self.assertEqual(rc, 0)
        self.assertFalse(target.exists())
        cfg = json.loads(buf.getvalue())
        self.assertIn("lrvx", cfg["mcpServers"])

    def test_corrupt_existing_file_is_a_clear_error(self) -> None:
        target = Path(self.tmp) / ".mcp.json"
        target.write_text("{ this is not valid json }")
        with self.assertRaises(SystemExit) as ctx:
            cli.cmd_init(_ns())
        self.assertIn("not valid JSON", str(ctx.exception))


class DispatcherTests(unittest.TestCase):
    """The `lrvx-mcp` entry point preserves back-compat: bare invocation
    must still launch the server, because that's what every MCP-client
    config in the wild calls.

    These tests stub out the real ``lrvx_mcp.server`` module so they
    don't require the optional ``mcp`` dependency to be installed.
    """

    def setUp(self) -> None:
        import types
        self._real_server = sys.modules.get("lrvx_mcp.server")
        stub = types.ModuleType("lrvx_mcp.server")
        stub.main = lambda: 0  # type: ignore[attr-defined]
        sys.modules["lrvx_mcp.server"] = stub

    def tearDown(self) -> None:
        if self._real_server is None:
            sys.modules.pop("lrvx_mcp.server", None)
        else:
            sys.modules["lrvx_mcp.server"] = self._real_server

    def test_no_args_calls_server_main(self) -> None:
        with patch.object(
                sys.modules["lrvx_mcp.server"], "main",
                return_value=0) as mock_serve:
            rc = cli.main([])
        self.assertEqual(rc, 0)
        mock_serve.assert_called_once()

    def test_serve_subcommand_calls_server_main(self) -> None:
        with patch.object(
                sys.modules["lrvx_mcp.server"], "main",
                return_value=0) as mock_serve:
            rc = cli.main(["serve"])
        self.assertEqual(rc, 0)
        mock_serve.assert_called_once()

    def test_python_dash_m_runs_the_cli(self) -> None:
        # `init --print` needs no MCP SDK and writes nothing.
        import subprocess
        out = subprocess.run(
            [sys.executable, "-m", "lrvx_mcp", "init", "--print"],
            capture_output=True, text=True, cwd=tempfile.gettempdir(),
            env=dict(os.environ, PYTHONPATH=str(REPO_ROOT / "mcp")))
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertIn("lrvx", json.loads(out.stdout)["mcpServers"])


class CliHelpTests(unittest.TestCase):
    def test_help_prints_usage_and_does_not_start_the_server(self) -> None:
        import contextlib
        import io

        for flag in ("--help", "-h", "help"):
            with self.subTest(flag=flag):
                out = io.StringIO()
                with contextlib.redirect_stdout(out):
                    rc = cli.main([flag])
                self.assertEqual(rc, 0)
                self.assertIn("usage: lrvx-mcp", out.getvalue())
                self.assertIn("init", out.getvalue())
                self.assertIn("serve", out.getvalue())


if __name__ == "__main__":  # pragma: no cover
    unittest.main()
