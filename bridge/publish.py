#!/usr/bin/env python3
"""
publish.py — publish self-contained single-file `unreal-mcp-bridge` binaries per RID and zip them.

docs/ARCHITECTURE.md §6 BUNDLE model, issue #45: `unreal-mcp-bridge-<rid>.zip` release artifacts.

The published binary is SELF-CONTAINED: the end user needs no .NET SDK/runtime installed.
Trimming is OFF (McpPlugin/ReflectorNet/SignalR are reflection-heavy; see the bridge csproj).

Output (paths relative to this script, so it works from any cwd):
  bridge/publish/<rid>/                       the publish dir (one apphost, no .pdb / loose DLLs)
  bridge/publish/unreal-mcp-bridge-<rid>.zip  the publish dir's contents at the zip root

Zip entries keep their Unix mode, so an unzipped osx/linux apphost stays executable.

Usage
-----
    python bridge/publish.py                                   # Release, all 4 RIDs, zipped
    python bridge/publish.py --platforms win-x64               # Release, only win-x64, zipped
    python bridge/publish.py --platforms osx-x64 osx-arm64 --no-zip   # publish dirs only (sign first)
    python bridge/publish.py --configuration Debug --platforms linux-x64
    python bridge/publish.py --zip-only --platforms win-x64    # zip already-published (signed) dirs

--no-zip skips zipping: CI signs the raw apphost, then zips it with --zip-only.

Exit code 0 = every selected RID published (and zipped); 1 = an unknown RID or any publish failed
(the remaining RIDs are still attempted).
"""
from __future__ import annotations

import argparse
import shutil
import stat
import subprocess
import sys
import zipfile
from pathlib import Path

BRIDGE_DIR = Path(__file__).resolve().parent
PROJECT_FILE = BRIDGE_DIR / "src" / "com.IvanMurzak.Unreal.MCP.Bridge.csproj"
PUBLISH_ROOT = BRIDGE_DIR / "publish"
APPHOST_NAME = "unreal-mcp-bridge"

# The 4 RIDs the bundle model ships (§6).
ALL_RUNTIMES = ["win-x64", "linux-x64", "osx-x64", "osx-arm64"]


def publish(runtime: str, configuration: str, output: Path) -> bool:
    if output.exists():
        shutil.rmtree(output)
    cmd = [
        "dotnet", "publish", str(PROJECT_FILE),
        "-c", configuration,
        "-r", runtime,
        "--self-contained", "true",
        "-p:PublishSingleFile=true",
        "-o", str(output),
    ]
    try:
        return subprocess.run(cmd).returncode == 0
    except FileNotFoundError:
        print("dotnet not found on PATH", file=sys.stderr)
        return False


def zip_dir(source: Path, zip_path: Path, prefix: str = "") -> None:
    """Zip the contents of `source` under `prefix`, preserving Unix permissions.

    Every non-Windows apphost (`unreal-mcp-bridge`, no .exe) is marked executable, even when zipped on Windows.
    """
    zip_path.parent.mkdir(parents=True, exist_ok=True)
    if zip_path.exists():
        zip_path.unlink()
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for path in sorted(source.rglob("*")):
            arcname = prefix + path.relative_to(source).as_posix()
            # Never group/world-writable: a Windows host reports 0o666 / 0o777.
            mode = stat.S_IMODE(path.stat().st_mode) & 0o755
            if path.is_dir():
                info = zipfile.ZipInfo(arcname + "/")
                info.external_attr = ((stat.S_IFDIR | 0o755) << 16) | 0x10
                zf.writestr(info, b"")
                continue
            info = zipfile.ZipInfo.from_file(path, arcname, strict_timestamps=False)
            # A Windows host reports no exec bit; force it on the non-Windows apphost.
            if path.name == APPHOST_NAME:
                mode |= 0o755
            info.external_attr = (stat.S_IFREG | (mode or 0o644)) << 16
            info.create_system = 3  # Unix, so unzip/ditto honour external_attr
            info.compress_type = zipfile.ZIP_DEFLATED
            with path.open("rb") as src, zf.open(info, "w") as dst:
                shutil.copyfileobj(src, dst, 1024 * 1024)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Publish self-contained single-file unreal-mcp-bridge binaries per RID and zip them."
    )
    parser.add_argument("--configuration", "-c", default="Release", help="Build configuration (default: Release)")
    parser.add_argument(
        "--platforms", "-p", nargs="+", default=[], metavar="RID",
        help=f"RIDs to publish (default: all of {' '.join(ALL_RUNTIMES)})",
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--no-zip", action="store_true", help="Publish dirs only (e.g. before signing)")
    mode.add_argument("--zip-only", action="store_true", help="Only zip existing publish dirs (e.g. after signing)")
    args = parser.parse_args()

    unknown = [r for r in args.platforms if r not in ALL_RUNTIMES]
    if unknown:
        print(f"Unknown RID(s) {', '.join(unknown)}. Available: {', '.join(ALL_RUNTIMES)}", file=sys.stderr)
        return 1
    runtimes = list(dict.fromkeys(args.platforms)) or ALL_RUNTIMES

    PUBLISH_ROOT.mkdir(parents=True, exist_ok=True)
    failed = []
    for runtime in runtimes:
        if not args.zip_only:
            print(f"Publishing {runtime} ({args.configuration})...", flush=True)
        output = PUBLISH_ROOT / runtime
        if args.zip_only:
            if not output.is_dir():
                print(f"Expected publish dir {output} not found", file=sys.stderr)
                failed.append(runtime)
                continue
        elif not publish(runtime, args.configuration, output):
            print(f"Failed to publish {runtime}", file=sys.stderr)
            failed.append(runtime)
            continue
        if not args.no_zip:
            zip_path = PUBLISH_ROOT / f"unreal-mcp-bridge-{runtime}.zip"
            zip_dir(output, zip_path)
            print(f"Created {zip_path}", flush=True)

    if failed:
        print(f"Bridge publish failed for: {', '.join(failed)}", file=sys.stderr)
        return 1
    print(f"All bridge publishes completed. Artifacts in: {PUBLISH_ROOT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
