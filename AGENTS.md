# AGENTS.md — Unreal-MCP

Unreal-MCP connects MCP clients (Claude, Cursor, Copilot, …) to [Unreal Engine](https://www.unrealengine.com/)
— the Unreal sibling of Unity-MCP and Godot-MCP. **Editor mode** (default) drives the Unreal Editor;
opt-in **runtime mode** drives a running PIE / Standalone / packaged Development build. Status: beta.
The MCP server is the shared [GameDev-MCP-Server](https://github.com/IvanMurzak/GameDev-MCP-Server)
(`gamedev-mcp-server`); **no server code lives here** — server changes go to that repo.

**[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) is authoritative** (defer to it on conflict; read the
relevant section before non-trivial work): IPC §1, tool registration §2, schema §3, dispatcher §4,
extensions §5, sidecar §6, UI §7, config §8, repo/CI §9, roadmap §10, risks §11, runtime §12, prompts &
resources §A. Other docs: [`README.md`](README.md) (users; canonical tool inventory),
[`docs/RELEASING.md`](docs/RELEASING.md), [`docs/EXTENSIONS.md`](docs/EXTENSIONS.md) (extension
authors), [`docs/RUNTIME-E2E.md`](docs/RUNTIME-E2E.md) (in-game walkthrough).

## Architecture

Unreal is C++ and cannot host the .NET McpPlugin/ReflectorNet/SignalR stack in-process, so it runs in an
auto-managed **.NET 9 sidecar** (`unreal-mcp-bridge`, `bridge/`), a local child of whatever loads the
plugin (Editor, or the game in an opt-in packaged build). The C++ plugin **listens** on loopback TCP; the
sidecar **dials** it with a one-shot token sent over **stdin** (never argv, never logged — §1.4) and
relays IPC ⇄ SignalR to the MCP server (cloud `ai-game.dev` by default, or a local `gamedev-mcp-server`).

Do not conflate the two child processes: `unreal-mcp-bridge` (always, when connected) and
`gamedev-mcp-server` (Custom/local mode only; downloaded by the CLI, pinned by
`cli/src/lib/server-version.ts` `SERVER_VERSION`, overridden by `UNREAL_MCP_SERVER_PATH`; the editor can
Start/Stop it via `FUnrealMcpServerManager`, Custom + http only).

**Runtime mode (§12).** Locked decisions: reuse the sidecar (no in-process C++ server); runtime infra +
`ping` live in `UnrealMcpRuntime`; a shipped game **never auto-connects**. `UUnrealMcpRuntimeSubsystem`
(a `UGameInstanceSubsystem`) only arms the listener; a connection is an explicit
`Connect(Host, Token, Mode, bAllowRemoteHost)` (C++, Blueprint, or `UnrealMcp.Connect` console command).
The runtime built-in surface is **`ping` only** (a system tool, so `tools/list` is legitimately empty;
the `UnrealMcp.RuntimeSubsystem` spec asserts the registry is exactly `{ping}`); all engine-development
tools are editor-only, and games bring their own via `IUnrealMcpToolProvider`. Security gates, all in
`Connect()` (§12.8): opt-in only; kill switch `UUnrealMcpRuntimeSettings::bRuntimeMcpEnabled`
(Project Settings → Plugins → Unreal MCP (Runtime), `DefaultGame.ini`, default false); Shipping gate `bUnrealMcpAllowShipping` in `*.Build.cs`
(default false → `UNREAL_MCP_ALLOW_SHIPPING=0`, also controls staging the sidecar into Shipping);
loopback host unless `bAllowRemoteHost=true`; loopback IPC + stdin token. Desktop only (Win64/Mac/Linux).
Consumers exclude the plugin from packaged games with a `TargetDenyList: ["Game","Client","Server"]` in
**their own `.uproject`** plugin reference (not our `.uplugin`); any game-module dependency on an
`UnrealMcp*` module overrides that.

## Layout

| Path | What lives there / where new files go |
| --- | --- |
| `UnrealMCP/` | The UE plugin (modules, conventions, UE build/test: [`UnrealMCP/AGENTS.md`](UnrealMCP/AGENTS.md)) |
| `bridge/` | .NET 9 sidecar + xUnit tests ([`bridge/AGENTS.md`](bridge/AGENTS.md)) |
| `cli/` | `unreal-mcp-cli` npm package (TypeScript, commander, vitest): commands in `cli/src/commands/`, logic in `cli/src/lib/`, tests in `cli/tests/`. Published on npm; CI owns releases |
| `samples/` | Example extension plugins: `UnrealAITemplate/` (editor, §5) and `UnrealAIRuntimeSample/` (runtime, §12.9) |
| `commands/` | Repo PowerShell tooling: `bump-version.ps1`, `release-github.ps1`, `test-module-uplugin.ps1` |
| `scripts/` | Python smokes run by CI (`connection_smoke.py`, `server_smoke.py`, `window_smoke.py`) |
| `tests/chain-fixtures/` | Byte-compared T2 chain fixtures (LF-pinned in `.gitattributes`) |
| `.github/` | Workflows + CI helper scripts (`.github/scripts/`, e.g. the UE machine lock) |
| `docs/` | Design, release and operator docs; images in `docs/img/` |
| `build/` | macOS signing entitlements |

## Build / test (run only the legs you touch)

- **UE plugin** (`UnrealMCP/**`): see [`UnrealMCP/AGENTS.md`](UnrealMCP/AGENTS.md).
- **bridge** (`bridge/**`): `dotnet restore|build|test bridge/Unreal-MCP-Bridge.sln` (Debug; `--no-restore` / `--no-build`).
- **cli** (`cli/**`, Node 20.19+/22.12+): `cd cli && npm install && npm run build && npm test`.
- **Live bridge e2e** (any tool-family change): server ⇄ bridge ⇄ headless editor over HTTP. Boot the
  editor with `UNREAL_MCP_BRIDGE_PATH`, `UNREAL_MCP_HOST=http://localhost:<port>` **and**
  `UNREAL_MCP_CONNECTION_MODE=Custom` (without it the sidecar dials Cloud and the server returns HTTP 500
  `No connected clients`). Assert tool sets via MCP `tools/list` on `POST /mcp`, not
  `/api/tools/<name>` (one tool only; drops `content[]` images). `/api/tools/<name>` needs
  `-H "Content-Type: application/json"`.

## Tool / prompt / resource surface

Built-in editor tools come in 7 families; the README holds the canonical, source-generated inventory.
Tool ids are kebab-case, validated `^[a-z0-9]+(-[a-z0-9]+)*$`. Each tool is `Standard` (`tools/list` +
`/api/tools/<name>`) or `System` (`/api/system-tools/<name>` only, never shown to the AI — §2.4), declared
with `FUnrealMcpToolBuilder::ToolType(EUnrealMcpToolType::System)` and routed by the sidecar's
`SurfaceRoutingToolSink`. System tools: `ping`, `unreal-skill-create` (writes a C++ tool file into
`UnrealMcpEditor/Private/Tools/Skills/`; live only after a rebuild), `unreal-skill-generate`
(sidecar-side, `bridge/src/Tools/SkillGenerateTool.cs`). Core prompt `level-design-brief`; core resources
`unreal://project/levels`, `unreal://project/icon` (fixed URIs only).

## Versioning

One semver for plugin / bridge / cli; `UnrealMCP/UnrealMCP.uplugin` `VersionName` is the source of truth.
`commands/bump-version.ps1` rewrites it plus the bridge csproj `<Version>`, `cli/package.json` (+ lock) and
the `cli/src/version.ts` `PACKAGE_VERSION` constant (no runtime `package.json` read — it breaks bundled binaries).
**Never bump from a feature task or hand-edit one alone** — the release flow owns versions
([`docs/RELEASING.md`](docs/RELEASING.md)). The consumed server version (`SERVER_VERSION`) is pinned
separately and diverges on purpose; bump it only after that shared release exists. The plugin↔sidecar
handshake is independent of it. NuGet pins: see `bridge/AGENTS.md`.

## Conventions

- **Naming:** plugin `UnrealMCP`; .NET namespace `com.IvanMurzak.Unreal.MCP.Bridge`; C++ rules in
  `UnrealMCP/AGENTS.md`.
- **Secrets:** `.env` stays gitignored (may hold `UNREAL_MCP_TOKEN`).
- **Commits:** conventional `<type>(<scope>): <description>` (scopes: plugin, bridge, cli, ipc, tools,
  dispatcher, schema, ui, sidecar, config, runtime, samples, ci, docs); `Closes #N`. Never `git add -A`.
  Never commit `Binaries/`, `Intermediate/`, `Saved/`, `bin/`, `obj/`, `node_modules/`, `dist/`, the
  `Source/ThirdParty/UnrealMcpBridge/<rid>/` payloads, or a generated `.sln` at a `.uproject` root
  (`bridge/Unreal-MCP-Bridge.sln` is the tracked exception).

## CI

`test_pull_request.yml` (`test-pull-request`) on PRs into `main`: bridge build + xUnit (ubuntu + windows),
`test-cli` on Node 20 and 22 (`test_cli.yml`), and two UE jobs — `plugin BuildPlugin + Automation` and
`connection + tool smoke` (matrix `ue: ['5.8']`, `UE_ROOT` derived from it). UE jobs run on self-hosted
Windows runners (each job asks for its engine's label, e.g. `unreal-5-8`), serialized by the machine-wide lock
`.github/scripts/ue-machine-lock.ps1` ([RELEASING](docs/RELEASING.md#machine-wide-ue-lock)), and are
gated on `vars.UNREAL_RUNNER_READY` / `vars.UNREAL_SMOKE_READY == 'true'` and same-repo PRs — otherwise
SKIPPED, never red. `release.yml` is `workflow_dispatch`-only (`dry_run` input; validates UE 5.5–5.8) and
publishes nothing on merge. Keep this file and `docs/RELEASING.md` in sync with the workflows.
