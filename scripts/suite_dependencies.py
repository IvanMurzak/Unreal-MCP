"""Reuse published bridge/server binaries for E2E; never rebuild or republish core."""
from pathlib import Path
import json
import re
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def resolve(output: Path) -> tuple[Path, Path]:
    import connection_smoke as smoke
    core = json.loads((ROOT / "UnrealMCP/UnrealMCP.uplugin").read_text())["VersionName"]
    folder = output / "dependencies" / f"core-{core}"
    folder.mkdir(parents=True, exist_ok=True)
    bridge = folder / "unreal-mcp-bridge.exe"
    if not bridge.exists():
        subprocess.run(["gh", "release", "download", f"v{core}", "--repo", "IvanMurzak/Unreal-MCP",
                        "--pattern", "unreal-mcp-bridge-win-x64.zip", "--dir", str(folder), "--clobber"], check=True)
        with zipfile.ZipFile(folder / "unreal-mcp-bridge-win-x64.zip") as archive:
            entries = [n for n in archive.namelist() if Path(n).name == bridge.name]
            if len(entries) != 1:
                raise ValueError("Published bridge archive must contain exactly one executable")
            bridge.write_bytes(archive.read(entries[0]))
    version = re.search(r"SERVER_VERSION = '([^']+)'", (ROOT / "cli/src/lib/server-version.ts").read_text())[1]
    server = smoke.resolve_server(folder / "Suite.uproject", None, version)
    if not server or not server.is_file():
        raise ValueError("Cannot obtain the pinned MCP server")
    return server.resolve(), bridge.resolve()
