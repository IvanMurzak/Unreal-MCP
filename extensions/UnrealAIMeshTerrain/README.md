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
| Sources | `meshTerrains`, `meshTerrainModifiers`, `meshTerrain` |
| Operations (`call`) | `meshTerrain`: `Rebuild`, `WaitForBuild`, `Surface`, `SurfacePoints`, `EnableMeshTerrain` |
| Modifier operations (`call`) | `meshTerrainModifier`: `AddNoise`, `AddPatch`, `AddWeight`, `AddSpline`, `AddShape`, `AddLattice`, `AddTexturePatch`, `AddRemesh`, `Add`, `Edit`, `Move`, `SetSplinePoints`, `SetLatticePoints`, `SetOrder`, `Enable`, `Disable`, `Bind`, `Remove` |
| Modifier members | `LatticeModifier.LatticePoints` |
| Members | `MeshPartition.Definition`, `MeshPartition.PriorityLayers`, `MeshPartition.Channels`, `MeshPartition.Bounds`, `MeshPartition.Build`, `MeshPartition.Sections`, `MeshPartition.Modifiers`, `MeshPartition.BaseMeshes`, `ModifierComponent.Terrain`, `ModifierComponent.Kind`, `ModifierComponent.Disabled`, `ModifierComponent.Modifier` |

```
meshTerrains | select Label Build Bounds PriorityLayers Channels
meshTerrainModifiers | select $ref Modifier
meshTerrainModifiers | where Kind == "Noise" | set Intensity = 800
meshTerrain | call WaitForBuild(unreal.find("Terrain"))
meshTerrain | call Surface(unreal.find("Terrain"), [1200, -800, 0])
meshTerrain | call SurfacePoints(unreal.find("Terrain"), [[0, 0, 0], [500, 0, 0], [1000, 0, 0]])
meshTerrain | call Rebuild(unreal.find("Terrain"))
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
- Errors are clear RQL failures: an unknown terrain, a timeout outside 1..600 s, a build that does not finish in time
  (with what is still pending), Mesh Terrain plugins not enabled (with what to enable).

### Modifiers

`meshTerrainModifier` places and changes the modifiers that shape a terrain, as Mesh Terrain Mode's Modifiers and Shapes
palettes do. Every call names its terrain (the modifier is bound to it, never to whichever terrain is nearest), is one
undo step, and returns after the terrain is rebuilt with `{Modifier, Actor, Kind, Terrain, PriorityLayer, Priority,
Disabled, Ready, Seconds, State}`.

```
meshTerrainModifier | call AddNoise(unreal.find("Terrain"), [0, 0, 0], [8000, 8000], 600, 0.0005)
meshTerrainModifier | call AddPatch(unreal.find("Terrain"), [1200, -800, 350], 1500, 800)
meshTerrainModifier | call AddSpline(unreal.find("Terrain"), [[0, 0, 100], [5000, 2000, 150]], 600, 400, false, "Red")
meshTerrainModifier | call AddShape(unreal.find("Terrain"), "Sphere", [3000, 0, 0], [2000, 2000, 1200], "Subtract")
meshTerrainModifier | call Edit(unreal.find("NoiseModifier").Modifier, [-1, -1, -1], -1, -1, 900)
meshTerrainModifier | call SetOrder(unreal.find("PatchModifier").Modifier, 10)
meshTerrainModifier | call Remove(unreal.find("PatchModifier").Modifier)
```

| Operation | What it places / changes |
| --- | --- |
| `AddNoise(terrain, location, size [X, Y], strength, frequency, falloff, label)` | Noise: hills over an area; Strength cm, Frequency 1/cm, Falloff 0..1 |
| `AddPatch(terrain, location, radius, falloff, label)` | Patch: a flat round pad at the location's Z |
| `AddWeight(terrain, channel, location, radius, falloff, value, label)` | WeightUtility: paints a channel of the definition (value 0..1) |
| `AddSpline(terrain, points, width, falloff, closed, channel, label)` | Spline: ground along a path at the points' Z (road, river bed, plateau outline) |
| `AddShape(terrain, shape, location, size, operation, rotation, label)` | Shapes: Box, Sphere, Cylinder, Capsule, Cone, Torus (Boolean Union / Subtract / Trim), Rectangle, Disc (MeshProject) |
| `AddLattice(terrain, location, size, resolution, label)` | Lattice: a grid of control points; `SetLatticePoints` moves them (`LatticePoints` lists them) |
| `AddTexturePatch(terrain, texture, location, size, height, label)` | TexturePatch: a heightmap stamp (mid grey = no change, white = +height / 2) |
| `AddRemesh(terrain, location, size, edgeLength, label)` | Remesh: finer or coarser triangles in a box |
| `Add(terrain, kind, location, label)` | any other scriptable kind with its defaults (InstancedPatch, SplineRemesh, MeshProject, Boolean, ProjectMeshLayers), then `set` |
| `Edit(modifier, size, radius, width, strength, falloff)` | the common parameters; a negative value keeps the current one |
| `Move(modifier, location)`, `SetSplinePoints(modifier, points, closed)`, `SetLatticePoints(modifier, points)` | placement and paths |
| `SetOrder(modifier, priority, priorityLayer)` | build order: the definition's priority layer, then Priority (higher is applied later, on top) |
| `Enable`, `Disable`, `Remove(modifier)`, `Bind(modifier, terrain)` | switch, delete, re-bind to another terrain |

Not offered: MeshProvider (the base meshes, made by the terrain's create / import), InstancedProjection and
InstancedTexturePatch (their instances are filled by other tools and read-only), SimpleWrite, LevelInstanceAdapter and
TestModifierComponent (engine-internal). Any other property is a `set` on the modifier followed by
`meshTerrain | call WaitForBuild(terrain)`; use the operations instead of `set` to move a modifier (`set Location` on
its actor does not rebuild the terrain), to change a WeightUtility's InnerValue / OuterValue (UE 5.8 builds it from cached
weights) and a Lattice's size or LatticeResolution (its points would no longer match: an engine ensure).

## Development

The shared helpers every operation uses (find a terrain, wait for its build, transactions, load its actors, sample the
surface, evaluate the modifier stack of a region, spawn a flat terrain the way the Create tool does) are in
`Source/UnrealAIMeshTerrain/Private/UnrealAIMeshTerrainCommon.h`. The Automation spec is `UnrealAIMeshTerrain`
(`Automation RunTests UnrealAIMeshTerrain`; the modifier operations: `UnrealAIMeshTerrain.Modifiers`); live checks are in `Tests/e2e/tools/`.

## License

Apache License 2.0. See [LICENSE](LICENSE).
