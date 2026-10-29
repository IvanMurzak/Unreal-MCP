"""Tests for the CI step scripts: .github/scripts/ue_ci.py, .github/scripts/ue_env.py, scripts/ue_machine_lock.py."""
from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ci = load("ue_ci", ROOT / ".github/scripts/ue_ci.py")
ue_env = load("ue_env", ROOT / ".github/scripts/ue_env.py")
import ue_machine_lock as lock  # noqa: E402  (scripts/ is on sys.path under `unittest discover -s scripts`)


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

    def test_suite_gates_extensions_below_57_and_exports_the_report(self):
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
        self.assertEqual(calls[0][-1], "--extensions")
        self.assertNotIn("--extensions", calls[1])
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

    def test_build_plugin_dist_reports_a_uat_failure(self):
        uat = fake_uat(self.temp, "sys.exit(5)\n")
        with mock.patch.dict(os.environ, {"UE_RUNUAT": uat}):
            self.assertEqual(ci.main(["build-plugin-dist"]), 1)

    def fake_plugin(self) -> Path:
        plugin = self.temp / "repo/UnrealMCP"
        for rel in ("Config/x.ini", "Resources/Icon128.png", "Content/a.txt", "Source/UnrealMcpRuntime/a.cpp",
                    "Source/UnrealMcpRql/a.cpp", "Source/UnrealMcpEditor/a.cpp", "Source/UnrealMcpEditorTests/a.cpp",
                    "ThirdPartyNotices.txt"):
            (plugin / rel).parent.mkdir(parents=True, exist_ok=True)
            (plugin / rel).write_text("x")
        (plugin / "UnrealMCP.uplugin").write_text(json.dumps({"Modules": [{"Name": "UnrealMcpRuntime"}]}))
        return plugin

    def patch_repo(self, plugin: Path):
        root = plugin.parent
        return mock.patch.multiple(ci, ROOT=root, PLUGIN=plugin, STAGED_BRIDGE=plugin / "Source/ThirdParty/UnrealMcpBridge")

    def test_stage_bridge_then_source_zip(self):
        plugin = self.fake_plugin()
        with self.patch_repo(plugin):
            self.assertEqual(ci.main(["stage-bridge"]), 1)  # no ./bridge-staging yet
            for rid in ci.RIDS:
                (plugin.parent / "bridge-staging" / rid).mkdir(parents=True)
                (plugin.parent / "bridge-staging" / rid / "unreal-mcp-bridge").write_text(rid)
            self.assertEqual(ci.main(["stage-bridge"]), 0)
            self.assertEqual(ci.main(["source-zip", "--version", "1.2.3"]), 0)
        names = zipfile.ZipFile(plugin.parent / "dist/unreal-mcp-plugin-source-1.2.3.zip").namelist()
        self.assertIn("UnrealMCP/UnrealMCP.uplugin", names)
        self.assertIn("UnrealMCP/Source/ThirdParty/UnrealMcpBridge/osx-arm64/unreal-mcp-bridge", names)
        self.assertFalse([n for n in names if "UnrealMcpEditorTests" in n])

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
        names = zipfile.ZipFile(plugin.parent / "dist/unreal-mcp-plugin-1.2.3.zip").namelist()
        self.assertIn("UnrealMCP.uplugin", names)
        self.assertIn("Binaries/ThirdParty/UnrealMcpBridge/win-x64/unreal-mcp-bridge", names)

    def test_cleanup_keeps_the_chain_python_link_and_unrelated_files(self):
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
        with mock.patch.object(ci, "ROOT", repo):
            self.assertEqual(ci.main(["cleanup", "--job", "connection-smoke", "--ue", "5.8"]), 0)
        self.assertFalse((repo / "nuget.config").exists())
        self.assertTrue((repo / "Directory.Build.targets").exists())
        self.assertFalse((repo / ".artifacts/nuget").exists())
        self.assertTrue((repo / ".artifacts/tests").exists())
        self.assertFalse((self.runner_temp / "UnrealMCP-Package").exists())
        self.assertFalse((self.runner_temp / "UBT-x.log").exists())
        if os.name != "nt":
            self.assertTrue(os.path.islink(self.runner_temp / "chain-py"))
        self.assertTrue(link_target.is_dir())

    def test_fixture_diff_without_a_raw_dump_fails(self):
        self.assertEqual(ci.main(["fixture-diff", "--ue", "5.8"]), 1)


class UeEnvTests(unittest.TestCase):
    def test_xcode_versions_compare_as_three_parts(self):
        self.assertEqual(ue_env.xcode3("15.2"), (15, 2, 0))
        self.assertLess(ue_env.xcode3("16.4"), ue_env.xcode3("16.9.0"))
        self.assertIsNone(ue_env.xcode3("abc"))

    def test_engine_root_override_and_version_check(self):
        with tempfile.TemporaryDirectory() as folder:
            build = Path(folder) / "Engine/Build/Build.version"
            build.parent.mkdir(parents=True)
            build.write_text(json.dumps({"MajorVersion": 5, "MinorVersion": 7}))
            with mock.patch.dict(os.environ, {"UE_ROOT_5_7": folder, "UE_ROOT_5_8": folder}):
                self.assertEqual(ue_env.engine_root("5.7"), Path(folder))
                with self.assertRaises(SystemExit):
                    ue_env.engine_root("5.8")

    @unittest.skipIf(os.name == "nt", "a junction needs a Windows host")
    def test_link_python_and_remove_link_never_touch_the_target(self):
        with tempfile.TemporaryDirectory() as folder, mock.patch.dict(os.environ, {"RUNNER_TEMP": folder}):
            python = ue_env.link_python()
            self.assertTrue(python.startswith(folder) and " " not in python)
            self.assertEqual(subprocess.run([python, "-c", "print(1)"], capture_output=True, text=True).stdout.strip(), "1")
            ue_env.remove_link(ue_env.link_path())
            self.assertFalse(os.path.lexists(ue_env.link_path()))
            self.assertTrue(Path(sys.executable).exists())


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


if __name__ == "__main__":
    unittest.main()
