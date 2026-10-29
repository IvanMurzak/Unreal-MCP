# Landscape RQL module

RQL module for Unreal's Landscape and Water systems: create, sculpt and paint landscapes; query landscapes (sections, paint layers), water bodies and water zones.

An optional editor plugin in the [Unreal MCP monorepository](../../README.md), with its own module, version and release archive.

## Requirements

- Unreal Engine 5.7 or later (the shared extension suite verifies UE 5.7 and 5.8).
- Unreal MCP 1.0.0 or later (the RQL data layer), installed in the same project.
- Engine plugins: `Water`. The installer enables these dependencies.
- A C++ toolchain for your Unreal Engine installation. Extension releases contain source files and must be compiled for your project and engine.

## Install

With the Unreal Editor closed:

```sh
npm install -g unreal-mcp-cli
unreal-mcp-cli install-plugin "<UEProject>"
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-landscape "<UEProject>"
```

Replace `<UEProject>` with the project directory. The installer selects this extension's latest indexed version independently of the core and other extensions. Use `--version X.Y.Z` to select a published historical version, or `--build` to compile after installation. Otherwise rebuild the project before opening the editor.

The plugin is installed under `Plugins/UnrealAILandscape/`. Connect Unreal MCP to your AI client to use the RQL module below.

## RQL module

The extension adds its feature to the editor's RQL data layer (`rql-query` / `rql-mutate` / `rql-schema`, [docs/rql.md](../../docs/rql.md)) instead of separate tools; its guide is part of the `unreal-rql` skill.

| Kind | Names |
| --- | --- |
| Sources | `landscapes`, `waterBodies`, `waterZones`, `landscape` |
| Operations (`call`) | `Create`, `Sculpt`, `Paint`, `CreateLayerMaterial` |
| Members | `LandscapeProxy.IsPrimary`, `LandscapeProxy.ComponentCount`, `LandscapeProxy.Sections`, `LandscapeProxy.PaintLayers`, `WaterBody.WaterType`, `WaterBody.WaterMaterial`, `WaterBody.WaterZone`, `WaterBody.Water` |

```
landscapes | select Label Sections PaintLayers LandscapeMaterial
waterBodies | select Label Water
waterZones | select Label ZoneExtent
landscape | call CreateLayerMaterial("/Game/Landscape/M_Terrain", "Grass #3a7d2c, Rock #7a7a7a")
landscape | call Create("Terrain", [0, 0, 0], 100, "/Game/Landscape/M_Terrain")
landscape | call Sculpt(unreal.find("Terrain"), [2000, 1500, 0], 1200, 600)
landscape | call Paint(unreal.find("Terrain"), "Grass", [0, 0, 0], 0)
```

`Create` makes a flat landscape of whole 63-quad components scaled to the requested size; `Sculpt` raises or lowers a
smooth circular area (cm); `Paint` adds a paint layer by name (creating its layer info asset) and paints it with a soft
edge; `CreateLayerMaterial` makes a material whose Landscape Layer Blend has one coloured layer per name.

## Releases and development

Each extension has its own versioned source ZIP in [Unreal MCP Releases](https://github.com/IvanMurzak/Unreal-MCP/releases). The shared release index records the latest version, historical releases and archive checksums; unchanged extensions are not copied into other component releases.

See the [monorepository and test guide](../../docs/EXTENSION-MONOREPO.md) for independent publication, isolated compilation and the combined one-editor test suite. See [extension development](../../docs/EXTENSIONS.md) for the RQL module API.

Licensed under [Apache License 2.0](LICENSE).
