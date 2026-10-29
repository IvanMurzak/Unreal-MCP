"""Cases for the commands/ tooling: bump_version, test_module_uplugin, release_github (hermetic: temp copies, fake gh)."""
from __future__ import annotations

import contextlib
import io
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "commands"))
import bump_version  # noqa: E402
import release_github  # noqa: E402
import test_module_uplugin  # noqa: E402

UPLUGIN = """{
  "FileVersion": 3,
  "Version": 1,
  "VersionName": "0.25.0",
  "Modules": [
    {
      "Name": "UnrealMcpEditor",
      "Type": "Editor",
      "LoadingPhase": "Default"
    }
  ]
}
"""
CSPROJ = """<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <Version>0.25.0</Version>
    <Copyright>Copyright © 2026 Ivan Murzak</Copyright>
  </PropertyGroup>
  <ItemGroup>
    <PackageReference Include="Some.Package" Version="1.2.3" />
  </ItemGroup>
</Project>
"""
PACKAGE_JSON = """{
  "name": "unreal-mcp-cli",
  "version": "0.25.0",
  "dependencies": {
    "@baizor/gamedev-cli-core": "^0.7.2"
  }
}
"""
PACKAGE_LOCK = """{
  "name": "unreal-mcp-cli",
  "version": "0.25.0",
  "lockfileVersion": 3,
  "packages": {
    "": {
      "name": "unreal-mcp-cli",
      "version": "0.25.0"
    },
    "node_modules/foo": {
      "version": "0.25.0"
    }
  }
}
"""
VERSION_TS = """// header
export const PACKAGE_NAME: string = 'unreal-mcp-cli';
export const PACKAGE_VERSION: string = '0.25.0';
"""
FILES = {
    "UnrealMCP/UnrealMCP.uplugin": UPLUGIN,
    "bridge/src/com.IvanMurzak.Unreal.MCP.Bridge.csproj": CSPROJ,
    "cli/package.json": PACKAGE_JSON,
    "cli/package-lock.json": PACKAGE_LOCK,
    "cli/src/version.ts": VERSION_TS,
}


def run_quiet(fn, *args):
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
        code = fn(*args)
    return code, out.getvalue()


class BumpVersionTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.write_tree("\n")

    def tearDown(self):
        self.tmp.cleanup()

    def write_tree(self, newline):
        for rel, text in FILES.items():
            path = self.root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(text.replace("\n", newline).encode("utf-8"))

    def bump(self, *args):
        return run_quiet(bump_version.main, ["--root", str(self.root), *args])

    def read(self, rel):
        return (self.root / rel).read_bytes().decode("utf-8")

    def test_bump_rewrites_only_the_version_strings(self):
        code, out = self.bump("--new-version", "0.26.0")
        self.assertEqual(code, 0, out)
        expected = {rel: text for rel, text in FILES.items()}
        expected["UnrealMCP/UnrealMCP.uplugin"] = UPLUGIN.replace('"VersionName": "0.25.0"', '"VersionName": "0.26.0"')
        expected["bridge/src/com.IvanMurzak.Unreal.MCP.Bridge.csproj"] = CSPROJ.replace(
            "<Version>0.25.0</Version>", "<Version>0.26.0</Version>")
        expected["cli/package.json"] = PACKAGE_JSON.replace('"version": "0.25.0"', '"version": "0.26.0"')
        expected["cli/src/version.ts"] = VERSION_TS.replace("'0.25.0'", "'0.26.0'")
        # Only the two root-level lock entries after "name": "unreal-mcp-cli"; node_modules/foo is untouched.
        expected["cli/package-lock.json"] = PACKAGE_LOCK.replace(
            '"name": "unreal-mcp-cli",\n  "version": "0.25.0"', '"name": "unreal-mcp-cli",\n  "version": "0.26.0"'
        ).replace('"name": "unreal-mcp-cli",\n      "version": "0.25.0"',
                  '"name": "unreal-mcp-cli",\n      "version": "0.26.0"')
        for rel, text in expected.items():
            self.assertEqual(self.read(rel), text, rel)
        self.assertIn('"node_modules/foo": {\n      "version": "0.25.0"', self.read("cli/package-lock.json"))
        self.assertIn("CLI package-lock.json updated (2 occurrences)", out)
        self.assertIn("Version: 0.25.0 -> 0.26.0", out)

    def test_preserves_crlf_and_writes_no_bom(self):
        self.write_tree("\r\n")
        code, out = self.bump("--new-version", "1.0.0")
        self.assertEqual(code, 0, out)
        for rel in FILES:
            raw = (self.root / rel).read_bytes()
            self.assertFalse(raw.startswith(b"\xef\xbb\xbf"), rel)
            self.assertEqual(raw.count(b"\n"), raw.count(b"\r\n"), rel)
        self.assertIn('"VersionName": "1.0.0"', self.read("UnrealMCP/UnrealMCP.uplugin"))

    def test_bom_is_dropped(self):
        path = self.root / "cli/package.json"
        path.write_bytes(b"\xef\xbb\xbf" + PACKAGE_JSON.encode("utf-8"))
        self.assertEqual(self.bump("--new-version", "0.26.0")[0], 0)
        self.assertEqual(path.read_bytes(), PACKAGE_JSON.replace("0.25.0", "0.26.0").encode("utf-8"))

    def test_prerelease_npm_version_is_replaced(self):
        (self.root / "cli/package.json").write_text(PACKAGE_JSON.replace("0.25.0", "0.25.0-rc.1+b5"), encoding="utf-8")
        self.assertEqual(self.bump("--new-version", "0.26.0")[0], 0)
        self.assertIn('"version": "0.26.0",', self.read("cli/package.json"))

    def test_what_if_writes_nothing(self):
        for flag in ("--what-if", "--dry-run"):
            code, out = self.bump("--new-version", "0.26.0", flag)
            self.assertEqual(code, 0, out)
            self.assertIn("Total replacements: 4", out)
            for rel, text in FILES.items():
                self.assertEqual(self.read(rel), text, rel)

    def test_invalid_version_fails(self):
        code, out = self.bump("--new-version", "1.2")
        self.assertEqual(code, 1)
        self.assertIn("Invalid semantic version format: 1.2", out)

    def test_same_version_is_a_noop(self):
        code, out = self.bump("--new-version", "0.25.0")
        self.assertEqual(code, 0)
        self.assertIn("same as current version", out)
        self.assertEqual(self.read("cli/package.json"), PACKAGE_JSON)

    def test_missing_uplugin_fails(self):
        (self.root / "UnrealMCP/UnrealMCP.uplugin").unlink()
        code, out = self.bump("--new-version", "0.26.0")
        self.assertEqual(code, 1)
        self.assertIn("Could not find .uplugin", out)

    def test_missing_file_is_a_warning(self):
        (self.root / "cli/src/version.ts").unlink()
        code, out = self.bump("--new-version", "0.26.0")
        self.assertEqual(code, 0, out)
        self.assertIn("[warn] File not found: cli/src/version.ts", out)
        self.assertIn("Updated 3 files", out)


class TestModuleUpluginTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name) / "UnrealMCP.uplugin"
        self.path.write_bytes(UPLUGIN.encode("utf-8"))

    def tearDown(self):
        self.tmp.cleanup()

    def toggle(self, action):
        return run_quiet(test_module_uplugin.main, ["--action", action, "--uplugin", str(self.path)])

    def names(self):
        return [m["Name"] for m in json.loads(self.path.read_text(encoding="utf-8"))["Modules"]]

    def test_add_and_remove_are_idempotent_and_round_trip(self):
        self.assertEqual(self.toggle("Add")[0], 0)
        self.assertEqual(self.names(), ["UnrealMcpEditor", "UnrealMcpEditorTests"])
        code, out = self.toggle("add")
        self.assertEqual(code, 0)
        self.assertIn("already present", out)
        self.assertEqual(self.names(), ["UnrealMcpEditor", "UnrealMcpEditorTests"])
        self.assertEqual(self.toggle("Remove")[0], 0)
        self.assertEqual(self.path.read_text(encoding="utf-8"), UPLUGIN)
        code, out = self.toggle("Remove")
        self.assertEqual(code, 0)
        self.assertIn("not present", out)

    def test_crlf_is_preserved(self):
        self.path.write_bytes(UPLUGIN.replace("\n", "\r\n").encode("utf-8"))
        self.toggle("Add")
        raw = self.path.read_bytes()
        self.assertEqual(raw.count(b"\n"), raw.count(b"\r\n"))

    def test_missing_file_and_modules_fail(self):
        self.path.unlink()
        self.assertEqual(self.toggle("Add")[0], 1)
        self.path.write_text('{"FileVersion": 3}\n', encoding="utf-8")
        code, out = self.toggle("Add")
        self.assertEqual(code, 1)
        self.assertIn("no Modules array", out)


class ReleaseGithubTests(unittest.TestCase):
    """Runs the wrapper against fake git/gh executables that log their argv."""

    def setUp(self):
        if os.name == "nt":
            self.skipTest("fake executables are POSIX shell scripts")
        self.tmp = tempfile.TemporaryDirectory()
        self.bin = Path(self.tmp.name)
        self.log = self.bin / "calls.log"
        for name in ("git", "gh"):
            exe = self.bin / name
            exe.write_text('#!/bin/sh\necho "%s $*" >> "%s"\n' % (name, self.log)
                           + ('[ "$1" = run ] && [ "$2" = list ] && echo 12345\n' if name == "gh" else "")
                           + "exit 0\n")
            exe.chmod(exe.stat().st_mode | stat.S_IEXEC)
        self.env = mock.patch.dict(os.environ, {"PATH": str(self.bin)})
        self.env.start()
        self.sleep = mock.patch.object(release_github.time, "sleep")
        self.sleep.start()

    def tearDown(self):
        self.sleep.stop()
        self.env.stop()
        self.tmp.cleanup()

    def calls(self):
        return self.log.read_text().splitlines() if self.log.exists() else []

    def test_dry_run_dispatches_and_waits(self):
        code, out = run_quiet(release_github.main, ["--mode", "dry-run", "--wait", "--ref", "rel"])
        self.assertEqual(code, 0, out)
        self.assertEqual(self.calls(), [
            "gh auth status",
            "gh workflow run release.yml --repo IvanMurzak/Unreal-MCP --ref rel -f dry_run=true",
            "gh run list --repo IvanMurzak/Unreal-MCP --workflow release.yml --limit 1 --json databaseId --jq .[0].databaseId",
            "gh run watch 12345 --repo IvanMurzak/Unreal-MCP",
        ])

    def test_publish_without_wait(self):
        code, out = run_quiet(release_github.main, ["--mode", "publish", "--repo", "o/r"])
        self.assertEqual(code, 0, out)
        self.assertEqual(self.calls(), ["gh auth status",
                                        "gh workflow run release.yml --repo o/r --ref main -f dry_run=false"])

    def test_prepare_requires_new_version(self):
        code, out = run_quiet(release_github.main, ["--mode", "prepare"])
        self.assertEqual(code, 1)
        self.assertIn("--new-version is required", out)
        self.assertEqual(self.calls(), [])

    def test_missing_gh_fails(self):
        (self.bin / "gh").unlink()
        code, out = run_quiet(release_github.main, ["--mode", "watch"])
        self.assertEqual(code, 1)
        self.assertIn("Required command not found on PATH: gh", out)


class HelpTests(unittest.TestCase):
    def test_every_script_has_help(self):
        for name in ("bump_version.py", "release_github.py", "test_module_uplugin.py"):
            result = subprocess.run([sys.executable, str(ROOT / "commands" / name), "--help"],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn("usage:", result.stdout)


if __name__ == "__main__":
    unittest.main()
