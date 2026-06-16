#!/usr/bin/env python3
"""
window_smoke.py — LIVE (GUI) end-to-end smoke for the "AI Game Developer" window.

This is the live complement to the headless C++ Automation specs
(`UnrealMcpDevControlSpec` / `UnrealMcpEditorViewModelSpec`, run under
`Automation RunTests UnrealMcp`). Those specs are the CI regression gate; this script is
run ONCE by an operator to prove the real Slate dock reacts to EVERY button/event — it
drives each control over the dev-control HTTP bridge, reads `/state` back to confirm the
view-model (and thus the dock) updated, and optionally captures window screenshots.

It is deliberately NOT a CI gate (it needs a GUI editor + a GPU and is inherently flaky
under automation). It is evidence. Self-contained: depends only on this repo + a UE editor
(screenshot capture is optional — see --capture-script).

What it drives
--------------
Connection mode (Custom/Cloud), Server URL, Transport (stdio/http), Auth option
(none/required) + token field + Generate, Connect / Disconnect / Stop / Start, Cloud
Authorize / Cancel / Revoke (+ injected device-auth pending/authorized/failed rows), the
agent selector, the footer Check button (Serialization Check window), Restart bridge /
Open log (intent only), and the footer external links Help/Bug/Star (intent only — asserts
the URL the button WOULD launch, never opens a browser). After each it reads `GET /state`
and asserts the readout (status dot/label, button label, transport, auth, token presence
MASKED, device-auth state, cloud-token presence) reflects the change.

Usage
-----
    # Port from the worktree's .worktree.env (UNREAL_MCP_DEV_CONTROL_PORT):
    python scripts/window_smoke.py --project <.uproject> --port 5177 --out-dir tmp/window-smoke

    # Drive an ALREADY-RUNNING editor you launched with the dev-control env set:
    #   set UNREAL_MCP_DEV_CONTROL=1 && set UNREAL_MCP_DEV_CONTROL_PORT=5177
    python scripts/window_smoke.py --port 5177 --attach --out-dir tmp/window-smoke

Screenshots are best-effort: pass --capture-script pointing at a window-capture tool
(e.g. the infra repo's .scripts/capture_window.py) to enable them; omitted → skipped.

Exit code 0 = every assertion passed; non-zero = a FAIL or the bridge never came up.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

DEFAULT_EDITOR = r"C:/Program Files/Epic Games/UE_5.7/Engine/Binaries/Win64/UnrealEditor.exe"
WINDOW_TITLE = "AI Game Developer"


def _req(port: int, method: str, path: str, body: dict | None = None) -> tuple[int, dict]:
    """Send one dev-control request; return (status_code, parsed-json-body)."""
    url = f"http://127.0.0.1:{port}{path}"
    data = json.dumps(body).encode("utf-8") if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return resp.status, json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:  # 4xx/5xx still carry a JSON body
        try:
            return e.code, json.loads(e.read().decode("utf-8"))
        except Exception:
            return e.code, {}


def _wait_health(port: int, timeout_s: float) -> bool:
    """Poll /health until it answers ok or the timeout elapses."""
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


class Driver:
    """Runs the control/state assertions and tallies pass/fail."""

    def __init__(self, port: int, out_dir: Path, capture_script: str | None) -> None:
        self.port = port
        self.out_dir = out_dir
        self.capture_script = capture_script
        self.passed = 0
        self.failed = 0

    def check(self, label: str, cond: bool, detail: str = "") -> None:
        mark = "PASS" if cond else "FAIL"
        if cond:
            self.passed += 1
        else:
            self.failed += 1
        print(f"  [{mark}] {label}" + (f" — {detail}" if detail else ""))

    def shot(self, name: str) -> None:
        """Best-effort window screenshot (only when --capture-script was given)."""
        if not self.capture_script:
            return
        out = self.out_dir / f"{name}.png"
        try:
            subprocess.run(
                [sys.executable, self.capture_script, "--title", WINDOW_TITLE, "--out", str(out), "--no-stitch"],
                timeout=60, check=False,
            )
            print(f"  [shot] {out}")
        except Exception as e:  # capture is evidence, not a gate
            print(f"  [shot] capture failed ({e}); continuing")

    def state(self) -> dict:
        _, body = _req(self.port, "GET", "/state")
        return body

    def run(self) -> None:
        port = self.port

        # --- Connection mode ---
        _req(port, "POST", "/control/connection-mode", {"mode": "Custom"})
        s = self.state()
        self.check("connection-mode -> Custom", s.get("connectionMode") == "Custom", s.get("connectionMode"))

        # --- Server URL ---
        _req(port, "POST", "/control/server-url", {"url": "http://127.0.0.1:5244"})
        s = self.state()
        self.check("server-url stored", s.get("customHost") == "http://127.0.0.1:5244", s.get("customHost"))

        # --- Transport (stdio/http) ---
        _req(port, "POST", "/control/transport", {"transport": "http"})
        s = self.state()
        self.check("transport -> http", s.get("transport") == "http", s.get("transport"))
        _req(port, "POST", "/control/transport", {"transport": "stdio"})
        s = self.state()
        self.check("transport -> stdio", s.get("transport") == "stdio", s.get("transport"))

        # --- Auth option + token + generate ---
        _req(port, "POST", "/control/auth-option", {"option": "required"})
        s = self.state()
        self.check("auth-option -> required", s.get("authOption") == "required", s.get("authOption"))
        _req(port, "POST", "/control/auth-token", {"token": "smoke-bearer-abcdef123456"})
        s = self.state()
        self.check("auth-token stored (presence)", s.get("hasCustomToken") is True)
        self.check("auth-token reported MASKED only (§8)",
                   "smoke-bearer" not in json.dumps(s), "raw token must not appear in /state")
        _req(port, "POST", "/control/click", {"target": "generate-token"})
        s = self.state()
        self.check("generate-token kept a token", s.get("hasCustomToken") is True)
        self.shot("01-custom-required-token")

        # --- Connect / Disconnect / Stop (SignalR connection — the "Unreal: <status>" row) ---
        _req(port, "POST", "/control/click", {"target": "connect"})
        s = self.state()
        self.check("connect -> keepConnected", s.get("keepConnected") is True, s.get("connectionState"))
        self.shot("02-connecting")
        _req(port, "POST", "/control/click", {"target": "stop"})
        s = self.state()
        self.check("stop -> not keepConnected", s.get("keepConnected") is False, s.get("connectionState"))
        _req(port, "POST", "/control/click", {"target": "disconnect"})

        # --- Local gamedev-mcp-server Start/Stop (issue #95 — the MCP-server card) + gating matrix ---
        # The full live launch/reachability + close-editor-kills-it matrix is driven by the operator harness
        # in scripts/server_smoke.py (it needs a real port + process checks). Here we assert the dock-side
        # contract the dev-control bridge exposes: gating reflects the mode/transport, and the explicit
        # start/stop toggle drives the live server state.
        #
        # (1) Custom + http -> Start is launchable.
        _req(port, "POST", "/control/connection-mode", {"mode": "Custom"})
        _req(port, "POST", "/control/transport", {"transport": "http"})
        s = self.state()
        self.check("server launchable in Custom+http", s.get("serverLaunchable") is True, str(s.get("serverLaunchable")))
        # (4a) Gating: Custom + stdio -> NOT launchable; a start click is a no-op (server stays stopped).
        _req(port, "POST", "/control/transport", {"transport": "stdio"})
        s = self.state()
        self.check("server NOT launchable in Custom+stdio", s.get("serverLaunchable") is False, str(s.get("serverLaunchable")))
        _, b = _req(port, "POST", "/control/click", {"target": "start-server"})
        self.check("start-server no-op in Custom+stdio (not running)", b.get("serverRunning") is False, str(b.get("serverRunning")))
        # (4b) Gating: Cloud -> NOT launchable; a start click is a no-op.
        _req(port, "POST", "/control/connection-mode", {"mode": "Cloud"})
        s = self.state()
        self.check("server NOT launchable in Cloud", s.get("serverLaunchable") is False, str(s.get("serverLaunchable")))
        _, b = _req(port, "POST", "/control/click", {"target": "start-server"})
        self.check("start-server no-op in Cloud (not running)", b.get("serverRunning") is False, str(b.get("serverRunning")))
        # Back to Custom+http and drive an actual launch -> stop cycle (1)+(2)+(5).
        _req(port, "POST", "/control/connection-mode", {"mode": "Custom"})
        _req(port, "POST", "/control/transport", {"transport": "http"})
        _, b = _req(port, "POST", "/control/click", {"target": "start-server"})
        self.check("start-server -> serverRunning (Custom+http)", b.get("serverRunning") is True, str(b.get("serverRunning")))
        self.shot("02b-server-running")
        _, b = _req(port, "POST", "/control/click", {"target": "stop-server"})
        self.check("stop-server -> serverRunning False (clean stop)", b.get("serverRunning") is False, str(b.get("serverRunning")))

        # --- Injected connection-status (status dots/labels readouts) ---
        _req(port, "POST", "/inject/connection-status", {"status": "Connected"})
        s = self.state()
        self.check("injected Connected -> /state", s.get("connectionState") == "Connected", s.get("connectionState"))
        self.check("button label is Disconnect", s.get("buttonLabel") == "Disconnect", s.get("buttonLabel"))
        self.shot("03-connected")

        # --- Cloud authorize / cancel / revoke (+ injected device-auth rows) ---
        _req(port, "POST", "/control/connection-mode", {"mode": "Cloud"})
        _req(port, "POST", "/control/click", {"target": "authorize"})
        s = self.state()
        # Sidecar-aware: Authorize() only enters Pending when OnSendAuth("auth-start") succeeds, which
        # needs a connected + handshaken sidecar. A plain live-UI run has no sidecar attached, so the
        # view-model deliberately falls to Failed (reason "No sidecar connected.") rather than wedging
        # in a code-less Pending — see UnrealMcpEditorViewModel.cpp::Authorize(). Accept either as the
        # expected outcome so a sidecar-less run still reports a full pass; the injected device-auth rows
        # below exercise the Pending/Authorized/Failed readouts directly regardless.
        auth_state = s.get("deviceAuthState")
        self.check("authorize -> Pending (or Failed when no sidecar)", auth_state in ("Pending", "Failed"), auth_state)
        _req(port, "POST", "/inject/device-auth",
             {"state": "pending", "verificationUrl": "https://ai-game.dev/device", "userCode": "WXYZ-1234"})
        self.shot("04-cloud-pending")
        _req(port, "POST", "/inject/device-auth", {"state": "authorized", "token": "cloud-bearer-smoke"})
        s = self.state()
        self.check("device-auth authorized -> hasCloudToken", s.get("hasCloudToken") is True, s.get("deviceAuthState"))
        self.shot("05-cloud-authorized")
        _req(port, "POST", "/control/click", {"target": "revoke"})
        s = self.state()
        self.check("revoke -> no cloud token", s.get("hasCloudToken") is False)
        # Re-authorize then cancel.
        _req(port, "POST", "/control/click", {"target": "authorize"})
        _req(port, "POST", "/control/click", {"target": "cancel"})
        s = self.state()
        self.check("cancel -> Idle", s.get("deviceAuthState") == "Idle", s.get("deviceAuthState"))
        _req(port, "POST", "/control/connection-mode", {"mode": "Custom"})

        # --- Agent selector ---
        _req(port, "POST", "/control/select-agent", {"agentId": "claude-code"})
        s = self.state()
        self.check("select-agent stored", s.get("selectedAgentId") == "claude-code", s.get("selectedAgentId"))

        # --- The Check button (Serialization Check window) ---
        code, body = _req(port, "POST", "/control/click", {"target": "check"})
        self.check("check button dispatched", code == 200 and body.get("ok") is True)
        s = self.state()
        self.check("serializationCheckTabId present", bool(s.get("serializationCheckTabId")), s.get("serializationCheckTabId"))
        self.shot("06-after-check")

        # --- Restart bridge / Open log (intent only) ---
        for target in ("restart-bridge", "open-log"):
            code, body = _req(port, "POST", "/control/click", {"target": target})
            self.check(f"{target} acknowledged (intent only)", code == 200 and body.get("ok") is True)

        # --- Footer external links (assert intent, never launched) ---
        for link, needle in (("help", "discord.gg"), ("bug", "/issues"), ("star", "IvanMurzak/Unreal-MCP")):
            code, body = _req(port, "POST", "/control/external-link", {"link": link})
            ok = code == 200 and needle in body.get("url", "") and body.get("launched") is False
            self.check(f"external-link {link} (intent, not launched)", ok, body.get("url"))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--project", help="Path to the host .uproject (omit with --attach).")
    ap.add_argument("--port", type=int, required=True, help="UNREAL_MCP_DEV_CONTROL_PORT (from .worktree.env).")
    ap.add_argument("--editor", default=DEFAULT_EDITOR, help="UnrealEditor.exe (GUI, not -Cmd).")
    ap.add_argument("--attach", action="store_true",
                    help="Drive an already-running editor (you launched it with the dev-control env set).")
    ap.add_argument("--out-dir", default="tmp/window-smoke", help="Screenshot output directory.")
    ap.add_argument("--capture-script", default=os.environ.get("UNREAL_MCP_CAPTURE_SCRIPT"),
                    help="Path to a window-capture script (e.g. the infra repo's .scripts/capture_window.py); "
                         "omit to skip screenshots.")
    ap.add_argument("--boot-timeout", type=float, default=600.0,
                    help="Seconds to wait for /health (first boot warms shaders).")
    args = ap.parse_args(argv)

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    proc: subprocess.Popen | None = None
    if not args.attach:
        if not args.project:
            ap.error("--project is required unless --attach is given.")
        env = dict(os.environ)
        env["UNREAL_MCP_DEV_CONTROL"] = "1"
        env["UNREAL_MCP_DEV_CONTROL_PORT"] = str(args.port)
        print(f"Launching editor: {args.editor}\n  project={args.project}\n  dev-control port={args.port}")
        proc = subprocess.Popen([args.editor, args.project], env=env)

    try:
        print(f"Waiting for dev-control /health on port {args.port} (up to {args.boot_timeout:.0f}s)...")
        if not _wait_health(args.port, args.boot_timeout):
            print("FAIL: dev-control bridge never came up. Is UNREAL_MCP_DEV_CONTROL=1 set and the dock open?")
            return 2
        print("Bridge is up. Driving controls...\n")
        driver = Driver(args.port, out_dir, args.capture_script)
        driver.run()
        print(f"\n=== Summary: {driver.passed} passed, {driver.failed} failed ===")
        if args.capture_script:
            print(f"Screenshots in: {out_dir.resolve()}")
        return 0 if driver.failed == 0 else 1
    finally:
        if proc is not None:
            print("Tearing down the editor...")
            proc.terminate()
            try:
                proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                proc.kill()


if __name__ == "__main__":
    raise SystemExit(main())
