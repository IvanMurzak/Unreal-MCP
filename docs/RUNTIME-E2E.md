<!-- Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0. -->

# Runtime MCP — End-to-End Verification Runbook

This runbook proves the **runtime (in-game) MCP path** end-to-end: a real game process —
PIE (Play-In-Editor) and a **packaged Development build** — spawns the `unreal-mcp-bridge`
sidecar, connects to a local MCP server, and serves runtime tools over the live connection.
It is the operator-driven complement to the headless Automation specs (which lock the
deterministic half — security gates, world-resolver switching, sidecar binary resolution,
connect/disconnect/orphan-safety — in `UnrealMcpEditorTests/`).

> See `docs/ARCHITECTURE.md` §12.4 (runtime bootstrap API), §12.5 (sidecar packaging),
> §12.6 (game-thread dispatch + world resolution), §12.8 (security), §12.10 (version floor).

## What is and isn't asserted automatically

- **Headless CI / Automation (`UnrealMcp.RuntimeSubsystem`, `UnrealMcp.Sidecar`, …):** the
  §12.8 security gates (kill switch, loopback-host policy), the §12.6 editor↔runtime
  world-resolver swap, connect/disconnect lifecycle + orphan-safety (`Disconnect` is a clean
  no-op when never connected; a rejected `Connect` orphans nothing; `Get(nullptr)` is null),
  the runtime-vs-editor tool-manifest separation (runtime set == 22, editor-only tools
  absent), and the sidecar binary resolver (RID mapping, bundled-path composition,
  `UNREAL_MCP_BRIDGE_PATH` override, graceful-degrade). The bridge xUnit
  `ConnectionConfigTests.AbsentFields_PreserveEnvFallback` proves the
  `UNREAL_MCP_HOST` / `UNREAL_MCP_TOKEN` env-fallback dev override survives a config message.
- **This runbook (live, operator / nightly):** the live sidecar spawn + handshake + tool
  round-trips against a running `gamedev-mcp-server`, for both PIE and a packaged build, plus
  the live no-orphan check (close the game → the sidecar process exits). The live chain needs
  a running server and (for the packaged half) a packaged game, neither of which is available
  on the hosted PR CI legs — see "CI scope" below.

## Prerequisites

- UE 5.7 at `C:\Program Files\Epic Games\UE_5.7` (the testbed is wired via the
  `Unreal-Test-Project/Plugins/UnrealMCP` junction — see the infra `CLAUDE.md`).
- The shared `gamedev-mcp-server` binary (`SERVER_VERSION` in
  `cli/src/lib/server-version.ts`, currently `8.0.0`). Obtain it via the CLI's server command
  (it downloads + caches the pinned release), or build it locally from the
  [GameDev-MCP-Server](https://github.com/IvanMurzak/GameDev-MCP-Server) repo and point
  `UNREAL_MCP_SERVER_PATH` at the result.
- A published / built `unreal-mcp-bridge` for the host RID. For PIE you can point
  `UNREAL_MCP_BRIDGE_PATH` at the flat `bridge/src/bin/Debug/net9.0/unreal-mcp-bridge.exe`;
  for a packaged build the bridge is **bundled** under
  `<Staged>/<Project>/Plugins/UnrealMCP/Binaries/ThirdParty/UnrealMcpBridge/<rid>/` (§12.5),
  resolved automatically — no env var needed in the packaged game.

## The runtime kill switch (REQUIRED before any Connect)

Runtime MCP is **off by default** (§12.8 #4). `UUnrealMcpRuntimeSettings::bRuntimeMcpEnabled`
is `Config = Game` (DefaultConfig), so enable it in the project's `Config/DefaultGame.ini` so
the setting travels into the packaged build:

```ini
[/Script/UnrealMcpRuntime.UnrealMcpRuntimeSettings]
bRuntimeMcpEnabled=True
```

Without this, `Connect()` (and `UnrealMcp.Connect`) log a warning and return `false`. This is
the single opt-in toggle; there is no auto-connect path (any auto-dial would be a security
defect, §12.8 #1).

## Step 1 — start the shared MCP server

Launch `gamedev-mcp-server` on a streamable-HTTP port (use a port from the worktree's
`.worktree.env` range):

```bash
gamedev-mcp-server port=<port> client-transport=streamableHttp
```

Leave it running. The runtime game will dial `http://localhost:<port>` in **Custom** mode.

## Step 2A — PIE runtime path (editor Play + console Connect)

1. Open the testbed in the editor (or boot it headless and keep it alive).
2. Enter **Play** (PIE) — the `UGameInstanceSubsystem` auto-instantiates and arms its loopback
   listener but does NOT connect.
3. In the console (backtick), opt in:

   ```
   UnrealMcp.Connect http://localhost:<port>
   ```

   (`UnrealMcp.Connect <host> [token]` — loopback hosts only, Custom mode. `UnrealMcp.Disconnect`
   tears it down.)
4. Watch the server log for the connect signals:
   `Client connected to McpServerHub. Total connected clients: 1` and
   `Version handshake successful. Environment: Unreal-MCP-Bridge`.
5. Drive runtime tools over the connection (see Step 3).
6. Stop PIE → the subsystem `Deinitialize` runs the orphan-safe teardown; confirm no
   `unreal-mcp-bridge` process remains (`tasklist | findstr unreal-mcp-bridge`).

## Step 2B — packaged Development build (the headline DoD)

1. Package a Development Win64 build of the testbed:

   ```bash
   "C:/Program Files/Epic Games/UE_5.7/Engine/Build/BatchFiles/RunUAT.bat" \
     BuildCookRun \
     -project="<infra_root>/Unreal-Test-Project/UnrealTestProject.uproject" \
     -noP4 -platform=Win64 -clientconfig=Development \
     -build -cook -stage -pak -archive \
     -archivedirectory="C:/tmp/UTPStage"
   ```

   - Use a SHORT `-archivedirectory` (e.g. `C:/tmp/UTPStage`) — a deep
     `.claude/worktrees/<long-name>/` staging path blows Windows MAX_PATH (260), the same
     environmental wrinkle as Suite 1b's `-Package`.
   - Confirm the bundled bridge staged into the package at
     `…/UnrealTestProject/Plugins/UnrealMCP/Binaries/ThirdParty/UnrealMcpBridge/win-x64/unreal-mcp-bridge.exe`
     (§12.5 — proves RuntimeDependencies live in the runtime Build.cs).
2. Launch the packaged game. The `bRuntimeMcpEnabled=True` from `DefaultGame.ini` rides along.
3. Open the in-game console and run `UnrealMcp.Connect http://localhost:<port>`.
4. Confirm the same connect signals in the server log as the PIE path.
5. Drive runtime tools (Step 3).
6. Close the game window → confirm the sidecar self-exits (no orphan).

## Step 3 — exercise runtime tools over the live connection

With the connection up, exercise (over the server's REST passthrough and/or `tools/list`):

- `ping` — readiness probe.
- `actor-create` — spawn a **native** actor AND a **Blueprint-class** actor (BP-class actors
  spawn fine at runtime, §12.7).
- `console-run-command` — run a CVar / console command in the live game.
- `game-time-dilation` — the R5 runtime sample tool
  (`samples/UnrealAIRuntimeSample/`), if that sample plugin is enabled in the testbed.
- `tools/list` — confirm the **runtime** tool set is present (≈22 built-ins + any registered
  sample/extension tools) and that editor-only tools (`asset-find`, `blueprint-create`,
  `editor-application-*`, …) are **absent**. Use MCP `tools/list` over `POST /mcp` for
  membership assertions — the `/api/tools/<name>` REST passthrough only invokes one named tool.

`-H "Content-Type: application/json"` is REQUIRED on `/api/tools/<name>` calls (without it the
server drops the body and returns `'x' is required.`).

## Step 4 — no-orphan check (mandatory)

After closing the game / stopping PIE:

```bash
tasklist | findstr unreal-mcp-bridge      # must print nothing
```

The sidecar self-exits when its parent game process vanishes (orphan layer 2) or on the
plugin `shutdown` IPC message (`Deinitialize` / `Disconnect`).

## CI scope (release / nightly — NOT per-PR)

Mirroring `docs/ARCHITECTURE.md` §9.3, the live runtime e2e is **release / nightly scope on
the self-hosted Windows runner, not a per-PR gate**:

- **Per-PR CI** runs the deterministic gates only: the bridge xUnit suite (incl. the
  env-fallback test), the cli suite, and — when `UNREAL_RUNNER_READY=true` — the self-hosted
  `plugin BuildPlugin + Automation (UE 5.7)` job, which runs the runtime Automation specs
  (security gates, resolver swap, connect/disconnect/orphan-safety, manifest separation,
  sidecar resolver). These need no running server and no packaged game.
- **The live packaged / PIE round-trips in this runbook are operator / nightly steps.** They
  require a running `gamedev-mcp-server` and a packaged game build (and, for screenshots, a
  GPU — headless `-nullrhi` has none), which the hosted PR legs cannot provide. Run them on the
  self-hosted runner (or an operator workstation) before a release, recording the result here
  or in the release notes.

## 5.5 version floor — status

`docs/ARCHITECTURE.md` §12.10 requires the **UE 5.5 floor to be EXERCISED** (a packaged
Development build on a 5.5 install) before claiming it. **This is NOT yet claimed.** All
runtime e2e to date has been verified on **UE 5.7 only** (the sole engine installed on the
dev/CI machine). The relevant runtime APIs (`UGameInstanceSubsystem`, `UWorld::SpawnActor`,
`UGameViewportClient`) are documented as stable 5.5→5.7, and screenshot APIs are
version-checked; but per the §11 honesty rule, 5.5 support is recorded as
**"not yet verified — 5.7 only"** until a 5.5 install runs this runbook.
