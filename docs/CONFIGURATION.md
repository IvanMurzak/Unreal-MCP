<b>[English](CONFIGURATION.md) | [中文](CONFIGURATION.zh-CN.md) | [日本語](CONFIGURATION.ja.md) | [Español](CONFIGURATION.es.md) | [فارسی](CONFIGURATION.fa.md) | [हिन्दी](CONFIGURATION.hi.md) | [العربية](CONFIGURATION.ar.md)</b>

# Configuration, updates and troubleshooting

How to connect the editor, keep the plugin current, tune it with environment variables, and fix the common problems. Install steps are in the [README](../README.md#quick-start); the design behind all of this is in [ARCHITECTURE.md](ARCHITECTURE.md).

- [First run](#first-run)
- [Updating the plugin](#updating-the-plugin)
- [Environment variables](#environment-variables)
- [Troubleshooting](#troubleshooting)

## First run

1. Open the **AI Game Developer** main window from the editor's **Tools** menu (the tab is registered under the Tools menu category).
2. **Choose a connection mode:**
   - **Cloud** (default) — connects to [ai-game.dev](https://ai-game.dev). Click **Authorize** to start the OAuth **device-code flow**: the window shows a verification URL and a short user code; open the URL, enter the code, approve, and the editor finishes authorizing. Use **Revoke** to clear the stored cloud token. Cloud is the default because it needs no open ports and avoids port conflicts with other programs; the connection is free. The relay stores no request or response content, only metadata, for 90 days ([README](../README.md#what-the-cloud-relay-sees)).
   - **Custom** — connects to a local `gamedev-mcp-server` you run (or any compatible server); no account, works offline and on air-gapped machines. Enter the server URL and point your AI client at it. Run the server yourself, or let the editor start and stop it (`UNREAL_MCP_START_SERVER`, Custom mode + http only; see [Environment variables](#environment-variables)).
3. The **Connection** section shows a status dot, a status label, and a Connect / Disconnect / Stop button; the bridge status reads `Running (restarts: N)` or `Stopped`. Use them to confirm the sidecar is live.
4. Point your AI client (Claude Code, Cursor, the AI Game Developer app, …) at the server. The **AI agents** section lists the agents currently connected; to write an MCP client config use `unreal-mcp-cli setup-mcp`.

Connection settings persist to `<Project>/Saved/Config/UnrealMcp/ai-game-developer-config.json` (`Saved/` is gitignored by every UE template, so tokens never land in VCS by default).

> **Pinned MCP client URL.** `unreal-mcp-cli setup-mcp <agent>` writes an MCP client config that points at the **project-pinned** cloud URL `<base>/mcp/p/<pin>`, so the agent routes to **this** project's editor even when your account drives several. Pass `--no-pin` to write the bare `<base>/mcp` URL instead. The pin is a routing path segment only — the OAuth resource stays `<base>/mcp`.

> **Project key.** When this machine is signed in (`unreal-mcp-cli login`, or the editor's Sign in), a Cloud config also carries `Authorization: Bearer agd_pk_…` — a non-expiring, revocable key bound to this project's pin — for every agent, so no agent needs its own OAuth. The key is cached in `~/.ai-game-dev/project-keys.json` and reused. Pass `--oauth` for a URL-only config (the agent signs in itself), or `--regenerate-key` to mint a new key and revoke the old one (the editor's AI agent panel has a **Regenerate key** button). Not signed in, or the server cannot mint a key ⇒ the URL-only config.

## Updating the plugin

Updating in place must always leave you running the **new** code. The risk is UE's incremental compiler: if the plugin source changes (new `.cpp` files, a new module) but the old `UnrealMCP/Intermediate/` build cache survives, UE can do a partial recompile against a stale module file-list and silently leave you on old/partial code. Each channel handles this differently:

- **[Fab](https://www.fab.com/listings/d1d5cbef-0aaa-4516-a309-92f39dee0c33) / Epic Marketplace → automatic.** The Epic Games Launcher replaces the precompiled binaries in place; nothing to compile, no cache to clear. The bridge is kept per plugin version, so after an update click **Get bridge** once more.
- **`unreal-mcp-cli update` → automatic clean rebuild.** `update` re-copies the plugin source and, by **default**, deletes the installed plugin's stale `Intermediate/` and the C++ `Binaries/` so UE performs a clean compile on the next editor launch — no manual steps. The bundled sidecar bridge under `Binaries/ThirdParty/UnrealMcpBridge/<rid>/` is kept intact: release-source installs refresh it from `Source/ThirdParty/...`, while repo/dev installs preserve the previously bundled copy when needed. Dev **junction** installs are never cleaned (that would wipe your live source tree's outputs). Pass `--no-clean` to opt out of the cache wipe.

  ```bash
  unreal-mcp-cli update <YourProject>            # default: clean rebuild on version change
  unreal-mcp-cli update <YourProject> --force    # re-copy even when versions match
  unreal-mcp-cli update <YourProject> --no-clean # keep the existing build cache
  unreal-mcp-cli update <YourProject> --no-extensions # leave the installed extensions as they are
  unreal-mcp-cli update <YourProject> --no-self  # leave the CLI as it is
  unreal-mcp-cli update <YourProject> --check    # only report; exit code 2 when something is outdated
  ```

- **Manual copy → clear the cache yourself.** If you overwrite `<YourProject>/Plugins/UnrealMCP/` by hand, **close the editor first**, delete `<YourProject>/Plugins/UnrealMCP/Intermediate/` and the C++ `Binaries/` (keep `Binaries/ThirdParty/` if a bundled bridge is present), then relaunch so UE recompiles cleanly.

## Environment variables

The plugin reads configuration with the precedence **process env → `<Project>/.env` → config file → built-in defaults**. The recognized `UNREAL_MCP_*` variables (defined in `UnrealMCP/Source/UnrealMcpRuntime/Private/Config/UnrealMcpConfig.cpp`; `UNREAL_MCP_CREDENTIALS_DIR` is read by the bridge, `UNREAL_MCP_SERVER_PATH` by the CLI):

| Variable | Purpose |
| --- | --- |
| `UNREAL_MCP_CONNECTION_MODE` | `Cloud` or `Custom`. **Defaults to `Cloud`.** |
| `UNREAL_MCP_HOST` | Server URL (Custom mode), e.g. `http://localhost:<port>` |
| `UNREAL_MCP_CLOUD_URL` | Cloud backend URL (defaults to ai-game.dev) |
| `UNREAL_MCP_TOKEN` | Auth token (sidecar IPC token / server token). **Secret — never commit.** |
| `UNREAL_MCP_AUTH_OPTION` | `none` or `oauth` (local server auth) |
| `UNREAL_MCP_KEEP_CONNECTED` | Persist/restore the connected state |
| `UNREAL_MCP_TOOLS` | Enabled-tools override (whitelist; empty = no filter) |
| `UNREAL_MCP_START_SERVER` | `true`: the editor starts the local `gamedev-mcp-server` itself once the bridge reports the project's port (Custom mode + http only; persisted as `startServer`) and stops it on exit. `unreal-mcp-cli exec` / `open --headless` set it. |
| `UNREAL_MCP_TRANSPORT` | `stdio` or `http` |
| `UNREAL_MCP_LOG_LEVEL` | Log verbosity |
| `UNREAL_MCP_TOOL_TIMEOUT` | How long one tool call may run in the editor, in milliseconds (default `300000`; persisted as `toolTimeoutMs`). Raise it for long `python-execute` snippets or asset work. The editor passes it plus 10 s to the local server it starts as `tool-timeout`; an older server (before McpPlugin 8.11.0) caps tool calls at 5 min whatever this says ([ARCHITECTURE §4](ARCHITECTURE.md)). A call that times out or is cancelled before the editor starts it is skipped, not run later. |
| `UNREAL_MCP_BRIDGE_PATH` | Path to a sidecar binary — the **dev/CI override**; wins over the bundled binary (§6). A packaged release auto-resolves the bundled sidecar, so end users never set this; from-source builds use it. |
| `UNREAL_MCP_CREDENTIALS_DIR` | Absolute folder of the machine credential store the bridge reads (and refreshes) instead of `~/.ai-game-dev`, set in the editor's process environment (not read from `.env`) — a path, never a token; a relative value is logged and ignored. For development/test editors whose launcher signs in under another profile (the AI Game Dev App's local dev stand); the twin of Godot's `GODOT_MCP_CREDENTIALS_DIR`. |
| `UNREAL_MCP_SERVER_PATH` | Path to a local `gamedev-mcp-server` binary (read by `unreal-mcp-cli`, not the plugin) — skips the server download + version check (§6). |
| `AIGD_SESSION_IDLE_SECONDS` | Set by `unreal-mcp-cli` on an editor it starts: after this many seconds without tool calls (calls in flight, play-in-editor, asset compilation and user input count as activity) the editor saves dirty packages that have a path and quits. Only armed together with `AIGD_SESSION_OWNER=cli` and a nonce, and only while the project lease still names this editor; unset = never. |
| `AIGD_SESSION_OWNER` / `AIGD_SESSION_NONCE` | `cli` and the nonce of `<project>/.ai-game-dev/session.json`: mark an editor a CLI session started. Only such an editor accepts `editor-quit`, and on exit it removes the lease if it still carries its nonce. |

> **Never commit `.env`.** A project-root `.env` can hold `UNREAL_MCP_TOKEN`, and UE project templates ship no `.gitignore`. `unreal-mcp-cli configure` appends `.env` to the target project's `.gitignore`; this repo's own scaffold already gitignores it. The sidecar IPC token travels over stdin (never argv) and is never logged.

## Troubleshooting

- **`No connected clients. Retrying [1..10]` then HTTP 500 from a local server.** The connection mode defaulted to `Cloud`, so the sidecar dialed ai-game.dev instead of your local server. Set `UNREAL_MCP_CONNECTION_MODE=Custom` (env, `.env`, or the UI toggle).
- **No `[Unreal-MCP] plugin loaded` line at boot.** The editor module failed to load — check the Output Log for a `StartupModule` error or a malformed `.uplugin`.
- **A tool body returns `'x' is required.` when invoked over the REST passthrough.** Send `-H "Content-Type: application/json"`; without it the server drops the JSON body.
- **Screenshot tools return `Screenshot tools are unavailable: this editor runs with the null RHI (-nullrhi)`.** Nothing renders under `-nullrhi`. Restart the editor without it: windowed, or headless with `-RenderOffscreen`, which renders screenshots (with `unreal-mcp-cli`, omit `--nullrhi`).
- **No sidecar / sidecar keeps restarting.** Check the bridge status in the **Connection** section (`Running (restarts: N)` / `Stopped`). If the log shows `no sidecar binary resolved for rid <rid> …`, neither a bundled binary, a downloaded one, nor `UNREAL_MCP_BRIDGE_PATH` resolved. A Fab install needs the bridge installed by hand: **Get bridge** opens the release page, extract the zip for your platform into the folder **Open bridge folder** shows, then click **Connect** (the Fab build never downloads or starts a program by itself); a CLI / packaged release bundles it, so there it means the plugin was packaged without your platform's bridge (reinstall the correct build); in a from-source build, set `UNREAL_MCP_BRIDGE_PATH` or run `unreal-mcp-cli bootstrap-local` to build one from source ([ARCHITECTURE](ARCHITECTURE.md) §6.3, §6.8).
- **Ports.** IPC uses a deterministic per-project port in `30000–39999` and probes forward (then an ephemeral port) on a collision; the local server uses a deterministic hash port in `20000–29999` with no probing — the CLI derives the same number without reading any config, and the server binds the exact requested port. The exact number is a debugging nicety, not a requirement.
- **Logs.** Use the main window's **Open log file** action, an RQL query (`console | where Verbosity == "Error"`), or the editor Output Log (`LogUnrealMcp` category).
