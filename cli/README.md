<div align="center">

<img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/readme/hero.svg" alt="Unreal MCP — let any AI agent inspect and drive the Unreal Editor" width="100%"/>

<br/>

[![CLI](https://img.shields.io/npm/v/unreal-mcp-cli?label=CLI&color=BE185D&logo=npm&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://www.npmjs.com/package/unreal-mcp-cli) [![Node.js](https://img.shields.io/badge/Node.js-22.14%2B-0E7490?logo=nodedotjs&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://nodejs.org/) [![License](https://img.shields.io/github/license/IvanMurzak/Unreal-MCP?label=license&color=047857&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://github.com/IvanMurzak/Unreal-MCP/blob/main/LICENSE) [![Docs](https://img.shields.io/badge/docs-ai--game.dev-4338CA?style=for-the-badge&labelColor=0F172A&logoColor=white)](https://ai-game.dev)

</div>

**`unreal-mcp-cli`** is the command-line tool of [Unreal-MCP](https://github.com/IvanMurzak/Unreal-MCP), the Unreal sibling of [`unity-mcp-cli`](https://www.npmjs.com/package/unity-mcp-cli) and [`godot-cli`](https://www.npmjs.com/package/godot-cli). It installs the UnrealMCP plugin, signs you in to [ai-game.dev](https://ai-game.dev), configures your AI agent, opens the Unreal Editor with the right connection settings and calls the plugin's tools from the terminal.

<p align="center">
  <img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/readme/terminal.svg" alt="Terminal: npm install -g unreal-mcp-cli, install-plugin, login, setup-mcp claude-code, open the editor" width="100%"/>
</p>

## Features

✅ **Install plugin** — copy (or junction) the UnrealMCP plugin into a project's `Plugins/`\
✅ **Extensions** — add optional engine extensions (GAS, Niagara, PCG, …)\
✅ **Sign in** — OAuth device-code login to `ai-game.dev`, once per machine\
✅ **Set up agents** — MCP configs for Claude Code, Cursor, Copilot, Codex and more\
✅ **Open & close** — open the editor, windowed or headless; close only CLI-started ones\
✅ **Call tools** — `rql`, `exec`, `run-tool`; a headless editor starts on demand\
✅ **Build & test** — compile C++ with UBT and run Automation tests without an editor\
✅ **Agent-friendly** — `--format json`, stable exit codes, a `commands` manifest\
✅ **Cross-platform** — Windows, macOS and Linux

## Quick start

Requires [Node.js](https://nodejs.org/) 22.14 or later.

```bash
npm install -g unreal-mcp-cli                       # or: npx unreal-mcp-cli@latest <command>

unreal-mcp-cli install-plugin ./MyGame              # plugin + bundled bridge into MyGame/Plugins
unreal-mcp-cli login                                # sign in to ai-game.dev once per machine
unreal-mcp-cli setup-mcp claude-code -p ./MyGame    # write the agent's MCP config
unreal-mcp-cli open ./MyGame                        # build if needed and open the editor
```

`install-plugin` and `update` use a local `UnrealMCP/` checkout when present; otherwise they download the plugin source asset of the GitHub Release that matches the CLI version and verify its signature (minisign / Ed25519) before extracting it. `--plugin-source <dir>` is the offline / CI / dev override.

# Commands

| Command | What it does |
| --- | --- |
| `create-project <path>` | Scaffold a minimal Unreal Engine C++ project |
| `open [path]` | Launch the Unreal Editor, wiring `UNREAL_MCP_*` env vars (engine resolved via `EngineAssociation` + `LauncherInstalled.dat`). On desktop platforms it auto-runs a pre-launch build when the project/plugin still needs native editor binaries, then auto-dismisses the known startup blocker dialogs; pass `--no-build` or `--no-auto-dismiss-startup-dialogs` to skip either behavior. A headless editor the CLI started is closed first. `--headless` instead starts a CLI-owned windowless editor (like `exec`) and waits until it is ready; `--idle <seconds>` sets its idle shutdown (also for a CLI-owned window without `--headless`) |
| `close [path]` | Close the editor running a project: an editor the CLI started saves and quits first (as with `--owned-only`), any other is terminated. `--owned-only` closes only an editor the CLI started (`exec` / `open --headless`): the plugin saves dirty packages that have a path and quits; an editor you opened is never touched (`--force` kills a CLI-owned editor that does not quit in time). No `--url` needed: the lease records the server the editor was started against |
| `exec [code]` | Run Python in the editor through `python-execute` (code from the argument, `-f <file>` or stdin; `--include` helper files). Reuses any running editor of the project; otherwise starts a headless one (`-RenderOffscreen`, so cameras and screenshots still render; `--window` for a GUI) that serves the project's local MCP server and saves + quits after `--idle <seconds>` without tool calls (default 300). Parallel cold calls start one editor and run one pre-launch UBT build (`.ai-game-dev/session.json` lease + startup lock; `.ai-game-dev/.gitignore` keeps both out of git). An editor you opened that does not answer on the local server (e.g. Cloud mode) fails fast after 60 s with a hint instead of waiting out `--ready-timeout`. A `--url` port that the project's `portOverride` overrules is rejected. Prints the result on stdout and `print()` / `unreal.log*()` lines on stderr; `--raw` prints the response JSON |
| `install-plugin [path]` | Copy (or `--junction`) the UnrealMCP plugin into `<project>/Plugins` — default source = local repo checkout when present, else the version-matched GitHub source asset |
| `remove-plugin [path]` | Remove the installed plugin |
| `install-extension <id> [path]` | Install a third-party Unreal-MCP **extension** plugin into `<project>/Plugins/<name>`, enable it + its gating engine plugins (e.g. `Niagara`) in the `.uproject`, and (re)compile (on next editor open, or now with `--build`). `--source <dir>` installs from a local copy (offline/CI); by default the **newest GitHub release** is installed (catalog version = offline fallback); `--version <x.y.z>` installs an exact version. Idempotent. Never downgrades implicitly. See [Extensions](#extensions) |
| `configure` | Write `UNREAL_MCP_*` into `<project>/.env` and gitignore `.env` (§8) |
| `setup-mcp <agent>` | Write an MCP client config snippet (claude-code, cursor, vscode-copilot). Over the default `http` transport it points the agent at the **project-pinned** cloud URL `<base>/mcp/p/<pin>` (so the agent routes to *this* project's editor); pass `--no-pin` for the bare `<base>/mcp` URL. When this machine is signed in, a Cloud config carries `Authorization: Bearer agd_pk_…` (this project's key, reused or minted) for every agent; `--oauth` writes a URL-only config instead (the agent runs its own login), `--regenerate-key` mints a new key and revokes the old one, and an explicit `--token` wins over both. With `--transport stdio` it instead downloads the pinned shared [`gamedev-mcp-server`](https://github.com/IvanMurzak/GameDev-MCP-Server) release into `<project>/Intermediate/UnrealMCP/server/<rid>/` (skipped when `UNREAL_MCP_SERVER_PATH` points at a local build) |
| `login` | OAuth device-code auth against ai-game.dev. Signs in once per machine into the shared machine credential store (`~/.ai-game-dev/credentials.json`): the default mints the account (agent) credential and derives the tool credential from it. `--tools-only` mints a tool credential only (CI/automation runners — the desktop App cannot pick it up); `--yes` confirms switching the machine to a different account; `--path <dir>` writes a project-local `.env` instead |
| `status` | Report project + plugin + running editor + connection + live reachability. With a project (`--path`, or the current folder when it holds a `.uproject`), exits **18** while a modal dialog blocks the editor, listing it with its buttons (see `dialog`); with an older plugin, when the editor log shows its `Editor blocked by modal dialog` line (written once a tool call waited on it) |
| `dialog [path]` | List the modal dialogs blocking the project's editor (title, text, buttons, id), or press a button: `--press <button>` (case-insensitive; `--id <id>` picks the dialog). Exit 6 `NOT_FOUND` when no open dialog has that button, 7 when no editor runs, 14 when the editor does not take the request within 5 s. Slate dialogs are invisible to Windows UI Automation, so the plugin serves the dialog on top from its modal loop through files in `<Project>/Saved/UnrealMCP/Dialogs/`: any OS, any connection mode (Cloud too), no server needed. A dialog shown before the plugin loaded (or a native message box) is not served: answer it in the editor |
| `wait-for-ready` | Block until the project's MCP server responds to a ping (exit 14 on timeout) |
| `rql [query]` | One RQL query from the command line, no JSON: `rql 'actors \| where Label starts-with "Tree" \| count'`. A query whose last stage is `set` or `call` goes to `rql-mutate` (`--dry-run`, `--max-targets <n>`), any other to `rql-query`; `--schema [type]` runs `rql-schema`; `-f <file>` (`-f -`: stdin) reads the query when a shell strips its quotes. Starts a headless editor like `run-tool`; text mode prints the answer's text (`--raw`: the response JSON). After a save (`SaveLevel()`, `SaveAll()`, `SaveAsset(...)`, `NewLevel(...)`) it prints the package files written (`Saved to disk: Content/Maps/Forest.umap`); a camelCase or SQL operator (`startsWith`, `like`) gets a hint with RQL's spelling. `rql --help` lists recipes |
| `run-tool <tool>` | Invoke an MCP tool over the local HTTP path. When no editor answers, starts a headless one like `exec` (a running editor, yours or the CLI's, is reused and never closed); `--no-start` disables that, `--once` closes the editor afterwards **only** if this call started it, `--nullrhi` starts it with `-nullrhi` (nothing renders, screenshot tools fail). A response with `status: "error"` exits 6. A call held by a modal dialog exits 18 `AWAITING_INPUT` with the dialogs (`data.dialogs`; `exec` and `run-system-tool` too) |
| `run-system-tool <tool>` | Invoke a system tool over the local HTTP path (same options as `run-tool`) |
| `tools [path]` | List the plugin's AI tools — hidden ones included — with their inputs, read from the editor (`unreal-tool-list`) and cached in `<project>/.ai-game-dev/tools.json` per installed plugin set, so it answers while the editor is closed (`--refresh` asks again; with no current cache it starts a headless editor like `run-tool`). `--filter <regex>` (an exact tool name shows just that tool, with its description), `--details`, `--format json` |
| `compile [path]` | Build the editor target with UnrealBuildTool, without an editor, and print `file:line:col` diagnostics (structured errors and warnings with `--format json`). `--strict` fails on warnings. Refuses (exit 6, `PROJECT_OPEN`) while an editor has the project open, so it never fights Live Coding / hot reload |
| `test [path]` | Run Automation tests headless (`UnrealEditor-Cmd -nullrhi -unattended -ExecCmds="Automation RunTests <filter>;Quit" -ReportExportPath=<dir>`), building the editor target first when needed. The verdict comes from the report's `index.json`, never the editor's exit code: exit 0 pass, 8 failures, 6 no verdict (crash, build failure, no report), 14 `--timeout <seconds>` (the editor tree is stopped). `--filter` (default `Project`), `--output <dir>`, `--junit-output <file>`, `--format text\|json\|ndjson\|github` (`github` = `::error file=..,line=..::` annotations). A filter that matches nothing is a warning. Refuses while an editor has the project open (use `run-tool` there) |
| `commands` | Print a manifest of every command (arguments, options, defaults) and the exit codes, generated from the command tree (`--format json` for agents) |
| `bootstrap-local [path]` | Build the bridge from source into `Intermediate/UnrealMCP/` (§6) — the MCP server is downloaded from GameDev-MCP-Server releases, not built here |
| `update [path]` | Re-sync the installed plugin from a local source or the version-matched GitHub source asset. On a version change it **auto-cleans** the stale UE C++ build cache (`Intermediate/` + C++ `Binaries/`) so the editor recompiles cleanly — the bundled sidecar bridge under `Binaries/ThirdParty/` is refreshed from `Source/ThirdParty/` when the release source asset ships it, otherwise preserved, junction (dev) installs are never cleaned, and `--no-clean` opts out |
| `install-engine [version]` | Detect installed engines from `LauncherInstalled.dat`; link to the Epic launcher for missing versions (never installs directly) |
| `setup-skills [agent] [path]` | Write the `unreal-mcp-cli` agent skill (cold-start workflow, RQL recipes, compile / test, exit codes; `references/` for depth) and `ai-game-dev-app` into an agent's skills folder (default `claude-code`; `--list` shows all), offline. Rendered from the same source as `--help`. |

## Exit codes and machine output

Shared with `unity-mcp-cli` and `godot-cli` (`unreal-mcp-cli commands` prints the table):

| Code | Meaning |
| --- | --- |
| 0 | Success |
| 1 | Generic or unclassified error |
| 2 | Usage error: bad flag or argument, an unknown tool or no project at the path (`NOT_FOUND`) (`--help` / `--version` exit 0). `update --check` keeps its own 2 = outdated |
| 6 | The command ran and failed (tool error, compilation errors, test run without a verdict, project open, refused credentials) |
| 7 | The editor or MCP server could not be started or reached |
| 8 | The test run completed with failing tests |
| 14 | Timed out (also HTTP 408 / 504) |
| 17 | Busy: the server answered 503 / 429 (or another CLI holds the editor startup lock) |
| 18 | The editor is blocked by a modal dialog |

`--format text|json|ndjson` (default `text`) on `run-tool`, `run-system-tool`, `exec`, `status`, `wait-for-ready`, `compile`, `test` and `commands`. `json`: stdout carries exactly one envelope `{"success","command","data","errors":[{"code","message","httpStatus"}],"warnings"}`; `ndjson`: progress lines `{"type":"progress",...}` on stderr and the envelope as one line on stdout. In both, logs go to stderr. Error codes: `USAGE`, `TOOL_FAILED`, `UNREACHABLE`, `TIMEOUT`, `BUSY`, `AWAITING_INPUT`, `TESTS_FAILED`, `TEST_RUN_ERROR`, `COMPILATION_FAILED`, `PROJECT_OPEN`, `NOT_FOUND`, `NOT_AUTHENTICATED`, `INTERRUPTED`, `ERROR`.

**A tool call that did not succeed** (`run-tool`, `run-system-tool`, `exec`, `rql`, `tools`) maps to the same exit code and error code in all three CLIs (the table lives in `@baizor/gamedev-cli-core`; `commands --format json` lists it as `errorCodes`). When the MCP server answered, its HTTP status decides, and the envelope error carries it: `errors[{code, message, httpStatus}]`.

| Condition | HTTP status | Exit | `errors[].code` |
|---|---|---|---|
| Bad arguments: a bad flag, tool arguments the CLI or the server rejected (any 4xx not listed here) | 400 (a 422 `[invalid_params]`) | `2` | `USAGE` |
| No tool by that name (list them with `tools`), or no project at the path | 404 | `2` | `NOT_FOUND` |
| No usable credentials, or the server refused them: sign in with `login` | 401, 403 | `6` | `NOT_AUTHENTICATED` |
| The tool ran and reported an error (`status: "error"`, any 5xx not listed here) | 409, 422, 500 | `6` | `TOOL_FAILED` |
| The project's code did not build, so no editor was started | — | `6` | `COMPILATION_FAILED` |
| Connection refused or reset, no engine plugin connected to the server, plugin lost after the call was sent | 502 | `7` | `UNREACHABLE` |
| No answer in time: `--timeout`, or the server's own tool timeout | 408, 504 | `14` | `TIMEOUT` |
| Busy and did not run the call (reloading, rate-limited, another CLI call holds the editor); retry later | 503, 429 | `17` | `BUSY` |
| A modal dialog blocks the editor | — | `18` | `AWAITING_INPUT` |
| Anything else (the call was aborted, an unexpected error) | — | `1` | `ERROR` |

When `exec` / `run-tool` / `tools` must start an editor and the project's C++ does not compile, nothing is started: they exit 6 (`COMPILATION_FAILED`) with the `file(line,col)` errors and the build log path. An editor that started but never became ready names its log (`Saved/Logs/<Project>.log`).

**Cloud mode and the local server.** A Cloud-mode project on a machine that has signed in to ai-game.dev stays in Cloud mode: `exec` / `run-tool` call the editor connected to the cloud and never start a local one; a sign-in that cannot be used (expired, revoked, unreadable) fails with `NOT_AUTHENTICATED` (exit 6). Only a machine that never signed in falls back to the project's local server (an editor started by the CLI serves it; the project config is not changed). An explicit `--url` / `--token` is used as is. `exec --raw` is unchanged. `--nullrhi` (on `open --headless`, `exec`, `run-tool`, `run-system-tool`, `tools`) starts a headless editor with `-nullrhi`, which renders nothing: the screenshot tools return an error instead of an image. Without it a headless editor renders offscreen (`-RenderOffscreen`), screenshots included.

## Engine resolution

`open` reads the project's `.uproject` `EngineAssociation` and resolves it against the Epic launcher manifest (`LauncherInstalled.dat`): a version association (`"5.7"`) maps to the matching install; an empty association falls back to the highest installed engine; a GUID (source build) requires `--engine-root`. `install-engine` enumerates the same manifest and, for a not-yet-installed version, hands back a `com.epicgames.launcher://` deep link rather than performing the multi-GB download.

## Connection mode

`open` (and the `openProject` library call) select the editor's MCP target via `UNREAL_MCP_CONNECTION_MODE`: `Custom` points the plugin's sidecar at a local/self-hosted server (`--host`), `Cloud` at ai-game.dev. Set it explicitly with `--connection-mode <Custom|Cloud>`. When you omit it, supplying a `--host` **infers `Custom`** — a host implies a non-Cloud target, and the plugin otherwise defaults to `Cloud` and ignores the host (silently dialing the cloud). An explicit `--connection-mode` always wins over that inference; with neither a host nor a mode the variable is left unset and the plugin default applies. Under `--no-connect` no `UNREAL_MCP_*` variables (the mode included) are wired onto the editor at all.

## Library export

```ts
import { configure, openProject, runTool, detectInstalledEngines } from 'unreal-mcp-cli';
```

`dist/lib.js` exposes the command logic as a side-effect-free library: no argv parsing, no stdout, no process exit. Every function returns a `{ kind: 'success' | 'failure' }` discriminated union and never throws past the boundary — the same contract as the Unity/Godot CLIs.

## Extensions

An **extension** is a third-party UE C++ Editor (or Runtime) plugin that implements `IUnrealMcpToolProvider` (see [`docs/EXTENSIONS.md`](https://github.com/IvanMurzak/Unreal-MCP/blob/main/docs/EXTENSIONS.md)) and contributes MCP tools to the core Unreal-MCP plugin. `install-extension` is the install channel for them:

```bash
# The newest GitHub release of the extension (the default channel):
unreal-mcp-cli install-extension com.acme.niagara ./MyGame

# An exact release version (may downgrade):
unreal-mcp-cli install-extension com.acme.niagara ./MyGame --version 1.2.0

# From a local copy (offline / CI / dev — mirrors godot-cli install-plugin --source):
unreal-mcp-cli install-extension com.acme.niagara ./MyGame --source ../Unreal-AI-Niagara/MyNiagaraExt

# Compile immediately instead of on next editor open:
unreal-mcp-cli install-extension com.acme.niagara ./MyGame --source <dir> --build
```

It (1) resolves `<id>` against the **shared extension catalog**, then resolves the **newest release online** (the `https://github.com/<repo>/releases/latest` redirect — no API rate limit, never a draft or prerelease; the tag must be a plain `vX.Y.Z` and the release must carry `<pluginName>-<X.Y.Z>.zip`). The catalog `version` is only the **last known release**, used when that resolution fails (offline, no matching asset). An explicit `--version` skips resolution and wins; an installed copy newer than the resolved version is kept, never downgraded implicitly. (2) places the plugin into `<project>/Plugins/<pluginName>/`, (3) enables the extension **and its gating engine plugins** (e.g. `Niagara`, read from the extension's own `.uplugin` `Plugins[]` ∪ the catalog hint) in the `.uproject`, and (4) compiles it. It is **idempotent** — a re-run that finds the same version already installed and enabled writes nothing.

**Catalog / manifest format.** The installable extensions are listed in a shared, **engine-agnostic** catalog (`src/utils/extensions-catalog.ts`, the typed mirror of the source-of-truth `extensions.catalog.json` — `{ schemaVersion, extensions[] }`, the same shape as Godot's `addons/godot_mcp/extensions.catalog.json`; a parity test keeps the two in lockstep). Each `ExtensionDescriptor` carries `extensionId` (reverse-DNS resolve key), `name`, `pluginName` (the `Plugins/<pluginName>` folder), `repo` (GitHub release source), `version` (last known release — offline fallback only), **`minCoreVersion`** (the minimum core Unreal-MCP version — surfaced as a warning when unmet), `enginePlugins` (gating plugins), and `tools`. The catalog currently lists **Niagara Tools** (`com.ivanmurzak.unreal-ai-niagara`); `--source <dir>` installs an unpublished or off-catalog extension regardless (the descriptor is then synthesized from the source `.uplugin`).

**Pluggable install source (Fab-ready).** `resolveInstallSource` (`src/utils/extension-source.ts`) is the single decision point for *where* the files come from — `--source` (local) or the GitHub release (github.com-only, fail-closed). A future **Fab** (Epic marketplace) channel is added as one new `kind` there plus a materializer branch, **without touching callers**.

**Compile-on-install strategy (chosen + documented).** Extensions ship as **source** and are compiled by Unreal's desktop build tooling. The default is **source-ship + compile-on-next-editor-open** (the install reports `rebuildRequired: true`); `--build` opts into an **eager** compile against the resolved engine on **Windows / macOS / Linux**. Source-ship is chosen over shipping precompiled-per-UE-version binaries because UE plugin binaries are version/config/platform-specific and ABI-unstable across engine minors — exactly why the core `install-plugin` also ships source and excludes the stale build cache.

**Engine-agnostic API.** `installExtension(opts)` and its `ExtensionDescriptor` / `InstallExtensionResult` shapes are deliberately engine-neutral (no Unreal-only types in the signature) so `unity-mcp-cli` / `godot-cli` can adopt the identical surface — the Unreal-specific bits (plugin placement, `.uproject` enable, UBT) live behind it.

## Develop

```bash
npm install
npm run build   # tsc -> dist/ (ESM)
npm test        # vitest (unit tests; fixtures under tests/fixtures/)
node bin/unreal-mcp-cli.js status

# Optional end-to-end integration test (NOT part of the default run):
UNREAL_MCP_CLI_INTEGRATION=1 npm test
```

# Supported AI Agents

`unreal-mcp-cli setup-mcp <agent>` writes a ready-to-use MCP client config so your favorite AI coding agent can drive the Unreal Editor: Claude Code, Claude Desktop, Cursor, VS Code / Visual Studio Copilot, Rider (Junie), GitHub Copilot CLI, Gemini, Antigravity, Cline, OpenCode, Codex, Kilo Code, or a custom config. `setup-mcp --list` shows each agent's id and config path.

<div align="center">
  <p>
    <a href="https://claude.ai/download"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/claude-64.png" alt="Claude" title="Claude" height="36"></a>&nbsp;&nbsp;
    <a href="https://openai.com/index/introducing-codex/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/codex-64.png" alt="Codex" title="Codex" height="36"></a>&nbsp;&nbsp;
    <a href="https://www.cursor.com/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/cursor-64.png" alt="Cursor" title="Cursor" height="36"></a>&nbsp;&nbsp;
    <a href="https://code.visualstudio.com/docs/copilot/overview"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/github-copilot-64.png" alt="GitHub Copilot" title="GitHub Copilot" height="36"></a>&nbsp;&nbsp;
    <a href="https://gemini.google.com/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/gemini-64.png" alt="Gemini" title="Gemini" height="36"></a>&nbsp;&nbsp;
    <a href="https://antigravity.google/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/antigravity-64.png" alt="Antigravity" title="Antigravity" height="36"></a>&nbsp;&nbsp;
    <a href="https://code.visualstudio.com/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/vs-code-64.png" alt="VS Code" title="VS Code" height="36"></a>&nbsp;&nbsp;
    <a href="https://www.jetbrains.com/rider/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/rider-64.png" alt="Rider" title="Rider" height="36"></a>&nbsp;&nbsp;
    <a href="https://opencode.ai/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/open-code-64.png" alt="Open Code" title="Open Code" height="36"></a>&nbsp;&nbsp;
    <a href="https://github.com/cline/cline"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/cline-64.png" alt="Cline" title="Cline" height="36"></a>
  </p>
</div>

```bash
unreal-mcp-cli setup-mcp claude-code -p ./MyGame    # pinned <base>/mcp/p/<pin> URL (routes to this project)
unreal-mcp-cli setup-mcp claude-code -p ./MyGame --no-pin   # bare, unpinned <base>/mcp URL instead
unreal-mcp-cli setup-mcp claude-code -p ./MyGame --oauth           # URL-only; the agent signs in itself
unreal-mcp-cli setup-mcp claude-code -p ./MyGame --regenerate-key  # new project key; the old one is revoked
unreal-mcp-cli setup-mcp cursor -p ./MyGame --transport stdio
```

> For the full Unreal-MCP project documentation, see the [main README](https://github.com/IvanMurzak/Unreal-MCP/blob/main/README.md) and the [architecture reference](https://github.com/IvanMurzak/Unreal-MCP/blob/main/docs/ARCHITECTURE.md). Backed by **[ai-game.dev](https://ai-game.dev)**.

<div align="center">
  <a href="https://stand-with-ukraine.pp.ua"><img src="https://raw.githubusercontent.com/IvanMurzak/IvanMurzak/main/images/ukraine/flag-rounded.svg" width="28" height="28" alt="Stand with Ukraine" title="Stand with Ukraine"></a>
</div>
