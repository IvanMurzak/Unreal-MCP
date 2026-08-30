# Unreal-MCP — Architecture Design

> Authoritative architecture design for the `IvanMurzak/Unreal-MCP` mono-repo.
>
> **Locked decisions (Ivan, 2026-06-10 — not relitigated here):** C++ UE editor plugin + auto-managed
> .NET sidecar reusing MCP-Plugin-dotnet + ReflectorNet (SignalR to ai-game.dev / custom server,
> device-code + bearer auth). UE 5.7 development target, declared floor 5.5+. CI on a self-hosted
> Windows runner with UE 5.7. Public mono-repo, Apache-2.0, Godot-MCP-style layout. Unity-MCP feature
> parity PLUS Unreal-unique tools (Blueprints first-class). Single main Slate window holding all
> settings (Unity-MCP parity, issue #107) + aux windows mandatory.

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
| §4 GameThread dispatcher | implemented | #4 (used by every tool family) |
| §5 Extensions mechanism | implemented | #7 (`IUnrealMcpToolProvider`, `samples/UnrealAITemplate`) |
| §6 Sidecar lifecycle | implemented | #4 (spawn / crash auto-restart / orphan-prevention / stdin-token) + #46 (BUNDLE-model resolution: env override → bundled `Binaries/ThirdParty/UnrealMcpBridge/<rid>/`, zero-click auto-spawn; the superseded download-on-first-run flow is dropped — see drift below) |
| §7 Slate UI | implemented | #24 (AI Game Developer main window) + #29 (MCP Tools/Prompts/Resources aux windows) + #107 (collapsed the separate Settings window into the single main window, Unity-MCP parity) |
| §8 Config & env | implemented | #8 (`UNREAL_MCP_*`, `.env`, on-disk config) |
| §9.1 cli (`unreal-mcp-cli`, 16 commands) | implemented | #3 |
| §9.2 Versioning | implemented | `commands/bump-version.ps1` single-sources `VersionName` across plugin/bridge/server/cli (the "≥ 6.8.0 w/ ProxyTool" pin is forward-looking — see §2.3 drift below) |
| §9.3 Test strategy | implemented | bridge xUnit + cli vitest + plugin Automation specs (#13–#25), CI wiring #28 |
| §9.4 CI (`test_pull_request` / `release` / `test_cli`) | implemented | #28 |
| §10 system tools (3: `ping`, `unreal-skill-create`, `unreal-skill-generate` — see §2.4) | implemented | #4 + owner ruling 2026-07-25 |
| §10 actor & component family (13) | implemented | #14 |
| §10 asset family (11) | implemented | #15 (issue #10) |
| §10 blueprint family (11) | implemented | #13 |
| §10 source family (6) | implemented | #23 |
| §10 screenshot family (4) | implemented | #21 |
| §10 editor/reflection family (9) | implemented | #22 |
| §10 level family (7) | implemented | #25 |

**Total shipped: 61 STANDARD core tools across 7 families, plus 3 SYSTEM tools (§2.4)** (counts verified
from the registration source).
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
  The status table marks §7 "implemented" for the window + its aux windows (MCP Tools/Prompts/
  Resources + Serialization Check; the separate Settings window was collapsed into the main window
  in #107); the deferred affordances above remain deferred.
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
│  └─ Slate UI: single main window (settings inline) + aux tabs            │
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
| plugin → sidecar | `prompt-manifest` | full snapshot: `revision` + array of prompt descriptors (§A.1) — IPC v2 |
| sidecar → plugin | `prompt-get` | `requestId`, `prompt`, `arguments`, `timeoutMs` (§A.1) — IPC v2 |
| plugin → sidecar | `prompt-response` | `requestId`, `status`, `messages[]` (role + text) (§A.1) — IPC v2 |
| plugin → sidecar | `resource-manifest` | full snapshot: `revision` + array of resource descriptors (§A.1) — IPC v2 |
| sidecar → plugin | `resource-read` | `requestId`, `uri`, `timeoutMs` (§A.1) — IPC v2 |
| plugin → sidecar | `resource-response` | `requestId`, `status`, `contents[]` (text XOR base64 blob + mimeType) (§A.1) — IPC v2 |
| sidecar → plugin | `tool-cancel` | `requestId` — cooperative cancellation (§4); reused for prompt-get / resource-read |
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

### 2.4 Tool surfaces — STANDARD vs SYSTEM (owner ruling 2026-07-25)

A tool is served on exactly one of two surfaces, mirroring the shared
`com.IvanMurzak.McpPlugin.McpToolType` the C# engines declare as `[AiTool(..., ToolType = McpToolType.System)]`:

| surface | reachable at | in `tools/list`? | for |
| --- | --- | --- | --- |
| **Standard** (default) | `/api/tools/<name>` + MCP `tools/call` | yes | the 61 authoring tools an AI agent drives |
| **System** | `/api/system-tools/<name>` **only** | **no** | host plumbing a CLIENT drives: liveness, skill authoring |

**The system tools are `ping`, `unreal-skill-create`, `unreal-skill-generate`** — the same three, with
the engine prefix swapped, on Unity (`unity-skill-*`) and Godot (`godot-skill-*`). They are not
capabilities: `ping` is how the `unreal-mcp-cli` and the desktop app decide the editor is reachable, and
the skill tools author documentation/tooling. Leaving them on the standard surface spends `tools/list`
tokens in every agent session on tools the agent must never call.

**C++ declaration.** `FUnrealMcpToolBuilder::ToolType(EUnrealMcpToolType::System)`; the flag lives on
`FUnrealMcpRegisteredTool::ToolType` (default `Standard`) and ships in the §2.2 manifest descriptor as
`"toolType": "standard" | "system"`. It is inside the schema hash, so moving a tool between surfaces
diffs as a CHANGED entry and re-registers the sidecar proxy on the other manager. An absent/unknown token
parses as `Standard`, so an older plugin against a newer sidecar behaves exactly as before.

**Sidecar routing.** `SurfaceRoutingToolSink` (wrapping `ToolManagerSink` + `SystemToolSink`) sends each
proxied tool to the manager its descriptor named, and remembers which surface each name landed on so a
later remove/toggle hits the right one. McpPlugin's `ISystemToolManager` is read-only by design — for the
C# engines the system set is fixed at `McpPluginBuilder.Build` time — so the dynamic path writes the
DI-singleton `SystemToolRunnerCollection` (a plain `Dictionary<string, IRunTool>`) that
`McpSystemToolManager` reads live on every call. No upstream API change was needed.

**Where each tool lives, and why the pair is split:**

- `ping` and `unreal-skill-create` are **C++** tools proxied over IPC. `unreal-skill-create` writes plugin
  C++ and needs the editor's own view of the install; `ping` must prove the editor round trip, which only a
  C++-side handler can.
- `unreal-skill-generate` is **sidecar-native** (`bridge/src/Tools/SkillGenerateTool.cs`). SKILL.md
  generation moved out of C++ in #101 and the sidecar already holds the descriptor catalog; a C++ handler
  may not synchronously wait on bridge state (§4, §11 risk 2), so a C++-homed generate tool could only
  fire-and-forget and misreport completion. It is declared through `McpPluginBuilder.AddTool` before
  `Build`, which partitions build-time runners by `IRunTool.ToolType` with no post-build mutation at all.

**`unreal-skill-create` emits C++ and says a rebuild is required** (owner ruling, option (b)). Unity's
`skill-create` writes a `.cs` Unity hot-compiles on domain reload; Unreal has no such path, and emitting C#
into the sidecar is not viable (`bridge/src` has no Roslyn and no `AssemblyLoadContext`, and it ships as a
prebuilt single-file binary an end user cannot recompile). The generated file lands in
`UnrealMCP/Source/UnrealMcpEditor/Private/Tools/Skills/` and **self-registers** via
`FUnrealMcpGeneratedSkillRegistrar`, so adding or deleting a skill never edits another file — the editor
coordinator calls `UnrealMcpGeneratedSkills::Register` exactly once. The result always reports
`rebuildRequired: true` / `callable: false`; triggering Live Coding automatically is deliberately deferred.
The plugin's editor module is the destination rather than a game module because a game module that depends
on `UnrealMcpRuntime` overrides a consumer's `TargetDenyList` and drags the plugin into packaged builds.
A precompiled (marketplace) install has no C++ source on disk and is refused with that reason.

**Every generated SKILL.md carries a provenance marker.** The YAML front matter closes with, as its LAST
block before the closing `---`:

```yaml
metadata:
  generated-by: mcp-plugin-dotnet
```

The two-space indent is load-bearing — it is what makes `generated-by` a nested mapping rather than a
sibling top-level scalar. The value deliberately carries **no version and no timestamp**, so regenerating
an unchanged tool rewrites a byte-identical file. It exists so a consumer can tell a generated skill from a
hand-authored one and dedup only its own output against the live tool catalog (matching on the front
matter's `name:`); such a consumer is expected to be **fail-open** — an unmarked file is kept. There is no
such consumer in this repo yet, and `PruneStaleSkillFolders` is **not** one: it prunes on the presence of a
`SKILL.md`, not on the marker.

The same two lines the shared `com.IvanMurzak.McpPlugin.Skills.SkillFileGenerator` emits, but this class is
independent of it rather than a subclass, so it stamps its own copy (unifying the two is a known deferred
follow-up). Same line *content*, not the same bytes: the shared generator terminates with
`Environment.NewLine` while this one joins the document with `"\n"`, so a consumer must match the marker
per line, never as one raw byte run. This generator's front matter is therefore LF-only on every platform;
note the `### Input JSON Schema` fences further down are serialized by `System.Text.Json` with
`WriteIndented`, whose newline defaults to `Environment.NewLine`, so a generated `SKILL.md` is
**mixed-ending on Windows** outside the front matter.

**Consequence for callers.** `POST /api/tools/ping` no longer resolves. The CLI probes
`/api/system-tools/ping` first and falls back to the legacy route only for a pre-§2.4 plugin
(`cli/src/utils/probe.ts`); `scripts/connection_smoke.py` asserts `ping` is ABSENT from `tools/list` and
exercises it over the REST system route instead. SKILL.md files are generated for standard tools only —
documenting a system tool would re-expose it through the skills channel.

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
├── Source/ThirdParty/UnrealMcpBridge/      # FAB-SURVIVING source location (#139/#187) — the
│   └── <rid>/  unreal-mcp-bridge[.exe]     #   engine-canonical Source/ThirdParty/<Lib>/<platform>/
│      (+ self-contained payload files)     #   layout, declared in Config/FilterPlugin.ini so it ships
│                                           #   in the dedicated source release zip (win-x64 / osx-arm64 / osx-x64 / linux-x64)
└── Binaries/                               # STAGED location — UBT stages Source/ThirdParty/UnrealMcpBridge/<rid>/
    └── ThirdParty/                         #   here at compile time (RuntimeDependencies two-arg form). Fab
        └── UnrealMcpBridge/                #   STRIPS this whole folder from a source submission, so
            └── <rid>/  unreal-mcp-bridge[.exe]   it is re-created only by a recompile (Epic's or local).
```

- **Two locations, one resolver (#139/#187 Fab readiness).** Fab accepts a *source* plugin and recompiles
  it per engine version, **stripping `Binaries/`, `Intermediate/`, `Saved/`** from the submitted zip, and
  requires redistributed third-party binaries under the engine-canonical `Source/ThirdParty/<Lib>/<platform>/`
  layout. So the prebuilt sidecar CANNOT live only under `Binaries/` — it lives in the Fab-surviving
  `Source/ThirdParty/UnrealMcpBridge/<rid>/` folder (declared in `Config/FilterPlugin.ini`), and
  `UnrealMcpRuntime.Build.cs`'s `RuntimeDependencies` two-arg `(target, source)` form **stages** it into
  `Binaries/ThirdParty/UnrealMcpBridge/<rid>/` at compile time. The resolver walks BOTH: the staged
  `Binaries/ThirdParty` path first (packaged games + non-Fab GitHub releases), then the surviving
  `Source/ThirdParty/UnrealMcpBridge/<rid>/` source (Epic-compiled Fab builds, whose `Binaries/` was
  stripped + may not re-stage). The `<rid>/` folders hold only binaries (no `*.Build.cs`), so UBT never
  treats `Source/ThirdParty/UnrealMcpBridge/` as a module.
- Resolver helpers (C++, `FUnrealMcpSidecarManager`): `ComposeBundledBridgePath` →
  `Binaries/ThirdParty/UnrealMcpBridge/<rid>/`; `ComposeSurvivingBridgePath` →
  `Source/ThirdParty/UnrealMcpBridge/<rid>/`; `ComposeBundledBridgeCandidates` returns the ordered list
  `ResolveBridgeBinaryPath` FileExists-walks. All resolve relative to the plugin's base dir via
  `IPluginManager::Get().FindPlugin(TEXT("UnrealMCP"))->GetBaseDir()` (the `Projects` module is a
  dependency — `UnrealMcpRuntime.Build.cs`).
- Binary basename: `unreal-mcp-bridge` on macOS/Linux, `unreal-mcp-bridge.exe` on Windows (matches
  `<AssemblyName>unreal-mcp-bridge</AssemblyName>` in the bridge csproj).
- `Binaries/ThirdParty/` is the UE-canonical home for prebuilt third-party runtime payloads and is
  what `RuntimeDependencies` + `-Rocket` BuildPlugin packaging expect for a packaged GAME; the
  `Source/ThirdParty/UnrealMcpBridge/<rid>/` source is the marketplace-distribution form Fab recompiles from.

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

`ResolveBridgeBinaryPath()` walks the §6.1 candidate list (staged `Binaries/ThirdParty/...` first, then
the Fab-surviving `Source/ThirdParty/UnrealMcpBridge/<rid>/`) and returns the first whose `FPaths::FileExists` is true; otherwise
empty (caller logs the actionable "plugin packaged without the <rid> bridge" error).

### 6.3 Resolution order (replaces the stub)

1. **Dev/CI override:** `UNREAL_MCP_BRIDGE_PATH` (process env or `.env`) — if set and the file
   exists, use it. Required for the bridge inner-dev-loop and live e2e.
2. **Bundled paths (in order):** the §6.1 candidate list — the staged
   `Binaries/ThirdParty/UnrealMcpBridge/<rid>/` path first, then the Fab-surviving
   `Source/ThirdParty/UnrealMcpBridge/<rid>/` source (#139/#187). The first that exists wins. This is the production path for every end user; the
   Fab-surviving fallback is what an Epic-compiled marketplace build resolves from after Fab strips
   `Binaries/`.
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

**Binaries are NEVER committed to git.** The signed per-RID payloads are staged into the Fab-surviving
`Source/ThirdParty/UnrealMcpBridge/<rid>/` folder by the release job at package time (a BuildPlugin input via
`RuntimeDependencies`'s two-arg form, #139/#187), and `.gitignore` keeps the
`Source/ThirdParty/UnrealMcpBridge/<rid>/` payloads (and `Binaries/`) out of VCS — only the
`Source/ThirdParty/UnrealMcpBridge/` folder STRUCTURE (`README.md` + per-RID `.gitkeep`) is
tracked in git. The **release-produced** `unreal-mcp-plugin-source-<version>.zip` asset is created
after staging, so it ships the real payloads; a dev source checkout still has empty
`Source/ThirdParty/UnrealMcpBridge/<rid>/` (and `Binaries/ThirdParty/UnrealMcpBridge/`) → the resolver returns empty →
devs use `UNREAL_MCP_BRIDGE_PATH`, exactly as before.

### 6.7 Fab (Epic marketplace) source-submission readiness (#139/#187)

Submitting `UnrealMCP` to Fab as a **source** plugin imposes constraints on the §6 BUNDLE model;
all are handled in-repo (issues #139/#187), and submission itself remains an operator-gated manual step
(no CI job submits — see `docs/RELEASING.md`):

1. **Sidecar must survive the strip, under the canonical ThirdParty layout.** Fab strips
   `Binaries/`/`Intermediate/`/`Saved/` and recompiles, and requires redistributed third-party binaries
   under the engine-canonical `Source/ThirdParty/<Lib>/<platform>/` layout — so the prebuilt sidecar lives
   in `Source/ThirdParty/UnrealMcpBridge/<rid>/` (declared in `Config/FilterPlugin.ini`), is staged
   into `Binaries/ThirdParty/UnrealMcpBridge/<rid>/` by `RuntimeDependencies`, and is ALSO resolved
   directly from `Source/ThirdParty/UnrealMcpBridge/<rid>/` (§6.1 candidate list) so an Epic-compiled build
   resolves it even when `Binaries/` did not re-stage. This does not regress the GitHub-release/npm bundle
   nor `release.yml`'s signed-binary staging — the dedicated CLI/Fab source asset and the packaged
   BuildPlugin asset are both cut from the same staged `Source/ThirdParty/UnrealMcpBridge/<rid>/` payloads.
2. **No shipped test/automation module.** Fab review flags shipped test modules, so the DISTRIBUTED
   `UnrealMCP.uplugin` omits `UnrealMcpEditorTests`. PR/dev CI still runs the `UnrealMcp.` Automation
   specs by transiently re-adding the module via `commands/test-module-uplugin.ps1` (idempotent
   add/remove around the Automation BuildPlugin), then reverting — the committed/distributed descriptor
   never carries the test module. `BuildPlugin -Rocket` on the distributed descriptor stays green.
3. **FilterPlugin hygiene.** `Config/FilterPlugin.ini` ships the `Source/ThirdParty/UnrealMcpBridge/...`
   tree; no source uses hardcoded/absolute paths (resolution is plugin-base-dir relative, §6.1); all
   packaged paths are ≤140 chars from the plugin root; the packaged zip excludes
   `Binaries/Intermediate/Saved/.vs` (Fab strips them; the resolver's surviving
   `Source/ThirdParty/UnrealMcpBridge/<rid>/` copy is what remains).
4. **Modules declare their supported platforms (#187).** Both modules in `UnrealMCP.uplugin`
   (`UnrealMcpRuntime`, `UnrealMcpEditor`) carry `"PlatformAllowList": ["Win64","Mac","Linux"]`, matching
   the desktop-only target platforms (console/mobile cannot spawn the .NET sidecar, §12.5). Fab review
   expects modules to enumerate their supported platforms rather than defaulting to all.

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

Connection settings live entirely inside the single **AI Game Developer** main window — there is
**no** separate Settings nomad tab and **no** "Project → Plugins → AI Game Developer"
`ISettingsModule` section (issue #107, Unity-MCP parity: Unity holds everything in one window).
The main window's connection section owns every setting, including the read-only IPC-bridge-port
line folded in from the former Settings window's Ports row (it reuses the main window's existing
`ConnectionInfoProvider`, which already resolves the bound port). Aux windows dedupe/focus on
reopen (the known Godot [low] gets fixed here, per the aux-windows task).

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
- **Local-token lifecycle — BUG-B audit (mcp-authorize i4): absent by construction.** The Unity
  sibling (mcp-authorize i2, Unity-MCP #897) hit BUG-B: the Custom-mode local server token *silently
  regenerated* after Configure, so the already-written client `.mcp.json` bearer went stale → Claude
  Code **401**. The Unreal C++ store is immune by design, so no fix is needed:
  - **Single resolved source.** The Custom-mode secret is the one persisted `token` field
    (`FUnrealMcpConfig::CustomToken`), read via `ResolveEffectiveToken()`. Both consumers read that
    SAME value — the local-server launch arg (`FUnrealMcpEditorCoordinator` → the sidecar's shared
    `ServerLaunchArguments` builder, `auth=token token=<secret>`) and the written client bearer
    (`FAiAgentConnectionInfo::FromPluginConfig` → `AgentConfigService`) — so they can never diverge at
    a given instant.
  - **No silent regeneration.** Unlike Unity's `SetDefault` (which mis-seeded a generated secret into
    the wrong slot and forced a drifting generate-if-empty re-mint), the C++ store performs NO token
    generation on construct/load: `CustomToken` defaults empty, has no generate-if-empty fallback, and
    lives in its own JSON field (never mode-routed at (de)serialize), so it round-trips deterministically
    and survives a connection-mode re-apply unchanged.
  - **Intentional change re-syncs.** The token changes ONLY on an explicit user action (typing it, or the
    "New" button `GenerateCustomToken`); both re-persist + push the new config to the sidecar AND refresh
    the agent configurators, whose tri-state status surfaces `ReconfigureNeeded` (the shared
    `McpPlugin.AgentConfig` token-aware validator, mcp-authorize i1) so the user re-Configures.
  - Locked by the `UnrealMcp.Config` + `UnrealMcp.AgentConfigModels` Automation specs.

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
| headless e2e | script: editor + local server + sidecar | self-hosted win, release (+nightly) | `UnrealEditor-Cmd.exe <proj> -ExecCmds="Automation RunTests UnrealMcp; Quit" -ReportExportPath=<dir> -unattended -nullrhi -nosplash -log`; CI parses the exported JSON report (`index.json`) for pass/fail + per-test results instead of scraping the log; live `ping`→`pong` via `POST /api/system-tools/ping` (§2.4 — `ping` is a SYSTEM tool) |
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
  → artifact build → gated publish): artifacts = dedicated source plugin zip
  (`unreal-mcp-plugin-source-<version>.zip`, CLI/Fab install asset), packaged plugin zip
  (`unreal-mcp-plugin-<version>.zip`, BuildPlugin output with the staged bundle),
  `unreal-mcp-bridge-<rid>.zip` ×4, npm publish, GitHub Release (server zips are
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

Prompts and resources shipped **empty-but-wired** in the MVP (the framework relays them already;
counts show 0 in the UI). The **M16** task chain then added the full developer→C++→IPC→bridge→manager
registration path for **custom prompts and resources** (the appendix **§A** below is the authoritative
design), with core samples `level-design-brief` (prompt) and `unreal://project/levels` +
`unreal://project/icon` (resources). Still deferred after M16: **templated / parameterized resource URIs**
(MVP is static fixed-URI only) and **populating the §7 Prompts / Resources aux windows** (registration and
e2e work without the windows populated — see §A.5).

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

## 12. Runtime (in-game) support

Sections 0-11 describe the editor-only plugin. §12 adds runtime MCP (PIE, Standalone, packaged
Development/Shipping) at Unity-MCP parity (Unity's `UnityMcpPluginRuntime.Initialize().Build().Connect()`,
README "Runtime usage (in-game)"). Three locked decisions: (a) reuse the .NET sidecar (no in-process
C++ server); (b) new `UnrealMcpRuntime` module (Type Runtime) holds engine-agnostic machinery +
runtime-safe tools, `UnrealMcpEditor` depends on it; (c) runtime connect is explicit opt-in — never
auto-connect in a shipped game.

> **Delivery status (M15 task DAG R1–R7).** R1 (`unreal-mcp-runtime-module-extraction`) has landed the
> module + the infra move + the editor-coordinator rename + the world provider (the pure refactor below);
> R2–R7 deliver sidecar-packaging, the bootstrap subsystem, runtime tool families, the extension bus,
> testbed verification and docs. Where the text below describes the bootstrap subsystem (§12.4), sidecar
> packaging into games (§12.5), the runtime world resolver (§12.6) or the runtime tool families (§12.7),
> those are the design contracts the later R-tasks implement — R1 ships only the module skeleton, the
> moved-down infra, and the editor-side world resolver.

### 12.0 Why this is mostly a refactor
The sidecar is build-agnostic (dials loopback, stdin token, config via IPC with env fallback
`UNREAL_MCP_HOST`/`UNREAL_MCP_TOKEN`, `SidecarHost.cs:200-203`, `Program.cs:86-88`). A packaged game
reuses the identical binary. The work is C++-side: move IPC bridge server, GameThread dispatcher,
registry, schema gen, object-ref resolver, sidecar manager, config out of the editor module into a
Runtime module; abstract the GEditor/world couplings; add an explicit opt-in bootstrap. Most editor
subsystems already depend only on runtime-safe modules (Core/CoreUObject/Engine/Networking/Sockets/
Json/JsonUtilities/Projects).

### 12.1 The `UnrealMcpRuntime` module
New module under `UnrealMCP/Source/UnrealMcpRuntime/`. `.uplugin` (Runtime module FIRST so it loads
before the editor module that depends on it):
```json
{ "Name": "UnrealMcpRuntime", "Type": "Runtime", "LoadingPhase": "Default" },
{ "Name": "UnrealMcpEditor",  "Type": "Editor",  "LoadingPhase": "Default" },
{ "Name": "UnrealMcpEditorTests", "Type": "Editor", "LoadingPhase": "Default" }
```
Type Runtime (loads in editor/PIE/Standalone/packaged — Unity `includePlatforms:[]` analog).
LoadingPhase Default (the bootstrap subsystem defers its own work, §12.4).

`UnrealMcpRuntime.Build.cs` deps:
- Public: `Core`
- Private: `CoreUObject`, `Engine`, `Networking`, `Sockets`, `Json`, `JsonUtilities`, `Projects`,
  `ImageWrapper`, `RenderCore`, `RHI` (runtime screenshot subset).
- `RuntimeDependencies.Add(.../UnrealMcpBridge/<rid>/*, StagedFileType.NonUFS)` MOVES here from the
  editor Build.cs (§12.5) so the sidecar bundles into packaged GAMES. **(R2 — in R1 it stays in the
  editor Build.cs.)**

STAYS editor-only in `UnrealMcpEditor.Build.cs`: UnrealEd, Slate, SlateCore, EditorSubsystem,
WorkspaceMenuStructure, InputCore, ApplicationCore, AssetRegistry, AssetTools, MaterialEditor,
EditorScriptingUtilities, BlueprintGraph, KismetCompiler, HTTP, FileUtilities, HTTPServer,
LiveCoding(Win64). `UnrealMcpEditor` adds `"UnrealMcpRuntime"` to PublicDependencyModuleNames.
Public headers move to `UnrealMcpRuntime/Public/`: `UnrealMcpToolRegistry.h`, `IUnrealMcpToolProvider.h`,
`UnrealMcpLog.h` (plus a runtime-owned `UnrealMcpRuntimeCoreTools.h` declaring the runtime-safe `ping`
Register). `UnrealMcpCoreTools.h` stays editor (the editor-only families' Register declarations). Single
`LogUnrealMcp` category moves down.

### 12.2 What moves DOWN vs stays editor-only
MOVES to UnrealMcpRuntime (from `UnrealMcpEditor/Private/`): `Bridge/UnrealMcpBridgeServer.*`+
`Bridge/UnrealMcpNdjson.*`; `Dispatch/UnrealMcpGameThreadDispatcher.*` (no GEditor);
`Tools/UnrealMcpToolRegistry.cpp`+`Public/UnrealMcpToolRegistry.h`; `Tools/UnrealMcpObjectRef.*`
(world abstraction via §12.6); `Tools/UnrealMcpPropertyJson.*`+`Tools/UnrealMcpAssetScopedRead.*`;
`Sidecar/UnrealMcpSidecarManager.*`; `Config/UnrealMcpConfig.*`; `Extensions/UnrealMcpExtensionManager.*`+
`Public/IUnrealMcpToolProvider.h`; `Tools/UnrealMcpLogCollector.*`; `Tools/UnrealMcpPingTool.cpp`;
`UnrealMcpLog.*`.
STAYS editor-only: all `UI/**`, `Server/UnrealMcpServerManager.*`, `DevControl/*`,
`UnrealMcpEditorViewModel.*`; families `UnrealMcpAssetTools.cpp`, `UnrealMcpBlueprintTools.cpp`,
`UnrealMcpSourceTools.*`, `UnrealMcpLevelTools.cpp`, `UnrealMcpScreenshotTools.cpp`, `UnrealMcpEditorTools.cpp`
and `UnrealMcpActorTools.cpp`; the renamed `FUnrealMcpEditorCoordinator`.

> **R1 scope note (ActorTools).** The design's eventual target makes `UnrealMcpActorTools.cpp`
> runtime-safe (`#if WITH_EDITOR`-guard its `FScopedTransaction` / `FActorLabelUtilities::SetActorLabelUnique`
> / `Editor.h` couplings; resolve the world via §12.6). That transform — together with the runtime
> console/reflection subset, the runtime screenshot subset and `level-get-data` — is **R4**
> (`unreal-mcp-runtime-tool-families`). R1 therefore leaves ActorTools (and all other families) in the
> editor module so the runtime module stays UnrealEd-free and GEditor-free (§12.1 dep list, R1 grep gate);
> ActorTools simply consumes the now-moved-down ObjectRef/PropertyJson via the runtime module. Moving it
> down before its WITH_EDITOR guards exist would force UnrealEd into the runtime Build.cs, contradicting
> §12.1.

Must-abstract GEditor couplings (the true blockers, handled per task): (1) `FUnrealMcpObjectRef::
GetEditorWorld()` — done in R1 via §12.6; (2) actor spawn `World->SpawnActor` (already the engine path;
`FScopedTransaction`/`SetActorLabel` `#if WITH_EDITOR` in R4); (3) `UnrealMcpEditorTools.cpp` PIE +
selection stay editor; `console-run-command` (`GEngine->Exec`) runtime-safe (R4).

### 12.3 Layering: editor depends on runtime — **Model A (chosen): single runtime-owned bridge**
The runtime module always constructs registry+dispatcher+bridge+sidecar (editor AND game) and registers
the runtime built-in (`ping` only — §12.7). The editor coordinator, when present, registers the
editor-only engine-development families on top of the SAME registry, keeps Slate/view-model/server-manager/
dev-control, and wires existing UI sinks to the runtime bridge. Connect policy differs (editor: config/UI;
game: opt-in §12.4) but is a connect TRIGGER, not a separate bridge. Max parity, one code path. (Rejected
Model B: two coordinators.)

> **R1 realization.** The renamed `FUnrealMcpEditorCoordinator` (was the misnamed `FUnrealMcpRuntime`,
> §12 executive finding 3) lives in the editor module and *builds* the runtime-owned types (registry,
> dispatcher, bridge, sidecar, extension manager, config — all now `UNREALMCPRUNTIME_API`-exported) then
> layers the editor families + Slate UI on the same registry. The standalone runtime bootstrap that builds
> the same stack without an editor is R3.

`UnrealMcpEditorTests` private-include reach-ins repoint by adding `UnrealMcpRuntime` to the test
Build.cs PrivateDependencyModuleNames + `$(ModuleDir)/../UnrealMcpRuntime/Private` to PrivateIncludePaths
(keeping the editor Private path for the editor-staying reach-ins). Keep the `UnrealMcp.` filter prefix in
R1 to prove equivalence. Unity-build ODR rule holds (unique helpers).

### 12.4 Runtime bootstrap API (explicit opt-in) — **R3**
A **`UGameInstanceSubsystem`** — auto-instantiated per UGameInstance but NEVER auto-connecting:
```cpp
UCLASS() class UNREALMCPRUNTIME_API UUnrealMcpRuntimeSubsystem : public UGameInstanceSubsystem {
  GENERATED_BODY()
public:
  virtual void Initialize(FSubsystemCollectionBase&) override; // builds registry/dispatcher/bridge — NO connect
  virtual void Deinitialize() override;                         // orphan-safe sidecar teardown
  UFUNCTION(BlueprintCallable, Category="Unreal MCP")
  bool Connect(const FString& Host, const FString& Token=TEXT(""),
               EUnrealMcpRuntimeConnectionMode Mode=EUnrealMcpRuntimeConnectionMode::Custom);
  UFUNCTION(BlueprintCallable, Category="Unreal MCP") void Disconnect();
  UFUNCTION(BlueprintCallable, BlueprintPure) bool IsConnected() const;
  static UUnrealMcpRuntimeSubsystem* Get(const UObject* WorldContext);
};
```
`Initialize` builds registry+runtime families+dispatcher+bridge, generates token, `BridgeServer->Start`
(listener armed) — does NOT spawn sidecar or connect. `Connect()` spawns the sidecar
(`SidecarManager->StartForPort`) and pushes a `config` IPC msg `keepConnected:true` + resolved
host/token/mode (reuse `BuildEffectiveConnectionConfig`). Defer sidecar spawn to Connect (a game that
never calls Connect spawns zero child procs). Also: a Blueprint `Connect MCP` node; console commands
`UnrealMcp.Connect <host> [token]` / `UnrealMcp.Disconnect` (QA without recompile). REJECT any
config-asset that auto-connects.

### 12.5 Sidecar packaging into non-editor builds — **R2**
Today `RuntimeDependencies.Add(.../UnrealMcpBridge/.../*)` is in `UnrealMcpEditor.Build.cs` → stages
into editor packages but NOT packaged games. **Move it to `UnrealMcpRuntime.Build.cs`** → UBT stages the
bridge into packaged Development/Shipping at `<Staged>/<Project>/Plugins/UnrealMCP/Binaries/ThirdParty/
UnrealMcpBridge/<rid>/`. `ComposeBundledBridgePath` resolves via `IPluginManager...FindPlugin("UnrealMCP")
->GetBaseDir()` — runtime-available, so `ResolveBridgeBinaryPath` works in a packaged game UNCHANGED.
Only Build.cs ownership moves. Risks: self-contained ~73-80 MB/RID — gate staging behind a target/config
switch. **Console/mobile cannot spawn an external .NET process → runtime MCP is Desktop-only
(Win64/Mac/Linux).**

### 12.6 GameThread dispatch + world resolution under a live game
Dispatcher already runtime-clean — moves down unchanged. World resolution → a provider in the runtime
module:
```cpp
namespace FUnrealMcpWorldProvider {
  UNREALMCPRUNTIME_API UWorld* GetActiveWorld();
  UNREALMCPRUNTIME_API void SetWorldResolver(TFunction<UWorld*()>);
  UNREALMCPRUNTIME_API void ClearWorldResolver();
}
```
**R1 ships this provider** and rewires `FUnrealMcpObjectRef::GetEditorWorld()` → `GetActiveWorld()`
delegating to the injected resolver. The EDITOR coordinator installs
`[]{return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;}` in Startup (preserves today's
behaviour byte-for-byte) and clears it in Shutdown — so the runtime module itself holds NO GEditor
reference. The RUNTIME subsystem (R3) installs `[this]{return GetGameInstance()->GetWorld();}` instead.
PIE subtlety (verify on 5.7): the editor resolver may later return `GEditor->PlayWorld` when set, so
editor-driven tools follow the user into PIE (matches Unity) — deferred until the runtime families need it.

### 12.7 Runtime built-in tool set — `ping` only (R4 reverted, issue #141)

**Decision (current): the runtime module ships exactly ONE built-in tool — `ping`.** Every
engine-development family (actor/component, console/reflection, screenshot, level read+write, blueprint,
asset, source, editor-application/selection) is **editor-only**, registered by the editor coordinator from
the editor module. A game obtains runtime tools by **bringing its own** — registering an
`IUnrealMcpToolProvider` via `UUnrealMcpRuntimeSubsystem::RegisterToolProvider` (the §12.9 extension bus).
The runtime module = lean infra (bridge / registry / dispatcher / sidecar / world-provider /
extension-manager) + `ping`. Parity is "framework + your custom tools," not "a subset of the built-ins
in-game."

> **§2.4 consequence:** `ping` is a SYSTEM tool, so it is served at `/api/system-tools/ping` and is NOT
> advertised in `tools/list`. A packaged game that registers no tools of its own therefore advertises an
> EMPTY tool list to an AI agent — which is the honest description of it: there is nothing an agent can
> do there yet. `ping` remains fully reachable for liveness probing, and the runtime registry still
> contains exactly `{ping}` (the deterministic `UnrealMcp.RuntimeSubsystem` gate asserts the REGISTRY, so
> it is unaffected). The moment the game registers an `IUnrealMcpToolProvider`, its tools appear normally.

Rationale: the engine-development tools are AI **authoring / inspection / dev** tools — and several are
RCE-class (`reflection-method-call`, `console-run-command`, arbitrary `object-modify`/`actor-*`). They
operate the editor and do not belong compiled into a shipped game by default. Keeping them editor-only
removes that surface from a packaged game entirely (rather than gating it behind §12.8's runtime
mitigations) and keeps the runtime module dependency-lean (no UnrealEd, no ImageWrapper/RenderCore/RHI).

> **History — R4 (PRs #121 c91f206 / #122 38667d3), now reverted.** R4 moved a "runtime-safe" subset
> (actor/component, a console/reflection subset, a `screenshot-game-view`/`screenshot-camera` subset, and
> read-only `level-get-data` — ~22 built-ins total) DOWN into the runtime module so they worked over a
> runtime connection, behind `World->SpawnActor` + `WITH_EDITOR` seams (no-op `FScopedTransaction`,
> `AActor::SetActorLabel` for `FActorLabelUtilities::SetActorLabelUnique`, `GetActorNameOrLabel` for the
> WITH_EDITOR-only `GetActorLabel`, `GEngine->GameViewport` for `GEditor->GetPIEViewport()`, a
> non-`CallInEditor` reflection gate). Issue #141 reverts that in intent: those families are restored to
> their editor-only implementations and registrations (the actor family `git mv`'d back to the editor
> module; console/reflection + `level-get-data` re-merged into `UnrealMcpEditorTools.cpp` /
> `UnrealMcpLevelTools.cpp`; the editor 4-tool screenshot family — `screenshot-viewport`, the PIE
> `screenshot-game-view`, `screenshot-camera`, `screenshot-isolated` — restored; the runtime
> `UnrealMcpRuntimeScreenshotTools.cpp` dropped). The editor still exposes all 62 built-ins across 8
> families, unchanged in count — only their module home and the runtime registration reverted. (Those are
> the PRE-§2.4 counts, correct as of #141; `ping` has since moved to the SYSTEM surface, leaving 61
> standard tools across 7 families — see §2.4.)

"the runtime built-in manifest is exactly `{ping}`; every engine-development tool is absent" is a
deterministic Automation gate (`UnrealMcp.RuntimeSubsystem` → "runtime manifest separation (ping-only
built-in)").

### 12.8 Security analysis (shipped-game remote-control surface)
A runtime connection is remote control of a running game (actor-create, object-modify, console-run-command
arbitrary CVars, reflection-method-call arbitrary UFunctions) — RCE-class if reachable. Mitigations (ALL in design):
1. **Explicit opt-in only** (the invariant): subsystem auto-instantiates but NEVER auto-connects. Any path
   that connects without an explicit developer call is a security defect.
2. **Loopback IPC + one-shot stdin token** (unchanged §1.4).
3. **Build-config gating:** `bUnrealMcpAllowShipping` Build.cs flag (default false) → with 0, Connect in
   Shipping logs+returns false.
4. **Kill switch:** `UUnrealMcpRuntimeSettings:UDeveloperSettings` `bRuntimeMcpEnabled` default FALSE.
5. **Loopback-host default:** Connect rejects non-loopback hosts unless explicit `bAllowRemoteHost`.
6. No token in argv/logs.

**Strongest footprint mitigation — consumer-side editor-only deny-list.** An editor-only consumer can
remove the entire shipped-game surface (mitigations 1–6 become moot) by pinning the plugin to the editor
in *their own* `.uproject` reference: `{ "Name": "UnrealMCP", "Enabled": true, "TargetDenyList": ["Game",
"Client", "Server"] }`. UE honours `TargetDenyList`/`TargetAllowList` on a plugin reference
(`PluginReferenceDescriptor::IsEnabledForTarget`), so the `UnrealMcpRuntime` module + the bundled sidecar
`RuntimeDependencies` (§12.5) are excluded from packaged `Game`/`Client`/`Server` builds entirely — zero
compiled footprint. This is a **consumer-side** opt-out (not a plugin default, since the plugin must stay
runtime-shippable for §12.7 in-game usage); a direct `*.Build.cs` dependency on an `UnrealMcp*` module
from a game module overrides the deny-list. See README → [Editor-only — exclude Unreal-MCP from packaged
games](../README.md#editor-only-exclude-from-packaged-games) for the full recipe and caveat.

### 12.9 Runtime extension tool registration
`IModularFeatures` (§5) is runtime-available — `IUnrealMcpToolProvider` works identically at runtime; the
extension manager moves down (R1). Recommended ergonomic: `UUnrealMcpRuntimeSubsystem::RegisterToolProvider(
IUnrealMcpToolProvider*)` (R3+). UE analog of Unity `WithToolsFromAssembly`/`[AiTool]` scan (README Chess
example). Subsystem rebuilds registry + re-pushes manifest on register/unregister.

### 12.10 Compatibility / version-floor
UGameInstance::GetWorld, UGameInstanceSubsystem, UWorld::SpawnActor, USceneCaptureComponent2D,
UGameViewportClient stable 5.5→5.7. Screenshot APIs drift slightly — version-check runtime variants on 5.5.
The 5.5 floor must be EXERCISED (packaged Development build on a 5.5 install) before claiming it.

---

## 13. Open questions for TD / Ivan (none block the scaffold or sidecar tasks)

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

---

## Appendix A — Custom prompts & resources (M16)

> Authoritative design for the **M16** prompt/resource registration path — the exact siblings of the §2/§3/§5
> tool path. M16 added the developer→C++→IPC→bridge→manager half on top of the existing empty-but-wired
> server↔sidecar relay, consuming `com.IvanMurzak.McpPlugin` (`IPromptManager` / `IResourceManager` /
> `IRunPrompt` / `IRunResource`) **as-is** — no upstream MCP-Plugin-dotnet change was required (the public
> manager surface already mirrors the tools path `ProxyTool` rides). The C++ identifiers below are shipped in
> `UnrealMcpRuntime/Public/`; the bridge types in `bridge/src/`.

### A.1 IPC protocol (the §1.3 analog)

Prompts/resources reuse the §1.2 NDJSON framing, the §2.2 revision-guarded full-snapshot diff, and the §1.5
reconnect re-push. `ipcVersion` was bumped **1 → 2**; the handshake negotiates it, so an old sidecar/plugin
pair still does tools-only. The new `type` discriminators are in the §1.3 message-schema table:

- plugin → sidecar: `prompt-manifest {revision, prompts[]}`, `resource-manifest {revision, resources[]}`.
- sidecar → plugin: `prompt-get {requestId, prompt, arguments, timeoutMs}`, `resource-read {requestId, uri, timeoutMs}`.
- plugin → sidecar: `prompt-response {requestId, status, messages[]}` (≈ `ResponseGetPrompt`),
  `resource-response {requestId, status, contents[]}` (≈ `ResponseResourceContent[]`).
- The generic `tool-cancel` (by `requestId`) is reused for both — no new cancel verbs.

**Prompt descriptor**: `{name (kebab), title, description, role ("user"|"assistant"), inputSchema (JSON Schema
→ the manager flattens to `arguments[]`), enabled, extensionId, schemaHash}`.
**Resource descriptor**: `{uri (→ the MCP route), name, description, mimeType, enabled, extensionId, schemaHash}`.

**Content**: a resource read returns one or more content blocks; each is `Text` (string) **XOR** `Blob`
(base64) + `mimeType`, so binary content (e.g. an image) rides as base64 exactly like a screenshot tool's
image payload.

**Scope (MVP)**: static fixed-URI resources only. Templated / parameterized resource URIs are **deferred**
(upstream McpPlugin has `RunResourceTemplates` / `ResponseResourceTemplate`, later-additive). The
list-changed notifications (`notifications/{prompts,resources}/list_changed`) are IN scope and free — the
manager owns them: `AddPrompt` / `AddResource` fire `On*Updated`, so the bridge needs zero push machinery
(same as tools).

### A.2 C++ surface (Model-A: contracts + registries + runtime families in `UnrealMcpRuntime`)

The prompt/resource contracts and registries live in the **runtime module** (re-exported by `UnrealMcpEditor`),
so the same headers serve editor and runtime extensions — exactly like the tool path.

- **Providers** — `IUnrealMcpPromptProvider` / `IUnrealMcpResourceProvider` (mirror `IUnrealMcpToolProvider`).
  Modular-feature names `UnrealMcpPromptProvider` / `UnrealMcpResourceProvider`; declaration entry points
  `RegisterPrompts(FUnrealMcpPromptRegistry&)` / `RegisterResources(FUnrealMcpResourceRegistry&)`. Both carry
  the same `GetExtensionId()` / `GetDisplayName()` / `GetExtensionVersion()` triple as the tool provider. One
  plugin may implement all three contracts.
- **Registries + builders** —
  - `FUnrealMcpPromptRegistry` / `FUnrealMcpPromptBuilder`: `Registry.Prompt("id").Title().Description().Role()
    .ParamString/Int/Number/Bool/Vector/Param(...).Handle(call → FUnrealMcpPromptResult)`. The role enum is
    **`EUnrealMcpPromptRole { User, Assistant }`**. The result is role-tagged messages
    (`FUnrealMcpPromptResult::Success(text, role, description)` / `::Error(description)`). Prompt arguments
    **reuse `FUnrealMcpParamSpec` + `EUnrealMcpParamRequirement` + the §3.2 schema generation verbatim**, and
    a prompt handler receives the same `FUnrealMcpToolCall` args+cancel surface as a tool handler.
  - `FUnrealMcpResourceRegistry` / `FUnrealMcpResourceBuilder`: `Registry.Resource("unreal://uri").Name()
    .Description().MimeType().Read(handler(uri) → FUnrealMcpResourceResult)`. The result is content blocks
    (`FUnrealMcpResourceResult::Text(uri, text, mimeType)` / `::Blob(uri, base64, mimeType)` /
    `::Success(contents)` / `::MakeError(error)`). A resource's **URI is its identity** (the registry key and
    the MCP route).
  - Both registries clone the tool registry's machinery: monotonic `Revision`, `BuildManifestJson()`,
    `Execute` / `Read`, `RegisterExtension` / `Remove*ForExtension`, `ComputeSchemaHash`, descriptor
    validation, and the §7/§8 enable filters (`Set*EnabledFilter` whitelist + `ApplyDisabled*` blocklist).
    They reuse the tool registry's `FUnrealMcpExtensionRegistrationResult` verbatim.
- **Runtime subsystem** — `UUnrealMcpRuntimeSubsystem::RegisterPromptProvider` / `UnregisterPromptProvider`
  and `RegisterResourceProvider` / `UnregisterResourceProvider` (thin `IModularFeatures::RegisterModularFeature`
  wrappers with identical not-owned semantics to `RegisterToolProvider`).
- **Extension manager** — `FUnrealMcpExtensionManager` is **kind-aware** (three registries + three
  modular-feature subscriptions) rather than triplicated, so its re-entrancy guard / deferred-rebuild /
  disabled-set persistence / ExtensionId sort are shared and load-bearing for §5 isolation. `OnChanged` fires
  the tool, prompt, and resource manifest pushes together.
- **Coordinator + bridge server** — the editor coordinator (and the runtime subsystem's PImpl) builds the
  prompt + resource registries alongside the tool registry, registers the core families, and hands all three
  to the single `FUnrealMcpBridgeServer`, which adds prompt/resource manifest pushes (on Ready + OnChanged)
  and `HandlePromptGet` / `HandleResourceRead` marshalling through the **existing**
  `FUnrealMcpGameThreadDispatcher` (§4 game-thread, no-modal-UI). No new threads or sockets.

### A.3 Bridge (mirror `ProxyTool` / `ProxyToolFactory` / `ManifestRegistrar`)

- **`ProxyPrompt : IRunPrompt`** — schema-blind; `Run` round-trips `prompt-get` → `ResponseGetPrompt`,
  fail-fast on `IpcDisconnectedException`. Built by `ProxyPromptFactory`.
- **`ProxyResource : IRunResource`** — composes `ProxyResourceContent : IRunResourceContent`
  (`Run` → `resource-read` → `ResponseResourceContent[]`) + `ProxyResourceList : IRunResourceList` (static
  MVP: a 1-element `ResponseListResource[]` synthesized from the descriptor, no IPC). Built by
  `ProxyResourceFactory`.
- **Registrars** — `PromptManifestRegistrar` / `ResourceManifestRegistrar` clone `ManifestRegistrar` (revision
  guard, `ResetForReconnect`, add/remove/changed/enabled diff). Their sinks (`PromptManagerSink` over
  `IPromptManager` / `ResourceManagerSink` over `IResourceManager`) forward to the manager's
  `AddPrompt(prompt)` / `AddResource(resource)` — note the **runner-only** manager signature (the
  name/route comes from the runner itself), unlike the tool path's `AddTool(name, runner)`.
- **`SidecarHost.Build()`** grabs `_plugin.McpManager.PromptManager` / `.ResourceManager` (both **nullable** →
  guard + log; default-constructed empty by `McpPluginBuilder.Build`, then filled by the registrar — the same
  empty-then-manifest model as tools), builds the registrars, and wires them to the IPC client. `IpcClient`
  routes the inbound manifests and owns the outbound `prompt-get` / `resource-read` channels (reusing the
  existing `PendingCallRegistry`).

### A.4 Schema / serialization

Prompt arguments ride `FUnrealMcpParamSpec` → JSON Schema (§3.2, unchanged); resource content uses
Text/Blob(base64) + mimeType; everything travels over the existing camelCase NDJSON `IpcProtocol.JsonOptions`.
No new serializer configuration.

### A.5 UI angle (deferred)

Populating the §7 empty **Prompts** / **Resources** aux windows from each registry's `BuildManifestJson()`
(parity with the Tools window) is a **follow-up** (M16-P4): name/title/description + role for prompts,
uri + mimeType for resources, plus an enable toggle. Registration and end-to-end use work **without** the
windows populated — the windows still render their subdued empty-state in this release.
