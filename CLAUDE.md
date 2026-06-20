# CLAUDE.md — Unreal-MCP

Unreal-MCP bridges LLMs (Claude, Cursor, Copilot, …) with the [Unreal Engine](https://www.unrealengine.com/)
editor via the [Model Context Protocol](https://modelcontextprotocol.io/) — the Unreal-engine sibling of
Unity-MCP and Godot-MCP. **Status: beta** — the plugin, the .NET sidecar, the
`unreal-mcp-cli`, the AI Game Developer editor UI, and **62 built-in tools across 8 families** have shipped
and are covered by CI; nothing is published to a package registry yet. The local MCP server is the
shared, engine-agnostic [GameDev-MCP-Server](https://github.com/IvanMurzak/GameDev-MCP-Server)
(binary `gamedev-mcp-server`) — no server source lives in this repo.

**The authoritative design is [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).** Read the relevant
section before implementing anything non-trivial — it locks the IPC protocol (§1), dynamic tool
registration (§2), schema generation (§3), the game-thread dispatcher (§4), extensions (§5), sidecar
lifecycle (§6), UI (§7), config (§8), repo/versioning/CI (§9), and the tool-family roadmap (§10). The
user-facing entry point is [`README.md`](README.md); the release runbook is
[`docs/RELEASING.md`](docs/RELEASING.md); the extension author guide is
[`docs/EXTENSIONS.md`](docs/EXTENSIONS.md).

## Layout

| Path | What it is |
| --- | --- |
| `UnrealMCP/` | The UE **editor plugin**. `UnrealMCP/UnrealMCP.uplugin` `VersionName` is the version single-source (currently `0.1.0`). **No `EngineVersion` pin** (UE treats it as exact-match, not a floor). Modules: `UnrealMcpEditor` + `UnrealMcpEditorTests` (both Type `Editor`, LoadingPhase `Default`) |
| `UnrealMCP/Source/UnrealMcpRuntime/Public/` | The extension contract (`IUnrealMcpToolProvider.h`), the tool registry (`UnrealMcpToolRegistry.h`), the runtime bootstrap (`UnrealMcpRuntimeSubsystem.h`), the kill-switch settings (`UnrealMcpRuntimeSettings.h`), and the runtime core-family `Register` entry points (`UnrealMcpRuntimeCoreTools.h`). The headers are re-exported by `UnrealMcpEditor`, so the same contract serves editor and runtime extensions |
| `UnrealMCP/Source/UnrealMcpEditor/Private/` | `Tools/` (the 8 families), `Config/`, `UI/`, plus Bridge/Schema/Dispatch/Sidecar code per ARCHITECTURE §9.1 |
| `UnrealMCP/Source/UnrealMcpEditorTests/` | Automation specs behind `WITH_DEV_AUTOMATION_TESTS`, names under the **`UnrealMcp.`** filter prefix. **No top-level test folder** — tests live per-leg |
| `bridge/` | .NET 9 **sidecar** `com.IvanMurzak.Unreal.MCP.Bridge` (binary `unreal-mcp-bridge`) — the McpPlugin host the plugin spawns; IPC ⇄ SignalR relay. xUnit tests in `bridge/tests/`. Hand-authored, TRACKED solution `bridge/Unreal-MCP-Bridge.sln` |
| `cli/` | `unreal-mcp-cli` npm package (TypeScript, commander, vitest) — 16 commands. Publish-ready (metadata complete); the first version is published manually by the owner, then CI takes over — see `docs/RELEASING.md` |
| `samples/UnrealAITemplate/` | The §5 extension template plugin (a `hello-extension` tool + an invalid-schema switch) |
| `commands/bump-version.ps1` | Rewrites the version across `.uplugin` / bridge csproj / `cli/package.json`. **Release-pipeline-owned — never run from a feature task**. It does NOT touch the consumed server pin (`cli/src/lib/server-version.ts` `SERVER_VERSION`) |

Unlike Unity/Godot, the editor is C++: the .NET `McpPlugin` host lives in the **sidecar process**, not
in-process. The plugin **listens** on a localhost TCP port; the sidecar **dials**, authenticating with a
one-shot token delivered over **stdin** (never argv — §1.4). Two child processes must not be conflated:
`unreal-mcp-bridge` (always required) and `gamedev-mcp-server` (Custom/local mode only — downloaded
by the CLI from GameDev-MCP-Server releases, pinned by `cli/src/lib/server-version.ts`
`SERVER_VERSION`; `UNREAL_MCP_SERVER_PATH` overrides the path for local builds).

## Build / test (per leg)

The repo is a three-leg mono-repo (plugin, bridge, cli). Run only the legs your change touches.

### UE plugin (`UnrealMCP/**`)

Needs UE 5.7 and a host C++ project with the plugin available (the infra repo's `Unreal-Test-Project/`
junctions `Plugins/UnrealMCP → <repo>/UnrealMCP`). Quote engine paths — they contain spaces.

```bash
# 1. Build (UBT). First build in a fresh checkout can take 10+ minutes; incremental 1-3 min.
"C:/Program Files/Epic Games/UE_5.7/Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe" \
  UnrealTestProjectEditor Win64 Development \
  -project="<host>/UnrealTestProject.uproject" -WaitMutex

# 2. Headless boot smoke — expect "[Unreal-MCP] plugin loaded" in the log:
"C:/Program Files/Epic Games/UE_5.7/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" \
  "<host>/UnrealTestProject.uproject" -nullrhi -nosplash -unattended -ExecCmds="QUIT_EDITOR" -log

# 3. Automation specs (filter prefix UnrealMcp.):
"C:/Program Files/Epic Games/UE_5.7/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" \
  "<host>/UnrealTestProject.uproject" -nullrhi -nosplash -unattended \
  -ExecCmds="Automation RunTests UnrealMcp; Quit" -ReportExportPath="<dir>" -log
```

**The exported JSON report is authoritative, not the exit code** (UnrealEditor-Cmd's exit code is
unreliable across versions for test failures). Parse `<ReportExportPath>/index.json` — **note it is
written UTF-8 with a BOM (`utf-8-sig`)**, so decode accordingly. Pass = `"failed": 0` AND
`"succeeded" >= 1`. A **missing** `index.json` after a run means a CRASH (assert/exception aborted the
editor) — treat as FAILURE, not as the 0/0 "no specs" case; find the crash in the Saved log
(`Fatal error` / `Unhandled Exception` / `[Callstack]`). Pixel-capture (screenshot) tools can't render
under `-nullrhi`; their GPU-free branches are spec-covered, full capture is windowed-verified.

### bridge (.NET 9) — `bridge/**`

```bash
dotnet restore bridge/Unreal-MCP-Bridge.sln
dotnet build   bridge/Unreal-MCP-Bridge.sln --configuration Debug --no-restore
dotnet test    bridge/Unreal-MCP-Bridge.sln --configuration Debug --no-build
```

**Publish (bundle source — docs/ARCHITECTURE.md §6 BUNDLE model).** The sidecar publishes as a
**self-contained, single-file** binary per RID so the end user needs no .NET installed. Trimming is
OFF (McpPlugin/ReflectorNet/SignalR are reflection-heavy). Run the repeatable publish script:

```bash
bash bridge/publish.sh                       # Release, all 4 RIDs (win-x64/linux-x64/osx-x64/osx-arm64), zipped
bash bridge/publish.sh Release win-x64       # one RID; --no-zip leaves the raw dir (e.g. before signing)
# PowerShell equivalent: bridge/publish.ps1 [-Platforms <rid> ...] [-NoZip]
```

Each `bridge/publish/<rid>/` holds exactly one apphost (`unreal-mcp-bridge[.exe]`, ~73–80 MB; no
`.pdb`, no loose runtime DLLs). The csproj engages `SelfContained` + `PublishSingleFile` ONLY when a
RID is supplied, so a plain `dotnet build`/`test` (no `-r`) stays framework-dependent at the flat
`bin/<cfg>/net9.0/unreal-mcp-bridge.exe` path the live-e2e harness (`UNREAL_MCP_BRIDGE_PATH`) reads.

### server — none in this repo

The MCP server lives in the shared [GameDev-MCP-Server](https://github.com/IvanMurzak/GameDev-MCP-Server)
repo and is built/tested/released there. **Never add Unreal-specific server code** — re-read
ARCHITECTURE §0 if a task seems to require it; server-side changes go to the shared repo.

### cli (Node 20.19+/22.12+) — `cli/**`

```bash
cd cli && npm install && npm run build && npm test     # build = tsc; test = vitest run
```

### Live bridge e2e (any tool-family change)

Stand up server ⇄ bridge ⇄ headless editor and exercise the tool over HTTP. Boot the editor with all
three of `UNREAL_MCP_BRIDGE_PATH`, `UNREAL_MCP_HOST=http://localhost:<port>`, and
**`UNREAL_MCP_CONNECTION_MODE=Custom`** (without the mode the sidecar dials Cloud and the local server
returns HTTP 500 after `No connected clients`). Tool-set/manifest assertions need MCP `tools/list` over
`POST /mcp`, not the `/api/tools/<name>` REST passthrough (which only invokes one named tool and drops
the `content[]` image array). `/api/tools/<name>` calls **require** `-H "Content-Type: application/json"`.

## Versioning

Single semver shared by plugin / bridge / cli, starting at **0.1.0**. The
`UnrealMCP/UnrealMCP.uplugin` `VersionName` is the **single source of truth**. Bump with:

```powershell
.\commands\bump-version.ps1 -NewVersion "0.2.0"        # add -WhatIf to preview
```

It rewrites the `.uplugin` `VersionName`, the bridge csproj `<Version>`, and
`cli/package.json` (+ lock). **Never hand-edit one of them alone, and never bump from a feature task** —
the release pipeline owns version changes. The consumed GameDev-MCP-Server version is pinned
separately in `cli/src/lib/server-version.ts` (`SERVER_VERSION`); bumping it requires the
corresponding shared release to already exist (see `docs/RELEASING.md`).

**Frozen NuGet pins** (lockstep with Godot-MCP; owned by the upstream release pipelines — NEVER bump
here): `com.IvanMurzak.ReflectorNet` **5.3.1**, `com.IvanMurzak.McpPlugin` **6.10.0** (bridge — 6.10.0 also
supplies the shared engine-agnostic `com.IvanMurzak.McpPlugin.AgentConfig` module the bridge now consumes;
keeps the ReflectorNet 5.3.1 dependency, so the Godot-MCP lockstep holds). This number drifts as upstream
releases (it has gone 6.7.0 → 6.9.0 → 6.10.0); the authoritative pin is always whatever
`bridge/src/com.IvanMurzak.Unreal.MCP.Bridge.csproj` declares — read it live, do not trust this line. The
§2.3 `ProxyTool` lives in `bridge/` (`bridge/src/Tools/ProxyTool.cs` + `ProxyToolFactory.cs`) — `IRunTool`
+ `ToolManager.AddTool` are already public, so no upstream API bump was required; a future upstream
ProxyTool can replace the local copy through its own pipeline.

## Conventions

- **Naming:** plugin `UnrealMCP`, module `UnrealMcpEditor`; C++ prefixes `FUnrealMcp*` / `UUnrealMcp*` /
  `SUnrealMcp*` (Slate) / `IUnrealMcp*` (interfaces). .NET root namespace
  `com.IvanMurzak.Unreal.MCP.Bridge`. **Tool ids are kebab-case** (`actor-create`,
  `blueprint-compile`); the registry validates the pattern `^[a-z0-9]+(-[a-z0-9]+)*$`.
- **C++ style:** Unreal — **tabs**, braces on new lines, UE types (`FString`, `TArray`, `TSharedPtr`).
  Log via the dedicated `LogUnrealMcp` category. File header: the
  `// Copyright (c) 2026 Ivan Murzak ...` one-liner (copy from a neighbour).
- **Unity-build ODR rule (load-bearing):** the `UnrealMcpEditor` / `UnrealMcpEditorTests` modules are
  unity-built — every `.cpp` is concatenated into one translation unit — so an `anonymous namespace`
  does **not** make a helper file-private. Give every per-family local helper a **family-unique** name
  (e.g. `LevelMakeStringArraySchema`, not `MakeStringArraySchema`) and every per-spec `Run`/helper a
  **spec-unique** name (or make it a `BEGIN_DEFINE_SPEC` member). A same-name/same-signature collision
  across families fails the build — sometimes with a misleading cascade error at the call site (e.g.
  `C2661: 'FAutomationTestBase::TestFalse': no overloaded function takes 1 arguments`) rather than a
  redefinition error (`C2084`).
- **C# style:** every `.cs` starts with the ASCII-art Apache-2.0 header (copy from a neighbour).
- **Game-thread / no-modal-UI tool-handler contract:** all Unreal API calls from tool handlers run on
  the game thread via the dispatcher (§4); the IPC reader thread never executes tool bodies. **A tool
  handler must not pump modal UI and must not synchronously wait on bridge state** — doing either can
  wedge the editor (ARCHITECTURE §11 risk 2). Tool handlers are **synchronous**
  (`FUnrealMcpToolHandler = TFunction<FUnrealMcpToolResult(const FUnrealMcpToolCall&)>`): long-running
  work (`source-compile`, `blueprint-compile`) runs inline on the game thread and blocks it for the
  duration (ARCHITECTURE §4 envisions an async-chaining `TFuture` handler surface once the registry
  grows one; until then a compile blocks). Only the dispatcher's `Dispatch()` returns a `TFuture` to
  the bridge, and the per-call timeout always completes it.
- **Disabled tools are gated at `Execute()`**, not merely excluded from the manifest — a disabled tool
  is rejected even if a stale `tools/list` dispatches it (`UnrealMcpToolRegistry::Execute`).
- **Secrets:** `.env` is gitignored and must stay that way (it can hold `UNREAL_MCP_TOKEN`); the sidecar
  IPC token travels via stdin, never argv, and is never logged.
- **Commits:** `<type>(<scope>): <description>` conventional commits (scopes: plugin, bridge,
  cli, ipc, tools, dispatcher, schema, ui, sidecar, config, samples, ci, docs). Reference issues with
  `Closes #N`. Never `git add -A`. Never commit `Binaries/`/`Intermediate/`/`Saved/`/`bin/`/`obj/`/
  `node_modules/`/`dist/`. The `.sln` at a `.uproject` root is generated — never commit it; the
  hand-authored `bridge/Unreal-MCP-Bridge.sln` is the un-ignored exception.

## CI

CI runs on every PR via **`test_pull_request.yml`** (workflow name `test-pull-request`):

- bridge build + xUnit on `ubuntu-latest` **and** `windows-latest`
- `test-cli / cli` on Node 20 **and** Node 22 (reusable `test_cli.yml`)
- `plugin BuildPlugin + Automation (UE <ver>)` and `connection + tool smoke (UE <ver>)` — a
  **`strategy.matrix.ue: ['5.7', '5.8']`** runs both engine versions (the engine path is driven by
  `UE_ROOT: C:\Program Files\Epic Games\UE_${{ matrix.ue }}`; the host's own game module is rebuilt for
  the matrix engine so one host project serves both). They run on the **self-hosted Windows runner**
  labelled `unreal-5-7` (legacy name — the single runner has both engines installed and executes the
  matrix legs **sequentially**), and are **gated on `UNREAL_RUNNER_READY` / `UNREAL_SMOKE_READY == 'true'`**.
  While unset the jobs are **SKIPPED** (never red-by-absence), and fork PRs skip them too (they
  also require `head.repo.full_name == github.repository`). The hosted bridge/server/cli legs always
  provide PR signal.

`release.yml` is version-gated and **publishes nothing on a normal merge** — see
[`docs/RELEASING.md`](docs/RELEASING.md). Keep this file, `docs/RELEASING.md`, and the infra
`implement-task` profile `test.md` in lockstep with the actual workflow command surface.
