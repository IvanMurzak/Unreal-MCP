# Mesh Terrain RQL module (Experimental)

**Experimental, Unreal Engine 5.8+ only.** RQL module for Unreal's experimental Mesh Terrain (the Mesh Partition plugin
behind Mesh Terrain Mode): create mesh terrains (flat or from a heightmap); query, add, edit, reorder and remove their
modifiers (noise, splines, shapes, lattices, ...); sculpt and paint; read the ground height, slope and paint channels;
rebuild a terrain and wait until its mesh, collision and rendering match its modifiers.

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
| Sources | `meshTerrains`, `meshTerrainModifiers`, `meshTerrain`, `meshTerrainCreate`, `meshTerrainBrush`, `meshTerrainModifier` |
| Operations (`call`) | `meshTerrain`: `Rebuild`, `WaitForBuild`, `Surface`, `SurfacePoints`, `EnableMeshTerrain`; `meshTerrainCreate`: `Create`, `ImportHeightmap`, `Delete`; `meshTerrainBrush`: `Sculpt`, `Paint`, `Erase`, `PaintWhere` |
| Modifier operations (`call`) | `meshTerrainModifier`: `AddNoise`, `AddPatch`, `AddWeight`, `AddSpline`, `AddShape`, `AddLattice`, `AddTexturePatch`, `AddRemesh`, `Add`, `Edit`, `Move`, `SetSplinePoints`, `SetLatticePoints`, `SetOrder`, `Enable`, `Disable`, `Bind`, `Remove` |
| Modifier members | `LatticeModifier.LatticePoints` |
| Members | `MeshPartition.Definition`, `MeshPartition.PriorityLayers`, `MeshPartition.Channels`, `MeshPartition.Bounds`, `MeshPartition.Build`, `MeshPartition.Sections`, `MeshPartition.Modifiers`, `MeshPartition.BaseMeshes`, `ModifierComponent.Terrain`, `ModifierComponent.Kind`, `ModifierComponent.Label`, `ModifierComponent.Bounds`, `ModifierComponent.Disabled`, `ModifierComponent.Modifier` |

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
it also loads the terrain's spatially loaded base and modifier actors (World Partition) when they are not. After the level
is reopened its modifiers are unloaded and `unreal.find("<modifier label>")` finds nothing until they are loaded: listing
`meshTerrainModifiers` (or a terrain's `Modifiers`) loads them.

- `Surface` / `SurfacePoints`: `Inside`, `Height` (world Z, cm, what traces and collision hit), `Slope` (degrees),
  `Location`, `Normal`, `Channels` (the paint weights there, from the evaluated modifier stack). `Normal` and `Slope` are
  the evaluated ground's under the point (a trace on a collision edge, such as a seam between sections, reports the edge's
  normal instead); the stack is evaluated in regions widened until one holds the triangle under the point (a Boolean
  shape's triangles can be 100+ m across).
- `Bounds` (member): the built ground's world box, `Min` / `Max` [X, Y, Z] (Z: the lowest and highest ground) and
  `SizeMeters`; `Base` {Min, Max, SizeMeters}: the terrain's own extent (its base meshes, the size Create or ImportHeightmap
  gave it: the built box is larger where a Boolean Union shape reaches past the edge); `Highest` / `Lowest` [X, Y, Z]:
  where the ground within `Base` is highest and lowest, to a vertex of the sections' collision.
- `Surface`, `SurfacePoints`, `WaitForBuild` and the members are read-only: they work in `rql-query`. They read the
  editor's built sections, which Play In Editor hides: during play they refuse (stop play first).
- `WaitForBuild(terrain, timeoutSeconds = 240)`: after `set` on a modifier or terrain.
- `Rebuild(terrain, timeoutSeconds = 240)`: rebuild every section from the modifiers.
- `Create(label, location, sizeMeters = 200, sizeYMeters = 0 (square), resolution = 0, definition, timeoutSeconds = 240)`:
  a flat terrain centred on `location` (within 200 km of the origin; its ground at `location` Z), 10 .. 20000 m per side, `resolution` quads along X
  (0: about one per metre, at most 1000; explicit at most 2000 per side, square quads), split into sections of at most
  500 x 500 quads like the Create tool's automatic mode (at most 16). The material, priority layers and paint channels come
  from the `MeshPartitionDefinition` (default `/MeshPartition/DataAssets/MPD_Default`; the definition, not the terrain, holds
  the material). One undo step; built when it returns. Labels must be unique among the level's Mesh Terrains.
- `ImportHeightmap(label, file, location, sizeMeters = 1000, heightScale = 100, resolution = 0, definition, timeoutSeconds = 240)`:
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
- An island with the sea all around: Mesh Terrain has no water, so the sea is a height (world Z 0 by convention). The guide's
  recipe: `Create` the terrain larger than the island with its ground below sea level (`Create("Island", [0, 0, -2000], 2400)`),
  raise the land with `AddPatch` (Radius = the flat land, Falloff = the beach slope down to the seabed, Radius + Falloff in cm
  under half the terrain's shorter side), shape mountains on top, and optionally place a plane at Z 0 as the sea surface (turn its
  collision off: `set BodyInstance.CollisionProfileName = "NoCollision"` on its StaticMeshComponent).
- Save the level before Play In Editor: with unsaved base meshes or modifiers the engine asks in a modal dialog
  ("Unsaved Mesh Partition Modifiers"). Place terrains side by side, not overlapping: the editor builds overlapping
  terrains fine, but Play In Editor's section build of one loads the other's actors and logs `LogLinker` warnings
  ("Mapping for ... not provided while outer '/Temp/MeshPartitionBuilderPackage' is instanced").
- `Sculpt(terrain, mode, point, radius, height = 100, strength = 1, falloff = 0.5)`: a brush stroke around a world X, Y
  within `radius` cm (1..25000) with a soft edge (`falloff` 0..1). `Raise` / `Lower` move the ground `height` cm at the
  centre, `Flatten` pulls it to world Z `height`, `Smooth` evens it out; `strength` (0 exclusive) ..1 scales the stroke.
- `PaintWhere(terrain, channel, minSlope = 0, maxSlope = 90, minHeight = -1e7, maxHeight = 1e7, strength = 1, label, within = [])`: paint a
  channel wherever the ground's slope (degrees) and height (world Z, cm) are in range, over the whole terrain or the
  `[MinX, MinY, MaxX, MaxY]` box (cm): sand on beaches, rock on cliffs, grass on flat ground. In range the channel moves to
  `strength`, with soft edges 2 degrees and 50 cm past the bounds; the rest keeps its weight. The slope and height are the
  ground's when the call runs (paint after shaping). Stored in brush modifiers like strokes, one per tile of at most
  600 x 600 m (`<terrain>_Paint` or `label`), at most 64 tiles; a later call over the same tiles reuses them. Returns
  `Brushes`, `NewBrushes`, `Vertices` (changed), `Matched` (in range), `Ready`, `State`, `Note` (tiles left unpainted, e.g.
  over very dense remeshed ground: pass a smaller Within there), `Seconds`.
- `Paint(terrain, channel, point, radius, weight = 1, falloff = 0.5)` / `Erase(terrain, channel, point, radius, falloff = 0.5)`:
  move a paint channel of the terrain's definition (`Channels`, e.g. `Red`) towards `weight` 0..1 (Erase: 0). The channels
  are generic slots (the default definition's Red, Green, Blue, Yellow; its material decides how each looks; the default
  material only tints them with flat debug colours over a grey base): a "rock" layer is painted into one of them. A stroke is at most 250 m in radius: larger features are modifiers (AddShape, AddNoise,
  AddTexturePatch, AddWeight).
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
- Waits: an operation waits at most 240 s for each build it needs, before and after its change (the MCP server ends a tool
  call after 300 s by default; `AddRoad`'s TimeoutSeconds bounds both waits together). When the
  build after a change takes longer (a remeshed shape), the operation still succeeds with `Ready` false and a `State` saying
  the change is applied and still building: call `WaitForBuild`, do not repeat the change. A wait before a change that times
  out fails and changes nothing. `Create` / `ImportHeightmap` whose build takes longer fail saying the terrain is created:
  wait for it, do not create it again. TimeoutSeconds accepts 1..600, but above about 290 s the server drops the call.
- Errors are clear RQL failures: an unknown terrain, a timeout outside 1..600 s, a size / resolution / section count
  over the limits, a missing or malformed heightmap, a brush off the terrain, an unknown sculpt mode or paint channel
  (with the ones there are), out-of-range radius / height / strength / weight / falloff, a build that does not finish in time
  (with what is still pending), Mesh Terrain plugins not enabled (with what to enable).

### Modifiers

`meshTerrainModifier` places and changes the modifiers that shape a terrain, as Mesh Terrain Mode's Modifiers and Shapes
palettes do. Every Add call names its terrain (the modifier is bound to it, never to whichever terrain is nearest); every
call is one undo step, and returns after the terrain is rebuilt with `{Modifier, Actor, Kind, Terrain, Location, PriorityLayer,
Priority, Disabled, Ready, Seconds, State}` (`Location`: the modifier actor's world position). Give each modifier a label (the last argument) to find it again with `unreal.find`: the
default labels ("NoiseModifier", "NoiseModifier2", ...) count across every terrain of the level.

```
meshTerrainModifier | call AddNoise(unreal.find("Terrain"), [0, 0, 0], [8000, 8000], 600, 0.0005, 0.5, "Hills")
meshTerrainModifier | call AddPatch(unreal.find("Terrain"), [1200, -800, 350], 1500, 800, "Pad")
meshTerrainModifier | call AddSpline(unreal.find("Terrain"), [[0, 0, 100], [5000, 2000, 150]], 600, 400, false, "Red")
meshTerrainModifier | call AddShape(unreal.find("Terrain"), "Sphere", [3000, 0, 0], [2000, 2000, 1200], "Subtract")
meshTerrainModifier | call Edit(unreal.find("Hills").Modifier, [-1, -1, -1], -1, -1, 900)
meshTerrainModifier | call SetOrder(unreal.find("Pad").Modifier, 10)
meshTerrainModifier | call Remove(unreal.find("Pad").Modifier)
```

| Operation | What it places / changes |
| --- | --- |
| `AddNoise(terrain, location, size [X, Y], strength, frequency, falloff, label)` | Noise: hills over an area; Strength cm, Frequency 1/cm, Falloff 0..1 |
| `AddPatch(terrain, location, radius, falloff, label)` | Patch: a flat round pad at the location's Z, cut into or built up from any ground height there |
| `AddWeight(terrain, channel, location, radius, falloff, value, label)` | WeightUtility: paints a channel of the definition (value 0..1) on the ground at every height |
| `AddSpline(terrain, points, width, falloff, closed, channel, label)` | Spline: ground along a path at the points' Z (road, river bed, plateau outline) |
| `AddRoad(terrain, points, width = 800, maxSlope = 8, falloff = Width, label, timeoutSeconds = 240)` | Spline: a road along the points' X, Y that follows the ground and cuts or fills where it is steeper than maxSlope (degrees), flat across Width; over a Boolean shape's coarse surface it also places a Remesh box with medium triangles over that shape (`Remesh`) and a SplineRemesh with fine ones in a corridor around the road (`Corridor`), so later edits rebuild only the corridor finely. Corners sharper than 20 degrees are rounded into curves (`RoundedCorners`); the road's spline is straight between its dense points, so it keeps its grade. Returns `{Modifier, Actor, Remesh, Corridor, Terrain, LengthMeters, StartZ, EndZ, EndGroundZ, Reached, MinLengthMeters, MaxSlope (built, along its centre line every metre; every Length / 3000 over 3 km), SteepestAt, RoundedCorners, Ready, Seconds, Note (where it is steeper than MaxSlope, and why), State}`; a path too short for the climb is built as far up as it goes (`Reached` false, `Note` says how long the path must be); a remeshed hill takes a minute or more to build, and every later change to that terrain rebuilds it too |
| `AddShape(terrain, shape, location, size, operation, rotation, label)` | Shapes: Box, Sphere, Cylinder, Capsule, Cone, Torus (X, Y outer diameter, Z ring height) as Boolean Union / Subtract / Trim; Rectangle, Disc as MeshProject |
| `AddLattice(terrain, location, size, resolution, label)` | Lattice: a grid of control points; `SetLatticePoints` moves them (`LatticePoints` lists them) |
| `AddTexturePatch(terrain, texture, location, size, height, label)` | TexturePatch: a heightmap stamp (mid grey = no change, white = +height / 2); needs a rendering editor (under `-nullrhi` the engine applies no texture); its vertical extents (engine default 100 m up and down, which clamp the stamp) are widened to the terrain's heights plus `height` (Edit Strength and Move too), so it takes in all the ground under it, overhangs included |
| `AddRemesh(terrain, location, size, edgeLength, label)` | Remesh: finer or coarser triangles in a box |
| `Add(terrain, kind, location, label)` | any other scriptable kind with its defaults (InstancedPatch, SplineRemesh, MeshProject, Boolean, ProjectMeshLayers), then `set` |
| `Edit(modifier, size, radius, width, strength, falloff)` | the common parameters; a negative value keeps the current one (a negative noise or texture height: `set` it) |
| `Move(modifier, location)`, `SetSplinePoints(modifier, points, closed)`, `SetLatticePoints(modifier, points)` | placement TO a world position, not an offset (Z moves every kind) and paths (closed 1 / 0 / -1 keep) |
| `SetOrder(modifier, priority, priorityLayer)` | build order: the definition's priority layer, then Priority (higher is applied later, on top) |
| `Enable`, `Disable`, `Remove(modifier)`, `Bind(modifier, terrain)` | switch, delete (also a base mesh a deleted terrain left behind), re-bind to another terrain |

A Patch or WeightUtility reaches the ground only within `MaxZDistance / 2` above and below its location (the engine's
default, 200 m in all, would miss the top of a 300 m hill): AddPatch and AddWeight widen it to the terrain's built heights
with 100 m to spare. A terrain raised later beyond that needs a larger `MaxZDistance` (`set` it). A Patch and brush strokes
move the terrain's vertices within their radius; a Boolean shape's surface is made of the shape's own triangles (a 1 km
Sphere's are 100+ m across), so a pad or stroke smaller than those finds no vertex to move there.

Leftovers: `meshTerrainModifiers | where Terrain == null` lists the modifiers bound to no terrain and the base meshes a
deleted terrain left behind (the terrain actor deleted alone, e.g. in the Outliner); `Bind` the modifiers to a terrain or
`Remove` them; base meshes can only be removed. A stray brush
stroke is cancelled exactly by the opposite stroke (Raise / Lower) at the same point with the same radius, height and
falloff: a brush stores offsets.

Not offered: MeshProvider (the base meshes, made by the terrain's create / import), InstancedProjection and
InstancedTexturePatch (their instances are filled by other tools and read-only), SimpleWrite, LevelInstanceAdapter and
TestModifierComponent (engine-internal). Any other property is a `set` on the modifier followed by
`meshTerrain | call WaitForBuild(terrain)`; use the operations instead of `set` to move a modifier (`set Location` on
its actor does not rebuild the terrain), to change a WeightUtility's InnerValue (`Edit` Strength; UE 5.8 builds it from cached
weights) and a Lattice's size (`Edit` Size; `set LatticeResolution` leaves its points mismatched: an engine ensure, so add a new
lattice for another resolution).

## Development

The shared helpers every operation uses (find a terrain, wait for its build, transactions, load its actors, sample the
surface, evaluate the modifier stack of a region, spawn a flat terrain the way the Create tool does) are in
`Source/UnrealAIMeshTerrain/Private/UnrealAIMeshTerrainCommon.h`. The Automation specs are `UnrealAIMeshTerrain`
(`Automation RunTests UnrealAIMeshTerrain`; create / import / delete: `UnrealAIMeshTerrain.Create`; brush strokes:
`UnrealAIMeshTerrain.Sculpt`; modifier operations: `UnrealAIMeshTerrain.Modifiers`); live checks are in `Tests/e2e/tools/`.

## License

Apache License 2.0. See [LICENSE](LICENSE).
