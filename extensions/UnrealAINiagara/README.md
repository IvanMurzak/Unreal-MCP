# Niagara RQL module

RQL module for Unreal's Niagara VFX system: discover templates, create systems, add emitters and modules, set module inputs (values, random ranges, curves), user parameters and materials, add scene collision (particles and Niagara Fluids), compile, place and preview effects.

An optional editor plugin in the [Unreal MCP monorepository](../../README.md), with its own module, version and release archive.

## Requirements

- Unreal Engine 5.7 or later (the shared extension suite verifies UE 5.7 and 5.8).
- Unreal MCP 0.26.0 or later (the RQL runtime and operations these extensions use), installed in the same project.
- Engine plugins: `Niagara`, `ChaosNiagara` (both on by default; Niagara's own modules such as Wind Force and the Niagara Fluids modules use ChaosNiagara types, and the Niagara editor cannot open systems that use them without it). The installer enables these dependencies.
- A C++ toolchain for your Unreal Engine installation. Extension releases contain source files and must be compiled for your project and engine.

## Install

With the Unreal Editor closed:

```sh
npm install -g unreal-mcp-cli
unreal-mcp-cli install-plugin "<UEProject>"
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-niagara "<UEProject>"
```

Replace `<UEProject>` with the project directory. The installer selects this extension's latest indexed version independently of the core and other extensions. Use `--version X.Y.Z` to select a published historical version, or `--build` to compile after installation. Otherwise rebuild the project before opening the editor.

The plugin is installed under `Plugins/UnrealAINiagara/`. Connect Unreal MCP to your AI client to use the RQL module below.

## RQL module

The extension adds its feature to the editor's RQL data layer (`rql-query` / `rql-mutate` / `rql-schema`, [docs/rql.md](../../docs/rql.md)) instead of separate tools; its guide is part of the `unreal-rql` skill, so agents author effects without the Niagara Python API. The operations drive the Niagara editor's own stack view models (the code paths of the System editor), so edits are undoable and the stack, rapid-iteration parameters and dynamic inputs stay consistent.

| Kind | Names |
| --- | --- |
| Sources | `niagaraSystems`, `niagaraEmitters`, `niagaraTemplates`, `niagara` |
| Operations (`call`) | `CreateSystem`, `AddEmitter`, `RemoveEmitter`, `RenameEmitter`, `SetEmitterEnabled`, `SetEmitterProperty`, `AddModule`, `RemoveModule`, `SetModuleEnabled`, `SetModuleInput`, `SetUserParameter`, `EnableCollision`, `SetMaterial`, `Compile`, `SpawnSystem`, `Activate`, `Deactivate`, `EnableFluids` |
| Members | `NiagaraSystem.Emitters`, `.Modules`, `.Inputs`, `.UserParameters`, `.Renderers`; `NiagaraRendererProperties.Emitter` |

```
niagaraTemplates | where Kind != "Emitter" | select Name Path Kind Description
niagara | call CreateSystem("/Game/VFX/NS_Campfire", "/Niagara/DefaultAssets/Templates/Emitters/Fountain")   # or the template's Name: "Fountain"
niagara | call AddEmitter("/Game/VFX/NS_Campfire", "/Niagara/DefaultAssets/Templates/Emitters/HangingParticulates", "Smoke")
unreal.asset("/Game/VFX/NS_Campfire").Inputs | where Module contains "Spawn Rate"
niagara | call SetModuleInput("/Game/VFX/NS_Campfire", "Fountain", "Add Velocity", "Velocity Speed", "random(150, 300)")
niagara | call SetModuleInput("/Game/VFX/NS_Campfire", "Fountain", "Scale Color", "Scale Alpha", "curve(0: 1, 0.7: 0.8, 1: 0)")
niagara | call EnableCollision("/Game/VFX/NS_Campfire")
niagara | call Compile("/Game/VFX/NS_Campfire")
niagara | call SpawnSystem("/Game/VFX/NS_Campfire", "(0, 0, 100)", "Campfire")
niagara | call Activate(unreal.find("Campfire"))
```

- **Templates**: `niagaraTemplates` lists the engine's Niagara system and emitter templates and, when the Niagara Fluids plugin is enabled, its gas (fire, smoke, explosions), liquid and utility templates, with descriptions (read from the asset registry; nothing is loaded). An installed but disabled Niagara Fluids shows as one `Kind "Plugin"` row; `niagara | call EnableFluids()` enables `NiagaraFluids` and `ChaosNiagara` (its modules use ChaosNiagara types) in the project file, then restart the editor.
- **Inputs**: `SetModuleInput` takes text: numbers, vectors, `#rrggbb` colours, enum entries, `random(min, max)`, `curve(t: value, ...)` over the particle's life, `@User.X` links, asset paths, `Property=Value` for data interfaces, `default`.
- **Collision**: `EnableCollision(system, emitter, mode)` adds and configures the Collision module on particle emitters (`RayTrace` on CPU, `SceneDepth` / `DistanceField` on GPU) and turns on world collision of Niagara Fluids grids (physics shapes of the meshes in the grid, static props included; mesh distance fields when the project generates them). Mesh distance fields need Project Settings > Rendering > Generate Mesh Distance Fields (`unreal.settings("Rendering") | set bGenerateMeshDistanceFields = true`) and an editor restart; the result names what is still missing.
- A system that uses types of a plugin that is not loaded (Niagara Fluids without ChaosNiagara) is refused with the plugin to enable, instead of reaching the Niagara editor's assertions.

## Releases and development

Each extension has its own versioned source ZIP in [Unreal MCP Releases](https://github.com/IvanMurzak/Unreal-MCP/releases). The shared release index records the latest version, historical releases and archive checksums; unchanged extensions are not copied into other component releases.

See the [monorepository and test guide](../../docs/EXTENSION-MONOREPO.md) for independent publication, isolated compilation and the combined one-editor test suite. See [extension development](../../docs/EXTENSIONS.md) for the RQL module API.

Licensed under [Apache License 2.0](LICENSE).
