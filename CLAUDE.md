# CLAUDE.md — Unreal-MCP

Unreal-MCP bridges LLMs (Claude, Cursor, Copilot, …) with [Unreal Engine](https://www.unrealengine.com/)
via the [Model Context Protocol](https://modelcontextprotocol.io/) — the Unreal-engine sibling of
Unity-MCP and Godot-MCP. It works in **two modes**: an **editor** mode (the default — drive the Unreal
**Editor**: spawn actors, edit levels, author/compile Blueprints, edit/compile C++, capture screenshots,
…) and an opt-in **in-game runtime** mode (drive a running PIE / Standalone / packaged **Development**
build so an AI can operate your live game). **Status: beta** — the plugin, the .NET sidecar, the
`unreal-mcp-cli`, the AI Game Developer editor UI, and **62 built-in editor tools across 8 families**
have shipped and are covered by CI. The `unreal-mcp-cli` is **published on npm** (the Fab / Epic
Marketplace listing for the precompiled plugin is coming soon). The local MCP server is the shared,
engine-agnostic [GameDev-MCP-Server](https://github.com/IvanMurzak/GameDev-MCP-Server)
(binary `gamedev-mcp-server`) — no server source lives in this repo.

**The authoritative design is [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)** (defer to it on any
conflict). Read the relevant section before implementing anything non-trivial — it locks the IPC
protocol (§1), dynamic tool registration (§2), schema generation (§3), the game-thread dispatcher (§4),
extensions (§5), sidecar lifecycle (§6), UI (§7), config (§8), repo/versioning/CI (§9), the tool-family
roadmap (§10), risks (§11), **runtime (in-game) support (§12)**, and custom prompts & resources (§A).
The user-facing entry point is [`README.md`](README.md); the release runbook is
[`docs/RELEASING.md`](docs/RELEASING.md); the extension author guide is
[`docs/EXTENSIONS.md`](docs/EXTENSIONS.md); the in-game operator walkthrough is
[`docs/RUNTIME-E2E.md`](docs/RUNTIME-E2E.md).

## Architecture in one screen

Unlike Unity/Godot (C# engines that host the .NET `McpPlugin`/ReflectorNet/SignalR stack **in-process**),
Unreal's editor and runtime are **C++** and cannot host that .NET stack. So the McpPlugin host runs as an
auto-managed **.NET 9 sidecar process** (`unreal-mcp-bridge`, `bridge/`) that is a **local child of
whichever host loads the plugin** — the **Editor** in dev, or the **game process itself** in an opt-in
packaged build. It is never remote: the C++ plugin **listens** on a loopback TCP port and the sidecar
**dials** it, authenticating with a one-shot token delivered over **stdin** (never argv — §1.4). The
sidecar relays IPC ⇄ SignalR to the MCP server (cloud `ai-game.dev` by default, or a local
`gamedev-mcp-server`). See the §0 system-overview diagram.

The plugin is split into two modules (`UnrealMCP/UnrealMCP.uplugin`, runtime FIRST so it loads before
the editor module that depends on it):

- **`UnrealMcpRuntime`** (Type **Runtime**, LoadingPhase Default) — the dependency-lean, engine-agnostic
  machinery that cooks into packaged games: the tool **registry**, the game-thread **dispatcher**, the
  loopback-TCP **bridge server** + NDJSON framing, the **sidecar manager**, the world provider, the
  **extension manager**, config, the `IUnrealMcp*Provider` contracts + builders, and the runtime built-in
  `ping`. NO UnrealEd / Slate / asset / blueprint / capture deps (deps: Core/CoreUObject/Engine/
  DeveloperSettings public; Networking/Sockets/Json/JsonUtilities/Projects private). Also home to the
  runtime bootstrap (`UUnrealMcpRuntimeSubsystem`) + kill-switch settings (`UUnrealMcpRuntimeSettings`).
- **`UnrealMcpEditor`** (Type **Editor**, LoadingPhase Default) — depends on `UnrealMcpRuntime` and layers
  the Slate UI + the **62 editor-only tools** (the 8 families below, several RCE-class) on the SAME
  registry, plus the local-server manager and the dev-control bridge.

> Three locked decisions (ARCHITECTURE §12): (a) reuse the .NET sidecar — no in-process C++ server;
> (b) the `UnrealMcpRuntime` module holds the engine-agnostic infra + the runtime-safe `ping`, and
> `UnrealMcpEditor` depends on it; (c) a runtime connection is **explicit opt-in** — a shipped game NEVER
> auto-connects. The 62 engine-development tools are all **editor-only**; a game brings its OWN gameplay
> tools via `IUnrealMcpToolProvider` (author guide: [`docs/EXTENSIONS.md`](docs/EXTENSIONS.md)).

## Layout

| Path | What it is |
| --- | --- |
| `UnrealMCP/` | The UE **plugin**. `UnrealMCP/UnrealMCP.uplugin` `VersionName` is the version single-source (currently **`0.6.1`**). **No `EngineVersion` pin** (UE treats that field as an exact-build match, not a floor, and would refuse to load on newer engines); the **5.5+ floor** is a CI/doc claim (CI-tested against **5.7** and **5.8**). Declared modules: **`UnrealMcpRuntime` (Type `Runtime`)** + **`UnrealMcpEditor` (Type `Editor`)**, both LoadingPhase `Default`, runtime first |
| `UnrealMCP/Source/UnrealMcpRuntime/Public/` | The **public** contracts — re-exported by `UnrealMcpEditor`, so the SAME headers serve editor and runtime extensions: the tool/prompt/resource provider interfaces (`IUnrealMcpToolProvider.h`, `IUnrealMcpPromptProvider.h`, `IUnrealMcpResourceProvider.h`) + their fluent registries (`UnrealMcpToolRegistry.h`, `UnrealMcpPromptRegistry.h`, `UnrealMcpResourceRegistry.h`); the runtime bootstrap (`UnrealMcpRuntimeSubsystem.h`); the kill-switch settings (`UnrealMcpRuntimeSettings.h`); and the runtime core-family `Register` entry points (`UnrealMcpRuntimeCoreTools.h` = `ping`, `UnrealMcpRuntimeCorePrompts.h`, `UnrealMcpRuntimeCoreResources.h`) |
| `UnrealMCP/Source/UnrealMcpRuntime/Private/` | The infra that cooks into games: `Bridge/` (TCP listener + NDJSON), `Dispatch/` (game-thread dispatcher), `Tools/` (registry, object-ref, world provider, property-JSON, scoped-read, log collector, `ping`), `Sidecar/`, `Config/`, `Extensions/`, plus the core `Prompts/`/`Resources/` families |
| `UnrealMCP/Source/UnrealMcpEditor/Private/` | The editor-only surface: `Tools/` (the 8 families = 62 tools), `UI/` (Slate main window + aux tabs), `Server/` (local `gamedev-mcp-server` manager), `DevControl/`, plus the renamed editor coordinator |
| `UnrealMCP/Source/UnrealMcpEditorTests/` | Automation specs behind `WITH_DEV_AUTOMATION_TESTS`, names under the **`UnrealMcp.`** filter prefix. **This module is NOT declared in the distributed `UnrealMCP.uplugin`** (Fab flags shipped test modules, §6.7) — PR/dev CI transiently re-adds it via `commands/test-module-uplugin.ps1` around the Automation BuildPlugin, then reverts. **No top-level test folder** — tests live per-leg |
| `bridge/` | .NET 9 **sidecar** `com.IvanMurzak.Unreal.MCP.Bridge` (binary `unreal-mcp-bridge`) — the self-contained McpPlugin host the plugin spawns; IPC ⇄ SignalR relay. xUnit tests in `bridge/tests/`. Hand-authored, TRACKED solution `bridge/Unreal-MCP-Bridge.sln` |
| `cli/` | `unreal-mcp-cli` npm package (TypeScript, commander, vitest) — 16 commands. **Published on npm**; CI owns subsequent releases (see `docs/RELEASING.md`) |
| `samples/UnrealAITemplate/` | The §5 **editor** extension template plugin (a `hello-extension` tool + an `UNREAL_AI_TEMPLATE_INVALID_SCHEMA` switch that demonstrates per-extension isolation) |
| `samples/UnrealAIRuntimeSample/` | The **runtime (in-game)** extension sample — a `Type=Runtime` plugin whose `game-time-dilation` tool reads/sets the live world's `AWorldSettings::TimeDilation`, callable over a runtime MCP connection (§12.9) |
| `commands/bump-version.ps1` | Rewrites the version across `.uplugin` / bridge csproj / `cli/package.json`. **Release-pipeline-owned — never run from a feature task**. It does NOT touch the consumed server pin (`cli/src/lib/server-version.ts` `SERVER_VERSION`) |

Two child processes must not be conflated: `unreal-mcp-bridge` (the sidecar; always required when a
connection is live) and `gamedev-mcp-server` (Custom/local mode only — downloaded by the CLI from
GameDev-MCP-Server releases, pinned by `cli/src/lib/server-version.ts` `SERVER_VERSION`;
`UNREAL_MCP_SERVER_PATH` overrides the path for local builds; the editor can also Start/Stop it directly
via `FUnrealMcpServerManager`, gated to Custom + http).

## Editor vs runtime (in-game)

- **Editor mode** (default, everything in §0–§11): the **Editor** loads the plugin, auto-spawns the
  sidecar, and serves the 62 editor tools + the core `ping`/prompt/resource families. This is the
  `unreal-mcp-cli` / AI Game Developer window flow.
- **Runtime mode** (§12): a running game can host an MCP connection. The entry point is a
  `UGameInstanceSubsystem`, `UUnrealMcpRuntimeSubsystem` (in `UnrealMcpRuntime`) — auto-instantiated per
  `UGameInstance` but it **never auto-connects**; `Initialize()` only arms the listener (no sidecar spawn,
  no dial). A connection is always an explicit opt-in `Connect(Host, Token, Mode, bAllowRemoteHost)` from
  C++ / a Blueprint node / the `UnrealMcp.Connect` console command, which then spawns the sidecar.
- **Runtime built-in surface = `ping` ONLY.** Every engine-development family (actor/component,
  `object-*`, level, console/reflection, screenshot, blueprint, asset, source, editor-application/
  selection) is **editor-only** — they drive the editor and several are RCE-class, so they are not
  compiled into a shipped game. A game gets runtime tools by **bringing its own** via
  `IUnrealMcpToolProvider` (see the §12.7 / §12.9 design; the deterministic
  `UnrealMcp.RuntimeSubsystem` Automation gate asserts the runtime manifest is exactly `{ping}`).
- **Runtime security gates** — all enforced inside `Connect()` (§12.8): (1) **opt-in only**, never
  auto-connect; (2) **kill switch** `UUnrealMcpRuntimeSettings::bRuntimeMcpEnabled` (Project Settings →
  Plugins → *Unreal MCP (Runtime)*, a **Game** config setting in `DefaultGame.ini`) defaults **false** —
  while off every `Connect()` is rejected and no sidecar spawns; (3) **Shipping gate** — the
  `bUnrealMcpAllowShipping` `*.Build.cs` flag defaults **false** (→ `UNREAL_MCP_ALLOW_SHIPPING=0`), so
  `Connect()` returns false in a Shipping build unless deliberately compiled in (the same flag also gates
  whether the sidecar is staged into a Shipping package); (4) **loopback-host default** — non-loopback
  hosts rejected unless the caller passes `bAllowRemoteHost=true`; (5) **loopback IPC + one-shot stdin
  token** (same model as the editor). Runtime MCP is **Desktop-only** (Win64/Mac/Linux) — console/mobile
  cannot spawn the .NET sidecar.
- **Excluding Unreal-MCP from packaged games (zero footprint).** A consumer who only wants the editor
  tooling pins the plugin to the editor with a **`TargetDenyList`** in **their own `.uproject`** plugin
  reference (NOT the plugin's `.uplugin`): `{ "Name": "UnrealMCP", "Enabled": true, "TargetDenyList":
  ["Game","Client","Server"] }`. UE honours it (`PluginReferenceDescriptor::IsEnabledForTarget`), so the
  `UnrealMcpRuntime` module + the bundled sidecar `RuntimeDependencies` are excluded from packaged
  `Game`/`Client`/`Server` builds (editor modules are stripped by Type regardless). **Caveat:** a direct
  `*.Build.cs` dependency on (or `#include` of) any `UnrealMcp*` module from a **game** module overrides
  the deny-list and pulls the runtime module into the game anyway — a pure editor-only project references
  nothing from the plugin, so it stays clean by construction.

## Build / test (per leg)

The repo is a three-leg mono-repo (plugin, bridge, cli). Run only the legs your change touches.

### UE plugin (`UnrealMCP/**`)

Needs UE 5.7 and a host C++ project with the plugin available (the `software` testbed
`engines/unreal/test-project/` junctions `Plugins/UnrealMCP → <repo>/UnrealMCP`). Quote engine paths —
they contain spaces.

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

The `UnrealMcpEditorTests` module is **not** in the distributed `.uplugin` (§6.7); to run the Automation
specs locally, transiently re-add it with `commands/test-module-uplugin.ps1` (idempotent add/remove around
the Automation build), then revert — never commit the test module into the descriptor.

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

The release job stages the signed per-RID slices into the plugin's Fab-surviving `Sidecar/<rid>/` folder
(declared in `Config/FilterPlugin.ini`); `UnrealMcpRuntime.Build.cs` `RuntimeDependencies` (two-arg form)
then stages them into `Binaries/ThirdParty/UnrealMcpBridge/<rid>/` at package time — the path the C++
resolver reads. Binaries are **never** committed to git.

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

## Tool / prompt / resource surface

The editor ships **62 built-in ("core") tools across 8 families** (counts from the registration source:
actor 13, blueprint 11, asset 11, editor/reflection 9, level 7, source 6, screenshot 4, ping 1 = 62).
Tool ids are kebab-case (`actor-create`, `blueprint-compile`); the registry validates
`^[a-z0-9]+(-[a-z0-9]+)*$`. The README carries the full, source-generated family-by-family list — treat
it as the canonical inventory. Plus shipped **core prompt** `level-design-brief` and **core resources**
`unreal://project/levels` + `unreal://project/icon` (static fixed-URI only; templated URIs deferred). The
runtime module ships only `ping`.

## Versioning

Single semver shared by plugin / bridge / cli, currently **0.6.1**. The `UnrealMCP/UnrealMCP.uplugin`
`VersionName` is the **single source of truth**. Bump with:

```powershell
.\commands\bump-version.ps1 -NewVersion "0.6.0"        # add -WhatIf to preview
```

It rewrites the `.uplugin` `VersionName`, the bridge csproj `<Version>`, and
`cli/package.json` (+ lock). **Never hand-edit one of them alone, and never bump from a feature task** —
the release pipeline owns version changes. The consumed GameDev-MCP-Server version is pinned
separately in `cli/src/lib/server-version.ts` (`SERVER_VERSION`) and deliberately diverges (plugin 0.x,
shared server 8.x); bumping it requires the corresponding shared release to already exist (see
`docs/RELEASING.md`). The plugin↔sidecar handshake (`sidecarVersion` vs `pluginVersion`) is independent
of the consumed server version, so a `SERVER_VERSION` bump never touches it.

**Frozen NuGet pins** (lockstep with Godot-MCP; owned by the upstream release pipelines — NEVER bump
here): `com.IvanMurzak.ReflectorNet` **5.3.1**, `com.IvanMurzak.McpPlugin` **6.10.0** (bridge — 6.10.0 also
supplies the shared engine-agnostic `com.IvanMurzak.McpPlugin.AgentConfig` module the bridge now consumes;
keeps the ReflectorNet 5.3.1 dependency, so the Godot-MCP lockstep holds). This number drifts as upstream
releases (it has gone 6.7.0 → 6.9.0 → 6.10.0); the authoritative pin is always whatever
`bridge/src/com.IvanMurzak.Unreal.MCP.Bridge.csproj` declares — read it live, do not trust this line. The
§2.3 `ProxyTool` (and the §A `ProxyPrompt`/`ProxyResource`) live in `bridge/` (`bridge/src/Tools/`,
`bridge/src/`) — `IRunTool`/`IRunPrompt`/`IRunResource` + the manager `Add*` APIs are already public, so no
upstream API bump was required; a future upstream version can replace the local copies through its own
pipeline.

## Conventions

- **Naming:** plugin `UnrealMCP`, modules `UnrealMcpRuntime` (Runtime) + `UnrealMcpEditor` (Editor);
  C++ prefixes `FUnrealMcp*` / `UUnrealMcp*` / `SUnrealMcp*` (Slate) / `IUnrealMcp*` (interfaces). .NET
  root namespace `com.IvanMurzak.Unreal.MCP.Bridge`. **Tool ids are kebab-case** (`actor-create`,
  `blueprint-compile`); the registry validates the pattern `^[a-z0-9]+(-[a-z0-9]+)*$`.
- **Module boundary (load-bearing):** `UnrealMcpRuntime` must stay **GEditor-free and UnrealEd/Slate-free**
  (it cooks into games — there is a CI grep gate). Engine-development tools, Slate UI, asset/blueprint/
  capture deps, and the local-server manager all stay in `UnrealMcpEditor`. The editor coordinator
  *builds* the runtime-owned types (registry/dispatcher/bridge/sidecar/extension-manager/config — all
  `UNREALMCPRUNTIME_API`-exported) then layers the editor families + UI on the SAME registry; world
  resolution goes through `FUnrealMcpWorldProvider` (the editor installs a `GEditor`-reading resolver, the
  runtime subsystem a `GameInstance`-reading one), so the runtime module holds no GEditor reference.
- **C++ style:** Unreal — **tabs**, braces on new lines, UE types (`FString`, `TArray`, `TSharedPtr`).
  Log via the dedicated `LogUnrealMcp` category. File header: the
  `// Copyright (c) 2026 Ivan Murzak ...` one-liner (copy from a neighbour).
- **Unity-build ODR rule (load-bearing):** the `UnrealMcpRuntime` / `UnrealMcpEditor` /
  `UnrealMcpEditorTests` modules are unity-built — every `.cpp` is concatenated into one translation unit
  — so an `anonymous namespace` does **not** make a helper file-private. Give every per-family local helper
  a **family-unique** name (e.g. `LevelMakeStringArraySchema`, not `MakeStringArraySchema`) and every
  per-spec `Run`/helper a **spec-unique** name (or make it a `BEGIN_DEFINE_SPEC` member). A
  same-name/same-signature collision across families fails the build — sometimes with a misleading cascade
  error at the call site (e.g. `C2661: 'FAutomationTestBase::TestFalse': no overloaded function takes 1
  arguments`) rather than a redefinition error (`C2084`).
- **C# style:** every `.cs` starts with the ASCII-art Apache-2.0 header (copy from a neighbour).
- **Game-thread / no-modal-UI tool-handler contract:** all Unreal API calls from tool/prompt/resource
  handlers run on the game thread via the dispatcher (§4); the IPC reader thread never executes handler
  bodies. **A handler must not pump modal UI and must not synchronously wait on bridge state** — doing
  either can wedge the host (ARCHITECTURE §11 risk 2). Tool handlers are **synchronous**
  (`FUnrealMcpToolHandler = TFunction<FUnrealMcpToolResult(const FUnrealMcpToolCall&)>`): long-running
  work (`source-compile`, `blueprint-compile`) runs inline on the game thread and blocks it for the
  duration (ARCHITECTURE §4 envisions an async-chaining `TFuture` handler surface once the registry
  grows one; until then a compile blocks). Only the dispatcher's `Dispatch()` returns a `TFuture` to
  the bridge, and the per-call timeout always completes it.
- **Disabled tools are gated at `Execute()`**, not merely excluded from the manifest — a disabled tool
  is rejected even if a stale `tools/list` dispatches it (`UnrealMcpToolRegistry::Execute`). A tool is
  served **iff** it passes the `enabledTools` whitelist (empty = no filter; `UNREAL_MCP_TOOLS` overrides)
  **and** is not in the `disabledTools` blocklist (the per-tool UI toggles); both sets are retained across
  sessions and survive an extension hot-reload (a re-registered tool inherits the retained toggle).
- **Secrets:** `.env` is gitignored and must stay that way (it can hold `UNREAL_MCP_TOKEN`); the sidecar
  IPC token travels via stdin, never argv, and is never logged.
- **Commits:** `<type>(<scope>): <description>` conventional commits (scopes: plugin, bridge,
  cli, ipc, tools, dispatcher, schema, ui, sidecar, config, runtime, samples, ci, docs). Reference issues
  with `Closes #N`. Never `git add -A`. Never commit `Binaries/`/`Intermediate/`/`Saved/`/`bin/`/`obj/`/
  `node_modules/`/`dist/` or the `Sidecar/<rid>/` binary payloads. The `.sln` at a `.uproject` root is
  generated — never commit it; the hand-authored `bridge/Unreal-MCP-Bridge.sln` is the un-ignored
  exception.

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
