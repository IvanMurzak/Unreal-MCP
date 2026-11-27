"""Tests for the CI step scripts: .github/scripts/ue_ci.py, .github/scripts/ue_env.py, scripts/ue_machine_lock.py, scripts/ue_json.py."""
from __future__ import annotations

import base64
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PureWindowsPath
import re
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from unittest import mock
import uuid
import zipfile

ROOT = Path(__file__).resolve().parents[1]
APPLE_SDK = ROOT / "tests/apple-sdk"


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ci = load("ue_ci", ROOT / ".github/scripts/ue_ci.py")
ue_env = load("ue_env", ROOT / ".github/scripts/ue_env.py")
import ue_json  # noqa: E402
import ue_machine_lock as lock  # noqa: E402  (scripts/ is on sys.path under `unittest discover -s scripts`)

# The negative tests below drive real guards, which print `::error::` / `::warning::` workflow commands
# (in-process and from child processes). On a runner those lines would become annotations on a green job,
# so command processing is paused for this module's run. The guards themselves are unchanged.
_STOP_COMMANDS_TOKEN = f"ue-ci-tests-{uuid.uuid4().hex}"


def setUpModule():
    if os.environ.get("GITHUB_ACTIONS") == "true":
        print(f"::stop-commands::{_STOP_COMMANDS_TOKEN}", flush=True)


def tearDownModule():
    if os.environ.get("GITHUB_ACTIONS") == "true":
        print(f"::{_STOP_COMMANDS_TOKEN}::", flush=True)


def fake_uat(folder: Path, body: str) -> str:
    """An executable stand-in for RunUAT: a Python script that receives BuildPlugin's arguments."""
    script = folder / "fake_uat.py"
    script.write_text("import sys, json, pathlib\nargs = dict(a.split('=', 1) for a in sys.argv[2:] if '=' in a)\n"
                      "package = pathlib.Path(args['-Package'])\n" + textwrap.dedent(body), encoding="utf-8")
    if os.name == "nt":
        launcher = folder / "fake_uat.bat"
        launcher.write_text(f'@"{sys.executable}" "{script}" %*\n', encoding="utf-8")
    else:
        launcher = folder / "fake_uat.sh"
        launcher.write_text(f'#!/bin/sh\nexec "{sys.executable}" "{script}" "$@"\n', encoding="utf-8")
        launcher.chmod(0o755)
    return str(launcher)


class CiStepTests(unittest.TestCase):
    def setUp(self):
        self.temp = Path(tempfile.mkdtemp())
        self.runner_temp = self.temp / "runner-temp"
        self.runner_temp.mkdir()
        self.github_env = self.temp / "github_env"
        self.github_env.write_text("")
        patcher = mock.patch.dict(os.environ, {"RUNNER_TEMP": str(self.runner_temp), "GITHUB_ENV": str(self.github_env),
                                               "UE_PLATFORM": "Win64", "UE_ROOT": str(self.temp / "UE")})
        patcher.start()
        self.addCleanup(patcher.stop)

    def tearDown(self):
        import shutil
        shutil.rmtree(self.temp, ignore_errors=True)

    def exported(self) -> dict:
        return dict(line.split("=", 1) for line in self.github_env.read_text().splitlines() if line)

    def test_suite_leaves_engine_gating_to_the_suite_and_exports_the_report(self):
        repo = self.temp / "repo"
        calls = []

        def fake_run(command, **_):
            calls.append([str(c) for c in command])
            (repo / ".artifacts").mkdir(parents=True, exist_ok=True)
            (repo / ".artifacts/suite-result.json").write_text(json.dumps({"report": "/r"}))
            return 0

        with mock.patch.object(ci, "ROOT", repo), mock.patch.object(ci, "run", fake_run):
            for argv in (["suite", "--ue", "5.6"], ["suite", "--ue", "5.8"], ["suite", "--mode", "full"]):
                self.assertEqual(ci.main(argv), 0)
        # Which extensions an engine builds is extension.json minEngineVersion, read by extension_suite.py itself.
        self.assertFalse(any("--extensions" in call for call in calls))
        self.assertIn("full", calls[2])
        self.assertEqual(self.exported()["REPORT_DIR"], "/r")

    def test_suite_failure_is_a_failed_step(self):
        with mock.patch.object(ci, "ROOT", self.temp / "repo"), mock.patch.object(ci, "run", lambda *a, **k: 3):
            self.assertEqual(ci.main(["suite", "--ue", "5.8"]), 1)

    def test_build_plugin_dist_c2_guard(self):
        for modules, expected in ((["UnrealMcpRuntime"], 0), (["UnrealMcpRuntime", "UnrealMcpEditorTests"], 1), ([], 1)):
            uat = fake_uat(self.temp, f"""
                package.mkdir(parents=True, exist_ok=True)
                (package / 'UnrealMCP.uplugin').write_text(json.dumps({{'Modules': [{{'Name': m}} for m in {modules!r}]}}))
            """)
            with mock.patch.dict(os.environ, {"UE_RUNUAT": uat}):
                self.assertEqual(ci.main(["build-plugin-dist"]), expected, modules)

    def test_only_the_validation_build_uses_strict_includes(self):
        calls = []
        with mock.patch.object(ci, "run", lambda command, **_: calls.append(command) or 5), \
                mock.patch.dict(os.environ, {"UE_RUNUAT": "uat"}):
            ci.main(["build-plugin-dist"])
            ci.main(["plugin-zip", "--version", "1.2.3"])
        self.assertEqual([("-StrictIncludes" in c) for c in calls], [True, False])

    def test_build_plugin_dist_reports_a_uat_failure(self):
        uat = fake_uat(self.temp, "sys.exit(5)\n")
        with mock.patch.dict(os.environ, {"UE_RUNUAT": uat}):
            self.assertEqual(ci.main(["build-plugin-dist"]), 1)

    def build_plugin_dist(self, uat_body: str):
        """build-plugin-dist against a fake RunUAT; returns (exit code, the retry sleeps)."""
        sleeps = []
        with mock.patch.dict(os.environ, {"UE_RUNUAT": fake_uat(self.temp, uat_body)}), \
                mock.patch.object(ci.ubt_conflict.time, "sleep", sleeps.append), \
                mock.patch.dict(ci.ubt_conflict.WAIT_BUDGET, {"left": 600}):
            return ci.main(["build-plugin-dist"]), sleeps

    def test_build_plugin_waits_out_a_ubt_mutex_conflict(self):
        # DEV-287: another UBT on the machine (a local or agent build) held UBT's global mutex (UE 5.7 wording).
        marker = self.temp / "tried"
        result = self.build_plugin_dist(f"""
            marker = pathlib.Path({str(marker)!r})
            if not marker.exists():
                marker.write_text('1')
                print({r"A conflicting instance of Global\UnrealBuildTool_Mutex_cc1e is already running."!r})
                print('Result: Failed (ConflictingInstance)')
                sys.exit(10)
            package.mkdir(parents=True, exist_ok=True)
            (package / 'UnrealMCP.uplugin').write_text(json.dumps({{'Modules': [{{'Name': 'UnrealMcpRuntime'}}]}}))
        """)
        self.assertEqual(result, (0, [ci.ubt_conflict.RETRY_DELAYS[0]]))

    def test_build_plugin_never_retries_a_compile_error(self):
        result = self.build_plugin_dist("print(\"Foo.cpp(1): error C2065: 'X': undeclared identifier\")\n"
                                        "print('A conflicting instance of UnrealBuildTool is already running.')\nsys.exit(6)\n")
        self.assertEqual(result, (1, []))

    def fake_plugin(self) -> Path:
        plugin = self.temp / "repo/UnrealMCP"
        for rel in ("Config/x.ini", "Resources/Icon128.png", "Content/a.txt", "Source/UnrealMcpRuntime/a.cpp",
                    "Source/UnrealMcpRql/a.cpp", "Source/UnrealMcpEditor/a.cpp", "Source/UnrealMcpFab/a.cpp",
                    "Source/UnrealMcpEditorTests/a.cpp",
                    "Source/UnrealMcpEditor/Private/Server/UnrealMcpServerDownload.cpp",
                    "Source/UnrealMcpEditor/Private/Sidecar/UnrealMcpBridgeDownloader.cpp",
                    "Source/UnrealMcpEditor/Private/Sidecar/UnrealMcpBridgeDownloader.h",
                    "ThirdPartyNotices.txt"):
            (plugin / rel).parent.mkdir(parents=True, exist_ok=True)
            (plugin / rel).write_text("// Copyright x")
        (plugin / "UnrealMCP.uplugin").write_text(json.dumps({"Modules": [{"Name": "UnrealMcpRuntime"}]}))
        return plugin

    def patch_repo(self, plugin: Path):
        root = plugin.parent
        return mock.patch.multiple(ci, ROOT=root, PLUGIN=plugin, STAGED_BRIDGE=plugin / "Source/ThirdParty/UnrealMcpBridge")

    def stage_fake_bridge(self, plugin: Path) -> None:
        """./bridge-staging/<rid>/ (signed dirs) + ./bridge-zips/ (release zips), as the release job downloads them."""
        for rid in ci.RIDS:
            (plugin.parent / "bridge-staging" / rid).mkdir(parents=True)
            (plugin.parent / "bridge-staging" / rid / "unreal-mcp-bridge").write_bytes(b"\x7fELF" + rid.encode())
            (plugin / "Source/ThirdParty/UnrealMcpBridge" / rid).mkdir(parents=True, exist_ok=True)
            (plugin / "Source/ThirdParty/UnrealMcpBridge" / rid / ".gitkeep").write_text("")
        (plugin.parent / "bridge-zips").mkdir()
        for rid in ci.RIDS:
            (plugin.parent / "bridge-zips" / f"unreal-mcp-bridge-{rid}.zip").write_bytes(rid.encode())

    def test_stage_bridge_then_source_zip(self):
        plugin = self.fake_plugin()
        with self.patch_repo(plugin):
            self.assertEqual(ci.main(["stage-bridge"]), 1)  # no ./bridge-zips + ./bridge-staging yet
            self.stage_fake_bridge(plugin)
            self.assertEqual(ci.main(["stage-bridge"]), 0)
            self.assertEqual(ci.main(["source-zip", "--version", "1.2.3"]), 0)
        archive = zipfile.ZipFile(plugin.parent / "dist/unreal-mcp-plugin-source-1.2.3.zip")
        names = archive.namelist()
        self.assertIn("UnrealMCP/UnrealMCP.uplugin", names)
        self.assertIn("UnrealMCP/Source/ThirdParty/UnrealMcpBridge/osx-arm64/unreal-mcp-bridge", names)
        self.assertFalse([n for n in names if "UnrealMcpEditorTests" in n])
        # The shipped trust anchor of the §6.8 bridge download: the SHA-256 of every per-RID release zip.
        sums = archive.read("UnrealMCP/Source/ThirdParty/UnrealMcpBridge/SHA256SUMS").decode()
        win = hashlib.sha256(b"win-x64").hexdigest()
        self.assertIn(f"{win}  unreal-mcp-bridge-win-x64.zip\n", sums)
        self.assertEqual(len(sums.splitlines()), len(ci.RIDS))

    def test_fab_zip_ships_the_manifest_and_no_executable(self):
        plugin = self.fake_plugin()
        with self.patch_repo(plugin):
            self.assertEqual(ci.main(["fab-zip", "--version", "1.2.3"]), 1)  # no SHA256SUMS: refuses
            self.stage_fake_bridge(plugin)
            self.assertEqual(ci.main(["stage-bridge"]), 0)
            self.assertEqual(ci.main(["fab-zip", "--version", "1.2.3"]), 0)
        names = zipfile.ZipFile(plugin.parent / "dist/unreal-mcp-plugin-fab-1.2.3.zip").namelist()
        self.assertIn("UnrealMCP/Source/ThirdParty/UnrealMcpBridge/SHA256SUMS", names)
        self.assertIn("UnrealMCP/Source/ThirdParty/UnrealMcpBridge/win-x64/.gitkeep", names)
        self.assertFalse([n for n in names if n.endswith("/unreal-mcp-bridge") or n.endswith(".exe")])
        self.assertFalse([n for n in names if "UnrealMcpEditorTests" in n])

    def test_fab_zip_is_the_manual_bridge_build(self):
        plugin = self.fake_plugin()
        rql = plugin / "Source/ThirdParty/RQL/src/value.cpp"
        rql.parent.mkdir(parents=True)
        rql.write_text("#include <a>\n", encoding="utf-8")
        runner = plugin / "Content/Python/Lib/site-packages/code_exec/runner.py"
        runner.parent.mkdir(parents=True)
        runner.write_text('"""doc"""\n', encoding="utf-8")
        with self.patch_repo(plugin):
            self.stage_fake_bridge(plugin)
            self.assertEqual(ci.main(["stage-bridge"]), 0)
            self.assertEqual(ci.main(["fab-zip", "--version", "1.2.3"]), 0)
        archive = zipfile.ZipFile(plugin.parent / "dist/unreal-mcp-plugin-fab-1.2.3.zip")
        names = archive.namelist()
        self.assertIn("UnrealMCP/Config/FabDistribution.ini", names)
        self.assertFalse([n for n in names if "BridgeDownloader" in n])
        self.assertTrue(archive.read("UnrealMCP/Source/ThirdParty/RQL/src/value.cpp").startswith(b"// Copyright (c) 2026 Ivan Murzak."))
        self.assertTrue(archive.read("UnrealMCP/Content/Python/Lib/site-packages/code_exec/runner.py").startswith(b"# Copyright (c) 2026 Ivan Murzak."))
        self.assertEqual(rql.read_text(encoding="utf-8"), "#include <a>\n")  # the repo keeps its verbatim copy

    def test_fab_zip_refuses_first_party_code_without_copyright(self):
        plugin = self.fake_plugin()
        (plugin / "Source/UnrealMcpRuntime/Bare.cpp").write_text("int x;\n", encoding="utf-8")
        with self.patch_repo(plugin):
            self.stage_fake_bridge(plugin)
            self.assertEqual(ci.main(["stage-bridge"]), 0)
            self.assertEqual(ci.main(["fab-zip", "--version", "1.2.3"]), 1)

    def test_fab_engine_zips_pin_one_engine_each_and_refresh_the_notices(self):
        plugin = self.fake_plugin()
        (plugin / "ThirdPartyNotices.txt").write_text("old")
        with self.patch_repo(plugin):
            self.assertEqual(ci.main(["fab-engine-zips", "--version", "1.2.3"]), 1)  # no released Fab asset: refuses
            self.stage_fake_bridge(plugin)
            self.assertEqual(ci.main(["stage-bridge"]), 0)
            self.assertEqual(ci.main(["fab-zip", "--version", "1.2.3"]), 0)
            (plugin / "ThirdPartyNotices.txt").write_text("corrected")
            (plugin / "UnrealMCP.uplugin").write_text(json.dumps({"Description": "corrected"}))
            stale = plugin.parent / "dist" / "unreal-mcp-plugin-fab-1.2.3-ue5.4.zip"  # an engine no longer built
            stale.write_bytes(b"old")
            github_output = self.runner_temp / "github_output"
            with mock.patch.dict(os.environ, {"GITHUB_OUTPUT": str(github_output)}):
                self.assertEqual(ci.main(["fab-engine-zips", "--version", "1.2.3"]), 0)
        names = [f"unreal-mcp-plugin-fab-1.2.3-ue{engine}.zip" for engine in ci.ENGINES]
        self.assertEqual(github_output.read_text(encoding="utf-8"), "fab_engine_zips=" + " ".join(names) + "\n")
        self.assertFalse(stale.exists())
        dist = plugin.parent / "dist"
        for engine in ci.ENGINES:
            with zipfile.ZipFile(dist / f"unreal-mcp-plugin-fab-1.2.3-ue{engine}.zip") as archive:
                pinned = json.loads(archive.read("UnrealMCP/UnrealMCP.uplugin"))
                self.assertEqual(archive.read("UnrealMCP/ThirdPartyNotices.txt"), b"corrected")
            self.assertEqual(pinned["EngineVersion"], f"{engine}.0")
            self.assertEqual(pinned["Description"], "corrected")
            self.assertEqual([m["Name"] for m in pinned["Modules"]], ["UnrealMcpRuntime"])  # the rest stays as released
        with zipfile.ZipFile(dist / "unreal-mcp-plugin-fab-1.2.3.zip") as released:
            self.assertNotIn("EngineVersion", json.loads(released.read("UnrealMCP/UnrealMCP.uplugin")))

    def test_fab_engines_match_the_release_matrix(self):
        workflow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
        self.assertEqual(sorted(set(re.findall(r"ue: '([0-9.]+)' }", workflow))), sorted(ci.ENGINES))

    def test_fab_zip_refuses_any_other_executable(self):
        plugin = self.fake_plugin()
        with self.patch_repo(plugin):
            self.stage_fake_bridge(plugin)
            self.assertEqual(ci.main(["stage-bridge"]), 0)
            (plugin / "Content/tool.dll").write_text("x")
            self.assertEqual(ci.main(["fab-zip", "--version", "1.2.3"]), 1)

    def test_find_executables_by_magic_bytes_shebang_and_extension(self):
        folder = self.temp / "scan"
        files = {"pe": b"MZ\x90\x00", "elf": b"\x7fELF\x02", "macho": b"\xcf\xfa\xed\xfe", "fat": b"\xca\xfe\xba\xbe",
                 "run": b"#!/bin/sh\n", "lib.so": b"x", "a/b/c.dylib": b"x", "readme.md": b"# MZ", "code.cpp": b"int x;"}
        for rel, data in files.items():
            (folder / rel).parent.mkdir(parents=True, exist_ok=True)
            (folder / rel).write_bytes(data)
        self.assertEqual(ci.find_executables(folder), ["a/b/c.dylib", "elf", "fat", "lib.so", "macho", "pe", "run"])

    def test_source_zip_refuses_an_engine_version_pin(self):
        plugin = self.fake_plugin()
        (plugin / "UnrealMCP.uplugin").write_text(json.dumps({"EngineVersion": "5.8.0", "Modules": []}))
        with self.patch_repo(plugin):
            self.assertEqual(ci.main(["source-zip", "--version", "1.2.3"]), 1)

    def test_plugin_zip_applies_the_r6_fallback_and_zips_the_package_root(self):
        plugin = self.fake_plugin()
        for rid in ci.RIDS:
            (plugin / "Source/ThirdParty/UnrealMcpBridge" / rid).mkdir(parents=True)
            (plugin / "Source/ThirdParty/UnrealMcpBridge" / rid / "unreal-mcp-bridge").write_text(rid)
        # The fake BuildPlugin stages only the Fab-surviving Source copy, not Binaries/ThirdParty (design R6).
        uat = fake_uat(self.temp, f"""
            import shutil
            shutil.copytree({str(plugin / 'Source/ThirdParty')!r}, package / 'Source/ThirdParty')
            (package / 'UnrealMCP.uplugin').write_text('{{}}')
        """)
        with self.patch_repo(plugin), mock.patch.dict(os.environ, {"UE_RUNUAT": uat}):
            self.assertEqual(ci.main(["plugin-zip", "--version", "1.2.3"]), 0)
        archive = zipfile.ZipFile(plugin.parent / "dist/unreal-mcp-plugin-1.2.3.zip")
        self.assertIn("UnrealMCP.uplugin", archive.namelist())
        apphost = archive.getinfo("Binaries/ThirdParty/UnrealMcpBridge/osx-arm64/unreal-mcp-bridge")
        self.assertTrue((apphost.external_attr >> 16) & 0o111, "the non-Windows apphost must stay executable")

    def released_source_zip(self) -> Path:
        """dist/unreal-mcp-plugin-source-1.2.3.zip as the release's build-plugin-zip makes it (signed bridge inside)."""
        plugin = self.fake_plugin()
        with self.patch_repo(plugin):
            self.stage_fake_bridge(plugin)
            self.assertEqual(ci.main(["stage-bridge"]), 0)
            self.assertEqual(ci.main(["source-zip", "--version", "1.2.3"]), 0)
        return plugin

    def prebuilt_uat(self, modules=("UnrealMcpRuntime",)) -> str:
        """A fake BuildPlugin: the package is the plugin it was given plus Win64 editor binaries (RuntimeDependencies
        stage only the Win64 RID into Binaries/ThirdParty), and it stamps EngineVersion like the real one."""
        return fake_uat(self.temp, f"""
            import shutil
            plugin = pathlib.Path(args['-Plugin']).parent
            shutil.copytree(plugin, package)
            (package / 'Binaries/ThirdParty/UnrealMcpBridge').mkdir(parents=True)
            shutil.copytree(plugin / 'Source/ThirdParty/UnrealMcpBridge/win-x64', package / 'Binaries/ThirdParty/UnrealMcpBridge/win-x64')
            (package / 'Binaries/Win64').mkdir(parents=True)
            (package / 'Binaries/Win64/UnrealEditor.modules').write_text('{{}}')
            for name in {list(modules)!r}:
                (package / 'Binaries/Win64' / ('UnrealEditor-' + name + '.dll')).write_bytes(b'MZ')
            descriptor = json.loads((package / 'UnrealMCP.uplugin').read_text())
            (package / 'UnrealMCP.uplugin').write_text(json.dumps({{**descriptor, 'EngineVersion': '5.6.0'}}))
        """)

    def test_prebuilt_zip_builds_the_released_source_for_one_engine_and_keeps_only_win64(self):
        plugin = self.released_source_zip()
        with self.patch_repo(plugin), mock.patch.dict(os.environ, {"UE_RUNUAT": self.prebuilt_uat()}):
            self.assertEqual(ci.main(["prebuilt-zip", "--version", "1.2.3", "--ue", "5.6"]), 0)
        with zipfile.ZipFile(plugin.parent / "dist/unreal-mcp-plugin-1.2.3-ue5.6-win64.zip") as archive:
            names = archive.namelist()
            descriptor = json.loads(archive.read("UnrealMCP/UnrealMCP.uplugin"))
        # The source asset's layout: one UnrealMCP/ folder, so the App extracts it like the source zip.
        self.assertTrue(all(n.startswith("UnrealMCP/") for n in names))
        self.assertIn("UnrealMCP/Binaries/Win64/UnrealEditor-UnrealMcpRuntime.dll", names)
        self.assertIn("UnrealMCP/Binaries/ThirdParty/UnrealMcpBridge/win-x64/unreal-mcp-bridge", names)
        self.assertIn("UnrealMCP/Source/ThirdParty/UnrealMcpBridge/win-x64/unreal-mcp-bridge", names)
        self.assertIn("UnrealMCP/Source/ThirdParty/UnrealMcpBridge/SHA256SUMS", names)
        self.assertFalse([n for n in names if "osx-" in n or "linux-" in n], "only the win-x64 bridge ships")
        self.assertEqual(descriptor["EngineVersion"], "5.6.0")
        self.assertEqual([m["Name"] for m in descriptor["Modules"]], ["UnrealMcpRuntime"])

    def test_prebuilt_zip_refuses_missing_inputs_and_binaries(self):
        plugin = self.fake_plugin()
        with self.patch_repo(plugin):
            self.assertEqual(ci.main(["prebuilt-zip", "--version", "1.2.3", "--ue", "5.6"]), 1)  # no source asset
        plugin = self.released_source_zip()
        with self.patch_repo(plugin), mock.patch.dict(os.environ, {"UE_RUNUAT": self.prebuilt_uat(modules=())}):
            self.assertEqual(ci.main(["prebuilt-zip", "--version", "1.2.3"]), 1)  # no --ue
            self.assertEqual(ci.main(["prebuilt-zip", "--version", "1.2.3", "--ue", "5.6"]), 1)  # no editor DLL
            with mock.patch.dict(os.environ, {"UE_PLATFORM": "Mac"}):
                self.assertEqual(ci.main(["prebuilt-zip", "--version", "1.2.3", "--ue", "5.6"]), 1)  # Windows only
        self.assertFalse((plugin.parent / "dist/unreal-mcp-plugin-1.2.3-ue5.6-win64.zip").exists())

    def test_engines_output_and_the_prebuilt_matrix_read_one_list(self):
        github_output = self.runner_temp / "github_output"
        with mock.patch.dict(os.environ, {"GITHUB_OUTPUT": str(github_output)}):
            self.assertEqual(ci.main(["engines"]), 0)
        self.assertEqual(github_output.read_text(encoding="utf-8"), f"engines={json.dumps(list(ci.ENGINES))}\n")
        workflow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
        self.assertIn("ue: ${{ fromJSON(needs.build-plugin-zip.outputs.engines) }}", workflow)
        self.assertIn("prebuilt-zip --version", workflow)

    def test_teardown_cleans_releases_and_removes_the_link_last(self):
        repo = self.temp / "repo"
        (repo / ".artifacts/nuget").mkdir(parents=True)
        (repo / ".artifacts/tests").mkdir(parents=True)
        (repo / "nuget.config").write_text("<!-- GENERATED by chain.py (chain/overrides/nuget.py) -->")
        (repo / "Directory.Build.targets").write_text("hand-written")
        link_target = self.temp / "python-home"
        link_target.mkdir()
        if os.name != "nt":
            os.symlink(link_target, self.runner_temp / "chain-py")
        (self.runner_temp / "UnrealMCP-Package").mkdir()
        (self.runner_temp / "UBT-x.log").write_text("log")
        order = []
        with mock.patch.object(ci, "ROOT", repo), mock.patch.dict(os.environ, {"GITHUB_JOB": "connection-smoke"}), \
                mock.patch.object(ci.ue_machine_lock, "release", lambda state: order.append("release")), \
                mock.patch.object(ci, "remove_link", lambda link: (order.append("link"), os.path.lexists(link) and os.unlink(link))):
            self.assertEqual(ci.main(["teardown"]), 0)
        self.assertEqual(order, ["release", "link"])
        self.assertFalse((repo / "nuget.config").exists())
        self.assertTrue((repo / "Directory.Build.targets").exists())
        self.assertFalse((repo / ".artifacts/nuget").exists())
        self.assertTrue((repo / ".artifacts/tests").exists())
        self.assertFalse((self.runner_temp / "UnrealMCP-Package").exists())
        self.assertFalse((self.runner_temp / "UBT-x.log").exists())
        self.assertFalse(os.path.lexists(self.runner_temp / "chain-py"))
        self.assertTrue(link_target.is_dir())

    def test_teardown_never_fails_the_job(self):
        with mock.patch.object(ci, "cleanup", mock.Mock(side_effect=OSError("disk"))), \
                mock.patch.object(ci.ue_machine_lock, "release", mock.Mock(side_effect=RuntimeError("x"))):
            self.assertEqual(ci.main(["teardown"]), 0)

    def test_teardown_fails_when_the_staged_bridge_survives(self):
        with mock.patch.dict(os.environ, {"GITHUB_JOB": "build-plugin-zip"}), mock.patch.object(ci, "cleanup", lambda *a: None), \
                mock.patch.object(ci, "clean_staged_bridge", mock.Mock(side_effect=ci.StepError("left"))), \
                mock.patch.object(ci.ue_machine_lock, "release", mock.Mock()) as release:
            self.assertEqual(ci.main(["teardown"]), 1)
        release.assert_called_once()  # the lock is still released

    def test_a_stale_lock_state_folder_does_not_count_as_holding_the_lock(self):
        state = lock.State(self.runner_temp / "ue-machine-lock")
        state.folder.mkdir()
        state.acquired.write_text("acquired")
        state.pid.write_text("999999\nnot-a-start-time")
        self.assertFalse(state.holds_lock())

    def test_chain_smoke_uses_the_public_server_when_the_lock_keeps_it_released(self):
        with tempfile.TemporaryDirectory() as folder:
            lock = Path(folder) / "lock.json"
            def chain(state):
                lock.write_text(json.dumps({"nodes": {"gamedev-mcp-server": {"state": state, "version": "9.5.4"}}}), encoding="utf-8")
                with mock.patch.dict(os.environ, {"CHAIN_LOCK_PATH": str(lock)}):
                    return ci.chain_server_released()
            self.assertTrue(chain("released"))
            self.assertFalse(chain("sha"))
            with mock.patch.dict(os.environ, {"CHAIN_LOCK_PATH": ""}):
                self.assertFalse(ci.chain_server_released())
            # A chain-built server lock still needs UNREAL_MCP_SERVER_PATH.
            lock.write_text(json.dumps({"nodes": {"gamedev-mcp-server": {"state": "sha"}}}), encoding="utf-8")
            with mock.patch.dict(os.environ, {"CHAIN_ACTIVE": "1", "CHAIN_LOCK_PATH": str(lock), "UNREAL_MCP_SERVER_PATH": ""}):
                with self.assertRaises(ci.StepError):
                    ci.smoke(None)

    def test_fixture_diff_without_a_raw_dump_fails(self):
        self.assertEqual(ci.main(["fixture-diff", "--ue", "5.8"]), 1)


class UeEnvTests(unittest.TestCase):
    def test_chmod_shim_skips_only_an_already_executable_u_plus_x(self):
        with tempfile.TemporaryDirectory() as folder:
            temp = Path(folder)
            github_path = temp / "github_path"
            with mock.patch.dict(os.environ, {"RUNNER_TEMP": folder, "GITHUB_PATH": str(github_path)}):
                ue_env.append_job_file("GITHUB_PATH", str(ue_env.install_chmod_shim()))
            shims = temp / "ue-shims"
            self.assertEqual(github_path.read_text(encoding="utf-8"), f"{shims}\n")
            shim = (shims / "chmod").read_bytes()
            self.assertTrue(shim.startswith(b"#!/bin/sh\n") and b"\r" not in shim)
            if os.name == "nt":
                return  # the shim runs where Build.sh does (POSIX)
            tool = temp / "dotnet"
            tool.write_text("")
            tool.chmod(0o555)
            subprocess.run([str(shims / "chmod"), "u+x", str(tool)], check=True)  # skipped: already executable
            subprocess.run([str(shims / "chmod"), "644", str(tool)], check=True)  # passed on to the real chmod
            self.assertEqual(tool.stat().st_mode & 0o777, 0o644)

    def test_chmod_shim_only_for_linux_ue_5_5_and_5_6(self):
        # UE 5.8's Linux build (UBA) fails on a chmod script first on PATH; 5.7+ needs no shim.
        self.assertTrue(ue_env.needs_chmod_shim("5.5", "Linux"))
        self.assertTrue(ue_env.needs_chmod_shim("5.6", "Linux"))
        self.assertFalse(ue_env.needs_chmod_shim("5.7", "Linux"))
        self.assertFalse(ue_env.needs_chmod_shim("5.8", "Linux"))
        self.assertFalse(ue_env.needs_chmod_shim("5.5", "Mac"))
        self.assertFalse(ue_env.needs_chmod_shim("5.6", "Win64"))

    def test_xcode_versions_compare_as_three_parts(self):
        self.assertEqual(ue_env.xcode3("15.2"), (15, 2, 0))
        self.assertLess(ue_env.xcode3("16.4"), ue_env.xcode3("16.9.0"))
        self.assertIsNone(ue_env.xcode3("abc"))

    def test_engine_root_is_the_install_of_the_running_python(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder) / "UE 5.7"
            (root / "Engine/Build").mkdir(parents=True)
            (root / "Engine/Build/Build.version").write_text(json.dumps({"MajorVersion": 5, "MinorVersion": 7}))
            python = root / "Engine/Binaries/ThirdParty/Python3/Mac/bin/python3"
            with mock.patch.object(sys, "executable", str(python)):
                self.assertEqual(ue_env.engine_root("5.7"), root)
                with self.assertRaises(SystemExit):
                    ue_env.engine_root("5.8")
            with self.assertRaises(SystemExit):  # not an engine's Python
                ue_env.engine_root("5.7")

    @staticmethod
    def engine_with_apple_sdk(folder: str, version: str) -> Path:
        """An engine root holding an excerpt of that UE version's real Engine/Config/Apple/Apple_SDK.json."""
        sdk = Path(folder) / "Engine/Config/Apple/Apple_SDK.json"
        sdk.parent.mkdir(parents=True)
        sdk.write_bytes((APPLE_SDK / f"UE_{version}.json").read_bytes())
        return Path(folder)

    def test_xcode_range_reads_every_engines_apple_sdk_json(self):
        # UE 5.5's file is JSONC (comments, trailing commas); 5.6+ hide comments in "//N" keys.
        expected = {"5.5": "16.9.0", "5.6": "16.9.0", "5.7": "26.9.0", "5.8": "27.9.0"}
        for version, high in expected.items():
            with self.subTest(version=version), tempfile.TemporaryDirectory() as folder:
                root = self.engine_with_apple_sdk(folder, version)
                self.assertEqual(ue_env.xcode_range(root), ((15, 2, 0), ue_env.xcode3(high)))

    def test_developer_dir_keeps_a_selected_xcode_in_ue55_range(self):
        selected = subprocess.CompletedProcess([], 0, stdout="/Applications/Xcode.app/Contents/Developer\n")
        with tempfile.TemporaryDirectory() as folder, \
                mock.patch.object(ue_env.subprocess, "run", return_value=selected), \
                mock.patch.object(ue_env, "xcode_version", return_value=(16, 2, 0)):
            self.assertIsNone(ue_env.developer_dir(self.engine_with_apple_sdk(folder, "5.5"), "5.5"))

    @unittest.skipIf(os.name == "nt", "a junction needs a Windows host")
    def test_link_python_and_remove_link_never_touch_the_target(self):
        with tempfile.TemporaryDirectory() as folder, mock.patch.dict(os.environ, {"RUNNER_TEMP": folder}):
            python = ue_env.link_python()
            self.assertTrue(python.startswith(folder) and " " not in python)
            self.assertEqual(subprocess.run([python, "-c", "print(1)"], capture_output=True, text=True).stdout.strip(), "1")
            ue_env.remove_link(ue_env.link_path())
            self.assertFalse(os.path.lexists(ue_env.link_path()))
            self.assertTrue(Path(sys.executable).exists())


class ConnectionSmokeLogTests(unittest.TestCase):
    """The editor-log markers connection_smoke.py reads, incl. the §6.8 bridge download."""

    def scan(self, *lines: str) -> dict:
        smoke = load("connection_smoke", ROOT / "scripts/connection_smoke.py")
        with tempfile.TemporaryDirectory() as folder:
            log = Path(folder) / "editor.log"
            log.write_text("\n".join(lines), encoding="utf-8")
            return smoke.scan_log(log)

    NO_BINARY = "LogUnrealMcp: Warning: [Unreal-MCP] no sidecar binary resolved for rid 'win-x64'"
    DOWNLOADING = "LogUnrealMcp: [Unreal-MCP] no bridge binary resolved; downloading https://github.com/x.zip"

    def test_no_binary_without_a_download_is_fatal(self):
        self.assertTrue(self.scan(self.NO_BINARY)["no_binary"])

    def test_a_download_in_flight_is_not_fatal(self):
        marks = self.scan(self.NO_BINARY, self.DOWNLOADING)
        self.assertFalse(marks["no_binary"])
        self.assertFalse(marks["bridge_download_failed"])

    def test_the_success_line_is_not_a_failure(self):
        ok = "LogUnrealMcp: [Unreal-MCP] MCP bridge downloaded and verified: C:/cache/unreal-mcp-bridge.exe"
        self.assertFalse(self.scan(self.NO_BINARY, self.DOWNLOADING, ok)["bridge_download_failed"])

    def test_every_downloader_failure_line_ends_the_wait(self):
        for failure in ("MCP bridge download failed: could not reach github.com",
                        "MCP bridge download rejected: the downloaded zip SHA256 did not match",
                        "MCP bridge download is not a readable zip.",
                        "MCP bridge download: failed to extract 'x'.",
                        "Cannot download the MCP bridge: this UnrealMCP build has no bridge checksum manifest"):
            marks = self.scan(self.NO_BINARY, self.DOWNLOADING, f"LogUnrealMcp: Warning: [Unreal-MCP] {failure}")
            self.assertTrue(marks["bridge_download_failed"], failure)


class ConnectionSmokeMetaTests(unittest.TestCase):
    def test_the_engine_version_is_major_minor_even_for_a_hotfix_install_path(self):
        smoke = load("connection_smoke", ROOT / "scripts/connection_smoke.py")
        self.assertEqual(smoke.record_meta(Path("/opt/runner-manager-unreal/UE_5.8.4"))["engine_version"], "5.8")
        self.assertEqual(smoke.record_meta(Path("/Users/Shared/Epic Games/UE_5.8"))["engine_version"], "5.8")
        self.assertEqual(smoke.record_meta(Path(r"C:\Program Files\Epic Games\UE_5.5"))["engine_version"], "5.5")


class ConnectionSmokeKillTests(unittest.TestCase):
    """The pre-launch stop reaches only the editor of the exact project, never one that shares its name."""

    PROJECT = r"C:\actions-runner\_work\host\UnrealTestProject.uproject"
    EDITOR = r'"C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"'

    smoke = load("connection_smoke", ROOT / "scripts/connection_smoke.py")

    def opens(self, *arguments: str, project: str = PROJECT) -> bool:
        return self.smoke.opens_project(" ".join([self.EDITOR, *arguments, "-nullrhi"]), project)

    def test_the_same_stem_in_another_folder_does_not_match(self):
        self.assertTrue(self.opens(f'"{self.PROJECT}"'))
        for other in (r"D:\aigd-labs\i2-vfx\UnrealTestProject.uproject",
                      r"C:\wt\d461f25d\agent\engines\unreal\test-project\UnrealTestProject.uproject",
                      r"C:\actions-runner2\_work\host\UnrealTestProject.uproject"):
            self.assertFalse(self.opens(f'"{other}"'), other)
            self.assertFalse(self.opens(other), other)

    def test_the_bare_stem_or_file_name_does_not_match(self):
        self.assertFalse(self.opens("UnrealTestProject"))
        self.assertFalse(self.opens("UnrealTestProject.uproject"))
        self.assertFalse(self.opens(r"-abslog=C:\logs\UnrealTestProject.log"))

    def test_a_longer_path_holding_the_project_path_does_not_match(self):
        self.assertFalse(self.opens(f"{self.PROJECT}.bak"))
        self.assertFalse(self.opens(f'"{self.PROJECT}2"'))
        self.assertFalse(self.opens(rf'"D:\mirror\{self.PROJECT}"'))
        self.assertFalse(self.opens(f'"{self.PROJECT} - Copy\\UnrealTestProject.uproject"'))

    def test_the_path_inside_another_argument_does_not_match(self):
        other = r'"D:\other\Other.uproject"'
        for argument in (f"-abslog={self.PROJECT}", f'-source="{self.PROJECT}"', f'-ExecCmds="Foo {self.PROJECT}"'):
            self.assertFalse(self.opens(other, argument), argument)

    def test_case_separators_and_quoting_do_not_matter(self):
        doubled = self.PROJECT.replace("\\", "\\\\")
        for spelling in (self.PROJECT, f'"{self.PROJECT}"', f"-project={self.PROJECT}", f'-Project="{self.PROJECT}"',
                         self.PROJECT.upper(), self.PROJECT.replace("\\", "/"), f'"{doubled}"'):
            self.assertTrue(self.opens(spelling), spelling)

    def test_a_path_with_spaces_matches_only_whole(self):
        project = r"C:\My Games\UnrealTestProject.uproject"
        self.assertTrue(self.opens(f'"{project}"', project=project))
        self.assertFalse(self.opens(r'"C:\My Games 2\UnrealTestProject.uproject"', project=project))

    def test_wildcard_and_regex_characters_in_the_path_are_literal(self):
        project = r"C:\wt[1]\a*b?\(x)+\UnrealTestProject.uproject"
        self.assertTrue(self.opens(f'"{project}"', project=project))
        self.assertFalse(self.opens(r'"C:\wt1\ab\x\UnrealTestProject.uproject"', project=project))

    def test_paths_that_only_look_alike_do_not_match(self):
        self.assertFalse(self.opens(r'"C:\Strasse\X.uproject"', project=r"C:\Straße\X.uproject"))
        self.assertFalse(self.opens(r'"\srv\share\X.uproject"', project=r"\\srv\share\X.uproject"))
        self.assertTrue(self.opens(r'"//srv/share/X.uproject"', project=r"\\srv\share\X.uproject"))

    def test_an_unreadable_command_line_does_not_match(self):
        self.assertFalse(self.smoke.opens_project("", self.PROJECT))

    def test_the_editor_listing_is_decoded_whole(self):
        def line(pid, command_line: str) -> str:
            return f"{pid}|{base64.b64encode(command_line.encode('utf-8')).decode('ascii')}"

        cyrillic = r'"C:\Проект\a|b\UnrealTestProject.uproject" -nullrhi'
        stdout = "\r\n".join(["noise", line(11, ""), line(12, cyrillic), line(13, "a\nb"), "²|QQ==", ""])
        self.assertEqual(self.smoke.parse_editor_processes(stdout), [(11, ""), (12, cyrillic), (13, "a\nb")])

    def test_a_failed_listing_stops_nothing(self):
        with mock.patch.object(self.smoke.os, "name", "nt"), \
                mock.patch.object(self.smoke.subprocess, "run", side_effect=FileNotFoundError("powershell")):
            self.assertEqual(self.smoke.list_editor_processes(), [])

    def test_only_the_editor_of_the_exact_project_is_stopped(self):
        forward = self.PROJECT.replace("\\", "/").lower()
        editors = [(101, f'{self.EDITOR} "{self.PROJECT}" -nullrhi'),
                   (102, rf'{self.EDITOR} "D:\aigd-labs\i2-vfx\UnrealTestProject.uproject" -nullrhi'),
                   (103, ""),
                   (104, f"{self.EDITOR} {forward}")]
        with mock.patch.object(self.smoke, "list_editor_processes", return_value=editors), \
                mock.patch.object(self.smoke, "kill_tree") as kill_tree:
            self.assertEqual(self.smoke.kill_existing_editors(PureWindowsPath(self.PROJECT)), 2)
        self.assertEqual([call.args[0] for call in kill_tree.call_args_list], [101, 104])


class UeJsonTests(unittest.TestCase):
    def test_strict_json_is_unchanged(self):
        text = json.dumps({"a": [1, 2.5, None, True], "b": {"c": "d"}})
        self.assertEqual(ue_json.loads(text), json.loads(text))

    def test_comments_and_trailing_commas_are_removed(self):
        text = '{\n  // line\n  "a": [1, 2, /* block,\n  comment */ 3,],\n  "b": {"c": 1,}, // after\n}'
        self.assertEqual(ue_json.loads(text), {"a": [1, 2, 3], "b": {"c": 1}})

    def test_comment_between_a_trailing_comma_and_the_bracket(self):
        self.assertEqual(ue_json.loads('["x", // why\n]'), ["x"])

    def test_comment_markers_and_commas_inside_strings_are_kept(self):
        text = r'{"//1": "see https://example.com/a//b", "s": "/* not a comment */", "q": "a \"// q\",]", "e": "x\\"}'
        self.assertEqual(ue_json.loads(text), {"//1": "see https://example.com/a//b", "s": "/* not a comment */",
                                               "q": 'a "// q",]', "e": "x\\"})

    def test_a_block_comment_does_not_join_tokens(self):
        with self.assertRaises(json.JSONDecodeError):
            ue_json.loads("[1/**/2]")

    def test_uplugin_module_names_accept_a_bom_and_trailing_commas(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "UnrealMCP.uplugin"
            path.write_bytes(b'\xef\xbb\xbf{"Modules": [{"Name": "A"},],}')
            self.assertEqual(ci.module_names(path), ["A"])


class MachineLockCliTests(unittest.TestCase):
    def cli(self, *args) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, str(ROOT / "scripts/ue_machine_lock.py"), *map(str, args)],
                              capture_output=True, text=True, timeout=120)

    def test_acquire_excludes_a_second_job_and_release_frees_it(self):
        with tempfile.TemporaryDirectory() as folder:
            path, state = Path(folder) / "lock/ue.lock", Path(folder)
            self.assertEqual(self.cli("acquire", "--lock-path", path, "--state-dir", state / "a", "--watch-pid", os.getpid()).returncode, 0)
            second = self.cli("acquire", "--lock-path", path, "--state-dir", state / "b", "--watch-pid", os.getpid(), "--wait-minutes", "0.05")
            self.assertEqual(second.returncode, 1)
            self.assertIn("held by:", second.stdout)
            self.assertEqual(self.cli("release", "--state-dir", state / "a").returncode, 0)
            handle = lock.try_lock(path)
            self.assertIsNotNone(handle)
            lock.unlock(handle)

    def test_a_dead_job_releases_the_lock(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "ue.lock"
            job = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(4)"])
            self.assertEqual(self.cli("acquire", "--lock-path", path, "--state-dir", Path(folder) / "s", "--watch-pid", job.pid).returncode, 0)
            self.assertIsNone(lock.try_lock(path))
            job.wait()
            deadline = time.monotonic() + 30
            handle = None
            while handle is None and time.monotonic() < deadline:
                time.sleep(1)
                handle = lock.try_lock(path)
            self.assertIsNotNone(handle, "the holder kept the lock after its job died")
            lock.unlock(handle)

    def test_release_without_acquire_is_a_no_op(self):
        with tempfile.TemporaryDirectory() as folder:
            result = self.cli("release", "--state-dir", Path(folder) / "none")
            self.assertEqual(result.returncode, 0)
            self.assertIn("nothing to release", result.stdout)


class RunWatchEncodingTests(unittest.TestCase):
    """run(watch=...) must keep draining the pipe whatever bytes the build prints (DEV-287 review)."""

    def test_non_utf8_output_through_a_cp1252_stdout_neither_hangs_nor_loses_lines(self):
        import io
        # A non-UTF-8 byte (OEM codepage) and a character cp1252 cannot encode, then enough output to fill
        # the pipe if the reader stopped draining, then the conflict line the watcher must still see.
        lines = [b"caf\xe9 \xe2\x98\x83\n", *([b"x" * 120 + b"\n"] * 4000),
                 b"A conflicting instance of UnrealBuildTool is already running.\n"]
        child = "import sys\n" + "".join(f"sys.stdout.buffer.write({line!r})\n" for line in lines) + "sys.stdout.flush()\n"
        script = Path(tempfile.mkdtemp()) / "child.py"
        script.write_text(child, encoding="ascii")
        seen = []
        sink = io.BytesIO()
        stdout = io.TextIOWrapper(sink, encoding="cp1252", errors="strict")
        real = sys.stdout
        sys.stdout = stdout
        try:
            started = time.monotonic()
            code = ci.run([sys.executable, str(script)], watch=seen.append)
        finally:
            stdout.flush()
            sys.stdout = real
        self.assertEqual(code, 0)
        self.assertLess(time.monotonic() - started, 60)
        self.assertTrue(any("conflicting instance" in line for line in seen))
        self.assertIn(b"caf\xe9", sink.getvalue())


if __name__ == "__main__":
    unittest.main()
