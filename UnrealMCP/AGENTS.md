# UnrealMCP/ — the UE plugin (5.5+ floor; CI on 5.8)

Packaging ships only `Source/`, `Config/`, `Resources/`, `*.uplugin` and `Config/FilterPlugin.ini` entries,
so this file stays out of the package; keep agent notes out of `Source/`.

- `UnrealMCP.uplugin` — modules `UnrealMcpRuntime` (Runtime), `UnrealMcpRql` (Editor), then `UnrealMcpEditor` (Editor), all LoadingPhase Default, and `UnrealMcpFab` (Editor, LoadingPhase None: `UnrealMcpEditor` loads it only when the Fab plugin is enabled; its EOSShared dependency is an `Optional`, Editor-only plugin reference). **No `EngineVersion` pin** (it is an exact-build match). Never add `UnrealMcpEditorTests` to it (Fab flags shipped test modules, §6.7).
- `Source/UnrealMcpRuntime/` — infra that cooks into games: `Public/` = extension contracts (`IUnrealMcp*Provider.h`, `UnrealMcp*Registry.h`; re-exported by the editor module, so editor and runtime extensions share them), runtime subsystem/settings, core `Register` entry points; `Private/` = `Bridge/` (TCP + NDJSON), `Dispatch/`, `Tools/`, `Sidecar/`, `Config/`, `Extensions/`, `Prompts/`, `Resources/`.
- `Source/UnrealMcpEditor/Private/` — editor-only: `Tools/` (one `UnrealMcp<Family>Tools.cpp` per family), `UI/` (Slate), `Server/` (local server manager), `Sidecar/` (the §6.8 bridge download), `DevControl/`, `Extensions/`.
- `Source/UnrealMcpFab/` — the Fab sign-in companion (DEV-281): restores the Epic sign-in the Fab plugin's EOS platform remembers on editor start and hands its tokens to the Fab tools (`UnrealMcpEditor/Public/UnrealMcpFabSignIn.h`). The only module that links EOSShared/EOSSDK; keep everything else Fab-free (reflection only).
- `Source/UnrealMcpRql/` — the RQL data layer ([`docs/rql.md`](../docs/rql.md)): the Unreal provider (`UnrealMcpRqlTypes`), the per-call session + core sources / members / methods (`UnrealMcpRqlSession`, `UnrealMcpRqlCore`), the `editor` operations (`UnrealMcpRqlEditorApi`), the rql-* tools, `Public/IUnrealMcpRqlModule.h` (the extension seam). Exceptions on, no unity build, no PCH; RQL types stay private to it.
- `Source/ThirdParty/RQL/` — the vendored RQL C++ runtime (unmodified, **PolyForm Shield 1.0.0**, see its README); compiled only through `UnrealMcpRql/Private/Vendor/`.
- `Content/Python/` — `python-execute`'s Python side: package `unreal_mcp/` — `python_execute.py` (glue) — and `Lib/site-packages/code_exec/` (vendored, unmodified
  Code-Exec runtime, **PolyForm Shield 1.0.0**, see its `VENDORED.md`). Shipped via `Config/FilterPlugin.ini` and the
  release source staging; PythonScriptPlugin is a `.uplugin` dependency (editor-only, `UncookedOnly`).
- `Source/UnrealMcpEditorTests/` — Automation specs (`WITH_DEV_AUTOMATION_TESTS`, prefix `UnrealMcp.`); new specs go here, no top-level test folder.
- `Source/ThirdParty/UnrealMcpBridge/<rid>/` — release-staged sidecar binaries (gitignored; `RuntimeDependencies` copy them to `Binaries/ThirdParty/...`); its `SHA256SUMS` (release-written, gitignored) anchors the §6.8 download.

**Rules**
- **Module boundary:** `UnrealMcpRuntime` stays GEditor/UnrealEd/Slate-free (deps: Core/CoreUObject/Engine/DeveloperSettings public; Networking/Sockets/Json/JsonUtilities/Projects private). Engine-dev tools, UI, asset/blueprint/capture deps go in `UnrealMcpEditor`. World access via `FUnrealMcpWorldProvider`.
- **Unity-build ODR rule:** modules are unity-built, so an anonymous namespace is NOT file-private. Give every local helper a family-unique name (`LevelMakeStringArraySchema`) and every spec helper a spec-unique name (or a `BEGIN_DEFINE_SPEC` member). Collisions can surface as misleading errors (e.g. `C2661` on `TestFalse`) rather than `C2084`.
- **Include what you use:** CI's distributed BuildPlugin runs with `-StrictIncludes` (no PCH, no unity build), so a file that compiles only through the shared PCH or a unity neighbour fails the PR. Reproduce locally by adding `-StrictIncludes` to `RunUAT BuildPlugin`. The test module and the extensions build the same way in `scripts/extension_suite.py`'s isolated profiles (`--mode isolated`).
- **Handlers:** run on the game thread via the dispatcher (§4), are synchronous (`FUnrealMcpToolHandler`; a long compile blocks the game thread), must not pump modal UI or wait on bridge state (§11 risk 2). A tool outside the `enabledTools` whitelist / `UNREAL_MCP_TOOLS` is rejected in `UnrealMcpToolRegistry::Execute`; a tool in `hiddenTools` (the per-tool Visible toggle, legacy key `disabledTools`) is only left out of `tools/list` and still runs.
- **Style:** tabs, braces on new lines, UE types; prefixes `FUnrealMcp*`/`UUnrealMcp*`/`SUnrealMcp*`/`IUnrealMcp*`; log to `LogUnrealMcp`; copy the copyright one-liner header from a neighbour.

**Build / test** (UE 5.7+, a host project with `Plugins/UnrealMCP` junctioned here; quote engine paths)
1. UBT (`<UE>/Engine/Binaries/DotNET/UnrealBuildTool/`): `UnrealBuildTool.exe <Host>Editor Win64 Development -project=<host>.uproject -WaitMutex` (first build 10+ min).
2. Boot smoke (`<UE>/Engine/Binaries/Win64/`): `UnrealEditor-Cmd.exe <host>.uproject -nullrhi -nosplash -unattended -ExecCmds="QUIT_EDITOR" -log` → expect `[Unreal-MCP] plugin loaded`.
3. Specs: add the test module with `commands/test_module_uplugin.py`, build, run `-ExecCmds="Automation RunTests UnrealMcp; Quit" -ReportExportPath=<dir>`, then revert the descriptor.
4. Judge by `<dir>/index.json` (UTF-8 **BOM**), not the exit code: pass = `failed == 0` and `succeeded >= 1`. Missing `index.json` = crash (check Saved log for `Fatal error` / `[Callstack]`). Screenshot capture can't render under `-nullrhi`.
