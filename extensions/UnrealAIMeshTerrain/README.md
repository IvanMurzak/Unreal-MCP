# Mesh Terrain RQL module (Experimental)

**Experimental, Unreal Engine 5.8+ only.** RQL module for Unreal's experimental Mesh Terrain (the Mesh Partition plugin
behind Mesh Terrain Mode): query mesh terrains, their built sections and their modifiers; read the ground height, slope
and paint channels; rebuild a terrain and wait until its mesh, collision and rendering match its modifiers.

An optional editor plugin in the [Unreal MCP monorepository](../../README.md), with its own module, version and release
archive. Mesh Terrain itself is experimental in Unreal Engine 5.8 and changes between engine versions; so does this
extension.

## Requirements

- Unreal Engine 5.8 or later (Mesh Partition does not exist before 5.8; the shared extension suite skips this extension on
  older engines through `extension.json` `minEngineVersion`).
- Unreal MCP 0.31.3 or later, installed in the same project.
- Engine plugins: `MeshPartition` and `MeshTerrainMode` (the installer enables them; `meshTerrain | call EnableMeshTerrain()`
  keeps them in the project file). A World Partition level: Mesh Terrain places its base meshes and modifiers as
  spatially loaded actors.
- A C++ toolchain for your Unreal Engine installation. Extension releases contain source files and must be compiled for
  your project and engine.

## Install

With the Unreal Editor closed:

```sh
npm install -g unreal-mcp-cli
unreal-mcp-cli install-plugin "<UEProject>"
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-mesh-terrain "<UEProject>"
```

Replace `<UEProject>` with the project directory. Use `--version X.Y.Z` to select a published historical version, or
`--build` to compile after installation. Otherwise rebuild the project before opening the editor.

## RQL module

The extension adds its feature to the editor's RQL data layer (`rql-query` / `rql-mutate` / `rql-schema`,
[docs/rql.md](../../docs/rql.md)) instead of separate tools; its guide is part of the `unreal-rql` skill.

| Kind | Names |
| --- | --- |
| Sources | `meshTerrains`, `meshTerrainModifiers`, `meshTerrain`, `meshTerrainCreate`, `meshTerrainBrush` |
| Operations (`call`) | `meshTerrain`: `Rebuild`, `WaitForBuild`, `Surface`, `SurfacePoints`, `EnableMeshTerrain`; `meshTerrainCreate`: `Create`, `ImportHeightmap`, `Delete`; `meshTerrainBrush`: `Sculpt`, `Paint`, `Erase` |
| Members | `MeshPartition.Definition`, `MeshPartition.PriorityLayers`, `MeshPartition.Channels`, `MeshPartition.Bounds`, `MeshPartition.Build`, `MeshPartition.Sections`, `MeshPartition.Modifiers`, `MeshPartition.BaseMeshes`, `ModifierComponent.Terrain`, `ModifierComponent.Kind`, `ModifierComponent.Disabled`, `ModifierComponent.Modifier` |

```
meshTerrains | select Label Build Bounds PriorityLayers Channels
meshTerrainModifiers | select $ref Modifier
meshTerrainModifiers | where Kind == "Noise" | set Intensity = 800
meshTerrain | call WaitForBuild(unreal.find("Terrain"))
meshTerrain | call Surface(unreal.find("Terrain"), [1200, -800, 0])
meshTerrain | call SurfacePoints(unreal.find("Terrain"), [[0, 0, 0], [500, 0, 0], [1000, 0, 0]])
meshTerrain | call Rebuild(unreal.find("Terrain"))
meshTerrainCreate | call Create("Terrain", [0, 0, 0], 500)
meshTerrainCreate | call ImportHeightmap("Alps", "Heightmaps/alps.png", [0, 0, 0], 2000, 100)
meshTerrainCreate | call Delete(unreal.find("Alps"))
meshTerrainBrush | call Sculpt(unreal.find("Terrain"), "Raise", [1200, -800, 0], 1500, 400)
meshTerrainBrush | call Paint(unreal.find("Terrain"), "Red", [1200, -800, 0], 2000, 1)
```

A Mesh Terrain is a `MeshPartition` actor (no other actor class: Mesh Terrain Mode's Create tool makes one) shaped by
modifier components (noise, splines, shapes, sculpt layers, ...) on top of its base meshes. The editor builds it
asynchronously into preview sections: first the full-resolution mesh, then a simplified (Nanite) static mesh with its
collision. Every operation waits for that build (`Build.Ready`), so heights, traces and rendering agree when it returns;
it also loads the terrain's spatially loaded base and modifier actors (World Partition) when they are not.

- `Surface` / `SurfacePoints`: `Inside`, `Height` (world Z, cm, what traces and collision hit), `Slope` (degrees),
  `Location`, `Normal`, `Channels` (the paint weights there, from the evaluated modifier stack).
- `WaitForBuild(terrain, timeoutSeconds = 120)`: after `set` on a modifier or terrain.
- `Rebuild(terrain, timeoutSeconds = 120)`: rebuild every section from the modifiers.
- `Create(label, location, sizeMeters = 200, sizeYMeters = 0 (square), resolution = 0, definition, timeoutSeconds = 120)`:
  a flat terrain centred on `location` (within 200 km of the origin; its ground at `location` Z), 10 .. 20000 m per side, `resolution` quads along X
  (0: about one per metre, at most 1000; explicit at most 2000 per side, square quads), split into sections of at most
  500 x 500 quads like the Create tool's automatic mode (at most 16). The material, priority layers and paint channels come
  from the `MeshPartitionDefinition` (default `/MeshPartition/DataAssets/MPD_Default`; the definition, not the terrain, holds
  the material). One undo step; built when it returns. Labels must be unique among the level's Mesh Terrains.
- `ImportHeightmap(label, file, location, sizeMeters = 1000, heightScale = 100, resolution = 0, definition, timeoutSeconds = 120)`:
  a 16-bit grayscale `.png` (8-bit and colour PNGs are read as gray) or a `.r16` / `.raw` (little-endian 16-bit, square),
  path absolute or relative to the project, at most 8193 pixels per side. **Units are Landscape's**: value 32768 lands on
  `location` Z and one step is `heightScale / 128` cm, so `heightScale` is the Z scale a Landscape would use for the same
  file (0.01 .. 10000; 100: -256 .. +256 m) and a heightmap exported from a Landscape imports at the same heights. The terrain is centred
  on `location`, `sizeMeters` wide on X, Y from the image's aspect (pixels are square), image row 0 at the -Y edge (as
  Landscape). `resolution` is quads along X (0: one per pixel step, so every vertex takes exactly one pixel's value; at
  most 2048; another resolution samples the image bilinearly), sections of at most 512 x 512 quads like the Import tool.
  Mesh Partition's importer reads only 16-bit `.png` files and samples them at texel centres, which would blend neighbouring
  pixels: every heightmap is re-encoded to a temporary 16-bit PNG laid out so the importer's samples land exactly on the
  vertex values, under `Saved/UnrealAIMeshTerrain` (deleted after the import).
- `Delete(terrain)`: the terrain, its base meshes, the modifiers that target it and its built sections (it loads its World
  Partition actors first), one undo step.
- Save the level before Play In Editor: with unsaved base meshes or modifiers the engine asks in a modal dialog
  ("Unsaved Mesh Partition Modifiers").
- `Sculpt(terrain, mode, point, radius, height = 100, strength = 1, falloff = 0.5)`: a brush stroke around a world X, Y
  within `radius` cm (1..25000) with a soft edge (`falloff` 0..1). `Raise` / `Lower` move the ground `height` cm at the
  centre, `Flatten` pulls it to world Z `height`, `Smooth` evens it out; `strength` (0 exclusive) ..1 scales the stroke.
- `Paint(terrain, channel, point, radius, weight = 1, falloff = 0.5)` / `Erase(terrain, channel, point, radius, falloff = 0.5)`:
  move a paint channel of the terrain's definition (`Channels`, e.g. `Red`) towards `weight` 0..1 (Erase: 0).
- Strokes are stored the way Mesh Terrain Mode's Sculpt and Paint tools store theirs: in a brush modifier
  (`ProjectMeshLayersModifier`, the mode's "BrushModifier", labelled `<terrain>_Brush`), the sculpt as offsets in its
  active sculpt layer and the paint in its weight attributes (a `Set` channel entry per painted channel), edited through
  the tools' own tool target. A stroke goes into a brush that covers its area, or near one that may grow to cover it (at
  most 600 x 600 m, about a million vertices), else into a new brush placed after the terrain's other modifiers; so
  repeated strokes accumulate in one brush and stay editable with the mode's Sculpt / Paint tools (select the brush). Each
  call is one undo step and returns when the terrain is rebuilt: `Brush`, `NewBrush`, `Vertices` (changed), `Before` /
  `After` (full-resolution height or weight at the centre), `Ready`, `Seconds`.
- Like the mode's brushes, a brush stores sculpt OFFSETS on top of the modifiers before it: when one of those changes later,
  the sculpted ground moves with it (a flattened pad is no longer flat). A brush that has another modifier after it over
  its area is not reused (a new brush is placed after it). A painted channel is `Set` by the brush over its whole box: the
  channel there keeps the values it had when it was first painted, whatever the modifiers below write later.
- When a stroke fails waiting for the build (timeout), it is already applied: call `WaitForBuild`, do not repeat it.
- Errors are clear RQL failures: an unknown terrain, a timeout outside 1..600 s, a size / resolution / section count
  over the limits, a missing or malformed heightmap, a brush off the terrain, an unknown sculpt mode or paint channel
  (with the ones there are), out-of-range radius / height / strength / weight / falloff, a build that does not finish in time
  (with what is still pending), Mesh Terrain plugins not enabled (with what to enable).

## Development

The shared helpers every operation uses (find a terrain, wait for its build, transactions, load its actors, sample the
surface, evaluate the modifier stack of a region, spawn a flat terrain the way the Create tool does) are in
`Source/UnrealAIMeshTerrain/Private/UnrealAIMeshTerrainCommon.h`. The Automation specs are `UnrealAIMeshTerrain`
(`Automation RunTests UnrealAIMeshTerrain`; create / import / delete: `UnrealAIMeshTerrain.Create`; brush strokes:
`UnrealAIMeshTerrain.Sculpt`); live checks are in `Tests/e2e/tools/`.

## License

Apache License 2.0. See [LICENSE](LICENSE).
