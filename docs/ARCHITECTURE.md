# Unreal-MCP — Architecture Design

> Authoritative architecture design for the `IvanMurzak/Unreal-MCP` mono-repo.
>
> **Locked decisions (Ivan, 2026-06-10 — not relitigated here):** C++ UE editor plugin + auto-managed
> .NET sidecar reusing MCP-Plugin-dotnet + ReflectorNet (SignalR to ai-game.dev / custom server,
> device-code + bearer auth). UE 5.7 development target, declared floor 5.5+. CI on a self-hosted
> Windows runner with UE 5.7. Public mono-repo, Apache-2.0, Godot-MCP-style layout. Unity-MCP feature
> parity PLUS Unreal-unique tools (Blueprints first-class). Main Slate window + 4 aux windows mandatory.

---

## Implementation status (M10 — shipped)

This document is the authoritative **design**; the M10 feature set has now shipped and this block
reconciles the design with the implementation. Each numbered section is marked with the PR(s) that
delivered it. Identifiers below (tool ids, command names, env vars, paths) were verified against
`main` source; the README carries the user-facing, source-generated tool list.

| Section | Status | Shipped by |
|---|---|---|
| §1 IPC bridge protocol | implemented | #4 (sidecar bridge e2e + live `ping`) |
| §2 Dynamic tool registration | implemented | #4; the §2.3 `ProxyTool` lives in `bridge/` (see correction below) |
| §3 Schema generation | implemented | exercised by every tool family (#13–#25) |
| §4 GameThread dispatcher | implemented | #4 (used by all 8 families) |
| §5 Extensions mechanism | implemented | #7 (`IUnrealMcpToolProvider`, `samples/UnrealAITemplate`) |
| §6 Sidecar lifecycle | implemented | #4 (spawn / crash auto-restart / orphan-prevention / stdin-token) + #46 (BUNDLE-model resolution: env override → bundled `Binaries/ThirdParty/UnrealMcpBridge/<rid>/`, zero-click auto-spawn; the superseded download-on-first-run flow is dropped — see drift below) |
| §7 Slate UI | implemented | #24 (AI Game Developer main window) + #29 (MCP Tools/Prompts/Resources/Settings aux windows) |
| §8 Config & env | implemented | #8 (`UNREAL_MCP_*`, `.env`, on-disk config) |
| §9.1 cli (`unreal-mcp-cli`, 16 commands) | implemented | #3 |
| §9.2 Versioning | implemented | `commands/bump-version.ps1` single-sources `VersionName` across plugin/bridge/server/cli (the "≥ 6.8.0 w/ ProxyTool" pin is forward-looking — see §2.3 drift below) |
| §9.3 Test strategy | implemented | bridge xUnit + cli vitest + plugin Automation specs (#13–#25), CI wiring #28 |
| §9.4 CI (`test_pull_request` / `release` / `test_cli`) | implemented | #28 |
| §10 ping family (1) | implemented | #4 |
| §10 actor & component family (13) | implemented | #14 |
| §10 asset family (11) | implemented | #15 (issue #10) |
| §10 blueprint family (11) | implemented | #13 |
| §10 source family (6) | implemented | #23 |
| §10 screenshot family (4) | implemented | #21 |
| §10 editor/reflection family (9) | implemented | #22 |
| §10 level family (7) | implemented | #25 |

**Total shipped: 62 core tools across 8 families** (counts verified from the registration source).
Prompts/Resources ship empty-but-wired (§10), as designed.

### Drift corrected against the implementation

- **§2.3 ProxyTool — shipped in `bridge/`, not via an upstream 6.8.0 bump.** The design proposed an
  additive MCP-Plugin-dotnet PR (`com.IvanMurzak.McpPlugin` 6.8.0). In practice the documented
  fallback was taken: `ProxyTool` + `ProxyToolFactory` live entirely under `bridge/src/Tools/`
  (`IRunTool` + `ToolManager.AddTool` are already public), so the bridge/server pin **stays frozen at
  `com.IvanMurzak.McpPlugin[.Server]` 6.7.0 / `com.IvanMurzak.ReflectorNet` 5.3.1** (no
  `UseLocalMcpPlugin` switch is wired today). A future upstream 6.8.0 ProxyTool can replace the local
  copy through its own pipeline; until then, §2.3/§9.2's "≥ 6.8.0" is forward-looking, not the
  shipped state.
- **§8 effective-served semantics.** The served tool set is computed as: served **iff** the tool
  passes the `enabledTools` **whitelist** (empty whitelist = no filter; `UNREAL_MCP_TOOLS` overrides
  it) **AND** is not in the `disabledTools` **blocklist** (the per-tool UI toggles). Both sets are
  retained members, so a registration or an extension hot-reload re-applies them — a rebuilt tool
  inherits the retained toggle and cannot silently re-enable. See
  `FUnrealMcpToolRegistry::ShouldToolBeEnabled`.
- **Execute()-level enable gate (#29).** A disabled tool is excluded from the served manifest **and**
  rejected at the execution boundary (`FUnrealMcpToolRegistry::Execute` returns
  `"Tool '<name>' is disabled."`), so a stale `tools/list` that still names it can never run it.
- **Image-content channel (#21).** The screenshot family returns a base64 **PNG as MCP image
  content** (`content[]` with an `image` entry), as anticipated by §1.2's "binary payloads travel as
  base64 strings inside JSON". Dimensions clamp to default 1024 / hard cap 2048 per side; capture
  needs a GPU-backed editor (headless `-nullrhi` returns a structured error).
- **LogCollector (#22).** `console-get-logs` / `console-clear-logs` are backed by a module-startup
  `FUnrealMcpLogCollector` — a `GLog` `FOutputDevice` ring buffer (the Godot `GodotLogCollector`
  pattern), as §10 describes.
- **Device-code auth (#24).** The cloud OAuth device-code flow shipped in the main window (Authorize
  → `auth-start` → `device-auth` events render the verification URL + user code; Cancel/Revoke wired),
  per §1.3 / §7 item 4.
- **§6 sidecar lifecycle — BUNDLE model shipped (#46).** The plugin **spawns** the sidecar, hands it
  the one-shot IPC token over **stdin** (§1.4), **crash auto-restarts** it (the Connection section
  reports `Running (restarts: N)` / `Stopped`), and prevents orphans (the sidecar self-exits when its
  parent editor vanishes). Binary resolution now follows the §6.3 order: `UNREAL_MCP_BRIDGE_PATH`
  (dev/CI override) → the **bundled** self-contained binary inside the plugin under
  `Binaries/ThirdParty/UnrealMcpBridge/<rid>/` (`ResolveBridgeBinaryPath` / `ResolveRid`), with
  macOS/Linux pre-spawn `+x` and macOS `com.apple.quarantine` strip (`PrepareBundledBinaryForSpawn`).
  A packaged plugin therefore auto-spawns at StartupModule with zero user action; a dev source
  checkout (no staged binary) still uses the env override and emits the rid-aware actionable warning.
  The earlier **download-on-first-run + version-skew re-download** design is **superseded and removed**
  (no `.lock`, no atomic-rename, no re-download recovery) — version lockstep is by construction
  (bundle built in the same release job at the same version). The bundled binaries are staged by the
  release job (`RuntimeDependencies`, `UnrealMcpEditor.Build.cs`) and are **never committed to git**;
  CI signing/notarization of those binaries is owned by the T3/T4 release tasks.
- **§7 UI — shipped with partial coverage.** The §7 Slate UI shipped (#24/#29), but two §7 design
  affordances did **not** make this release: the **toolbar button** (§7's tab-registration paragraph,
  "under Window → AI Game Developer plus a toolbar button") — the main window is opened from its nomad
  tab only, registered under the editor's **Tools** menu category
  (`WorkspaceMenu::GetMenuStructure().GetToolsCategory()`), not the **Window** menu; and the
  **connection timeline** (Unreal → MCP server → AI agent — see §7's Connection section design) — the
  Connection section ships a single status dot / label / button instead. The bridge status string is `Running (restarts: N)` /
  `Stopped` (no PID/version), the AI agents section lists connected agents only (no config writing).
  The status table marks §7 "implemented" for the window + 4 aux windows; the deferred affordances
  above remain deferred.
- **§7 in-UI local-server Start — LANDED (issue #95, supersedes the prior "no in-UI start-local-server
  control" decision).** Operator decision 2026-06-15: the MCP-server card's **Start/Stop** button now
  launches and supervises the LOCAL shared `gamedev-mcp-server` directly from the editor — replacing
  the interim #94 wiring that (re)started the bridge sidecar. A new plugin-side `FUnrealMcpServerManager`
  (`Source/UnrealMcpEditor/Private/Server/UnrealMcpServerManager.{h,cpp}`) owns its lifecycle, the
  plugin-side C++ analog of Unity's in-process `McpServerManager.cs` (NOT in the bridge): binary
  resolution (`UNREAL_MCP_SERVER_PATH` override → cached binary → download), spawn via
  `FPlatformProcess::CreateProc`, startup verification, crash-restart with backoff, orphan-port kill,
  PID persistence + reattach across module reloads, and graceful-then-force stop on editor shutdown so
  no `gamedev-mcp-server` orphans on editor close. **Gated to Custom mode + http transport** (Start is
  hidden/disabled, and a click is a no-op, in Cloud or Custom+stdio). The binary cache shares the CLI's
  §6 layout `{Project}/Intermediate/UnrealMCP/server/<rid>/`; the consumed server version is pinned in
  `FUnrealMcpServerManager::ServerVersion`, kept in lockstep with the CLI's
  `cli/src/lib/server-version.ts` `SERVER_VERSION`. This is distinct from the always-on
  `unreal-mcp-bridge` sidecar (§6) — two different child processes, two different managers.
- **§9.1 `.uplugin` `EngineVersion`.** The §9.1 tree comment reads "EngineVersion floor 5.5.0", but
  the shipped `UnrealMCP.uplugin` carries **no `EngineVersion` field at all** — UE treats it as an
  exact-build match (not a floor) and would refuse to load on newer engines. The 5.5+ floor is a
  documented/CI claim, not a descriptor pin. (Corrected inline in §9.1.)
- **On-disk config path.** Confirmed shipped at
  `<Project>/Saved/Config/UnrealMcp/ai-game-developer-config.json` (§8), matching the design.

---

## 0. System overview

Unity and Godot embed the .NET `McpPlugin` host **in-process** (C# engine). Unreal's editor is C++,
so the McpPlugin host moves into a separate **sidecar process** owned by the plugin. The sidecar is
the SignalR "plugin" peer the MCP server already knows how to talk to — the server cannot tell an
Unreal sidecar from a Unity editor.

```
┌─────────────────────────  Unreal Editor (C++)  ─────────────────────────┐
│  UnrealMCP plugin (Editor module)                                        │
│  ┌─ FUnrealMcpToolRegistry ── core tool families + extension providers   │
│  ├─ FUnrealMcpSchemaGenerator ── FProperty → JSON Schema                 │
│  ├─ FUnrealMcpGameThreadDispatcher ── AsyncTask + TPromise               │
│  ├─ FUnrealMcpBridgeServer ── localhost TCP listener, NDJSON framing     │
│  ├─ FUnrealMcpSidecarManager ── resolve(bundled) / spawn / watchdog / kill│
│  └─ Slate UI: main window + 4 aux tabs, ISettingsModule section          │
└───────────────▲──────────────────────────────────────────────────────────┘
                │ IPC: 127.0.0.1 TCP, newline-delimited JSON, token-authed
┌───────────────▼──────────  unreal-mcp-bridge (.NET 9, self-contained) ───┐
│  McpPlugin host (NuGet com.IvanMurzak.McpPlugin + ReflectorNet)           │
│  ┌─ IpcClient ── dials the plugin, reconnect w/ backoff                  │
│  ├─ ProxyToolFactory ── manifest → IRunTool proxies → IToolManager       │
│  └─ McpPlugin SignalR client ── Cloud (ai-game.dev) / Custom server      │
└───────────────▲──────────────────────────────────────────────────────────┘
                │ SignalR /hub/mcp-server (+ bearer / device-code auth)
        MCP server (ai-game.dev, or local gamedev-mcp-server — the shared host)
                ▲  MCP (stdio / streamable HTTP)
        AI client (Claude Code, Cursor, the AI-Game-Dev app, …)
```

**Naming conventions** (used throughout): plugin name `UnrealMCP`, module `UnrealMcpEditor`,
C++ prefixes `FUnrealMcp*` / `UUnrealMcp*` / `SUnrealMcp*` (Slate) / `IUnrealMcp*` (interfaces).
Sidecar assembly/namespace `com.IvanMurzak.Unreal.MCP.Bridge`, binary `unreal-mcp-bridge`.
Local server: the **shared, engine-agnostic** `gamedev-mcp-server` (released from
[IvanMurzak/GameDev-MCP-Server](https://github.com/IvanMurzak/GameDev-MCP-Server) and consumed by
Unity-MCP / Godot-MCP / Unreal-MCP alike — no server source lives in this repo). npm package
`unreal-mcp-cli`. Tool ids are kebab-case (`actor-create`, `blueprint-compile`) matching Unity/Godot.

Two distinct child processes must not be conflated:

| Process | Role | Managed by | Mode |
|---|---|---|---|
| `unreal-mcp-bridge` (sidecar) | McpPlugin host; IPC ⇄ SignalR relay | C++ plugin (`FUnrealMcpSidecarManager`) | always required |
| `gamedev-mcp-server` | the shared MCP server (one host for Unity/Godot/Unreal) | the CLI downloads it (§6, `cli/src/lib/download-server.ts`); the MCP client launches it via the `setup-mcp` stdio config, or the user runs it directly | Custom/local mode only; Cloud mode talks to ai-game.dev |

---

## 1. IPC bridge protocol (plugin ⇄ sidecar)

### 1.1 Transport — local TCP, plugin listens

**Decision: TCP on `127.0.0.1`, deterministic per-project port; the C++ plugin LISTENS, the sidecar
DIALS.** Rejected: named pipes — Windows named pipes vs Unix domain sockets need two divergent
implementations on both sides, UE has no unified pipe-server API, and .NET `NamedPipeClientStream`
on Unix is a socket anyway. UE ships `FTcpListener`/`FSocket` (Networking/Sockets modules) and .NET
`TcpClient` makes the TCP path one implementation for Win/macOS/Linux. Loopback binding is the
first security boundary; the token (§1.4) is the second.

Port selection (mirrors Unity's deterministic hashing, `transport.md`: SHA256 of project path →
20000–29999, but in a **disjoint band** so Unity+Unreal coexist on one machine):

- `IpcPort = 30000 + (SHA256("unreal-mcp-ipc:" + AbsoluteNormalizedProjectPath) % 10000)` → 30000–39999.
- If bind fails (port taken), probe `IpcPort+1 … IpcPort+9`, then ephemeral (`0`) as last resort.
- The **actual** bound port is always passed to the sidecar via launch args, so determinism is a
  debugging nicety, never a correctness requirement. Multi-editor-instance coexistence falls out:
  different project paths hash differently; two editors on the same project resolve via probing.
- The local `gamedev-mcp-server` port (Custom mode) keeps Unity's scheme: `20000 + (SHA256(projectPath) % 10000)`.

Plugin-listens (not sidecar-listens) because the plugin is the stable parent: a crashed sidecar is
respawned and simply re-dials; the plugin never has to discover a child's ephemeral port; and an
orphaned sidecar can never squat the project's port across editor restarts.

### 1.2 Framing — newline-delimited JSON (NDJSON)

**Decision: UTF-8 JSON, one message per `\n`-terminated line.** Rejected: length-prefixed binary —
better for huge payloads but loses `nc`/log debuggability and buys nothing at our message sizes.
Rules: no raw newlines inside a message (standard JSON string escaping guarantees this); max line
length 64 MiB (reader aborts the connection on violation — protects both sides); binary payloads
(screenshots) travel as base64 strings inside JSON, exactly as they already do over SignalR.
`ipcVersion` in the handshake (§1.3) lets a future version negotiate length-prefixed framing
without breaking old pairs.

UE side: `FRunnable` reader thread accumulates `FSocket::Recv` into a byte buffer, splits on `\n`,
parses with `FJsonSerializer`/`TJsonReader`. .NET side: `StreamReader.ReadLineAsync` +
`System.Text.Json`. Writes on each side go through a single mutex-guarded writer (one full line per
send) so messages never interleave. Heartbeat-vs-jumbo-frame policy: a near-cap frame (huge
screenshot) can hold the single writer past the 15 s heartbeat budget — we accept the resulting
false-dead + reconnect (§1.5 recovers cleanly) rather than adding a priority writer or per-type
queues in MVP.

### 1.3 Message schema

Envelope: every message is a JSON object with a `type` field. `requestId` correlates calls.

| Direction | `type` | Purpose |
|---|---|---|
| sidecar → plugin | `handshake` | first message after connect: `ipcVersion`, `sidecarVersion`, `token` |
| plugin → sidecar | `handshake-ack` | `ipcVersion`, `pluginVersion`, `engineVersion`, `projectPath`, effective connection config (§8) |
| plugin → sidecar | `tool-manifest` | full snapshot: `revision` (monotonic int) + array of tool descriptors (§2.2) |
| sidecar → plugin | `tool-call` | `requestId`, `tool`, `arguments` (raw JSON object), `timeoutMs` |
| plugin → sidecar | `tool-response` | `requestId`, `status: "success"\|"error"`, `content` (MCP content array), `structured` (object) |
| sidecar → plugin | `tool-cancel` | `requestId` — cooperative cancellation (§4) |
| plugin → sidecar | `config` | UI/config changed: connect/disconnect, mode, host, token, enabled-tools map |
| sidecar → plugin | `status` | SignalR state for the UI: `connectionState`, `keepConnected`, `cloudAuthState`, `serverProcessState`, `aiAgents[]` |
| plugin → sidecar | `auth-start` | begin the device-code flow (UI Authorize button) |
| plugin → sidecar | `auth-cancel` | abort an in-progress device-code flow |
| plugin → sidecar | `auth-revoke` | revoke + clear the stored cloud token |
| sidecar → plugin | `device-auth` | device-code flow progress: `verificationUrl`, `userCode`, `state` |
| plugin ⇄ sidecar | `ping` / `pong` | heartbeat, every 5 s; peer declared dead after 15 s silence |
| plugin → sidecar | `shutdown` | graceful exit request (editor quitting) |
| either | `log` | structured log forwarding (`level`, `message`) for unified output |

Example request/response pair (the contract the sidecar-bridge task implements first):

```json
{"type":"tool-call","requestId":"a1b2c3d4","tool":"actor-create","timeoutMs":30000,
 "arguments":{"classPath":"/Script/Engine.PointLight","name":"KeyLight",
              "location":{"x":0,"y":0,"z":300}}}

{"type":"tool-response","requestId":"a1b2c3d4","status":"success",
 "content":[{"type":"text","text":"Spawned PointLight 'KeyLight' at (0, 0, 300)."}],
 "structured":{"actorPath":"/Temp/Untitled_1.Untitled_1:PersistentLevel.KeyLight",
               "actorLabel":"KeyLight","class":"/Script/Engine.PointLight"}}
```

`tool-response.status/content/structured` deliberately mirrors `ResponseCallTool`
(McpPlugin.Common) so the sidecar maps IPC → SignalR with zero re-shaping — minus
`ResponseStatus.Processing`: the IPC status enum is terminal-only because every tool, including
async ones, completes via exactly one terminal `tool-response` (the dispatcher chains `TFuture`s
instead of emitting interim messages, §4); MCP-Plugin-dotnet's deferred Processing path
(`RequestTrackingService`) is intentionally unused over IPC.

### 1.4 Local auth

The plugin generates a 32-byte cryptographically random token per sidecar launch using the
**platform CSPRNG** (BCryptGenRandom / `/dev/urandom`; no FGuid-derived construction — FGuid is
not specified to be cryptographically random). The token is delivered over the child's **stdin
pipe**, never on the command line: `FPlatformProcess::CreateProc` is given a `PipeReadChild`
handle as the child's stdin, the plugin writes one base64url line into it, and the sidecar reads
exactly one line from stdin before dialing. Launch arguments carry only the non-secrets:
`unreal-mcp-bridge --ipc-port=31234 --parent-pid=<editor pid>`. argv is the wrong place for a
secret — `/proc/<pid>/cmdline` / `ps` is world-readable on Linux/macOS, and Windows
process-creation auditing (Event 4688, Sysmon, EDR) plus WER crash dumps persist command lines
to disk logs; stdin is captured by none of these. First message must be a `handshake` carrying
the exact token; anything else (or 5 s of silence) → socket closed. Token rotates every relaunch
and is never written to disk or logs. (Loopback bind is the primary defense; the token blocks
other local users on shared machines.) Sidecar-side, handshake rejection is **fatal after 3
consecutive rejections** — the sidecar exits instead of re-dialing forever (§6, orphan layer 3).

### 1.5 Lifecycle state machine (plugin-side, `FUnrealMcpSidecarManager`)

```
Stopped → LaunchingSidecar → WaitingHandshake → Ready ⇄ Degraded → Stopping → Stopped
```

- `LaunchingSidecar`: binary present + version match (§6) → `FPlatformProcess::CreateProc`.
- `WaitingHandshake`: accept + handshake within 10 s, else kill + retry (counts as a crash).
- `Ready`: heartbeats flowing; tool calls accepted.
- `Degraded`: socket lost but process alive (sidecar re-dials; plugin keeps listening), or
  process dead → auto-restart with backoff 1 s, 2 s, 5 s, 10 s, 30 s (cap); >5 crashes in 5 min →
  `Stopped` + error surfaced in the main window.
- `Stopping`: send `shutdown`, wait 3 s, then `FPlatformProcess::TerminateProc`. Wired to editor
  shutdown (`FCoreDelegates::OnEnginePreExit` + module `ShutdownModule`).
- On `Ready` the plugin immediately pushes `tool-manifest` + `config`; after any reconnect it
  re-pushes both. State-reset is concrete, not hand-wavy: **every `handshake-ack` resets the
  sidecar's `lastApplied` manifest revision to −1**, so the re-pushed manifest is always applied
  even when its `revision` is unchanged (the §2.2 rule-3 guard only filters races *within* one
  connection). On IPC disconnect the sidecar **immediately fails ALL pending proxy calls** with
  the structured "Unreal editor bridge disconnected" error — it never lets them hang to
  `timeoutMs`. Both sides silently drop `tool-response`s for unknown or already-completed
  `requestId`s (covers late responses straddling a reconnect).

---

## 2. Dynamic tool registration (sidecar + MCP-Plugin-dotnet)

### 2.1 Verified foundation

Spot-verified in MCP-Plugin-dotnet source (feasibility probe confirmed accurate):

- `IRunTool` is schema-blind: `InputSchema`/`OutputSchema` are plain `JsonNode?` properties, and
  `Run(string requestId, IReadOnlyDictionary<string, JsonElement>?, CancellationToken)` receives
  raw JSON — no ReflectorNet/MethodInfo binding required (`IRunTool.cs:20–86`).
- Runtime mutation after `Build()`: `IMcpPlugin.McpManager.ToolManager` (`IMcpPlugin.cs:20`,
  `IMcpManager.cs:36`) exposes `AddTool(name, IRunTool)` / `RemoveTool(name)` /
  `SetToolEnabled(name, bool)`, each firing `_onToolsUpdated` (`McpToolManager.cs:73–121`) which
  cascades to the server's tools-list-changed notification.
- `AddTool` **skips** (returns false, warns) when the name exists (`McpToolManager.cs:75–79`) —
  so a *changed* tool must be `RemoveTool` → `AddTool`.
- `TokenCount` is `ceil(len(json(name,title,description,inputSchema,outputSchema))/4)`
  (`RunTool.TokenCount.cs:52–90`) — trivially reproducible for proxy tools.

### 2.2 Manifest → registration flow

Tool descriptor (one entry in `tool-manifest.tools[]`), shaped to fill `IRunTool` 1:1:

```json
{"name":"actor-create","title":"Create Actor","description":"Spawn an actor …",
 "skillDescription":"…","skillBody":null,
 "inputSchema":{"type":"object","properties":{…},"required":["classPath"]},
 "outputSchema":{"type":"object","properties":{…}},
 "readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false,
 "enabled":true,"extensionId":"core","schemaHash":"sha256:…"}
```

Sidecar flow on each `tool-manifest` (revision N):

1. Diff against the previously applied manifest by `name` + `schemaHash`
   (hash = SHA256 of the canonicalized descriptor minus `enabled`):
   removed → `ToolManager.RemoveTool`; added → `ToolManager.AddTool`; changed → `RemoveTool`+`AddTool`;
   enabled-flag-only change → `SetToolEnabled`.
2. Each added tool is a `ProxyTool : IRunTool` whose `Run` serializes a `tool-call` IPC message,
   awaits the matching `tool-response` (or forwards `CancellationToken` as `tool-cancel`), and maps
   it to `ResponseCallTool`.
3. Out-of-order revisions are ignored if `revision <= lastApplied` — but this guard only filters
   races *within* one connection: **every `handshake-ack` resets the sidecar's `lastApplied`
   manifest revision to −1** (§1.5), so a post-reconnect re-push with an unchanged revision is
   always applied, never discarded.
4. IPC down ⇒ every proxy tool's `Run` fails fast with a structured "Unreal editor bridge
   disconnected" error (no hang). At the moment of disconnect, all already-pending proxy calls
   are failed immediately with that same error (never left to `timeoutMs`); `tool-response`s for
   unknown or already-completed `requestId`s are silently dropped on both sides.

Hot reload: the plugin bumps `revision` and re-sends the full manifest whenever the registry
changes (extension plugin loaded/unloaded §5, per-tool enable toggled in UI §7). Full-snapshot +
sidecar-side diffing keeps the C++ side stateless about what the sidecar saw.

### 2.3 Exact additive API PR for MCP-Plugin-dotnet

One small PR (additive only — no core rework; per `MCP-Plugin-dotnet/CLAUDE.md`, public-API
changes cascade, so notify lead + release as a minor NuGet bump, e.g. 6.8.0):

- `McpPlugin/src/Mcp/Tool/ProxyTool.cs` — `public sealed class ProxyTool : IRunTool`: ctor takes
  `(string name, string? title, string? description, string? skillDescription, string? skillBody,
  JsonNode? inputSchema, JsonNode? outputSchema, bool? readOnlyHint, bool? destructiveHint,
  bool? idempotentHint, bool? openWorldHint,
  Func<string, IReadOnlyDictionary<string, JsonElement>?, CancellationToken, Task<ResponseCallTool>> handler)`;
  `Enabled` is a settable bool (default true); `TokenCount` replicates the chars/4 formula
  (extract `RunTool.CalculateTokenCount` core into a shared internal static helper).
- `McpPlugin/src/Mcp/Tool/IDynamicToolFactory.cs` + `ProxyToolFactory` — DI-friendly
  `CreateProxyTool(...)` returning `IRunTool` (matches the probe's proposal).
- `McpPluginBuilder.WithDynamicToolFactory()` — registers the factory as a singleton.
- Tests: xUnit copy of the existing `MockRunTool` pattern (`McpBuilderTests.cs:156–173`) proving
  add → list_changed → call → remove against a built plugin.

Until the NuGet release lands, `bridge/` builds with a `UseLocalMcpPlugin=true` project switch
(same pattern AI-Game-Dev-Server uses) referencing the local MCP-Plugin-dotnet checkout.
Fallback if the PR were rejected (it won't be — same owner): `ProxyTool` can live entirely in
`bridge/` since `IRunTool` + `ToolManager.AddTool` are already public; the PR is for reuse, not
necessity. **Verdict: minor additive API (b), confirmed.**

---

## 3. Tool schema generation from UE reflection

### 3.1 Two declaration surfaces

1. **Declarative builder (primary).** Core tools declare schemas explicitly via
   `FUnrealMcpToolRegistry` — explicit beats inferred for LLM-facing descriptions, and most tool
   inputs are not UObject-shaped anyway. Typed params route through the §3.2 mapping.
2. **Reflection-derived (for `reflection-method-*` + struct params).** `FUnrealMcpSchemaGenerator`
   walks `UFunction`/`FProperty` to build schemas at runtime — powering `reflection-method-find`
   (returns callable-method schemas) and `Param<FMyStruct>(…)` in the builder (recursing
   `UScriptStruct::PropertyLink`).

### 3.2 FProperty → JSON Schema mapping (authoritative table)

| UE type (FProperty class) | JSON Schema | Wire convention |
|---|---|---|
| `FBoolProperty` | `boolean` | |
| `FIntProperty`/`FInt64Property`/`FUInt32Property`/`FByteProperty` (no enum) | `integer` | |
| `FFloatProperty`/`FDoubleProperty` | `number` | |
| `FStrProperty`/`FNameProperty`/`FTextProperty` | `string` | FText flattened to string (localization out of scope) |
| `FEnumProperty` / `FByteProperty`+UEnum | `string` + `enum:[…]` | short names (`"Visible"`, not `EVisibility::Visible`) |
| `FVector` | `object {x,y,z: number}` | matches Unity-MCP's Vector3 JSON |
| `FVector2D` | `object {x,y}` | |
| `FRotator` | `object {pitch,yaw,roll: number}` | degrees |
| `FQuat` | `object {x,y,z,w}` | builder prefers FRotator in tool params |
| `FTransform` | `object {location: FVector, rotation: FRotator, scale: FVector}` | |
| `FLinearColor`/`FColor` | `object {r,g,b,a: number}` | 0–1 floats |
| `FGuid` | `string` | standard GUID text |
| `FStructProperty` (other) | `object` | recurse `PropertyLink`; cycle-guard via visited-set, depth cap 8 |
| `FArrayProperty` | `array` + `items` | |
| `FSetProperty` | `array` + `uniqueItems:true` | |
| `FMapProperty` (string-keyed) | `object` + `additionalProperties` | non-string keys → array of `{key,value}` pairs |
| `FObjectProperty`/`TObjectPtr`/`FSoftObjectPath`/`FClassProperty` | `string` | **path-or-name string ref**: assets/classes = object path (`/Game/Maps/Arena.Arena`, `/Script/Engine.PointLight`); actors = actor label or full path (`PersistentLevel.KeyLight`); resolved by `FUnrealMcpObjectRef::Resolve` (analog of Unity's `ObjectRef`, label → `FindObject` → soft-path load) |
| `FDelegateProperty`/`FInterfaceProperty`/`FFieldPathProperty` | skipped | excluded from schemas and from `actor-modify` writes |

Property serialization (tool *results*) reuses the same table in reverse via a
`FUnrealMcpJsonConverter` built on `FJsonObjectConverter` plus the custom object-ref and
math-type rules above, with Unity-style **scoped reads** (`paths`/`viewQuery` token-saving
parameters on `*-get-data` tools) implemented as post-serialization JSON-path filtering.

### 3.3 Declaration pattern + one full sample

**Decision: static self-registration into `FUnrealMcpToolRegistry`** (a plugin-owned singleton
created by the module; extensions reach it via `IModularFeatures`, §5). Each core family is one
`FUnrealMcp<Family>Tools` class with a static `Register`. No macros-over-UFUNCTION magic for MVP —
the builder is plain C++, debuggable, and schema-explicit.

```cpp
// UnrealMCP/Source/UnrealMcpEditor/Private/Tools/UnrealMcpActorTools.cpp
void FUnrealMcpActorTools::Register(FUnrealMcpToolRegistry& Registry)
{
    Registry.Tool(TEXT("actor-create"))
        .Title(TEXT("Create Actor"))
        .Description(TEXT("Spawn a new actor in the currently loaded level from a class path "
                          "(e.g. '/Script/Engine.StaticMeshActor' or a Blueprint asset path). "
                          "Optionally set name/label, location, rotation and parent attachment."))
        .Param<FString>(TEXT("classPath"), TEXT("Native class or Blueprint asset path."), EUnrealMcpParam::Required)
        .Param<FString>(TEXT("name"),      TEXT("Actor label. Auto-generated when omitted."))
        .Param<FVector>(TEXT("location"),  TEXT("World location. Defaults to origin."))
        .Param<FRotator>(TEXT("rotation"), TEXT("World rotation in degrees."))
        .Param<FString>(TEXT("parentActor"), TEXT("Label/path of an actor to attach to."))
        .Returns<FUnrealMcpActorRefResult>()          // struct → outputSchema via §3.2
        .DestructiveHint(false).ReadOnlyHint(false)
        .Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
        {
            // Already ON the game thread (dispatcher, §4). Sync body; async tools return TFuture.
            const FString ClassPath = Call.GetString(TEXT("classPath"));
            UClass* Class = FUnrealMcpObjectRef::ResolveClass(ClassPath);
            if (!Class || !Class->IsChildOf(AActor::StaticClass()))
                return FUnrealMcpToolResult::Error(
                    FString::Printf(TEXT("'%s' is not a spawnable Actor class."), *ClassPath));

            FTransform Xform(Call.GetRotator(TEXT("rotation"), FRotator::ZeroRotator),
                             Call.GetVector(TEXT("location"), FVector::ZeroVector));
            AActor* Actor = GEditor->GetEditorSubsystem<UEditorActorSubsystem>()
                ->SpawnActorFromClass(Class, Xform.GetLocation(), Xform.Rotator());
            if (!Actor)
                return FUnrealMcpToolResult::Error(TEXT("SpawnActorFromClass failed."));
            if (Call.Has(TEXT("name")))
                Actor->SetActorLabel(Call.GetString(TEXT("name")));

            return FUnrealMcpToolResult::Success(
                FString::Printf(TEXT("Spawned %s '%s'."), *Class->GetName(), *Actor->GetActorLabel()),
                FUnrealMcpActorRefResult::From(Actor)); // → structured + content text
        });
}
```

The registry compiles each declaration into the §2.2 descriptor once (schema + hash cached) and
bumps the manifest revision.

---

## 4. GameThread dispatcher

`FUnrealMcpGameThreadDispatcher` — the analog of Unity's `MainThread.Instance.Run()` /
Godot's `GodotMainThread`:

- **Entry:** `TFuture<FUnrealMcpToolResult> Dispatch(FUnrealMcpToolCall Call, TFunction<…> Body, FTimespan Timeout)`.
  Creates a `TPromise`, posts via `AsyncTask(ENamedThreads::GameThread, …)`; if already on the game
  thread (UI-triggered debug invocations) runs inline.
- **The IPC reader thread never executes tool bodies** and never blocks on the future: the bridge
  server attaches a continuation (`.Next(…)`) that serializes `tool-response` back through the
  writer mutex. Slow tools therefore never stall heartbeats — `ping`/`pong` stays on the IPC thread.
- **Timeout:** per-call (`tool-call.timeoutMs`, default 30 s, sidecar passes through the MCP
  server's plugin-timeout). A parallel timer fires the promise with a structured timeout error;
  the flag set on `FUnrealMcpToolCall::CancelRequested` (a `FThreadSafeBool`) lets long tool bodies
  exit early at their own checkpoints. A game-thread task cannot be force-killed — an abandoned
  body finishes silently, logs at `Verbose`, and its late result is dropped (requestId already
  completed; double-set of the promise is guarded).
- **Cancellation:** `tool-cancel` sets the same flag — one cooperative mechanism for both paths.
- **Async tools** (e.g. `source-compile` waiting on UBT, `blueprint-compile` follow-ups) return a
  `TFuture` from the handler itself; the dispatcher chains rather than blocks, so the game thread
  is only occupied for the synchronous slice.
- **Editor-state guards:** dispatch checks `GIsSavingPackage`/cook-in-progress and queues (FIFO,
  drained by an `FTSTicker` lambda) instead of re-entering critical editor operations.
  *Footnote:* verify at implementation time whether `GIsSavingPackage` (deprecated in newer UE)
  or `UE::IsSavingPackage()` is the correct check across the 5.5 floor → 5.7 dev target.

---

## 5. Extensions mechanism (3rd-party UE plugins contribute tools)

**Decision: `IModularFeatures` with feature name `FName("UnrealMcpToolProvider")`.** Rejected:
static registry + module scan — it requires extensions to link against our module at static-init
time and breaks when load order shifts; `IModularFeatures` is UE's purpose-built, link-free,
load-order-independent registration bus.

```cpp
// Public/IUnrealMcpToolProvider.h  (the ONLY header extension authors need)
class IUnrealMcpToolProvider : public IModularFeature
{
public:
    static FName GetModularFeatureName() { return FName(TEXT("UnrealMcpToolProvider")); }
    virtual FString GetExtensionId() const = 0;     // stable, e.g. "com.foo.niagara-ai"
    virtual FText   GetDisplayName() const = 0;
    virtual FString GetExtensionVersion() const = 0;
    virtual void    RegisterTools(FUnrealMcpToolRegistry& Registry) = 0;
};
```

- **Discovery:** on plugin boot, `IModularFeatures::Get().GetModularFeatureImplementations<…>()`;
  plus subscriptions to `OnModularFeatureRegistered/Unregistered` so late-loaded or hot-unloaded
  plugins trigger a registry rebuild + manifest revision bump (the §2.2 diff handles the rest).
- **Deterministic ordering:** providers sorted by `ExtensionId` before registration; within one
  provider, declaration order. Duplicate tool name across providers → later registration rejected
  + per-extension error recorded (first-wins, deterministic by the sort).
- **Error isolation:** UE builds without C++ exceptions, so isolation is contract-level, not
  catch-level: each tool descriptor is validated (name pattern, schema well-formed, handler bound)
  and a provider whose `RegisterTools` produces invalid entries gets those entries dropped and the
  failure shown on its UI row — never blocking other extensions. Tool-body crashes are editor
  crashes regardless of family (same as any UE plugin code); the registry adds `ensure`-level
  guards around handler invocation results.
- **Per-extension enable/disable:** persisted in the §8 config (`disabledExtensions[]`,
  `disabledTools[]`); a disabled extension's tools are kept out of the manifest entirely.
- **UI:** extensions section in the main window (Unity analog `MainWindowEditor.Extensions.cs:175`)
  lists id, name, version, tool count, toggle, error badge.
- **Template story:** `samples/UnrealAITemplate/` in-repo — a minimal standalone UE plugin with one
  `hello-extension` tool, proving the contract end-to-end; doubles as the extension-author doc's
  working example and the isolation test fixture (it can be switched to emit an invalid schema).

---

## 6. Sidecar lifecycle

**Decision: BUNDLE the sidecar inside the plugin.** Prebuilt, **self-contained, single-file**
`unreal-mcp-bridge` binaries for all four RIDs ship inside the plugin and are resolved from disk at
spawn time — there is **no download-on-first-run**. Rationale: the owner requirement is zero .NET
install + zero manual steps for the end user; a bundle guarantees the binary is present and
version-correct the instant the compiled plugin loads, works fully offline/air-gapped, and welds
the sidecar to the exact plugin version it was tested against (no skew window). The earlier
download-on-first-run design (Unity's `DownloadServerBinaryIfNeeded` flow) is **superseded**: it is
retained only for the **shared `gamedev-mcp-server`** (a different process, owned by the CLI — see
"Local server acquisition"), never for the bridge.

### 6.1 Bundled layout (exact path strings)

```
UnrealMCP/                                  # plugin root (UnrealMCP.uplugin lives here)
└── Binaries/
    └── ThirdParty/
        └── UnrealMcpBridge/
            ├── win-x64/    unreal-mcp-bridge.exe   (+ self-contained payload files)
            ├── osx-arm64/  unreal-mcp-bridge       (+ payload)
            ├── osx-x64/    unreal-mcp-bridge       (+ payload)
            └── linux-x64/  unreal-mcp-bridge       (+ payload)
```

- Parent dir constant (C++): `Binaries/ThirdParty/UnrealMcpBridge/<rid>/`, resolved relative to the
  plugin's base dir via `IPluginManager::Get().FindPlugin(TEXT("UnrealMCP"))->GetBaseDir()` (the
  `Projects` module is already a dependency — `UnrealMcpEditor.Build.cs`).
- Binary basename: `unreal-mcp-bridge` on macOS/Linux, `unreal-mcp-bridge.exe` on Windows (matches
  `<AssemblyName>unreal-mcp-bridge</AssemblyName>` in the bridge csproj).
- `Binaries/ThirdParty/` is the UE-canonical home for prebuilt third-party runtime payloads and is
  what `RuntimeDependencies` + `-Rocket` BuildPlugin packaging expect.

### 6.2 UE platform → .NET RID mapping

| `UE` platform | RID dir | Notes |
|---|---|---|
| `Win64` (`PLATFORM_WINDOWS`) | `win-x64` | only x64 editor RID shipped |
| `Mac` (`PLATFORM_MAC`), Apple Silicon | `osx-arm64` | chosen at runtime — see below |
| `Mac` (`PLATFORM_MAC`), Intel | `osx-x64` | chosen at runtime — see below |
| `Linux` (`PLATFORM_LINUX`) | `linux-x64` | only x64 editor RID shipped |

**macOS arm64-vs-x64 selection at runtime.** The plugin must NOT assume the editor's own
architecture equals the host CPU (an Intel-built editor can run under Rosetta on Apple Silicon, and
vice-versa). The bridge is a separate child process, so it matches the **host CPU**, not the
editor's translated architecture. `FUnrealMcpSidecarManager::ResolveRid()` reads the **physical** CPU
via `sysctlbyname("hw.optional.arm64", …)` (returns 1 on Apple Silicon even under a translated
process) and prefers `osx-arm64/`; if that dir is absent (defensive), it falls back to `osx-x64/`
(runs under Rosetta 2). Intel host → `osx-x64/`. Native is preferred because Rosetta-translating the
.NET host is slower and has historically been a source of JIT edge cases.

`ResolveBridgeBinaryPath()` composes `<PluginBaseDir>/Binaries/ThirdParty/UnrealMcpBridge/<rid>/<basename>`
and returns it only if `FPaths::FileExists` is true; otherwise empty (caller logs the actionable
"plugin packaged without the <rid> bridge" error).

### 6.3 Resolution order (replaces the stub)

1. **Dev/CI override:** `UNREAL_MCP_BRIDGE_PATH` (process env or `.env`) — if set and the file
   exists, use it. Required for the bridge inner-dev-loop and live e2e.
2. **Bundled path:** the §6.2 composed path. This is the production path for every end user.
3. **Neither resolves:** return empty; `StartForPort` logs an actionable error (e.g. "no sidecar
   binary resolved for rid <rid>; reinstall the plugin for your platform, or set
   UNREAL_MCP_BRIDGE_PATH"). No download fallback exists.

### 6.4 Version-lockstep guarantee

The bundled binary is built from the SAME source tree at the SAME `VersionName` as the plugin in the
same release job. The plugin/bridge handshake (`sidecarVersion` vs `pluginVersion`, §1.3) therefore
always matches by construction; a skew can only occur if the user hand-replaces the bundled binary
(still caught by the handshake check, which kills + hard-fails to the UI — there is no re-download
recovery to attempt). `commands/bump-version.ps1` already moves the `.uplugin` `VersionName` and the
bridge csproj `<Version>` together, so the plugin zip and the bundled bridge share one version.

### 6.5 Offline / air-gapped guarantee

No network access is required at any point of sidecar bring-up: the binary is on disk inside the
plugin, the token is generated locally (CSPRNG, §1.4), and the IPC is loopback TCP. Network is only
touched when the sidecar dials the MCP server (Cloud or Custom) — i.e. only when the user has
actively chosen to connect. A fully offline editor still loads the plugin and spawns the sidecar.

### 6.6 Spawn / watchdog interaction with the bundled binary (§1.5)

The §1.5 state machine is unchanged in shape; only the `LaunchingSidecar` precondition simplifies:

- `LaunchingSidecar`: the binary is **present by construction** (no "version match → download" gate).
  The plugin verifies `FileExists`, then on macOS/Linux runs a pre-spawn prep
  (`PrepareBundledBinaryForSpawn`): `chmod 0755` to ensure the executable bit, and on macOS
  `removexattr com.apple.quarantine` so Gatekeeper's first-exec check is bypassed offline. The prep
  is skipped when the dev override is used (a dev-built binary is already runnable). Then `CreateProc`
  exactly as before (token over stdin, §1.4).
- **Auto-spawn now succeeds at StartupModule.** With the bundle, the path resolves on first boot,
  `SpawnProcess` succeeds, the sidecar dials, handshakes, and the watchdog takes over — zero clicks.
  Today a fresh install with no bundled binary (a dev source checkout) instead logs the §6.3 step-3
  actionable warning and spawns nothing, as before — devs use `UNREAL_MCP_BRIDGE_PATH`.
- **Crash auto-restart / orphan prevention** (§1.5 backoff + the three orphan layers) are unchanged;
  they operate on whatever binary `ResolveBridgeBinaryPath()` returned, bundled or override. The three
  orphan layers remain: (1) plugin `TerminateProc` on shutdown; (2) sidecar self-exits when the
  `--parent-pid` editor (plus its process start time) vanishes; (3) sidecar self-exits after 60 s
  without a successful handshake (a rejected handshake does not reset the timer, and 3 consecutive
  rejections are fatal, §1.4).
- **Zero-click reconnect (token cache).** The Cloud OAuth token persists as `cloudToken` in
  `<Project>/Saved/Config/UnrealMcp/ai-game-developer-config.json` (`FUnrealMcpConfig`). On every
  (re)connect `BuildEffectiveConnectionConfig()` re-reads it and pushes it to the sidecar via the
  §1.3 `config` message, so after the one-time device-code browser approval the spawn → dial →
  handshake → connect path is fully automatic on later launches. There is **no separate token store**
  introduced by the bundle model.
- **No `.lock`/atomic-rename/re-download machinery** — those existed only to serialize concurrent
  first-run downloads and version-mismatch re-downloads. With a read-only bundled binary they are
  gone from the design. Multiple editors share the same read-only files with no coordination.
- **Multi-editor coexistence:** one sidecar per editor process (per-project port + probing, §1.1).
  Two editors on the *same* project each get their own sidecar — fine, since the MCP server
  multiplexes plugin connections; the bundled binaries are shared read-only with no lock needed.
- **Dev override:** `UNREAL_MCP_BRIDGE_PATH` env/`.env` var points at a locally built sidecar (skips
  the bundled path) — required for the bridge task's inner dev loop and CI;
  `unreal-mcp-cli bootstrap-local` (§9) automates a from-source bridge build for that var.

**Binaries are NEVER committed to git.** They are staged into the plugin tree by the release job at
package time (a BuildPlugin input via `RuntimeDependencies`), and `.gitignore` keeps `Binaries/` out
of VCS. A dev source checkout has an empty `Binaries/ThirdParty/UnrealMcpBridge/` → the resolver
returns empty → devs use `UNREAL_MCP_BRIDGE_PATH`, exactly as before.

### Local server acquisition (the shared GameDev-MCP-Server)

The local MCP server is **not part of this repo**: it is the shared, engine-agnostic
[`IvanMurzak/GameDev-MCP-Server`](https://github.com/IvanMurzak/GameDev-MCP-Server) (binary
`gamedev-mcp-server`, Docker `aigamedeveloper/mcp-server`), the single host consumed by Unity-MCP,
Godot-MCP, and Unreal-MCP. The **CLI owns server acquisition** (`cli/src/lib/download-server.ts`);
the C++ plugin never downloads the server (only the bridge — see the TODO in
`UnrealMcpSidecarManager.cpp`).

- **URL contract:**
  `https://github.com/IvanMurzak/GameDev-MCP-Server/releases/download/v<SERVER_VERSION>/gamedev-mcp-server-<rid>.zip`
  (7 RIDs; tags are v-prefixed). Zip layouts are NOT uniform: the win zips are FLAT (exe + ~7
  sidecar files — appsettings.json, NLog.config, server.json, web.config, … — at the zip root)
  while the osx/linux zips wrap everything in a `<rid>/` folder. The CLI therefore extracts to a
  staging dir, finds the binary (shallowest match), and moves it **plus every sidecar file beside
  it** into the install dir — the sidecar files must land next to the exe.
- **Version pin — independent of the plugin version:** the consumed server version is the
  `SERVER_VERSION` constant in `cli/src/lib/server-version.ts` (single source; plugin 0.x and
  shared server 8.x deliberately diverge — `commands/bump-version.ps1` never touches it). A
  `version` marker file beside the binary records the installed version; missing/mismatched →
  re-download. Bumping the pin requires the corresponding GameDev-MCP-Server release to already
  exist (docs/RELEASING.md).
- **Install path:** `<Project>/Intermediate/UnrealMCP/server/<rid>/gamedev-mcp-server(.exe)` +
  `version` marker — the same §6 layout the bridge uses.
- **Trigger:** `unreal-mcp-cli setup-mcp <agent> --transport stdio` (the path where an MCP client
  needs a launchable local binary) downloads/refreshes automatically. A failed download degrades
  to a warning — the config is still written and works once the binary is provided.
- **Dev override:** `UNREAL_MCP_SERVER_PATH` env var points at a locally built server binary and
  skips the download + version check entirely (mirrors `UNREAL_MCP_BRIDGE_PATH`). Local server
  source development happens in the GameDev-MCP-Server repo itself.
- **Version independence (handshake):** the plugin⇄bridge `SidecarVersion` handshake
  (`bridge/src/Program.cs`) carries the **plugin/bridge** semver and is entirely independent of
  the consumed server version — a `SERVER_VERSION` bump never touches the handshake, and a plugin
  release never requires a server release (or vice versa).

---

## 7. Slate UI plan

All tabs are nomad tabs registered through `FGlobalTabmanager` in `StartupModule`, menu entries
under **Window → AI Game Developer** plus a toolbar button. Tab ids:

| Tab id | Window | Unity source mapped |
|---|---|---|
| `UnrealMcpMainWindow` | AI Game Developer (main) | `MainWindowEditor.*` |
| `UnrealMcpToolsWindow` | MCP Tools (list + per-tool toggles, token counts) | `McpToolsWindow.cs` |
| `UnrealMcpPromptsWindow` | MCP Prompts | `McpPromptsWindow` |
| `UnrealMcpResourcesWindow` | MCP Resources | `McpResourcesWindow` |
| `UnrealMcpSettingsWindow` | Settings page | `UnityMcpProjectSettingsProvider` |

The Settings page is **also** registered via `ISettingsModule` ("Project → Plugins → AI Game
Developer") rendering the same widget — UE users expect Project Settings discoverability; the
nomad tab satisfies the 4-aux-windows mandate. Aux windows dedupe/focus on reopen (the known
Godot [low] gets fixed here, per the aux-windows task).

**Main window sections, 1:1 from `MainWindowEditor.CreateGUI.cs:218–237`** (top to bottom) —
with one explicit deviation: Unity's `SetupDebugButtons` (line 235) is **not** ported as-is;
the decision is to ship a minimal debug row inside the Header/Settings section instead (see
item 1), not to defer debug affordances entirely:

1. **Header/Settings** — log-level dropdown, tool-timeout field, plugin version
   (`SetupSettingsSection`); plus a minimal debug row: **"Open log file"** and **"Restart
   bridge"** actions (the latter mirrors item 7's Restart button for discoverability) — this
   row is the deliberate replacement for Unity's `SetupDebugButtons`.
2. **Connection panel** — status circle + "Unreal: Connected/Connecting…/Disconnected" label,
   validated server-URL field (Custom mode), Connect/Disconnect/Stop button with the exact
   tri-state logic of `GetButtonText/GetConnectionStatusClass` (`MainWindowEditor.CreateGUI.cs:248–267`);
   connection timeline: Unreal point → MCP server point → AI-agent point with status dots.
3. **Connection-mode toggle** — Cloud (`ai-game.dev`) / Custom (`SetupConnectionModeToggle`).
4. **Cloud auth** — device-code flow: Authorize button sends `auth-start` (§1.3) → `device-auth`
   IPC events render verification URL + user code (clickable/copyable); Cancel sends
   `auth-cancel`; Revoke sends `auth-revoke`, clearing the cloud token; auth-rejected
   handling clears token + prompts re-auth (Unity `MainWindowEditor.Connection.cs:87–100`).
5. **Connection alerts** — inline warnings (port conflict, version mismatch, sidecar crash-looped).
6. **MCP server section** (Custom mode) — local `gamedev-mcp-server` Start/Stop + status, transport
   stdio/http toggle, auth none/required + masked token field + Generate button (crypto-random,
   restart-on-apply), copyable raw JSON client config snippets (`SetupMcpServerSection`). **LANDED
   (issue #95):** the Start/Stop button drives `FUnrealMcpServerManager` (launch + supervise the local
   server, gated to Custom + http — see the §7 deviation block above); the button label toggles
   Start↔Stop with the live server run-state and is disabled outside Custom+http.
7. **Bridge status** (Unreal-specific, no Unity analog) — sidecar state machine value, PID,
   binary version, restart count, Restart button.
8. **AI agents** — connected-agent labels + status dot, agent auto-configure list (Claude Code,
   Cursor, …) writing client configs (`SetupAiAgentSection`/`ConfigureAgents`).
9. **Features** — "N / M Tools" + "~K tokens total", Prompts, Resources counts; each row opens its
   aux window (`SetupToolsSection/Prompts/Resources` + `SubscribeToFeatureStats`).
10. **Extensions** — §5 list with toggles.
11. **Footer** — Discord / GitHub Issues / Star buttons (`SetupSocialButtons`).

**State ownership:** a single `FUnrealMcpEditorViewModel` (game-thread-only) over (a) the config
store (§8) and (b) the sidecar `status`/`device-auth` IPC feed. IPC events arrive on the reader
thread and are marshalled via `AsyncTask(GameThread)` before touching any Slate state — the M9b
Godot dock lesson (main-thread-marshalled subscriptions; Disconnect must genuinely stop reconnect:
the `config` message carries `keepConnected:false` and the sidecar fully tears down the SignalR
client, not merely drops one connection). Widgets bind through `TAttribute` lambdas reading the
view-model; writes go config-store-first, then push `config` over IPC.

---

## 8. Config & env

**Variables** (union of Unity's `EnvironmentUtils.cs:43–51` set and Godot's `GODOT_MCP_LOG_LEVEL`,
renamed `UNREAL_MCP_*`):

`UNREAL_MCP_CONNECTION_MODE` (Cloud|Custom), `UNREAL_MCP_HOST`, `UNREAL_MCP_CLOUD_URL`,
`UNREAL_MCP_TOKEN`, `UNREAL_MCP_AUTH_OPTION` (none|required), `UNREAL_MCP_KEEP_CONNECTED`,
`UNREAL_MCP_TOOLS` (enabled-tools override list), `UNREAL_MCP_START_SERVER`,
`UNREAL_MCP_TRANSPORT` (stdio|http), `UNREAL_MCP_LOG_LEVEL`, plus dev-only
`UNREAL_MCP_BRIDGE_PATH` (§6). `worktree.py` gains `UNREAL_MCP_*` emission (small infra PR, per
the connection-config task).

- **`.env` support:** project-root `<Project>/.env`, parsed by the C++ plugin with exactly
  `GodotMcpEnvFile`'s rules (skip blanks/`#`, split on first `=`, trim, recognized keys only,
  strip matching quotes) — Godot proved the file layer is essential for GUI-launched editors that
  inherit no shell exports; UE launched from the Epic launcher has the same problem.
  **Commit hazard mitigation:** a project-root `.env` can hold `UNREAL_MCP_TOKEN`, and UE project
  templates ship no `.gitignore` — so (a) the Unreal-MCP repo's own scaffold `.gitignore`
  includes `.env`; (b) `unreal-mcp-cli configure` appends `.env` to the **target project's**
  `.gitignore`, creating that file if absent; (c) the docs carry an explicit "never commit
  `.env`" warning.
- **On-disk config:** **`<Project>/Saved/Config/UnrealMcp/ai-game-developer-config.json`**.
  Chosen over project-root JSON because `Saved/` is gitignored by every UE template — tokens never
  land in VCS by default (Unity uses `UserSettings/`, Godot `user://`; `Saved/` is the UE
  equivalent). JSON (System.Text.Json-compatible camelCase), not `.ini`, so plugin, sidecar and
  cli could share parsing if ever needed — but **only the plugin reads/writes it**; the effective
  config reaches the sidecar exclusively via the `config` IPC message (single source of truth).
- **Precedence (highest wins):** process env → `.env` → config file → built-in defaults. Same
  realization as Godot (`GodotMcpConfigStore.cs:24–39`): persisted values load into backing fields
  first, `.env` overwrites those fields, process env is read live on access. Env/.env overrides
  are **never persisted back** — the Unity `OverrideRecord` baseline-restore pattern
  (`EnvironmentUtils.cs:73–95`) carries over so Save() round-trips disk baselines.
- **Token protection:** masked in UI (reveal-on-hold), never logged at any level (log scrubber on
  the `log` IPC channel + NLog layout rule sidecar-side), file written with no extra ACL work but
  documented as secret; `UNREAL_MCP_TOKEN` recommended for CI instead of the file.

---

## 9. Repo layout, versioning, tests, CI

### 9.1 Layout (`IvanMurzak/Unreal-MCP`, public, Apache-2.0)

```
Unreal-MCP/
├── UnrealMCP/                          # the UE plugin (in-tree, like addons/godot_mcp)
│   ├── UnrealMCP.uplugin               # VersionName single-source; NO EngineVersion pin (5.5+ floor is a CI/doc claim, not a descriptor field — see status block)
│   ├── Source/UnrealMcpEditor/         # Type Editor, LoadingPhase Default
│   │   ├── Public/   (IUnrealMcpToolProvider.h, UnrealMcpToolRegistry.h, …)
│   │   └── Private/  (Bridge/ Tools/ Schema/ Dispatch/ UI/ Config/ Sidecar/)
│   ├── Source/UnrealMcpEditorTests/    # Automation specs (Type Editor, WITH_DEV_AUTOMATION_TESTS)
│   └── Resources/Icon128.png
├── bridge/                             # .NET sidecar — com.IvanMurzak.Unreal.MCP.Bridge
│   ├── src/  publish.(sh|ps1)          # self-contained single-file per RID
│   └── tests/                          # xUnit (IPC framing, manifest diff, proxy tools, lifecycle)
├── cli/                                # npm `unreal-mcp-cli` (TypeScript, vitest, dist/lib.js export)
│                                       #   commands (port of unity-mcp-cli's set, cli/src/commands/):
│                                       #   create-project, open, close, install-plugin, remove-plugin,
│                                       #   configure (UNREAL_MCP_* env/.env), setup-mcp, login (device code),
│                                       #   status, wait-for-ready, run-tool, run-system-tool,
│                                       #   bootstrap-local, update, install-engine (LauncherInstalled.dat
│                                       #   detection at minimum), setup-skills
├── samples/UnrealAITemplate/           # §5 extension template plugin
├── docs/  (ARCHITECTURE.md ← this doc, RELEASING.md, EXTENSIONS.md, claude/)
├── .github/workflows/  (test_pull_request.yml, test_cli.yml, release.yml)
├── commands/bump-version.ps1
├── CLAUDE.md  README.md  LICENSE  .gitignore  .editorconfig
```

Infra side (separate tasks): `Unreal-Test-Project/` testbed in the infra repo (UE 5.7, minimal,
plugin junction `Plugins/UnrealMCP → ..\Unreal-MCP\UnrealMCP`, analog of `Godot-Test-Project/`);
submodule registration only after the public-repo gate.

### 9.2 Versioning

Single semver `MAJOR.MINOR.PATCH` shared by plugin (`.uplugin` `VersionName`), bridge and cli —
`commands/bump-version.ps1` rewrites all of them (Unity/Godot convention). The plugin↔sidecar
download URL embeds the version (§6) and the handshake double-checks it, so the pair can never
skew. The **consumed shared-server version is independent**: `cli/src/lib/server-version.ts`
`SERVER_VERSION` pins the GameDev-MCP-Server release (§6) and is bumped deliberately, never by
`bump-version.ps1`. `ipcVersion` (integer, starts at 1) only bumps on breaking IPC changes. NuGet
pins (`com.IvanMurzak.McpPlugin` ≥ 6.8.0 w/ ProxyTool, `com.IvanMurzak.ReflectorNet`) are owned by
upstream release pipelines — never bumped ad-hoc (Godot lockstep rule).

### 9.3 Test strategy

| Suite | Framework | Where it runs | Scope |
|---|---|---|---|
| bridge unit | xUnit (+Shouldly/Moq, McpPlugin conventions) | hosted ubuntu, PR | framing codec, manifest diff, ProxyTool mapping, backoff, config push |
| cli unit | vitest | hosted ubuntu, PR | command logic, engine discovery (`LauncherInstalled.dat` fixture), lib export |
| plugin unit/functional | UE Automation Spec (`BEGIN_DEFINE_SPEC`, filter `UnrealMcp.`) | self-hosted win UE 5.7, PR | schema generator table (§3.2 golden files), registry/dedup/ordering, dispatcher timeout+cancel, env/.env/config precedence, view-model status logic |
| headless e2e | script: editor + local server + sidecar | self-hosted win, release (+nightly) | `UnrealEditor-Cmd.exe <proj> -ExecCmds="Automation RunTests UnrealMcp; Quit" -ReportExportPath=<dir> -unattended -nullrhi -nosplash -log`; CI parses the exported JSON report (`index.json`) for pass/fail + per-test results instead of scraping the log; live `ping`→`pong` via `POST /api/tools/ping` (Godot testbed runbook port) |
| windowed/visual | operator runbook | local RTX machine | screenshots family, Slate windows (no GPU in headless — Godot lesson) |

PR scope = first three rows. Release scope adds e2e + packaging. Per-tool live verification
happens in each tool-family task against `Unreal-Test-Project` (Godot tool-wave playbook).

### 9.4 CI shape

- `test_pull_request.yml`: job A bridge build/test (ubuntu + windows), job B cli (ubuntu, via
  reusable `test_cli.yml`), job C plugin compile + Automation on the self-hosted runner — **runner
  label `unreal-5-7`**, distinct from existing release labels so M1/M4 capacity isn't starved
  (ci-workflows task constraint). Job C builds via
  `RunUAT.bat BuildPlugin -Plugin=UnrealMCP.uplugin -Package=<out> -TargetPlatforms=Win64` (also
  validates marketplace packaging) and then runs the Automation filter against the testbed project.
- `release.yml` (Godot `release.yml` skeleton: version-from-source job → gate compute → test fan-out
  → artifact build → gated publish): artifacts = plugin zip (BuildPlugin output, engine-agnostic
  source plugin), `unreal-mcp-bridge-<rid>.zip` ×4, npm publish, GitHub Release (server zips are
  released from the shared GameDev-MCP-Server repo, not here — §6).
  **Tag/release firing is Ivan-GATED**; full-rerun-only policy for artifact-passing
  workflows (gh-rerun lesson) documented in `docs/RELEASING.md`. `workflow_dispatch` exposes a
  **`dry_run` input**: when true, the test fan-out + artifact-build jobs run (artifacts uploaded
  for inspection) while the tag/GitHub-Release/npm-publish jobs are hard-skipped — this is how
  the release pipeline is rehearsed end-to-end without publishing anything (and what the
  ci-workflows task's "release.yml dry-run-able" DoD means).

---

## 10. Tool families — MVP per task, + backlog

Names mirror Unity-MCP (`Editor/Scripts/API/Tool/` inventory) with Unreal vocabulary:
GameObject→actor, Scene→level, Prefab→Blueprint-or-level-instance. `ping` ships in the
sidecar-bridge task. Every tool below is declared via §3.3 and runs on the dispatcher.

**actor family** (task: actor-tools; Unity GameObject parity ~14): `actor-create`,
`actor-destroy`, `actor-duplicate`, `actor-find` (scoped reads via `paths`/`viewQuery`),
`actor-modify` (FProperty writes incl. transform), `actor-set-parent` (attach),
`actor-component-add`, `actor-component-destroy`, `actor-component-get`,
`actor-component-modify`, `actor-component-list-all` (paginated UActorComponent classes),
`object-get-data`, `object-modify` (generic UObject by path).

**asset family** (task: asset-tools; Unity Assets parity ~20): `asset-find` (AssetRegistry
filters: name/class/path/tags), `asset-get-data`, `asset-create-folder`, `asset-copy`,
`asset-move` (rename), `asset-delete`, `asset-refresh` (rescan paths),
`asset-material-create` (UMaterialInstanceConstant from parent), `asset-material-modify`
(scalar/vector/texture params), `asset-material-get-data` (read-only graph/param info — the
"shader" analog), `asset-import` (FBX/texture via AssetImportTask — Unreal-relevant addition).

**level family** (task: level-tools; Unity Scene parity 8): `level-create`, `level-open`,
`level-save` (+save-as), `level-get-data` (actor-tree snapshot, scoped reads),
`level-list-loaded` (sublevels + World Partition read-only awareness), `level-set-current`
(active sublevel), `level-unload-sublevel`.

**blueprint family — FLAGSHIP, Unreal-unique** (task: blueprint-tools): MVP floor =
read + structure-edit + compile + spawn:
- `blueprint-create` — new BP class from parent UClass path (`FKismetEditorUtilities::CreateBlueprint`).
- `blueprint-get` — graph summary for LLM inspection: variables (name/type/defaults), components,
  functions/events with node-count + call-graph edges, implemented interfaces, parent chain.
- `blueprint-add-component` / `blueprint-remove-component` — SCS (Simple Construction Script) edit.
- `blueprint-add-variable` / `blueprint-modify-variable` — typed via §3.2 pin-type mapping
  (`FBlueprintEditorUtils::AddMemberVariable`).
- `blueprint-set-default` — CDO property edit (same write path as `actor-modify`).
- `blueprint-add-function` / `blueprint-add-event` — stubs: entry/result nodes wired, BlueprintCallable
  parent events (BeginPlay, Tick, input actions) bound.
- `blueprint-compile` — `FKismetEditorUtilities::CompileBlueprint` + **structured error/warning
  list** (node, graph, message) — this is the AI feedback loop and is mandatory.
- `blueprint-spawn` — instance into the current level (closes the create→edit→verify loop).
- Graph *authoring* depth decision: MVP does **not** ship free-form node-graph wiring
  (`blueprint-add-node`/`connect-pins` are backlog) — structure-edit + compile feedback already
  covers the 80% AI loop; full K2Node authoring is high-risk surface and lands post-MVP behind the
  same family.

**source family** (task: source-script-tools; Unity Script reinterpreted): `source-read` (sliced),
`source-create-class` (header+cpp from templates: UObject/AActor/UActorComponent/empty),
`source-update`, `source-delete`, `source-list` (module sources), `source-compile` (Live Coding
when active, else UBT invoke) with structured error report. All file ops jailed to
`<Project>/Source/`. No Roslyn-style eval analog in MVP; optional Python-scripting-plugin
execution tool = backlog (decision: out of MVP — extra plugin dependency).

**screenshot family** (task: screenshot-tools): `screenshot-viewport` (active editor viewport),
`screenshot-game-view` (PIE), `screenshot-camera` (from a CameraActor/CameraComponent via
SceneCapture2D), `screenshot-isolated` (actor render with showflag isolation + background option —
Godot SubViewport pattern → `USceneCaptureComponent2D`). PNG, size-capped (default ≤1024,
hard cap 2048), returned as MCP image content.

**editor/reflection family** (task: editor-reflection-tools): `editor-application-get-state` /
`editor-application-set-state` (PIE start/stop/pause), `editor-selection-get` /
`editor-selection-set`, `console-get-logs` / `console-clear-logs` (GLog `FOutputDevice` listener →
capped ring buffer, Godot `GodotLogCollector` pattern), `console-run-command` (Unreal-unique:
console cmds incl. CVars), `reflection-method-find` / `reflection-method-call` (UFunction
discovery + invocation incl. `CallInEditor`, static + instance, schemas via §3.1(2)).

**Later backlog** (explicitly post-MVP, ordered by expected demand): Blueprint node-graph
authoring (add-node/connect-pins/comment); Niagara (system create, user-param set, component
spawn); Material **graph** authoring (expression nodes); Landscape (sculpt/paint/import
heightmap); Sequencer (track/keyframe authoring); PCG graph tools; DataTable row CRUD; animation
BP state machines; Behavior Trees; plugin management family (`package-*` analog over
IPluginManager); profiler family (`stat` capture analog); `tests-run` (Automation runner
exposure); Skills generation (sidecar-side, free once prompts land — Godot generates them on boot).

Prompts and resources ship empty-but-wired in MVP (the framework relays them already; counts show
0 in the UI) — first content is backlog.

---

## 11. Top-5 risks & mitigations

1. **Blueprint structure-edit APIs are brittle across UE versions** (SCS editing, K2 schema
   internals churn between 5.5→5.7). *Mitigation:* MVP floor avoids free-form graph wiring; pin
   all BP edits through `FKismetEditorUtilities`/`FBlueprintEditorUtils` public surface;
   Automation specs per engine-API touchpoint run on 5.7 CI, and the 5.5 floor is validated before
   first release by a one-off compile/test pass on a 5.5 install (declared floor is only honest
   when exercised).
2. **Game-thread deadlock/stall**: a tool body that itself pumps a modal dialog or waits on the
   IPC thread can wedge the editor. *Mitigation:* dispatcher never blocks the reader thread
   (§4), tool authors get a hard rule (no modal UI, no synchronous waits on bridge state),
   timeout always completes the future, and `source-compile`/`blueprint-compile` are async-chained.
3. **Sidecar/plugin pairing skew or download failure** (offline studios, GitHub throttling,
   antivirus quarantining a fresh exe). *Mitigation:* version handshake hard-check + single
   auto-redownload; `UNREAL_MCP_BRIDGE_PATH` escape hatch; `unreal-mcp-cli bootstrap-local` from-source
   path; clear main-window alert with manual instructions.
4. **MCP-Plugin-dotnet ProxyTool PR cadence** blocks the sidecar task. *Mitigation:* verified the
   PR is additive-only against current source (§2.3); `UseLocalMcpPlugin=true` local-ref pattern
   unblocks development before the NuGet release; worst case the ProxyTool lives in `bridge/`
   (public APIs suffice).
5. **Self-hosted UE 5.7 runner as a single point of failure** for PR CI (long compile times,
   one machine, shared with release duties). *Mitigation:* distinct `unreal-5-7` label; plugin job
   caches UBT intermediates between runs; bridge/cli legs stay on hosted runners so most PRs get
   fast signal even when the UE leg queues; Automation filter kept tight (`UnrealMcp.` prefix).

---

## 12. Open questions for TD / Ivan (none block the scaffold or sidecar tasks)

1. **Fab packaging ambition**: should `release.yml` produce a Fab-submission-ready archive from
   day one (extra metadata/screenshot requirements), or is the GitHub zip enough until the Fab
   gate is actually approached? (Design assumes GitHub-only until the gate.)
2. **macOS/Linux editor support claim**: the sidecar ships 4 RIDs and the plugin code is
   platform-clean, but CI only proves Win64. Declare "Windows supported, mac/Linux experimental"
   in the first README, or hold the claim until a mac runner exists?
3. **`UNREAL_MCP_TOOLS` semantics**: Unity treats it as an enabled-tools override list
   (`EnvironmentUtils.EnvTools`). Keep list-of-names, or adopt family-glob syntax
   (`actor-*,blueprint-*`) given Unreal's larger expected tool count? (Design assumes Unity-compatible
   list-of-names for parity; glob is a backward-compatible extension later.)

---

## Self-review checklist (performed)

- All 10 mandated sections resolved with concrete picks; no TBD on the critical path (the only
  deliberate deferrals — BP graph authoring, prompts/resources content, Python eval — are
  explicitly post-MVP backlog, not open design).
- All 17 open task files cross-checked: scaffold (§9.1 tree, naming, boot log), sidecar-bridge
  (§1, §2, §6, ping e2e), connection-config (§8, worktree.py note), 7 tool families (§10 lists
  match each task's goal), main-window + aux-windows (§7 incl. dedup/focus fix), extensions
  (§5 incl. template + isolation), cli (§9 layout/test), ci-workflows (§9.4 runner label, gates),
  docs-distribution (this doc → `docs/ARCHITECTURE.md`), app-integration (GATED — only the
  `dist/lib.js` export commitment, §9.1, touches it). The test-project task was already
  COMPLETED 2026-06-10 (testbed committed in infra `44f2d78`; task file deleted per plans
  convention) — the testbed itself matches §9.1/§9.3 (junction + headless smoke).
- No contradiction with locked decisions (C++ + sidecar, UE 5.7/floor 5.5+, mono-repo, parity+,
  windows mandatory, self-hosted CI).
- Adversarial pass on §1/§3 (highest-risk): added writer-mutex interleaving rule, 64 MiB frame cap,
  out-of-order revision guard, handshake timeout-as-crash, FText/delegate/interface property
  exclusions, non-string TMap key convention, promise double-set guard, manifest re-push on
  reconnect idempotency.
