# Sidecar bundle staging (Fab-surviving, engine-canonical ThirdParty layout)

This folder holds the prebuilt, self-contained `unreal-mcp-bridge` sidecar binaries,
one per .NET RID subfolder (`win-x64/`, `osx-arm64/`, `osx-x64/`, `linux-x64/`).

**Why `Source/ThirdParty/UnrealMcpBridge/` and not `Binaries/ThirdParty/`?** Fab (Epic's
marketplace) accepts a *source* plugin and recompiles it per engine version, **stripping
`Binaries/`, `Intermediate/`, and `Saved/`** from the submitted zip. A sidecar that lived
only under `Binaries/` would be gone in the Epic-compiled build. Fab review additionally
requires redistributed third-party binaries to live under the engine-canonical
`Source/ThirdParty/<Lib>/<platform>/` layout (issue #187). This folder survives the strip,
is declared in `Config/FilterPlugin.ini`, and is staged into
`Binaries/ThirdParty/UnrealMcpBridge/<rid>/` at compile time by `UnrealMcpRuntime.Build.cs`'s
`RuntimeDependencies` (docs/ARCHITECTURE.md §6). It contains only prebuilt binaries and no
`*.Build.cs`, so UBT never treats it as a module.

**The binaries are NEVER committed to git** — `.gitignore` ignores everything under each RID
folder except this README and the `.gitkeep` placeholders, so the folder structure ships in the
source zip while the ~73–80 MB self-contained payloads are staged only transiently:

- by `release.yml` (signed, per RID) before `BuildPlugin` for a GitHub/npm release, and
- by Epic's Fab recompile (from the surviving source you submit).

For a local packaged-game verification, publish the sidecar for your RID and place it here:
`bash bridge/publish.sh Release win-x64 --no-zip` then copy `bridge/publish/win-x64/*` into
`UnrealMCP/Source/ThirdParty/UnrealMcpBridge/win-x64/` (see docs/RELEASING.md).
