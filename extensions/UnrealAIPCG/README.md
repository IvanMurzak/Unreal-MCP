# PCG RQL module

RQL module for Unreal's Procedural Content Generation (PCG) framework: author PCG graphs (node types with descriptions, graph templates, nodes, connections, node settings, graph parameters), place them on PCG volumes or actors, generate and read back what they made. Agents use it for procedural scattering: forests, rocks and props over a landscape, filtered by slope and height, kept off roads and splines, with one density parameter and a seed per variant.

An optional editor plugin in the [Unreal MCP monorepository](../../README.md), with its own module, version and release archive.

## Requirements

- Unreal Engine 5.7 or later (the shared extension suite verifies UE 5.7 and 5.8).
- Unreal MCP 0.26.0 or later (the RQL runtime and operations these extensions use), installed in the same project.
- Engine plugins: `PCG` (Procedural Content Generation Framework; the extension depends on it, so enabling the extension enables it). The installer enables these dependencies.
- A C++ toolchain for your Unreal Engine installation. Extension releases contain source files and must be compiled for your project and engine.

## Install

With the Unreal Editor closed:

```sh
npm install -g unreal-mcp-cli
unreal-mcp-cli install-plugin "<UEProject>"
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-pcg "<UEProject>"
```

Replace `<UEProject>` with the project directory. The installer selects this extension's latest indexed version independently of the core and other extensions. Use `--version X.Y.Z` to select a published historical version, or `--build` to compile after installation. Otherwise rebuild the project before opening the editor.

The plugin is installed under `Plugins/UnrealAIPCG/`. Connect Unreal MCP to your AI client to use the RQL module below.

## RQL module

The extension adds its feature to the editor's RQL data layer (`rql-query` / `rql-mutate` / `rql-schema`, [docs/rql.md](../../docs/rql.md)) instead of separate tools; its guide is part of the `unreal-rql` skill, so agents build PCG graphs instead of writing their own C++ or Python generators. The operations use the PCG graph API the graph editor uses (nodes from the palette's settings classes and their preconfigured variants, labelled edges, override pins, user parameters), so edited graphs open and run in the PCG graph editor as if built by hand.

| Kind | Names |
| --- | --- |
| Sources | `pcgGraphs`, `pcgNodeTypes`, `pcgTemplates`, `pcg` |
| Operations (`call`) | `CreateGraph`, `AddNode`, `RemoveNode`, `Connect`, `Disconnect`, `SetNodeSetting`, `SetParameter`, `RemoveParameter`, `BindParameter`, `AddComponent`, `SetGraph`, `SpawnVolume`, `Generate`, `Cleanup` |
| Members | `PCGGraph.Nodes`, `.Edges`, `.Parameters`; `PCGNode.Title`, `.Type`, `.Inputs`, `.Outputs`, `.Settings`; `PCGComponent.BoundGraph`, `.IsGenerating`, `.Generated` |

```
pcgNodeTypes | where Category == "Filter" | select Name Type Description Inputs Outputs
pcg | call CreateGraph("/Game/PCG/PCG_Forest")
pcg | call AddNode("/Game/PCG/PCG_Forest", "Surface Sampler")
pcg | call AddNode("/Game/PCG/PCG_Forest", "Get Landscape Data")
pcg | call Connect("/Game/PCG/PCG_Forest", "GetLandscapeData", "SurfaceSampler", "", "Surface")
pcg | call SetNodeSetting("/Game/PCG/PCG_Forest", "StaticMeshSpawner", "Meshes", "/Game/Trees/SM_Pine: 3, /Game/Trees/SM_Birch: 1")
pcg | call SetParameter("/Game/PCG/PCG_Forest", "Density", "0.02")
pcg | call BindParameter("/Game/PCG/PCG_Forest", "Density", "SurfaceSampler", "PointsPerSquaredMeter")
pcg | call SpawnVolume("/Game/PCG/PCG_Forest", "(0, 0, 0)", "(50000, 50000, 20000)", "Forest")
pcg | call Generate("Forest", 7)
actors | where Label == "Forest" | get Components | where $ is PCGComponent | select Generated
```

- **Discovery**: `pcgNodeTypes` lists every node the graph editor's palette offers (preconfigured variants included) with its category, description and default pins; `pcgTemplates` lists the graph templates (`CreateGraph(path, template)` copies one). A graph's `Nodes` have `Settings` (name, type with enum entries, current value, override pin), `Inputs` / `Outputs` (pins with connections); `Edges` and `Parameters` describe the whole graph.
- **Settings** are text: numbers, `(x, y, z)` vectors, `#rrggbb` colours, enum entries by name, `true` / `false`, asset paths, attribute selectors (`$Position.Z`, `$Density`), filter thresholds as a number or an attribute, nested paths (`ActorSelector.ActorSelectionTag`), a Static Mesh Spawner's weighted `Meshes`; anything else as UE text. A value that does not parse changes nothing; a wrong node, pin, setting or entry fails listing what exists.
- **Parameters**: `SetParameter` adds or sets a graph (user) parameter; `BindParameter` wires it to a node setting through a Get Graph Parameter node and the setting's override pin, so one density parameter drives the sampler.
- **Generation**: `Generate(actor, seed)` runs the actor's PCG components in the editor world and waits for them, returning instance counts per mesh, points, components, actors, and the PCG errors and warnings logged meanwhile (it waits at most 45 s; `Generated: false` means it goes on in the editor: read the `Generated` member; `editor | call SaveLevel()` afterwards keeps the instances in the map); another seed gives another variant. Generated content is PCG's (not part of the undo step); `Cleanup` removes it.
- An open PCG graph editor is reopened on a changed graph so it shows the change. Created graphs stay unsaved until `editor | call SaveAsset(path)`.

## Releases and development

Each extension has its own versioned source ZIP in [Unreal MCP Releases](https://github.com/IvanMurzak/Unreal-MCP/releases). The shared release index records the latest version, historical releases and archive checksums; unchanged extensions are not copied into other component releases.

See the [monorepository and test guide](../../docs/EXTENSION-MONOREPO.md) for independent publication, isolated compilation and the combined one-editor test suite. See [extension development](../../docs/EXTENSIONS.md) for the RQL module API.

Licensed under [Apache License 2.0](LICENSE).
