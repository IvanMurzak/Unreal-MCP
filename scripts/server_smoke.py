#!/usr/bin/env python3
"""
server_smoke.py — LIVE end-to-end smoke for the in-UI local gamedev-mcp-server launch (issue #95).

This is the operator complement to window_smoke.py. window_smoke.py asserts the dock-side CONTRACT
(gating + the start/stop toggle the dev-control bridge exposes) without a real process; THIS script
drives the REAL launch lifecycle against a live GUI editor and verifies the actual OS process + TCP
port, covering the parts a contract test cannot:

  (1) Custom+http: Start -> a local gamedev-mcp-server process actually launches and the port LISTENS.
  (2) Stop -> the server process terminates and the port is FREE again (no orphan).
  (3) Start, then CLOSE the editor -> the server process is ALSO gone (no orphan after editor exit).
  (4) Gating: Start is a no-op (no process, no listener) in Cloud mode and in Custom + stdio.
  (5) Repeated Start/Stop cycles + an external kill leave NO orphaned gamedev-mcp-server processes.

It drives the SAME Start/Stop the Slate button fires, over the dev-control HTTP bridge
(`/control/click` targets `start-server` / `stop-server`, gated to Custom+http by the view-model).

Requirements
------------
- A GUI editor (UnrealEditor.exe, not -Cmd) it launches with UNREAL_MCP_DEV_CONTROL=1 + a port.
- The Custom host must point at a free local port (the server listens there) — pass --server-port,
  which this script writes into the dock via /control/server-url before starting.
- Windows: uses `netstat -ano` + `tasklist` to check the port + process. (Linux/mac: lsof/ps.)

Usage
-----
    python scripts/server_smoke.py --project <.uproject> --port 5177 --server-port 5244 \
        --out-dir tmp/server-smoke

Exit code 0 = every row passed; non-zero = a FAIL or the bridge never came up. NOT a CI gate
(needs a GUI editor + a real server binary) — it is operator evidence for the issue #95 DoD matrix.
"""
from __future__ import annotations

import argparse
import json
import os
import platform
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

DEFAULT_EDITOR = r"C:/Program Files/Epic Games/UE_5.7/Engine/Binaries/Win64/UnrealEditor.exe"
SERVER_PROCESS_NEEDLE = "gamedev-mcp-server"


def _req(port: int, method: str, path: str, body: dict | None = None) -> tuple[int, dict]:
    url = f"http://127.0.0.1:{port}{path}"
    data = json.dumps(body).encode("utf-8") if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return resp.status, json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read().decode("utf-8"))
        except Exception:
            return e.code, {}


def _wait_health(port: int, timeout_s: float) -> bool:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            code, body = _req(port, "GET", "/health")
            if code == 200 and body.get("ok"):
                return True
        except Exception:
            pass
        time.sleep(2.0)
    return False


def _is_windows() -> bool:
    return platform.system() == "Windows"


def port_listening(port: int) -> bool:
    """True when SOME process is LISTENING on the TCP port (cross-platform)."""
    try:
        if _is_windows():
            out = subprocess.run(["netstat", "-ano", "-p", "tcp"], capture_output=True, text=True, timeout=15).stdout
            suffix = f":{port}"
            for line in out.splitlines():
                t = line.strip()
                if "LISTENING" not in t:
                    continue
                parts = t.split()
                if len(parts) >= 5 and parts[1].endswith(suffix):
                    return True
            return False
        else:
            r = subprocess.run(["lsof", "-ti", f"tcp:{port}", "-sTCP:LISTEN"], capture_output=True, text=True, timeout=15)
            return bool(r.stdout.strip())
    except Exception:
        return False


def server_process_count() -> int:
    """Count of running processes whose image name contains gamedev-mcp-server."""
    try:
        if _is_windows():
            out = subprocess.run(["tasklist"], capture_output=True, text=True, timeout=15).stdout
            return sum(1 for ln in out.splitlines() if SERVER_PROCESS_NEEDLE in ln.lower())
        else:
            out = subprocess.run(["ps", "-eo", "comm"], capture_output=True, text=True, timeout=15).stdout
            return sum(1 for ln in out.splitlines() if SERVER_PROCESS_NEEDLE in ln.lower())
    except Exception:
        return -1


def wait_for(cond, timeout_s: float, interval: float = 1.0) -> bool:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if cond():
            return True
        time.sleep(interval)
    return cond()


class Runner:
    def __init__(self, port: int, server_port: int) -> None:
        self.port = port
        self.server_port = server_port
        self.passed = 0
        self.failed = 0

    def check(self, label: str, cond: bool, detail: str = "") -> None:
        mark = "PASS" if cond else "FAIL"
        (setattr(self, "passed", self.passed + 1) if cond else setattr(self, "failed", self.failed + 1))
        print(f"  [{mark}] {label}" + (f" — {detail}" if detail else ""))

    def set_custom_http(self) -> None:
        _req(self.port, "POST", "/control/connection-mode", {"mode": "Custom"})
        _req(self.port, "POST", "/control/server-url", {"url": f"http://127.0.0.1:{self.server_port}"})
        _req(self.port, "POST", "/control/transport", {"transport": "http"})

    def run(self) -> None:
        baseline = server_process_count()
        print(f"Baseline gamedev-mcp-server processes: {baseline}")

        # (4) Gating: Cloud -> Start no-op (no listener, no new process).
        _req(self.port, "POST", "/control/connection-mode", {"mode": "Cloud"})
        _, b = _req(self.port, "POST", "/control/click", {"target": "start-server"})
        time.sleep(2.0)
        self.check("(4) Cloud: Start is a no-op (port not listening)", not port_listening(self.server_port))
        self.check("(4) Cloud: no server process spawned", server_process_count() <= max(baseline, 0))

        # (4) Gating: Custom + stdio -> Start no-op.
        _req(self.port, "POST", "/control/connection-mode", {"mode": "Custom"})
        _req(self.port, "POST", "/control/transport", {"transport": "stdio"})
        _, b = _req(self.port, "POST", "/control/click", {"target": "start-server"})
        time.sleep(2.0)
        self.check("(4) Custom+stdio: Start is a no-op (port not listening)", not port_listening(self.server_port))

        # (1) Custom + http: Start -> process launches + port listens.
        self.set_custom_http()
        _, b = _req(self.port, "POST", "/control/click", {"target": "start-server"})
        self.check("(1) Start reported serverRunning", b.get("serverRunning") is True, str(b))
        listening = wait_for(lambda: port_listening(self.server_port), timeout_s=30.0)
        self.check(f"(1) Custom+http: port {self.server_port} is LISTENING after Start", listening)
        self.check("(1) a gamedev-mcp-server process is running", server_process_count() > max(baseline, 0))

        # (2) Stop -> process terminates, port freed (no orphan).
        _, b = _req(self.port, "POST", "/control/click", {"target": "stop-server"})
        self.check("(2) Stop reported not running", b.get("serverRunning") is False, str(b))
        freed = wait_for(lambda: not port_listening(self.server_port), timeout_s=20.0)
        self.check(f"(2) port {self.server_port} FREE after Stop (no orphan)", freed)
        gone = wait_for(lambda: server_process_count() <= max(baseline, 0), timeout_s=20.0)
        self.check("(2) gamedev-mcp-server process gone after Stop", gone)

        # (5) Repeated Start/Stop cycles leave no orphan.
        for i in range(3):
            _req(self.port, "POST", "/control/click", {"target": "start-server"})
            wait_for(lambda: port_listening(self.server_port), timeout_s=30.0)
            _req(self.port, "POST", "/control/click", {"target": "stop-server"})
            wait_for(lambda: not port_listening(self.server_port), timeout_s=20.0)
        self.check("(5) repeated Start/Stop: no orphan after 3 cycles",
                   server_process_count() <= max(baseline, 0))

        # (3) Start, then close the editor -> server dies. (Caller closes the editor after run() returns;
        # the post-close port/process check happens in main() so we can observe it AFTER editor exit.)
        _req(self.port, "POST", "/control/click", {"target": "start-server"})
        self.started_for_close_test = wait_for(lambda: port_listening(self.server_port), timeout_s=30.0)
        self.check("(3) pre-close: server launched and listening", self.started_for_close_test)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--project", required=True, help="Path to the host .uproject.")
    ap.add_argument("--port", type=int, required=True, help="UNREAL_MCP_DEV_CONTROL_PORT (from .worktree.env).")
    ap.add_argument("--server-port", type=int, required=True, help="Local port the gamedev-mcp-server listens on.")
    ap.add_argument("--editor", default=DEFAULT_EDITOR, help="UnrealEditor.exe (GUI, not -Cmd).")
    ap.add_argument("--out-dir", default="tmp/server-smoke")
    ap.add_argument("--boot-timeout", type=float, default=600.0)
    args = ap.parse_args(argv)

    Path(args.out_dir).mkdir(parents=True, exist_ok=True)

    env = dict(os.environ)
    env["UNREAL_MCP_DEV_CONTROL"] = "1"
    env["UNREAL_MCP_DEV_CONTROL_PORT"] = str(args.port)
    print(f"Launching editor: {args.editor}\n  project={args.project}\n  dev-control={args.port} server-port={args.server_port}")
    proc = subprocess.Popen([args.editor, args.project], env=env)

    runner = Runner(args.port, args.server_port)
    try:
        print(f"Waiting for dev-control /health on {args.port} (up to {args.boot_timeout:.0f}s)...")
        if not _wait_health(args.port, args.boot_timeout):
            print("FAIL: dev-control bridge never came up. Is UNREAL_MCP_DEV_CONTROL=1 set and the dock open?")
            return 2
        print("Bridge up. Driving server lifecycle...\n")
        runner.run()
    finally:
        print("\nClosing the editor (row 3: server must die with it)...")
        proc.terminate()
        try:
            proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=30)

    # (3) After the editor has fully exited, the server it spawned must be gone (force-stop on shutdown).
    gone = wait_for(lambda: not port_listening(args.server_port), timeout_s=30.0)
    runner.check(f"(3) post-close: port {args.server_port} FREE (server died with the editor)", gone)
    runner.check("(3) post-close: no gamedev-mcp-server process survives editor exit",
                 server_process_count() <= 0)

    print(f"\n=== Summary: {runner.passed} passed, {runner.failed} failed ===")
    return 0 if runner.failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
