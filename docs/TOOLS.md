<b>[English](TOOLS.md) | [中文](TOOLS.zh-CN.md) | [日本語](TOOLS.ja.md) | [Español](TOOLS.es.md) | [فارسی](TOOLS.fa.md) | [हिन्दी](TOOLS.hi.md) | [العربية](TOOLS.ar.md)</b>

# Unreal-MCP tools

The canonical inventory of the tools the **UnrealMCP** editor plugin registers. Unreal-MCP ships **30 built-in tools in 8 families** that your AI agent can call, plus **4 system tools** it cannot (host plumbing for the CLI and the desktop app). Tool ids are kebab-case (`rql-query`, `blueprint-compile`), the same convention as Unity-MCP and Godot-MCP. [Extensions](#extensions) and [your own plugins](EXTENSIONS.md) add more.

| Family | Tools | Count |
| --- | --- | ---: |
| RQL data layer | `rql-query`, `rql-mutate`, `rql-schema` | 3 |
| Blueprint | `blueprint-*`: create, get, add-component, remove-component, add-variable, modify-variable, set-default, add-function, add-event, compile, spawn | 11 |
| Source / C++ | `source-*`: read, create-class, update, delete, list, compile | 6 |
| Screenshot | `screenshot-*`: viewport, game-view, camera, isolated | 4 |
| Fab | `fab-search`, `fab-library`, `fab-add` | 3 |
| Console | `console-run-command` | 1 |
| Python | `python-execute` | 1 |
| Tool list | `unreal-tool-list` | 1 |
| **System** (not shown to the AI) | `ping`, `unreal-skill-create`, `unreal-skill-generate`, `editor-quit` | 4 |

> **Source of truth.** Standard tools: `UnrealMcp*Tools.cpp` in `UnrealMCP/Source/UnrealMcpEditor/Private/Tools/` (except `UnrealMcpSkillTools.cpp`) and `UnrealMcpRqlTools.cpp` in `UnrealMcpRql`. System tools: `UnrealMcpPingTool.cpp`, `UnrealMcpSkillTools.cpp`, `UnrealMcpEditorSession.cpp` and the sidecar's `SkillGenerateTool.cs`. Update this page when a registration changes.

All 30 standard tools are **editor-only**. A runtime (in-game) connection serves only `ping` plus the tools your game registers: see [RUNTIME.md](RUNTIME.md).

## Families in detail

<details>
  <summary><b>RQL — the data layer (3)</b></summary>

One query language for the whole editor instead of one tool per operation ([`docs/rql.md`](rql.md)): find actors, components and assets, read members, filter, aggregate, and change them in one call. It replaces the former actor, object, asset, level, editor-state, selection, console-log and reflection tools.

| Tool id | What it does |
| --- | --- |
| `rql-query` | Read-only RQL query over the live editor (`actors | where $ is PointLight | select $ref Label Intensity`); compact table results |
| `rql-mutate` | RQL query ending with `set` / `call` (`... | set Location = "(0, 0, 100)"`, `editor | call SpawnActor("PointLight")`): all-or-nothing, one undo step, `dry_run` previews |
| `rql-schema` | Sources, functions and the classes in the editor world; `type` lists one type's members and methods |

Beyond reflected properties, RQL reaches DataTable rows (`dataTables`, `Rows["Sword"].Damage`), gameplay tags as text (`gameplayTags`, `set Tags = ["Ability.Fire"]`), Project Settings saved to their ini (`unreal.settings("Maps & Modes")`) Blueprint graphs (`EventGraph | call AddCallFunction("PrintString")`, pin connections, `call Compile()`), Widget Blueprint trees (`call AddWidget("Button", "Ok", parent)`) and Level Sequences (bindings, tracks, `call AddKey("Location.X", 30, 100)`).

The `unreal-rql` skill (written by `unreal-skill-generate`) teaches the language and lists what installed extensions add.

</details>

<details>
  <summary><b>Blueprint family (11) — Unreal's flagship surface</b></summary>

| Tool id | What it does |
| --- | --- |
| `blueprint-create` | Create a new Blueprint class from a parent `UClass` path |
| `blueprint-get` | Graph summary for LLM inspection (variables, components, functions/events, parent chain) |
| `blueprint-add-component` | Add a component via the Simple Construction Script |
| `blueprint-remove-component` | Remove an SCS component |
| `blueprint-add-variable` | Add a typed member variable |
| `blueprint-modify-variable` | Modify a member variable |
| `blueprint-set-default` | Edit a CDO (class-default) property |
| `blueprint-add-function` | Add a function stub (entry/result nodes wired) |
| `blueprint-add-event` | Add/bind an event stub (BeginPlay, Tick, input, …) |
| `blueprint-compile` | Compile the Blueprint and return a **structured error/warning list** (the AI feedback loop) |
| `blueprint-spawn` | Instance the Blueprint into the current level |

</details>

<details>
  <summary><b>Console family (1)</b></summary>

| Tool id | What it does |
| --- | --- |
| `console-run-command` | Run a console command / CVar |

Logs, PIE state and the selection are RQL: `console | where Verbosity == "Error"`, `editor | set IsPlaying = true`, `editor | set Selection = [...]`. Play testing simulates the game inside the call (deterministic, independent of the editor ticking between calls): `editor | call StartPlay()`, `editor | call AdvancePlay(1.5, 0, "W, LeftShift")` (holds keys for 1.5 s, returns the pawn's location, velocity, how far the call moved it, movement mode and applied mapping contexts), `editor | call StopPlay()`; the Enhanced Input extension adds `enhancedInput | call InjectInput(action, value, seconds)` ([docs/rql.md](rql.md#play-testing)).

</details>

<details>
  <summary><b>Tool list family (1)</b></summary>

| Tool id | What it does |
| --- | --- |
| `unreal-tool-list` | List every tool, including the ones hidden from `tools/list` (marked hidden; they still run when called by name); regex `filter`, `includeDescription`, `includeInputs` |

</details>

<details>
  <summary><b>Python family (1)</b></summary>

| Tool id | What it does |
| --- | --- |
| `python-execute` | Run Python in the editor (full `unreal` API, pre-imported): plain statements, the last expression is the result, `print` comes back as logs, errors point at `snippet.py` lines; `include` adds helper files/folders from `<Project>/.ai-game-dev/snippets`; `timeout` (seconds, default 30) stops a runaway snippet |

Uses the engine's PythonScriptPlugin (enabled by Unreal-MCP; editor-only, never in a packaged game) and the [Code-Exec](https://github.com/IvanMurzak/Code-Exec) runtime vendored in `UnrealMCP/Content/Python`. Blueprint graph wiring is not exposed to Python: use the Blueprint tools for that.

</details>

<details>
  <summary><b>Fab family (3) — search and add Fab content</b></summary>

| Tool id | What it does |
| --- | --- |
| `fab-search` | Search the [Fab](https://www.fab.com) marketplace (`query`, `listingType`, `freeOnly`, `sort`, paging); returns each listing's uid, title, type, price and formats |
| `fab-library` | List the Fab assets the signed-in user owns, with where each is already imported; `filter` searches the whole library |
| `fab-add` | Download a listing the user owns and import it like the Fab panel's *Add to Project* (Unreal Engine packs, Megascans with `quality`, glTF/FBX); returns the content folder, skips listings already in the project |

They drive the engine's own Fab plugin (on by default since UE 5.5): its Epic sign-in and its import workflows. The first `fab-library` / `fab-add` without a sign-in opens Epic's sign-in page in the browser (opening Fab from the editor's toolbar also signs in, from a remembered Epic login). Buying, or adding a free listing to the library, stays on fab.com. Complete projects and engine code plugins are installed from the Epic Games Launcher, not added to a project.

</details>

<details>
  <summary><b>Source / C++ family (6)</b></summary>

| Tool id | What it does |
| --- | --- |
| `source-read` | Read a project C++ source file (sliced) |
| `source-create-class` | Scaffold a new C++ class (header + cpp from templates) |
| `source-update` | Edit a source file |
| `source-delete` | Delete a source file |
| `source-list` | List module source files |
| `source-compile` | Compile project C++ (Live Coding when active, else UBT) with a structured error report; new classes or changed reflected declarations (`restartRequired`) say how to rebuild and restart the editor |

All file operations are jailed to `<Project>/Source/`.

</details>

<details>
  <summary><b>Screenshot / viewport-capture family (4)</b></summary>

| Tool id | What it does |
| --- | --- |
| `screenshot-viewport` | Capture the active editor viewport; optional `savePath` also writes the PNG inside the project |
| `screenshot-game-view` | Capture the PIE / game view; optional `savePath` as above |
| `screenshot-camera` | Render from a resolved camera actor via `USceneCaptureComponent2D`; optional `savePath` as above |
| `screenshot-isolated` | Render an actor in isolation (transient SceneCapture2D + show-only list); optional `savePath` as above |

Captures return a base64 **PNG as MCP image content** so the LLM can inspect the render directly. Each capture renders a new frame at call time, also while the editor window is in the background or minimized, and its text says whether the frame is fresh and whether effects and animation were ticking. While an AI agent is connected, the plugin keeps the editor ticking and rendering at full rate in the background (also when minimized) ("Use Less CPU when in Background" is overridden for that session only; your saved preference is unchanged). Dimensions are clamped (default 1024, hard cap 2048 per side). Pixel capture needs an editor that renders: windowed, or headless with `-RenderOffscreen` (the `unreal-mcp-cli` default). Under `-nullrhi` nothing renders, so these tools return an error that names `-nullrhi` and the fix.

</details>

<details>
  <summary><b>System tools (4) — NOT exposed to your AI</b></summary>

These are **system tools**: host plumbing the `unreal-mcp-cli` and the desktop app drive directly over `POST /api/system-tools/<name>`. They are deliberately absent from MCP `tools/list`, so they never appear to (or spend tokens in) an AI session — the same split Unity and Godot use for their system tools.

| Tool id | What it does |
| --- | --- |
| `ping` | Liveness probe — round-trips the plugin ⇄ sidecar ⇄ server chain |
| `unreal-skill-create` | Generate a new MCP tool as a C++ file in the plugin's editor module. **Requires an editor rebuild** before the new tool is callable (Live Coding, or rebuild the project) — the result says so explicitly. Needs a source install of the plugin |
| `unreal-skill-generate` | Regenerate every `SKILL.md` from the registered tools into a project-relative skills folder (default `.claude/skills`) |
| `editor-quit` | Save dirty packages that have a path and quit an editor a CLI session started (`unreal-mcp-cli close --owned-only`). Refuses unless `nonce` matches the session lease, so it never closes an editor you opened |

</details>

## Extensions

Install an extension when your project uses one of the engine systems below, or [create your own](EXTENSIONS.md). Each extension is an RQL module: it extends the core `rql-query` / `rql-mutate` / `rql-schema` tools and adds its guide to the `unreal-rql` skill, so no separate MCP tools appear in `tools/list`.

```bash
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-gas "<UEProject>"
```

Extensions need Unreal Engine 5.7+, Unreal MCP 0.26.0+ in the same project, and a C++ toolchain: releases ship as source and compile for your project (on next editor open, or now with `--build`). The install id is `com.ivanmurzak.unreal-ai-<id>` with the id from the table below. Install options (`--version`, `--build`, offline `--source`): [Installing an extension into a project](EXTENSIONS.md#installing-an-extension-into-a-project).

| Extension (id) | Description |
| --- | --- |
| **[AI&nbsp;Control&nbsp;Rig](../extensions/UnrealAIControlRig/README.md)**<br>`control-rig` | Create rigs with bones, controls and a two-bone IK solver; query Control Rig blueprints and components |
| **[AI&nbsp;Enhanced&nbsp;Input](../extensions/UnrealAIEnhancedInput/README.md)**<br>`enhanced-input` | Query Input Actions and Mapping Contexts, create actions and contexts, map keys |
| **[AI GAS](../extensions/UnrealAIGAS/README.md)**<br>`gas` | Gameplay Ability System authoring without C++: effects (cost, cooldown, damage, regeneration), ability Blueprints, ability system components with a ready-made Health / Mana / Stamina attribute set, input bindings, Play In Editor checks |
| **[AI Landscape](../extensions/UnrealAILandscape/README.md)**<br>`landscape` | Landscape and Water: create, sculpt and paint landscapes; create oceans, lakes and rivers; query landscapes, water bodies and water zones |
| **[AI Niagara](../extensions/UnrealAINiagara/README.md)**<br>`niagara` | Niagara VFX: query systems and their emitters, place systems in the level |
| **[AI PCG](../extensions/UnrealAIPCG/README.md)**<br>`pcg` | Procedural Content Generation: author PCG graphs (nodes, connections, settings, parameters), place them on volumes, generate scatters (forests, rocks, props) and read the counts |

## Per-tool visibility

Every tool has a **Visible** checkbox in the **MCP Tools** window — the standalone **MCP Tools** tab (registered under the editor's **Tools** menu). The window shows each tool's title, family, and description, plus an "N / M tools visible" summary line. **Visible** only decides whether the tool is **listed in the MCP `tools/list`**, i.e. whether AI agents see it. It is not an access control: a hidden tool still runs when a client calls it by name (`tools/call`), and `unreal-tool-list` lists hidden tools too (marked hidden) so an agent can find them.

Two settings apply (see [ARCHITECTURE](ARCHITECTURE.md) §7/§8):

- the **`enabledTools` whitelist** (overridable via `UNREAL_MCP_TOOLS`) decides whether a tool is **served at all** — when non-empty, only listed tools are served, and any other tool is neither listed nor callable (`Execute()` refuses it); empty means "no filter";
- the **`hiddenTools` list** — the Visible checkboxes you flip in the UI — hides a served tool from `tools/list`.

Both are persisted across editor sessions and survive an extension hot-reload (a re-registered tool inherits the retained setting, so a rebuild never shows a tool you hid). A config file from an older plugin version keeps working: its `disabledTools` list is read as `hiddenTools`, and the next save writes `hiddenTools`.

All connection settings live in the single **AI Game Developer** main window's Connection section (there is no separate Settings tab or Project-Settings page — Unity-MCP parity). The **MCP Prompts** and **MCP Resources** windows are wired but ship empty in this release — each renders a subdued empty-state message (the "N / M visible" summary is unique to the Tools window).
