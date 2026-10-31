#!/usr/bin/env python3
"""The steps of the Unreal-MCP self-hosted UE jobs, one subcommand each (Python stdlib only).

Workflows stay thin: after the `.github/actions/ue-env` action, a step is
`${{ env.CHAIN_PY }} .github/scripts/ue_ci.py <command>`. No step depends on the runner's shell, so the
same job runs unchanged on Windows, macOS and Linux. Every UE job ends with ONE `teardown` step.

Self-hosted UE commands read what ue_env.py exported: UE_ROOT, UE_PLATFORM, UE_RUNUAT, UE_BUILD,
UE_EDITOR_CMD. Commands that export values for later steps append them to GITHUB_ENV.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(Path(__file__).resolve().parent), str(ROOT / "scripts"), str(ROOT / "bridge")]

from chain_feed import NUGET_CONFIG_NAME, TARGETS_NAME, _is_ours  # noqa: E402  (vendored, same folder)
from publish import ALL_RUNTIMES as RIDS, zip_dir  # noqa: E402  (bridge/publish.py)
import ue_json  # noqa: E402  (scripts/)
import ue_machine_lock  # noqa: E402  (scripts/)
from ue_env import IS_WINDOWS, add_job_env, link_path, remove_link  # noqa: E402  (same folder)

PLUGIN = ROOT / "UnrealMCP"
STAGED_BRIDGE = PLUGIN / "Source/ThirdParty/UnrealMcpBridge"
# The SHA-256 manifest of the per-RID release zips, shipped INSIDE the plugin: the editor verifies a downloaded bridge
# against it (docs/ARCHITECTURE.md §6.8, FUnrealMcpBridgeDownloader). Gitignored, written by stage-bridge.
BRIDGE_SUMS = "SHA256SUMS"
# The files of the source plugin assets, relative to UnrealMCP/.
SOURCE_ASSET_INPUTS = ("UnrealMCP.uplugin", "Config", "Resources", "Content", "Source/UnrealMcpRuntime", "Source/UnrealMcpRql",
                       "Source/UnrealMcpEditor", "Source/ThirdParty", "ThirdPartyNotices.txt")
# What Epic's Fab review calls an executable: native binaries by magic bytes (PE, ELF, Mach-O thin + fat), scripts by
# shebang, and the usual executable / native-library extensions.
EXECUTABLE_MAGIC = (b"MZ", b"\x7fELF", b"\xfe\xed\xfa\xce", b"\xfe\xed\xfa\xcf", b"\xce\xfa\xed\xfe", b"\xcf\xfa\xed\xfe",
                    b"\xca\xfe\xba\xbe", b"#!")
EXECUTABLE_SUFFIXES = {".exe", ".dll", ".so", ".dylib", ".bat", ".cmd", ".ps1", ".sh", ".com", ".msi", ".lib", ".a", ".pdb"}
# Only the chain's own feeds: .artifacts/tests holds the suite's build caches and the smoke host.
CHAIN_FEEDS = ("nuget", "npm", "server", "unity-dll", "nuget-cache")
# This job's per-run BuildPlugin / report outputs under RUNNER_TEMP.
TEMP_OUTPUTS = ("UnrealMCP-Package", "UnrealMCP-Dist-Package", "UnrealMCP-Source-Package", "AutomationReport")


class StepError(Exception):
    """A step failure: printed as a GitHub error annotation, exit 1."""


def env(name: str) -> str:
    value = os.environ.get(name, "")
    if not value:
        raise StepError(f"{name} is not set")
    return value


def temp() -> Path:
    return Path(env("RUNNER_TEMP"))


def run(command: list, **options) -> int:
    print("+ " + subprocess.list2cmdline([str(c) for c in command]), flush=True)
    return subprocess.run([str(c) for c in command], **options).returncode


def remove_tree(path: Path) -> None:
    """Best effort. Windows: rmdir handles paths past MAX_PATH that shutil cannot."""
    if not os.path.lexists(path):
        return
    print(f"Removing {path}", flush=True)
    if path.is_symlink() or path.is_file():
        path.unlink()
        return
    if IS_WINDOWS:
        subprocess.run(["cmd", "/c", "rmdir", "/s", "/q", str(path)], stderr=subprocess.DEVNULL)
    shutil.rmtree(path, ignore_errors=True)


def count_files(folder: Path) -> int:
    return sum(1 for p in folder.rglob("*") if p.is_file()) if folder.is_dir() else 0


def module_names(uplugin: Path) -> list:
    return [m.get("Name") for m in ue_json.read(uplugin).get("Modules") or []]


def build_plugin(package: Path) -> None:
    """RunUAT BuildPlugin -Rocket of the committed (test-free) descriptor for this host's platform."""
    remove_tree(package)
    code = run([env("UE_RUNUAT"), "BuildPlugin", f"-Plugin={PLUGIN / 'UnrealMCP.uplugin'}", f"-Package={package}",
                f"-TargetPlatforms={env('UE_PLATFORM')}", "-Rocket"])
    if code != 0:
        raise StepError(f"BuildPlugin failed with exit code {code}")


# ── shared by the UE jobs ────────────────────────────────────────────────────────────────────────

def clean_staged_bridge(_args) -> None:
    """Remove the gitignored, transiently staged bridge binaries from a persistent self-hosted workspace.

    The self-hosted Unreal jobs check out with `clean: false`, so untracked files survive between jobs on
    the same runner slot. release.yml's build-plugin-zip stages the self-contained bridge into the
    gitignored UnrealMCP/Source/ThirdParty/UnrealMcpBridge/<rid>/ folder. Left behind, a later PR plugin
    job BuildPlugin-packages those binaries, FUnrealMcpSidecarManager then finds a "bundled" bridge, and
    the "degrades when no bundled binary exists" spec fails. Removes ONLY ignored files:
      * Source/ThirdParty/UnrealMcpBridge with `git clean -fdX` (the tracked README.md and <rid>/.gitkeep
        stay: -X never touches tracked or non-ignored files);
      * Binaries/ThirdParty after proving nothing under it is tracked. Not `git clean`: Binaries/ is
        ignored as a whole, so git would delete all of UnrealMCP/Binaries/ even with the narrower pathspec.
    """
    git = ["git", "-c", f"safe.directory={ROOT.as_posix()}"]
    source, binaries = "UnrealMCP/Source/ThirdParty/UnrealMcpBridge", "UnrealMCP/Binaries/ThirdParty"
    if run(git + ["clean", "-fdX", "--", source], cwd=ROOT) != 0:
        raise StepError(f"git clean -fdX -- {source} failed")
    tracked = subprocess.run(git + ["ls-files", "--", binaries], cwd=ROOT, capture_output=True, text=True, check=True).stdout.strip()
    if tracked:
        raise StepError(f"Refusing to delete {binaries}: it contains tracked files:\n{tracked}")
    remove_tree(ROOT / binaries)
    status = subprocess.run(git + ["status", "--porcelain=v1", "--ignored", "--", source, binaries],
                            cwd=ROOT, capture_output=True, text=True, check=True).stdout
    left = [line for line in status.splitlines() if line.startswith("!! ")]
    if left:
        raise StepError("Staged bridge files survived the clean:\n" + "\n".join(left))
    print(f"Staged bridge binaries cleaned ({source}, {binaries}).")


def chain_sweep(_args) -> None:
    """Remove a killed chain run's generated overrides and feeds from this `clean: false` workspace.

    Left behind, they would silently re-point the NEXT ordinary bridge build at a local feed.
    """
    for name in (NUGET_CONFIG_NAME, TARGETS_NAME):
        path = ROOT / name
        if path.is_file() and _is_ours(path):
            path.unlink()
            print(f"chain: removed stale generated {path}")
    for name in CHAIN_FEEDS:
        remove_tree(ROOT / ".artifacts" / name)


def build_plugin_dist(_args) -> None:
    """BuildPlugin -Rocket of the DISTRIBUTED (test-free) descriptor for this host's platform.

    C2 (#139): the committed UnrealMCP.uplugin omits the UnrealMcpEditorTests module (Fab review flags
    shipped test/automation modules), so this proves the marketplace package builds green without it.
    The Automation suite rebuilds WITH the test module in its own isolated copy. The Fab Win64 zip itself
    is built by release.yml's Windows-only build-plugin-zip.
    """
    package = temp() / "UnrealMCP-Dist-Package"
    build_plugin(package)
    descriptor = package / "UnrealMCP.uplugin"
    if descriptor.is_file():
        modules = module_names(descriptor)
        # An unreadable or empty Modules array would pass the guard below blind: fail loud.
        if not modules:
            raise StepError(f"Could not read Modules from the distributed .uplugin at {descriptor}; refusing to pass the C2 guard blind.")
        if "UnrealMcpEditorTests" in modules:
            raise StepError("Distributed package SHIPS UnrealMcpEditorTests; Fab review would flag it (C2 regression).")


def suite(args) -> None:
    """Isolated compilation + the one-editor Automation (and, in full mode, E2E) suite; exports REPORT_DIR."""
    result_file = ROOT / ".artifacts/suite-result.json"
    command = [sys.executable, ROOT / "scripts/extension_suite.py", "--engine", env("UE_ROOT"), "--mode", args.mode,
               "--ue-lock-held", "--result-file", result_file]
    # Extensions support UE 5.7+; older engines keep the core-only gates.
    if args.ue and tuple(map(int, args.ue.split("."))) < (5, 7):
        command.append("--extensions")
    code = run(command, cwd=ROOT)
    if result_file.is_file():
        report = json.loads(result_file.read_text(encoding="utf-8")).get("report")
        if report:
            add_job_env("REPORT_DIR", report)
    if code != 0:
        raise StepError(f"Extension suite failed (exit {code})")


def teardown(_args) -> int:
    """The ONE always() step that ends every UE job. Best effort: it never fails the job.

    In order: this job's own regenerable outputs (RUNNER_TEMP is not pruned on these shared runners; the
    host DerivedDataCache / Intermediate / Binaries speed cache and Docker are deliberately kept), the
    staged bridge (build-plugin-zip), the machine-wide lock, and last the chain python link this very
    process runs through (removing a link never touches the engine behind it). Fails the job only when the
    staged bridge survived (it would leak to the next job on this slot); every part still runs first.
    """
    job = os.environ.get("GITHUB_JOB", "")
    lock_state = ue_machine_lock.State(temp() / "ue-machine-lock")
    holds_lock = lock_state.holds_lock()
    parts = [(lambda: cleanup(job, holds_lock), False)]
    if job == "build-plugin-zip":
        parts.append((lambda: clean_staged_bridge(None), True))
    parts += [(lambda: ue_machine_lock.release(lock_state), False), (lambda: remove_link(link_path()), False)]
    failed = False
    for part, required in parts:
        try:
            part()
        except Exception as error:  # noqa: BLE001  (every part runs, whatever failed before it)
            print(f"::{'error' if required else 'warning'}::teardown: {error}", flush=True)
            failed |= required
    return 1 if failed else 0


def cleanup(job: str, holds_lock: bool) -> None:
    (ROOT / "chain-leg.json").unlink(missing_ok=True)
    if job == "connection-smoke":
        chain_sweep(None)
        for name in ("chain", "chain-record", *(p.name for p in temp().glob("chain-fixture-unreal-*"))):
            remove_tree(temp() / name)
        host = os.environ.get("HOST_UPROJECT", "")
        # The host logs only while this job holds the machine-wide UE lock: another job may be using them.
        if holds_lock and host and Path(host).is_file():
            for sub in ("Saved/Logs", "Saved/Crashes"):
                remove_tree(Path(host).parent / sub)
    for name in TEMP_OUTPUTS:
        remove_tree(temp() / name)
    for log in temp().glob("UBT-*.log"):
        log.unlink(missing_ok=True)
    print("Per-run cleanup done (DDC / Intermediate / Binaries and Docker intentionally preserved).")


# ── connection-smoke ─────────────────────────────────────────────────────────────────────────────

def build_bridge(_args) -> None:
    """Build the bridge sidecar and export it as UNREAL_MCP_BRIDGE_PATH (PR builds bundle no sidecar)."""
    if run(["dotnet", "build", ROOT / "bridge/Unreal-MCP-Bridge.sln", "--configuration", "Debug"]) != 0:
        raise StepError("bridge build failed")
    bridge = ROOT / "bridge/src/bin/Debug/net9.0" / ("unreal-mcp-bridge.exe" if IS_WINDOWS else "unreal-mcp-bridge")
    if not bridge.is_file():
        raise StepError(f"bridge binary not found at {bridge}")
    add_job_env("UNREAL_MCP_BRIDGE_PATH", str(bridge))


def smoke_host(_args) -> None:
    """Sync tests/smoke-host + this checkout's plugin source into the smoke host; export HOST_UPROJECT, SMOKE_LOGS.

    The host lives beside extension_suite.py's build caches and survives jobs on this runner slot, so
    its Binaries / Intermediate / DerivedDataCache stay warm and the host build below is incremental.
    """
    from extension_suite import smoke_host as sync
    uproject = sync()
    add_job_env("HOST_UPROJECT", str(uproject))
    add_job_env("SMOKE_LOGS", str(uproject.parent / "Saved/Logs"))


def build_host(_args) -> None:
    """Incremental host editor build with this checkout's plugin (and the host's game module) for this engine."""
    uproject = Path(env("HOST_UPROJECT"))
    target = uproject.stem + "Editor"
    log = temp() / f"UBT-{target}-{os.environ.get('GITHUB_JOB', 'job')}-{os.environ.get('RUNNER_NAME', 'runner')}.log"
    code = run([env("UE_BUILD"), target, env("UE_PLATFORM"), "Development", f"-project={uproject}", "-WaitMutex",
                "-NoLiveCoding", f"-log={log}"])
    if code != 0:
        raise StepError(f"Host editor target ({target}) build failed for UE_ROOT={env('UE_ROOT')} (exit {code})")


def smoke(_args) -> None:
    """The live relay smoke: client -> gamedev-mcp-server -> sidecar -> plugin -> editor -> tool.

    --mode custom downloads the public server release (or, under a chain lock, reuses the chain-built
    UNREAL_MCP_SERVER_PATH), launches the host editor in Custom mode and asserts the tool battery. The
    editor inherits UNREAL_MCP_BRIDGE_PATH. It also writes the RAW MCP dump for the T2 chain fixture to
    RUNNER_TEMP, never under the host project.
    """
    if os.environ.get("CHAIN_ACTIVE") == "1":
        server = os.environ.get("UNREAL_MCP_SERVER_PATH", "")
        if not server or not Path(server).is_file():
            raise StepError(f"chain: UNREAL_MCP_SERVER_PATH '{server}' is not an existing chain-built server")
        print(f"chain: smoke server UNREAL_MCP_SERVER_PATH={server}")
        print((Path(server).parent / "chain-identity.json").read_text(encoding="utf-8"))
    code = run([sys.executable, ROOT / "scripts/connection_smoke.py", "--mode", "custom", "--project", env("HOST_UPROJECT"),
                "--ue", env("UE_EDITOR_CMD"), "--server-port", "18080", "--timeout", "240",
                "--record-out", temp() / "chain-record/raw.json", "--battery", ROOT / "tests/chain-fixtures/battery.json"], cwd=ROOT)
    if code != 0:
        raise StepError(f"connection + tool smoke FAILED (exit {code})")


def fixture_diff(args) -> int:
    """T2 chain fixture (docs/chain-fixtures.md §F5): canonicalise the smoke's raw dump and diff it.

    Diffs against the committed tests/chain-fixtures/<ue>/tools.jsonl with the VENDORED chain_fixture.py.
    A missing committed fixture FAILS (fixture-missing); exit 3 is a contract-change: review fresh.jsonl
    in the chain-fixture-unreal-<ue> artifact and commit it.
    """
    raw = temp() / "chain-record/raw.json"
    out = temp() / f"chain-fixture-unreal-{args.ue}"
    committed = ROOT / f"tests/chain-fixtures/{args.ue}/tools.jsonl"
    out.mkdir(parents=True, exist_ok=True)
    if not raw.is_file():
        print(f"::error title=chain fixture::raw-dump-missing: {raw} was not written by the smoke")
        return 1
    fixture = [sys.executable, ROOT / ".github/scripts/chain_fixture.py"]
    child_env = dict(os.environ, PYTHONUTF8="1")
    fresh = out / "fresh.jsonl"
    code = run(fixture + ["canonicalize", raw, "--out", fresh], env=child_env)
    if code != 0:
        shutil.copy2(raw, out)  # no fresh.jsonl to review, so the raw dump is the only evidence
        print(f"::error title=chain fixture::canonicalize exited {code}")
        return code
    # Masking evidence without uploading the raw dump: the canonical form must carry <path>, never the host path.
    print(f"chain-mask: <path> placeholders in fresh.jsonl = {fresh.read_text(encoding='utf-8').count('<path>')}")
    if not committed.is_file():
        print(f"::error title=chain fixture::fixture-missing: {committed.relative_to(ROOT).as_posix()} is not committed. "
              f"Review fresh.jsonl in the chain-fixture-unreal-{args.ue} artifact and commit it.")
        return 1
    diff = subprocess.run([str(c) for c in fixture + ["diff", committed, fresh]], env=child_env, capture_output=True)
    text = diff.stdout.decode("utf-8", errors="replace")
    (out / "diff.txt").write_text(text, encoding="utf-8")
    print(text, end="")
    sys.stderr.write(diff.stderr.decode("utf-8", errors="replace"))
    if diff.returncode == 0:
        print(f"chain fixture: identical to {committed.relative_to(ROOT).as_posix()} (diff exit 0)")
    elif diff.returncode == 3:
        print(f"::error title=chain fixture::contract-change: the editor tool contract differs from "
              f"{committed.relative_to(ROOT).as_posix()} (unified diff above and in diff.txt)")
    else:
        print(f"::error title=chain fixture::diff exited {diff.returncode}")
    return diff.returncode


# ── release: signed bridge + plugin zips ─────────────────────────────────────────────────────────

def stage_bridge(_args) -> None:
    """Stage the 4 signed RID payloads (./bridge-staging/<rid>/) into the Fab-surviving Source/ThirdParty folder.

    The C++ resolver and UnrealMcpRuntime.Build.cs RuntimeDependencies read
    UnrealMCP/Source/ThirdParty/UnrealMcpBridge/<rid>/ (#139/#187), which survives a Fab strip (Binaries/
    is stripped) and is declared in Config/FilterPlugin.ini; BuildPlugin then stages it into the
    package's Binaries/ThirdParty/UnrealMcpBridge/<rid>/. The payloads are gitignored.

    Also writes SHA256SUMS over the per-RID release zips (./bridge-zips/, the exact bytes publish-release attaches):
    the plugin's trust anchor for downloading the bridge when it ships without one (the Fab asset, §6.8).
    """
    zips = ROOT / "bridge-zips"
    lines = []
    for rid in RIDS:
        name = f"unreal-mcp-bridge-{rid}.zip"
        if not (zips / name).is_file():
            raise StepError(f"Bridge release zip missing for RID '{rid}' at {zips / name}")
        lines.append(f"{hashlib.sha256((zips / name).read_bytes()).hexdigest()}  {name}\n")
    for rid in RIDS:
        source = ROOT / "bridge-staging" / rid
        if not source.is_dir():
            raise StepError(f"Staged bridge dir missing for RID '{rid}' at {source}")
        dest = STAGED_BRIDGE / rid
        remove_tree(dest)
        shutil.copytree(source, dest)
        count = count_files(dest)
        print(f"Staged {count} file(s) for {rid} -> {dest}")
        if count < 1:
            raise StepError(f"No files staged for RID '{rid}'.")
    (STAGED_BRIDGE / BRIDGE_SUMS).write_text("".join(lines), encoding="utf-8", newline="\n")
    print(f"Wrote {STAGED_BRIDGE / BRIDGE_SUMS}:\n{''.join(lines)}", end="")


def stage_source_asset(folder: str) -> Path:
    """Copy the distributed plugin source (test-free, no EngineVersion pin, with the bridge SHA256SUMS) to RUNNER_TEMP."""
    descriptor = ue_json.read(PLUGIN / "UnrealMCP.uplugin")
    if str(descriptor.get("EngineVersion") or "").strip():
        raise StepError(f"The distributed source descriptor must not pin EngineVersion (found '{descriptor['EngineVersion']}').")
    if "UnrealMcpEditorTests" in module_names(PLUGIN / "UnrealMCP.uplugin"):
        raise StepError("The distributed source descriptor must not ship UnrealMcpEditorTests.")
    if not (STAGED_BRIDGE / BRIDGE_SUMS).is_file():
        raise StepError(f"{STAGED_BRIDGE / BRIDGE_SUMS} is missing (stage-bridge writes it); a plugin without it cannot download its bridge.")
    stage_root = temp() / folder
    remove_tree(stage_root)
    stage = stage_root / "UnrealMCP"
    for rel in SOURCE_ASSET_INPUTS:
        source = PLUGIN / rel
        if not source.exists():
            raise StepError(f"Source-asset input missing: {source}")
        target = stage / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(source, target) if source.is_dir() else shutil.copy2(source, target)
    if (stage / "Source/UnrealMcpEditorTests").exists():
        raise StepError("The source asset staging unexpectedly included UnrealMcpEditorTests.")
    return stage


def source_zip(args) -> None:
    """The SOURCE install asset unreal-mcp-plugin-source-<version>.zip (the CLI install path; bundles the bridge)."""
    stage = stage_source_asset("UnrealMCP-Source-Package")
    for rid in RIDS:
        count = count_files(stage / "Source/ThirdParty/UnrealMcpBridge" / rid)
        print(f"Source asset bridge for {rid}: {count} file(s).")
        if count < 1:
            raise StepError(f"Source asset is missing the staged bridge payload for RID '{rid}'.")
    zip_dir(stage, ROOT / f"dist/unreal-mcp-plugin-source-{args.version}.zip", prefix="UnrealMCP/")


def find_executables(folder: Path) -> list:
    """Every file under `folder` an executable check flags: by extension, or by its first bytes (magic / shebang)."""
    found = []
    for path in sorted(p for p in folder.rglob("*") if p.is_file()):
        with path.open("rb") as handle:
            head = handle.read(4)
        if path.suffix.lower() in EXECUTABLE_SUFFIXES or any(head.startswith(magic) for magic in EXECUTABLE_MAGIC):
            found.append(path.relative_to(folder).as_posix())
    return found


def fab_zip(args) -> None:
    """The Fab asset unreal-mcp-plugin-fab-<version>.zip: the source asset WITHOUT any executable.

    Epic's Fab review accepts source files only, so the per-RID bridge payloads are dropped (their .gitkeep
    placeholders stay); the editor downloads the bridge for its own version and verifies it against the shipped
    SHA256SUMS (docs/ARCHITECTURE.md §6.8). Fails if anything executable is left.
    """
    stage = stage_source_asset("UnrealMCP-Fab-Package")
    for rid in RIDS:
        payload = stage / "Source/ThirdParty/UnrealMcpBridge" / rid
        remove_tree(payload)
        payload.mkdir(parents=True)
        (payload / ".gitkeep").write_bytes(b"")  # the tracked placeholder stage-bridge replaced with the payload
    found = find_executables(stage)
    if found:
        raise StepError("The Fab asset must contain no executable, found:\n" + "\n".join(found))
    print(f"Fab asset: {count_files(stage)} file(s), no executable.")
    zip_dir(stage, ROOT / f"dist/unreal-mcp-plugin-fab-{args.version}.zip", prefix="UnrealMCP/")


def plugin_zip(args) -> None:
    """BuildPlugin -Rocket with the staged signed bridge, verify both bridge copies, zip the PACKAGED plugin."""
    package = temp() / "UnrealMCP-Package"
    build_plugin(package)
    # RuntimeDependencies stage Source/ThirdParty/UnrealMcpBridge/<rid>/ into the package's
    # Binaries/ThirdParty/UnrealMcpBridge/<rid>/ (the resolver's staged path). If UBT did not (design R6),
    # copy them there so the shipped zip always carries them. The Fab-surviving Source copy must ship INSIDE
    # the package too: Fab strips Binaries/, so it is what Epic's recompile stages from.
    for rid in RIDS:
        packaged = package / "Binaries/ThirdParty/UnrealMcpBridge" / rid
        if count_files(packaged) < 1:
            print(f"RuntimeDependencies did not stage {rid} into the package; applying the post-step copy from "
                  "Source/ThirdParty/UnrealMcpBridge/ (design R6 fallback).")
            shutil.copytree(STAGED_BRIDGE / rid, packaged, dirs_exist_ok=True)
        binaries, source = count_files(packaged), count_files(package / "Source/ThirdParty/UnrealMcpBridge" / rid)
        print(f"Packaged bridge for {rid}: {binaries} file(s) in Binaries/, {source} in the Fab-surviving Source/.")
        if binaries < 1:
            raise StepError(f"Packaged plugin is missing the bundled bridge for RID '{rid}'.")
        if source < 1:
            raise StepError(f"Packaged plugin is missing the Fab-surviving Source/ThirdParty/UnrealMcpBridge/{rid} payload (FilterPlugin.ini).")
    zip_dir(package, ROOT / f"dist/unreal-mcp-plugin-{args.version}.zip")


COMMANDS = {
    "clean-staged-bridge": clean_staged_bridge,
    "chain-sweep": chain_sweep,
    "build-plugin-dist": build_plugin_dist,
    "suite": suite,
    "teardown": teardown,
    "build-bridge": build_bridge,
    "smoke-host": smoke_host,
    "build-host": build_host,
    "smoke": smoke,
    "fixture-diff": fixture_diff,
    "stage-bridge": stage_bridge,
    "source-zip": source_zip,
    "fab-zip": fab_zip,
    "plugin-zip": plugin_zip,
}


def main(argv: list | None = None) -> int:
    # The runner reads step output as UTF-8; a Windows console code page would mangle (or refuse) the rest.
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=sorted(COMMANDS))
    parser.add_argument("--ue", help="the matrix engine version (suite, fixture-diff)")
    parser.add_argument("--mode", default="verification", help="extension_suite.py mode (suite)")
    parser.add_argument("--version", help="release version (source-zip, fab-zip, plugin-zip)")
    args = parser.parse_args(argv)
    try:
        return COMMANDS[args.command](args) or 0
    except (StepError, subprocess.CalledProcessError) as error:
        print(f"::error::{error}", flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
