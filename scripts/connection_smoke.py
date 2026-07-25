#!/usr/bin/env python3
"""
connection_smoke.py — Headless Unreal-MCP connection + tool e2e smoke (self-contained).

Verifies, with NO GUI and NO human, that the UnrealMCP plugin connects to an MCP server and serves
tool calls end-to-end through the relay. Self-contained: depends only on this repo + a UE editor +
network access to the public GameDev-MCP-Server releases. NO dependency on the infra superproject.

Modes
-----
  --mode cloud   — connect to the hosted ai-game.dev server using the project's stored cloudToken
                 (Saved/Config/UnrealMcp/ai-game-developer-config.json). Signal: POST /mcp flips
                 401 -> 200 once the plugin registers the session.

  --mode custom  — DOWNLOAD the public `gamedev-mcp-server` release (latest, or --server-version),
                 run it locally in streamableHttp mode, launch the editor in Custom mode pointed at
                 it, and verify the plugin connects. Signal: tools/list returns the plugin's tools.
                 No auth. The server is fetched from
                 https://github.com/IvanMurzak/GameDev-MCP-Server/releases (cached under the
                 project's Intermediate/), or pass --server / set UNREAL_MCP_SERVER_PATH to reuse a
                 local build.

Both modes finish with a tool battery (ping + a few read-only tools) over the relay, proving the
whole chain (smoke -> server -> sidecar -> plugin -> editor -> tool -> back).

Usage
-----
  python scripts/connection_smoke.py --mode custom --project <.uproject>
        [--ue <UnrealEditor-Cmd.exe>] [--timeout 180] [--poll 5] [--connect-only]
        [--keep-editor] [--no-kill-existing] [--server-port 18080]
        [--server <gamedev-mcp-server[.exe]>] [--server-version latest|8.0.0]

Exit code 0 = all requested stages passed; non-zero = FAIL / timeout / setup error.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import socket
import ssl
import subprocess
import sys
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

DEFAULT_UE = Path(r"C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe")
CONFIG_REL = Path("Saved") / "Config" / "UnrealMcp" / "ai-game-developer-config.json"
MCP_PROTOCOL_VERSION = "2025-06-18"
SERVER_REPO = "IvanMurzak/GameDev-MCP-Server"
SERVER_BASENAME = "gamedev-mcp-server.exe" if os.name == "nt" else "gamedev-mcp-server"

MARK_PLUGIN_LOADED = "[Unreal-MCP] plugin loaded"
MARK_SIDECAR_SPAWN = "spawned sidecar"
MARK_NO_BINARY = "no sidecar binary resolved"
RE_FATAL = re.compile(r"Fatal error|Unhandled Exception|\[Callstack\]")


def _ascii_safe_stdio() -> None:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[attr-defined]
        except (AttributeError, ValueError):
            pass


def log(msg: str) -> None:
    print(f"[smoke] {msg}", flush=True)


def read_config(project: Path) -> dict:
    cfg_path = project.parent / CONFIG_REL
    if not cfg_path.is_file():
        raise SystemExit(f"FAIL: no plugin config at {cfg_path} — open the editor & authorize once first.")
    return json.loads(cfg_path.read_text(encoding="utf-8-sig"))


# --------------------------------------------------------------------------- MCP streamable HTTP

def _parse_messages(body: str, content_type: str) -> list:
    msgs: list = []
    if "text/event-stream" in content_type:
        for line in body.splitlines():
            line = line.strip()
            if line.startswith("data:"):
                payload = line[len("data:"):].strip()
                if payload and payload != "[DONE]":
                    try:
                        msgs.append(json.loads(payload))
                    except json.JSONDecodeError:
                        pass
    elif body.strip():
        try:
            msgs.append(json.loads(body))
        except json.JSONDecodeError:
            pass
    return msgs


def mcp_post(url: str, token: str | None, payload: dict, session_id: str | None = None,
             timeout: float = 30.0) -> tuple[int, str | None, list]:
    headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    if session_id:
        headers["Mcp-Session-Id"] = session_id
    req = urllib.request.Request(url, data=json.dumps(payload).encode(), method="POST", headers=headers)
    ctx = ssl.create_default_context()
    try:
        with urllib.request.urlopen(req, timeout=timeout, context=ctx) as resp:
            sid = resp.headers.get("Mcp-Session-Id") or session_id
            ctype = resp.headers.get("Content-Type", "")
            return resp.status, sid, _parse_messages(resp.read().decode("utf-8", "replace"), ctype)
    except urllib.error.HTTPError as e:
        return e.code, session_id, []
    except (urllib.error.URLError, socket.timeout, ssl.SSLError, ConnectionError):
        return 0, session_id, []


def mcp_initialize(url: str, token: str | None) -> tuple[int, str | None]:
    status, sid, _ = mcp_post(url, token, {
        "jsonrpc": "2.0", "id": 1, "method": "initialize",
        "params": {"protocolVersion": MCP_PROTOCOL_VERSION, "capabilities": {},
                   "clientInfo": {"name": "unreal-conn-smoke", "version": "0"}}})
    return status, sid


def mcp_list_tools(url: str, token: str | None, sid: str | None) -> list[str]:
    _s, sid, msgs = mcp_post(url, token, {"jsonrpc": "2.0", "id": 2, "method": "tools/list"}, sid)
    names: list[str] = []
    for m in msgs:
        for t in (m.get("result", {}).get("tools") or []):
            if t.get("name"):
                names.append(t["name"])
    return names


def _result_text(msg: dict) -> str:
    return " ".join(str(i.get("text", "")) for i in (msg.get("result", {}).get("content") or [])
                    if isinstance(i, dict) and i.get("type") == "text")


def mcp_call_tool(url: str, token: str | None, sid: str | None, name: str, args: dict,
                  call_id: int) -> tuple[bool, str | None, str]:
    _s, sid, msgs = mcp_post(url, token, {"jsonrpc": "2.0", "id": call_id, "method": "tools/call",
                                          "params": {"name": name, "arguments": args}}, sid)
    for m in msgs:
        if m.get("id") == call_id:
            if "error" in m:
                return False, sid, str(m.get("error"))[:120]
            res = m.get("result", {})
            text = _result_text(m) or json.dumps(res.get("structuredContent", ""))[:120]
            if res.get("isError"):
                return False, sid, text[:120]
            return True, sid, text.strip()[:120]
    return False, sid, "no result"


SMOKE_BATTERY = [
    ("editor-application-get-state", lambda nonce: {}),
    ("level-list-loaded", lambda nonce: {}),
    ("actor-find", lambda nonce: {}),
    ("actor-component-list-all", lambda nonce: {}),
]

# `ping` is a SYSTEM tool (docs/ARCHITECTURE.md §2.4, owner ruling 2026-07-25), so it is deliberately
# ABSENT from tools/list and cannot be exercised through tools/call. It is checked over the REST
# system-tools route instead — which is the stronger assertion anyway: it proves the surface split
# itself works end to end (server -> sidecar system manager -> IPC -> editor -> back), not just that
# some tool round-trips.
SYSTEM_PING_ROUTE = "/api/system-tools/ping"


def rest_system_ping(base_url: str, token: str | None, nonce: str,
                     timeout: float = 30.0) -> tuple[bool, str]:
    """POST the nonce to /api/system-tools/ping and confirm it comes back echoed."""
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    req = urllib.request.Request(
        base_url.rstrip("/") + SYSTEM_PING_ROUTE,
        data=json.dumps({"message": nonce}).encode(),
        method="POST", headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout, context=ssl.create_default_context()) as resp:
            body = resp.read().decode("utf-8", "replace")
            ok = resp.status == 200 and nonce in body
            return ok, f"HTTP {resp.status} {body.strip()[:100]}"
    except urllib.error.HTTPError as e:
        return False, f"HTTP {e.code}"
    except (urllib.error.URLError, socket.timeout, ssl.SSLError, ConnectionError) as e:
        return False, f"{type(e).__name__}: {e}"


def run_e2e(url: str, rest_base: str, token: str | None, nonce: str) -> tuple[bool, str]:
    status, sid = mcp_initialize(url, token)
    if status != 200 or not sid:
        return False, f"initialize failed (HTTP {status})"
    mcp_post(url, token, {"jsonrpc": "2.0", "method": "notifications/initialized"}, sid)
    tools: list[str] = []
    for _ in range(6):
        tools = mcp_list_tools(url, token, sid)
        if tools:
            break
        time.sleep(2)
    if not tools:
        return False, "tools/list returned no tools"
    log(f"e2e: tools/list -> {len(tools)} tools (e.g. {', '.join(sorted(tools)[:6])})")

    # §2.4: a SYSTEM tool must NOT be advertised to MCP clients. Assert the split rather than merely
    # tolerating the absence — a `ping` that reappeared here would mean the surface regressed.
    if "ping" in tools:
        return False, "ping is advertised in tools/list — it must be a SYSTEM tool (§2.4)"

    results: list[tuple[str, bool]] = []
    call_id = 10
    for name, mkargs in SMOKE_BATTERY:
        if name not in tools:
            continue
        ok, sid, text = mcp_call_tool(url, token, sid, name, mkargs(nonce), call_id)
        call_id += 1
        results.append((name, ok))
        log(f"e2e: {name:<30} -> {'OK  ' if ok else 'FAIL'}  {text[:60]}")

    # The system-surface half of the round trip, over REST (the route the CLI + desktop app probe). The base
    # is passed in, not derived from `url`: cloud and custom mount the REST surfaces differently (see main).
    ping_ok, ping_detail = rest_system_ping(rest_base, token, nonce)
    results.append(("ping (system)", ping_ok))
    log(f"e2e: {'ping (system-tools)':<30} -> {'OK  ' if ping_ok else 'FAIL'}  {ping_detail[:60]}")

    passed = sum(1 for _, ok in results if ok)
    detail = f"{passed}/{len(results)} tools OK ({len(tools)} listed): " + \
             ", ".join(f"{n}{'' if ok else '!'}" for n, ok in results)
    return passed == len(results), detail


# --------------------------------------------------------------------------- local server (custom)

def server_rid() -> str:
    import platform
    m = platform.machine().lower()
    if sys.platform.startswith("win"):
        return "win-arm64" if "arm" in m else "win-x64"
    if sys.platform == "darwin":
        return "osx-arm64" if "arm" in m else "osx-x64"
    return "linux-arm64" if "arm" in m or "aarch64" in m else "linux-x64"


def latest_server_version() -> str:
    try:
        with urllib.request.urlopen(
                f"https://api.github.com/repos/{SERVER_REPO}/releases/latest", timeout=20) as r:
            tag = json.load(r).get("tag_name", "") or ""
        return tag.lstrip("v") or "8.0.0"
    except (urllib.error.URLError, json.JSONDecodeError, socket.timeout):
        return "8.0.0"


def download_server(version: str, cache_dir: Path) -> Path | None:
    rid = server_rid()
    url = f"https://github.com/{SERVER_REPO}/releases/download/v{version}/gamedev-mcp-server-{rid}.zip"
    cache_dir.mkdir(parents=True, exist_ok=True)
    zippath = cache_dir / f"gamedev-mcp-server-{rid}.zip"
    log(f"downloading server v{version} ({rid}) ...")
    try:
        with urllib.request.urlopen(url, timeout=120) as r, open(zippath, "wb") as f:
            f.write(r.read())
        with zipfile.ZipFile(zippath) as z:
            z.extractall(cache_dir)
    except (urllib.error.URLError, socket.timeout, zipfile.BadZipFile) as e:
        log(f"server download failed: {e}")
        return None
    # win-x64 zips are flat; *nix zips nest under <rid>/ — find the binary at the shallowest depth.
    found = sorted(cache_dir.rglob(SERVER_BASENAME), key=lambda p: len(p.parts))
    return found[0] if found else None


def resolve_server(project: Path, override: str | None, version: str) -> Path | None:
    if override:
        p = Path(override)
        return p if p.is_file() else None
    env = os.environ.get("UNREAL_MCP_SERVER_PATH")
    if env and Path(env).is_file():
        return Path(env)
    cache = project.parent / "Intermediate" / "connection-smoke" / "server" / version
    binp = cache / SERVER_BASENAME
    if binp.is_file():
        return binp
    return download_server(version, cache)


def start_server(server_bin: Path, port: int, logdir: Path) -> tuple[subprocess.Popen, Path]:
    logpath = logdir / f"gamedev-mcp-server-{port}.log"
    fh = open(logpath, "w", encoding="utf-8")
    proc = subprocess.Popen([str(server_bin), "--port", str(port), "--client-transport", "streamableHttp"],
                            stdout=fh, stderr=subprocess.STDOUT, cwd=str(server_bin.parent))
    return proc, logpath


def wait_port(port: int, timeout: float) -> bool:
    start = time.monotonic()
    while time.monotonic() - start < timeout:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=2):
                return True
        except OSError:
            time.sleep(1)
    return False


# --------------------------------------------------------------------------- process management

def kill_existing_editors(project: Path) -> int:
    if os.name != "nt":
        return 0
    ps = ("Get-CimInstance Win32_Process -Filter \"Name='UnrealEditor.exe' OR Name='UnrealEditor-Cmd.exe'\" "
          f"| Where-Object {{ $_.CommandLine -like '*{project.stem}*' }} "
          "| ForEach-Object { Stop-Process -Id $_.ProcessId -Force; $_.ProcessId }")
    out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True)
    return len([l for l in out.stdout.splitlines() if l.strip().isdigit()])


def kill_tree(pid: int) -> None:
    if os.name == "nt":
        subprocess.run(["taskkill", "/PID", str(pid), "/T", "/F"], capture_output=True, text=True)
    else:
        subprocess.run(["kill", "-9", str(pid)], capture_output=True, text=True)


def tail(path: Path, n: int = 25) -> str:
    try:
        return "\n".join(path.read_text(encoding="utf-8", errors="replace").splitlines()[-n:])
    except OSError:
        return "(log not readable yet)"


def scan_log(path: Path) -> dict:
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return {}
    return {"plugin_loaded": MARK_PLUGIN_LOADED in text, "sidecar_spawned": MARK_SIDECAR_SPAWN in text,
            "no_binary": MARK_NO_BINARY in text, "fatal": bool(RE_FATAL.search(text))}


def main() -> int:
    ap = argparse.ArgumentParser(description="Headless Unreal-MCP connection + tool smoke.")
    ap.add_argument("--mode", choices=["cloud", "custom"], default="custom")
    ap.add_argument("--project", type=Path, required=True, help="path to the host .uproject")
    ap.add_argument("--ue", type=Path, default=Path(os.environ.get("UE_EDITOR_CMD", str(DEFAULT_UE))))
    ap.add_argument("--timeout", type=float, default=180.0)
    ap.add_argument("--poll", type=float, default=5.0)
    ap.add_argument("--connect-only", action="store_true")
    ap.add_argument("--keep-editor", action="store_true")
    ap.add_argument("--no-kill-existing", action="store_true")
    ap.add_argument("--server-port", type=int, default=18080)
    ap.add_argument("--server", type=str, default=None, help="custom: gamedev-mcp-server path override")
    ap.add_argument("--server-version", type=str, default="latest", help="custom: release version or 'latest'")
    args = ap.parse_args()
    _ascii_safe_stdio()

    project = args.project.resolve()
    if not project.is_file():
        raise SystemExit(f"FAIL: project not found: {project}")
    if not args.ue.is_file():
        raise SystemExit(f"FAIL: UnrealEditor-Cmd.exe not found: {args.ue} (set UE_EDITOR_CMD)")

    logdir = project.parent / "Saved" / "Logs"
    logdir.mkdir(parents=True, exist_ok=True)
    editor_env = dict(os.environ)
    server_proc: subprocess.Popen | None = None
    server_logpath: Path | None = None

    if args.mode == "cloud":
        cfg = read_config(project)
        token: str | None = (cfg.get("cloudToken") or "").strip() or None
        if not token:
            raise SystemExit("FAIL: no cloudToken — authorize once in the editor first.")
        base = (cfg.get("cloudUrl") or "https://ai-game.dev").rstrip("/")
        mcp_url = base + "/mcp"
        # REST base for the §2.4 system-tools route. In CLOUD mode nginx mounts the WHOLE MCP server under
        # /mcp and strips that prefix, so the REST surfaces live at <base>/mcp/api/... — do NOT strip it back
        # off (<base>/api/... reaches the billing backend, not the MCP server). This is why it is threaded
        # explicitly rather than derived from mcp_url.
        rest_base = mcp_url
        log(f"mode=cloud  mcp={mcp_url}  (token len {len(token)})")
    else:
        token = None
        port = args.server_port
        version = latest_server_version() if args.server_version == "latest" else args.server_version
        server_bin = resolve_server(project, args.server, version)
        if not server_bin:
            raise SystemExit("FAIL: could not obtain gamedev-mcp-server "
                             f"(version {version}); pass --server <path> or set UNREAL_MCP_SERVER_PATH.")
        log(f"mode=custom  server={server_bin.name}  version={version}  port={port}")
        server_proc, server_logpath = start_server(server_bin, port, logdir)
        if not wait_port(port, 40):
            kill_tree(server_proc.pid)
            print(f"RESULT: FAIL  -- local server never bound port {port}\n{tail(server_logpath, 20)}")
            return 1
        log(f"local server listening on {port}")
        mcp_url = f"http://localhost:{port}/mcp"
        # Local server: /mcp and /api/... are siblings off the root.
        rest_base = f"http://localhost:{port}"
        editor_env["UNREAL_MCP_CONNECTION_MODE"] = "Custom"
        editor_env["UNREAL_MCP_HOST"] = f"http://localhost:{port}"
        editor_env["UNREAL_MCP_KEEP_CONNECTED"] = "true"

    if not args.no_kill_existing:
        n = kill_existing_editors(project)
        if n:
            log(f"killed {n} pre-existing editor(s)")
            time.sleep(2)
    logpath = logdir / f"conn-smoke-{int(time.time())}.log"
    cmd = [str(args.ue), str(project), "-nullrhi", "-nosplash", "-unattended", "-NoSound", f"-abslog={logpath}"]
    log(f"launching headless editor -> log: {logpath.name}")
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            cwd=str(project.parent), env=editor_env)

    connect_result = "TIMEOUT"
    e2e_ok: bool | None = None
    e2e_detail = ""
    connected_at = None
    marks: dict = {}
    start = time.monotonic()
    try:
        while time.monotonic() - start < args.timeout:
            time.sleep(args.poll)
            if proc.poll() is not None:
                connect_result = "EDITOR_EXITED"; break
            if server_proc and server_proc.poll() is not None:
                connect_result = "SERVER_EXITED"; break
            marks = scan_log(logpath)
            if marks.get("no_binary"):
                connect_result = "NO_SIDECAR_BINARY"; break
            if marks.get("fatal"):
                connect_result = "EDITOR_CRASH"; break
            elapsed = int(time.monotonic() - start)
            if args.mode == "cloud":
                status, _sid = mcp_initialize(mcp_url, token)
                log(f"t+{elapsed:>3}s  /mcp init -> HTTP {status}  [sidecar={marks.get('sidecar_spawned')}]")
                connected = status == 200
            else:
                status, sid = mcp_initialize(mcp_url, token)
                tools = mcp_list_tools(mcp_url, token, sid) if status == 200 else []
                log(f"t+{elapsed:>3}s  /mcp init={status} tools={len(tools)}  [sidecar={marks.get('sidecar_spawned')}]")
                connected = len(tools) > 0
            if connected:
                connect_result = "PASS"; connected_at = elapsed; break

        if connect_result == "PASS" and not args.connect_only:
            log("connection up — running e2e tool battery...")
            e2e_ok, e2e_detail = run_e2e(mcp_url, rest_base, token, nonce=f"conn-smoke-{int(start)}")
    finally:
        marks = scan_log(logpath) or marks
        if not args.keep_editor and proc.poll() is None:
            log("tearing down editor (kill tree)...")
            kill_tree(proc.pid)
        if server_proc and server_proc.poll() is None:
            log("stopping local server...")
            kill_tree(server_proc.pid)

    overall = connect_result == "PASS" and (args.connect_only or e2e_ok is True)
    print("\n" + "=" * 70)
    print(f"MODE    : {args.mode}")
    print(f"CONNECT : {'PASS' if connect_result == 'PASS' else 'FAIL (' + connect_result + ')'}"
          + (f"  (~{connected_at}s)" if connected_at is not None else ""))
    if not args.connect_only:
        print(f"E2E     : {'SKIPPED' if e2e_ok is None else ('PASS' if e2e_ok else 'FAIL')}"
              + (f"  -- {e2e_detail}" if e2e_detail else ""))
    print(f"RESULT  : {'PASS' if overall else 'FAIL'}")
    if not overall:
        print(f"  markers: {marks}")
        print(f"  --- editor log tail ({logpath.name}) ---\n{tail(logpath, 25)}")
        bridge_log = project.parent / "Saved" / "Logs" / "UnrealMcpBridge.log"
        if bridge_log.is_file():
            print(f"  --- sidecar log tail ({bridge_log.name}) ---\n{tail(bridge_log, 25)}")
        if server_logpath and server_logpath.is_file():
            print(f"  --- server log tail ({server_logpath.name}) ---\n{tail(server_logpath, 20)}")
    print("=" * 70)
    return 0 if overall else 1


if __name__ == "__main__":
    sys.exit(main())
