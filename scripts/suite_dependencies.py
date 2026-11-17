"""Reuse published bridge/server binaries for E2E; never rebuild or republish core.

A binary the caller passes (--server / --bridge) is kept: only the missing one is fetched. A bridge built from the
same commit as the plugin is how a branch with unreleased RQL renderer changes runs its E2E (a published bridge
answers such a plugin with bridge-outdated)."""
from __future__ import annotations

from pathlib import Path
import json
import os
import re
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def resolve(output: Path, server: Path | None = None, bridge: Path | None = None) -> tuple[Path, Path]:
    for given, flag in ((server, "--server"), (bridge, "--bridge")):
        if given and not given.is_file():
            raise ValueError(f"{flag} {given} is not a file")
    import connection_smoke as smoke
    core = json.loads((ROOT / "UnrealMCP/UnrealMCP.uplugin").read_text())["VersionName"]
    folder = output / "dependencies" / f"core-{core}"
    folder.mkdir(parents=True, exist_ok=True)
    return (server.resolve() if server else published_server(smoke, folder),
            bridge.resolve() if bridge else published_bridge(smoke, core, folder))


def published_bridge(smoke, core: str, folder: Path) -> Path:
    # The bridge ships for win-x64, osx-arm64, osx-x64 and linux-x64; Windows on ARM runs win-x64.
    rid = {"win-arm64": "win-x64"}.get(smoke.server_rid(), smoke.server_rid())
    bridge = folder / ("unreal-mcp-bridge.exe" if os.name == "nt" else "unreal-mcp-bridge")
    if not bridge.exists():
        subprocess.run(["gh", "release", "download", f"v{core}", "--repo", "IvanMurzak/Unreal-MCP",
                        "--pattern", f"unreal-mcp-bridge-{rid}.zip", "--dir", str(folder), "--clobber"], check=True)
        with zipfile.ZipFile(folder / f"unreal-mcp-bridge-{rid}.zip") as archive:
            entries = [n for n in archive.namelist() if Path(n).name == bridge.name]
            if len(entries) != 1:
                raise ValueError("Published bridge archive must contain exactly one executable")
            bridge.write_bytes(archive.read(entries[0]))
        if os.name != "nt":
            bridge.chmod(bridge.stat().st_mode | 0o111)
    return bridge.resolve()


def published_server(smoke, folder: Path) -> Path:
    version = re.search(r"SERVER_VERSION = '([^']+)'", (ROOT / "cli/src/lib/server-version.ts").read_text())[1]
    server = smoke.resolve_server(folder / "Suite.uproject", None, version)
    if not server or not server.is_file():
        raise ValueError("Cannot obtain the pinned MCP server")
    return server.resolve()
