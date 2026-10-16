"""Build plugins in isolation, then run their Automation tests in ONE editor process.

All writable plugin copies are scoped to engine version + profile. No shared testbed is edited.
Python stdlib only; Windows UE toolchain. See docs/EXTENSION-MONOREPO.md.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
EXCLUDED = {"Binaries", "Intermediate", "Saved", "DerivedDataCache", ".git", ".vs", "node_modules"}


def extensions() -> dict[str, Path]:
    return {p.stem: p.parent for p in sorted((ROOT / "extensions").glob("*/*.uplugin"))}


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def copy_plugin(source: Path, dest: Path, strict: bool = False) -> None:
    """Incremental source sync, preserving ONLY this profile's own build cache."""
    expected: set[Path] = set()
    for file in source.rglob("*"):
        rel = file.relative_to(source)
        if rel.parts[0] == "Tests" or any(part in EXCLUDED for part in rel.parts) or not file.is_file():
            continue
        expected.add(rel)
        data = file.read_bytes()
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
    marker.write_text(identity)
    plugins = base / "Plugins"
    allowed = {"UnrealMCP", *selected}
    if plugins.exists() and any(p.name not in allowed for p in plugins.iterdir()):
        raise ValueError(f"Unexpected plugin in isolated profile {plugins}; use a fresh output directory")
    copy_plugin(ROOT / "UnrealMCP", plugins / "UnrealMCP")
    descriptor_path = plugins / "UnrealMCP/UnrealMCP.uplugin"
    descriptor = json.loads(descriptor_path.read_text(encoding="utf-8-sig"))
    if core_tests:
        descriptor["Modules"].append({"Name": "UnrealMcpEditorTests", "Type": "Editor", "LoadingPhase": "Default"})
    write_json(descriptor_path, descriptor)
    for name in selected:
        copy_plugin(extensions()[name], plugins / name, strict)
    template = ROOT / "tests/host-project"
    shutil.copytree(template, base, dirs_exist_ok=True)
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
    config.write_text('[/Script/AutomationController.AutomationControllerSettings]\n'
                      f'+Groups=(Name="UnrealMcpSuite",Filters=({group}))\n', encoding="utf-8")
    return project


def build(engine: Path, project: Path) -> None:
    command = [str(engine / "Engine/Build/BatchFiles/Build.bat"), "UnrealMcpTestsEditor", "Win64", "Development",
               f"-Project={project}", "-WaitMutex", "-NoHotReloadFromIDE"]
    print(f"Building {project.parent} (no editor launch)", flush=True)
    with (project.parent / "build.log").open("w", encoding="utf-8") as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)


def check_report(report: Path, selected: list[str], core_tests: bool = True) -> dict:
    data = json.loads(report.read_text(encoding="utf-8-sig"))
    tests = data.get("tests", [])
    if not tests or data.get("failed", 0) or data.get("notRun", 0) or data.get("inProcess", 0):
        raise ValueError(f"Incomplete/failed Automation report: {report}")
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
    if process.poll() is None:
        subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"], check=False, capture_output=True)
    process.wait(timeout=30)


def automation(engine: Path, project: Path, selected: list[str], timeout: int,
               server: Path | None = None, bridge: Path | None = None) -> dict:
    report = project.parent / "Reports" / time.strftime("%Y%m%d-%H%M%S")
    report.mkdir(parents=True)
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
                                       stdout=server_log, stderr=subprocess.STDOUT, cwd=server.parent)
        print(f"Started owned server PID {server_proc.pid}", flush=True)
        if not smoke.wait_port(port, 40):
            kill_owned(server_proc)
            server_log.close()
            raise ValueError("Local MCP server failed to start")
        env.update(UNREAL_MCP_HOST=f"http://localhost:{port}", UNREAL_MCP_KEEP_CONNECTED="true",
                   UNREAL_MCP_BRIDGE_PATH=str(bridge))
    command = [str(engine / "Engine/Binaries/Win64/UnrealEditor-Cmd.exe"), str(project),
               "-nullrhi", "-nosplash", "-unattended", "-NoSound", "-log",
               "-ExecCmds=Automation RunTests Group:UnrealMcpSuite", f"-ReportExportPath={report}",
               f"-abslog={report / 'editor.log'}"]
    editor = None
    try:
        editor = subprocess.Popen(command, env=env, cwd=project.parent, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        print(f"Started ONE owned editor PID {editor.pid}; report {report}", flush=True)
        deadline = time.monotonic() + timeout
        while not (report / "index.json").exists():
            if editor.poll() is not None or time.monotonic() > deadline:
                raise ValueError(f"Editor exited or timed out without Automation report: {report}")
            time.sleep(1)
        result = check_report(report / "index.json", selected)
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
    parser.add_argument("--output", type=Path, default=(Path(os.environ.get("SystemDrive", "C:")) / "wt/unreal-tests" / hashlib.sha256(str(ROOT).encode()).hexdigest()[:8]) if os.name == "nt" else ROOT / ".artifacts/tests")
    parser.add_argument("--extensions", nargs="*", help="Plugin names; default all")
    parser.add_argument("--mode", choices=["full", "verification", "isolated", "automation", "prepare"], default="full")
    parser.add_argument("--result-file", type=Path)
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("--server", type=Path)
    parser.add_argument("--bridge", type=Path)
    args = parser.parse_args()
    engine = args.engine.resolve()
    selected = args.extensions if args.extensions is not None else list(extensions())
    if any(n not in extensions() for n in selected):
        parser.error("Unknown extension")
    output = args.output.resolve()
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
            result = automation(engine, project, selected, args.timeout, args.server, args.bridge)
            write_json(project.parent / "suite-result.json", result)
            if args.result_file:
                write_json(args.result_file, result)
            print(json.dumps(result), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
