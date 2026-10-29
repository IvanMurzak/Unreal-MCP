"""Build plugins in isolation, then run their Automation tests in ONE editor process.

All writable plugin copies are scoped to engine version + profile. No shared testbed is edited.
Python stdlib only; the host OS's UE toolchain (Windows, macOS or Linux). See docs/EXTENSION-MONOREPO.md.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import time

from extension_release import components as extensions

ROOT = Path(__file__).resolve().parents[1]
EXCLUDED = {"Binaries", "Intermediate", "Saved", "DerivedDataCache", ".git", ".vs", "node_modules"}
# This host's UBT platform, Build script and editor binary, relative to the engine root.
if os.name == "nt":
    UE_PLATFORM, UE_BUILD, UE_EDITOR = "Win64", "Engine/Build/BatchFiles/Build.bat", "Engine/Binaries/Win64/UnrealEditor-Cmd.exe"
elif sys.platform == "darwin":
    UE_PLATFORM, UE_BUILD, UE_EDITOR = "Mac", "Engine/Build/BatchFiles/Mac/Build.sh", "Engine/Binaries/Mac/UnrealEditor-Cmd"
else:
    UE_PLATFORM, UE_BUILD, UE_EDITOR = "Linux", "Engine/Build/BatchFiles/Linux/Build.sh", "Engine/Binaries/Linux/UnrealEditor-Cmd"
# POSIX: owned processes lead their own process group, so kill_owned can stop the whole tree.
NEW_SESSION = os.name != "nt"


@contextmanager
def machine_lock(already_held: bool = False, *, path: Path | None = None, timeout_seconds: float = 3600):
    """Use the SAME exclusive file handle as CI, including from interactive local runs."""
    if already_held:
        yield
        return
    if os.name != "nt":
        # .github/scripts/ue-machine-lock.ps1 holds an exclusive flock() on this same file.
        import fcntl
        lock_path = path or Path("/var/tmp/UnrealMCP-ci/ue.lock")
        lock_path.parent.mkdir(parents=True, exist_ok=True)
        deadline = time.monotonic() + timeout_seconds
        announced = False
        with lock_path.open("a+b") as handle:
            # Open to every account, as the CI holder does (folder 1777, file 0666): a CI runner under
            # another account must still be able to open a lock this local run created first.
            for target, mode in ((lock_path.parent, 0o1777), (lock_path, 0o666)):
                try:
                    target.chmod(mode)
                except OSError:
                    pass
            while True:
                try:
                    fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    if time.monotonic() > deadline:
                        raise OSError(f"Cannot acquire UE machine lock {lock_path}") from None
                    if not announced:
                        print("Waiting for the machine-wide UE lock (another local/CI build owns it)", flush=True)
                        announced = True
                    time.sleep(1)
            yield  # closing the file releases the flock()
        return
    import ctypes
    from ctypes import wintypes
    lock_path = path or Path(os.environ.get("ProgramData", "C:/ProgramData")) / "UnrealMCP-ci/ue.lock"
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    create = kernel.CreateFileW
    create.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                       wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    create.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    deadline = time.monotonic() + timeout_seconds
    announced = False
    while True:
        handle = create(str(lock_path), 0xC0000000, 0, None, 4, 0x80, None)
        if handle != wintypes.HANDLE(-1).value:
            break
        error = ctypes.get_last_error()
        if error not in (32, 33) or time.monotonic() > deadline:
            raise OSError(error, f"Cannot acquire UE machine lock {lock_path}")
        if not announced:
            print("Waiting for the machine-wide UE lock (another local/CI build owns it)", flush=True)
            announced = True
        time.sleep(1)
    try:
        yield
    finally:
        kernel.CloseHandle(handle)


def sync_template(template: Path, dest: Path) -> None:
    """Write only changed template files: an identical Target.cs/Build.cs keeps UBT's caches valid."""
    for file in template.rglob("*"):
        if file.is_file():
            target = dest / file.relative_to(template)
            target.parent.mkdir(parents=True, exist_ok=True)
            data = file.read_bytes()
            if not target.exists() or target.read_bytes() != data:
                target.write_bytes(data)


def smoke_host() -> Path:
    """Sync tests/smoke-host into its persistent folder beside the suite's build caches; return the .uproject."""
    dest = default_output() / "smoke-host" / "UnrealTestProject"
    sync_template(ROOT / "tests/smoke-host", dest)
    return dest / "UnrealTestProject.uproject"


def default_output() -> Path:
    if os.name == "nt":
        return Path(os.environ.get("SystemDrive", "C:") + "/wt/unreal-tests") / hashlib.sha256(str(ROOT).encode()).hexdigest()[:8]
    return ROOT / ".artifacts/tests"


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(value, indent=2) + "\n"
    if not path.exists() or path.read_text(encoding="utf-8") != text:
        path.write_text(text, encoding="utf-8")


def copy_plugin(source: Path, dest: Path, strict: bool = False, *, core_tests: bool = False) -> None:
    """Incremental source sync, preserving ONLY this profile's own build cache."""
    expected: set[Path] = set()
    for file in source.rglob("*"):
        rel = file.relative_to(source)
        if rel.parts[0] == "Tests" or any(part in EXCLUDED for part in rel.parts) or not file.is_file():
            continue
        expected.add(rel)
        data = file.read_bytes()
        if core_tests and file.name == "UnrealMCP.uplugin":
            descriptor = json.loads(data.decode("utf-8-sig"))
            descriptor["Modules"].append({"Name": "UnrealMcpEditorTests", "Type": "Editor", "LoadingPhase": "Default"})
            data = (json.dumps(descriptor, indent=2) + "\n").encode("utf-8")
        if strict and file.suffix == ".cs" and file.name.endswith(".Build.cs"):
            text = data.decode("utf-8-sig")
            text = re.sub(r"PCHUsage\s*=\s*[^;]+;", "PCHUsage = PCHUsageMode.NoPCHs;", text)
            # Module-scoped overrides keep installed engine build settings untouched.
            text = re.sub(r"(PCHUsage = PCHUsageMode.NoPCHs;)", r"\1\n\t\tbUseUnity = false;", text)
            data = text.encode("utf-8")
        target = dest / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists() or target.read_bytes() != data:
            target.write_bytes(data)
    for file in dest.rglob("*"):
        rel = file.relative_to(dest)
        if file.is_file() and not any(p in EXCLUDED for p in rel.parts) and rel not in expected:
            file.unlink()


def prepare(engine: Path, profile: str, selected: list[str], output: Path, *, strict: bool = False,
            core_tests: bool = False) -> Path:
    version = json.loads((engine / "Engine/Build/Build.version").read_text(encoding="utf-8-sig"))
    engine_key = f"UE-{version['MajorVersion']}.{version['MinorVersion']}-" + hashlib.sha256(str(engine).encode()).hexdigest()[:8]
    base = output / engine_key / profile
    base.mkdir(parents=True, exist_ok=True)
    marker = base / ".unreal-mcp-suite"
    identity = str(ROOT)
    if any(base.iterdir()) and (not marker.exists() or marker.read_text() != identity):
        raise ValueError(f"Refusing foreign build directory: {base}")
    if not marker.exists():
        marker.write_text(identity)
    plugins = base / "Plugins"
    allowed = {"UnrealMCP", *selected}
    if plugins.exists() and any(p.name not in allowed for p in plugins.iterdir()):
        raise ValueError(f"Unexpected plugin in isolated profile {plugins}; use a fresh output directory")
    copy_plugin(ROOT / "UnrealMCP", plugins / "UnrealMCP", core_tests=core_tests)
    for name in selected:
        copy_plugin(extensions()[name], plugins / name, strict)
    sync_template(ROOT / "tests/host-project", base)
    project = base / "UnrealMcpTests.uproject"
    write_json(project, {"FileVersion": 3, "EngineAssociation": f"{version['MajorVersion']}.{version['MinorVersion']}",
                        "Modules": [{"Name": "UnrealMcpTests", "Type": "Runtime", "LoadingPhase": "Default"}],
                        "Plugins": [{"Name": n, "Enabled": True} for n in sorted(allowed)],
                        "DisableEnginePluginsByDefault": True})
    filters = ["UnrealMcp."] if core_tests else []
    filters += selected
    config = base / "Config/DefaultEngine.ini"
    config.parent.mkdir(exist_ok=True)
    group = ",".join(f'(Contains="{p}",MatchFromStart=True)' for p in filters)
    config_text = ('[/Script/AutomationController.AutomationControllerSettings]\n'
                   f'+Groups=(Name="UnrealMcpSuite",Filters=({group}))\n')
    if not config.exists() or config.read_text(encoding="utf-8") != config_text:
        config.write_text(config_text, encoding="utf-8")
    return project


def build(engine: Path, project: Path) -> None:
    command = [str(engine / UE_BUILD), "UnrealMcpTestsEditor", UE_PLATFORM, "Development",
               f"-Project={project}", "-WaitMutex", "-NoHotReloadFromIDE"]
    print(f"Building {project.parent} (no editor launch)", flush=True)
    log_path = project.parent / "build.log"
    with log_path.open("w", encoding="utf-8") as log:
        completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
    if completed.returncode:
        print("\n".join(log_path.read_text(encoding="utf-8", errors="replace").splitlines()[-80:]), flush=True)
        completed.check_returncode()


def check_report(report: Path, selected: list[str], core_tests: bool = True) -> dict:
    data = json.loads(report.read_text(encoding="utf-8-sig"))
    tests = data.get("tests", [])
    if not tests or data.get("failed", 0) or data.get("notRun", 0) or data.get("inProcess", 0):
        failures = [t for t in tests if t.get("state") not in ("Success", "SuccessWithWarnings")]
        raise ValueError(f"Incomplete/failed Automation report: {report}; "
                         f"counts={ {k: data.get(k) for k in ('succeeded', 'failed', 'notRun', 'inProcess')} }; "
                         f"nonpassing={json.dumps(failures, ensure_ascii=False)[:16000]}")
    paths: list[str] = []
    for test in tests:
        if test.get("state") not in ("Success", "SuccessWithWarnings"):
            raise ValueError(f"Automation test did not pass: {test}")
        paths.append(test["fullTestPath"])
    inventory = json.loads((ROOT / "tests/automation-inventory.json").read_text())
    required = {prefix: minimum for prefix, minimum in inventory.items()
                if (core_tests and prefix.startswith("UnrealMcp.")) or any(prefix.startswith(n) for n in selected)}
    if any(not any(p == name or p.startswith(name + ".") for p in required) for name in selected):
        raise ValueError("A selected extension is missing from tests/automation-inventory.json")
    for prefix, minimum in required.items():
        count = sum(p == prefix or p.startswith(prefix + ".") for p in paths)
        if count < minimum:
            raise ValueError(f"Missing Automation coverage: {prefix}: expected >= {minimum}, got {count}")
    return {"passed": len(paths), "prefixes": len(required)}


def kill_owned(process: subprocess.Popen) -> None:
    if NEW_SESSION:
        # Even after the leader exited: its group (e.g. ShaderCompileWorker) may still be alive.
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    elif process.poll() is None:
        stopped = subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"], check=False, capture_output=True)
        if stopped.returncode and process.poll() is None:
            print(f"Owned process {process.pid} tree stop failed: {stopped.stdout!r} {stopped.stderr!r}", flush=True)
            process.kill()
    process.wait(timeout=30)


def automation(engine: Path, project: Path, selected: list[str], timeout: int,
               server: Path | None = None, bridge: Path | None = None, result_file: Path | None = None) -> dict:
    report = project.parent / "Reports" / time.strftime("%Y%m%d-%H%M%S")
    report.mkdir(parents=True)
    if result_file:
        write_json(result_file, {"status": "running", "report": str(report)})
    env = os.environ.copy()
    # Inherit no credentials/remote endpoints from a developer or another task.
    for key in list(env):
        if key.startswith("UNREAL_MCP_"):
            del env[key]
    env.update(UNREAL_MCP_CONNECTION_MODE="Custom", UNREAL_MCP_KEEP_CONNECTED="false")
    server_proc = None
    server_log = None
    if server:
        if not bridge or not server.is_file() or not bridge.is_file():
            raise ValueError("Live E2E needs existing --server and --bridge binaries")
        import connection_smoke as smoke
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        server_log = (report / "server.log").open("w")
        server_proc = subprocess.Popen([str(server), "--port", str(port), "--client-transport", "streamableHttp"],
                                       stdout=server_log, stderr=subprocess.STDOUT, cwd=server.parent,
                                       start_new_session=NEW_SESSION)
        print(f"Started owned server PID {server_proc.pid}", flush=True)
        if not smoke.wait_port(port, 40):
            kill_owned(server_proc)
            server_log.close()
            raise ValueError("Local MCP server failed to start")
        env.update(UNREAL_MCP_HOST=f"http://localhost:{port}", UNREAL_MCP_KEEP_CONNECTED="true",
                   UNREAL_MCP_BRIDGE_PATH=str(bridge))
    command = [str(engine / UE_EDITOR), str(project),
               "-nullrhi", "-nosplash", "-unattended", "-NoSound", "-log",
               "-ExecCmds=Automation RunTests Group:UnrealMcpSuite", f"-ReportExportPath={report}",
               f"-abslog={report / 'editor.log'}"]
    editor = None
    try:
        editor = subprocess.Popen(command, env=env, cwd=project.parent, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT,
                                  start_new_session=NEW_SESSION)
        print(f"Started ONE owned editor PID {editor.pid}; report {report}", flush=True)
        deadline = time.monotonic() + timeout
        while not (report / "index.json").exists():
            if editor.poll() is not None or time.monotonic() > deadline:
                raise ValueError(f"Editor exited or timed out without Automation report: {report}")
            time.sleep(1)
        while True:
            try:
                result = check_report(report / "index.json", selected)
                break
            except (json.JSONDecodeError, PermissionError):
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.1)
        if server:
            # Reuse this SAME editor, server and sidecar for every preserved E2E check.
            url = f"http://localhost:{port}"
            while True:
                if editor.poll() is not None or time.monotonic() > deadline:
                    raise ValueError("Editor/MCP connection failed before E2E")
                try:
                    status, sid = smoke.mcp_initialize(url + "/mcp", None)
                    tools = smoke.mcp_list_tools(url + "/mcp", None, sid) if status == 200 else []
                    expected = {t["name"] for n in selected for t in json.loads((extensions()[n] / "extension.json").read_text())["tools"]}
                    if expected.issubset(tools):
                        break
                except (OSError, ValueError):
                    pass
                time.sleep(1)
            subprocess.run(["pwsh", "-NoProfile", "-File", str(ROOT / "scripts/run-extension-e2e.ps1"),
                            "-ProjectDir", str(project.parent), "-Url", url, "-Plugins", ",".join(selected)],
                           env=env, check=True, timeout=max(1, deadline - time.monotonic()))
            result["e2e"] = "passed"
        result.update(editorLaunches=1, editorPid=editor.pid, report=str(report))
        return result
    finally:
        if editor:
            kill_owned(editor)
        if server_proc:
            kill_owned(server_proc)
        if server_log:
            server_log.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=default_output())
    parser.add_argument("--extensions", nargs="*", help="Plugin names; default all")
    parser.add_argument("--mode", choices=["full", "verification", "isolated", "automation", "prepare"], default="full")
    parser.add_argument("--ue-lock-held", action="store_true", help="CI only: caller already holds the machine-wide lock")
    parser.add_argument("--result-file", type=Path)
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("--server", type=Path)
    parser.add_argument("--bridge", type=Path)
    args = parser.parse_args()
    with machine_lock(args.ue_lock_held):
        engine = args.engine.resolve()
        selected = args.extensions if args.extensions is not None else list(extensions())
        if any(n not in extensions() for n in selected):
            parser.error("Unknown extension")
        output = args.output.resolve()
        if args.result_file:
            write_json(args.result_file, {"status": "running"})
        if args.mode == "full" and (not args.server or not args.bridge):
            from suite_dependencies import resolve
            args.server, args.bridge = resolve(output)
        if args.mode in ("full", "verification", "isolated"):
            # No siblings are present in these copies, including their headers and DLLs.
            for name in [None, *selected]:
                project = prepare(engine, "isolated/" + (name or "core"), [name] if name else [], output, strict=True)
                build(engine, project)
        if args.mode != "isolated":
            profile = "all" if selected == list(extensions()) else "selected-" + hashlib.sha256(",".join(selected).encode()).hexdigest()[:8]
            project = prepare(engine, profile, selected, output, core_tests=True)
            if args.mode != "prepare":
                build(engine, project)
                result = automation(engine, project, selected, args.timeout, args.server, args.bridge, args.result_file)
                write_json(project.parent / "suite-result.json", result)
                if args.result_file:
                    write_json(args.result_file, result)
                print(json.dumps(result), flush=True)
        return 0



if __name__ == "__main__":
    sys.exit(main())
