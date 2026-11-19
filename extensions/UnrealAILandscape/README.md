# Landscape RQL module

RQL module for Unreal's Landscape and Water systems: create, sculpt, flatten, carve, paint and delete landscapes; create oceans, lakes and rivers (a river routed downhill between two points, its bed carved); query landscapes (sections, paint layers), water bodies (with a render status) and water zones.

An optional editor plugin in the [Unreal MCP monorepository](../../README.md), with its own module, version and release archive.

## Requirements

- Unreal Engine 5.7 or later (the shared extension suite verifies UE 5.7 and 5.8).
- Unreal MCP 0.26.0 or later (the RQL runtime and operations these extensions use), installed in the same project.
- Engine plugins: `Water`. The installer enables these dependencies. The plugin also adds the `WaterBodyCollision` collision profile Water requires (its `Config/Engine.ini`), so a project with the extension packages without the "Collision Profile settings do not include an entry for the Water Body Collision profile" cook error.
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
| Sources | `landscapes`, `waterBodies`, `waterZones`, `landscape`, `water` |
| Operations (`call`) | `landscape`: `Create`, `Sculpt`, `Flatten`, `Carve`, `Smooth`, `Ramp`, `RoadBetween`, `Bumps`, `Coverage`, `Paint`, `PaintWhere`, `CreateLayerMaterial`, `Surface`, `Delete`, `Rebuild`; `water`: `CreateOcean`, `CreateLake`, `CreateRiver`, `CreateRiverBetween`, `SetMaterial`, `SetZone`, `EnableWater` |
| Members | `LandscapeProxy.Edits`, `LandscapeProxy.IsPrimary`, `LandscapeProxy.ComponentCount`, `LandscapeProxy.Sections`, `LandscapeProxy.PaintLayers`, `LandscapeProxy.Bounds`, `WaterBody.WaterType`, `WaterBody.WaterMaterial`, `WaterBody.WaterZone`, `WaterBody.Water`, `WaterBody.Path` |

```
landscapes | select Label Sections PaintLayers LandscapeMaterial
waterBodies | select Label Water
waterZones | select Label ZoneExtent
landscape | call CreateLayerMaterial("/Game/Landscape/M_Terrain", "Grass #3a7d2c, Rock #7a7a7a")
landscape | call Create("Terrain", [0, 0, 0], 100, "/Game/Landscape/M_Terrain")
landscape | call Sculpt(unreal.find("Terrain"), [2000, 1500, 0], 1200, 600)
landscape | call Flatten(unreal.find("Terrain"), [-3000, 2500, 0], 1500, 400, 800)
landscape | call Carve(unreal.find("Terrain"), [[-5000, 0, 0], [5000, 0, 0]], 600, 150)
landscape | call Bumps(unreal.find("Terrain"))
landscape | call Coverage(unreal.find("Terrain"), "Rock")
landscape | call Smooth(unreal.find("Terrain"), [2000, 1500, 0], 800)
landscape | call Ramp(unreal.find("Terrain"), [[-4000, 0, 100], [0, 2000, 250], [4000, 2000, 400]], 800, 500, 8)
landscape | call RoadBetween(unreal.find("Terrain"), [-80000, -80000, 0], [40000, 20000, 0], 800, 8, 500, "road to the top")
landscape | call Paint(unreal.find("Terrain"), "Grass", [0, 0, 0], 0)
landscape | call PaintWhere(unreal.find("Terrain"), "Rock", 30, 90)
landscape | call Surface(unreal.find("Terrain"), [2000, 1500, 0])
landscapes | where IsPrimary | select Label Bounds
landscape | call Delete(unreal.find("Terrain"))
water | call CreateOcean("Ocean", 0)
water | call CreateRiverBetween("River", unreal.find("Terrain"), [-20000, 30000, 0], [60000, -90000, 0], 1500, 300)
waterBodies | select Label Water Path
```

`Create` makes a flat landscape of about one vertex per metre in at most 8 x 8 components (63-quad sections, 2 x 2
per component above 504 m, 127-quad sections above 1008 m, so a 2 km landscape takes seconds), scaled to the requested
size; `Sculpt` raises or lowers a smooth circular area (cm: the full height at the centre, a cosine down to nothing at
`Radius`); `Flatten` levels everything within `Radius` to a world height (a building pad), then blends back to the old
ground between `Radius` and `Radius + Falloff` (`Falloff` defaults to 0: a sharp step at the edge); `Carve` digs a channel a given depth below the ground along your
points (a riverbed or ditch: a level bottom `Width` wide, then 1:1 banks at most 2 x `Width` long); `Smooth` replaces the ground within a radius by
the smoothest surface meeting the ground around it (a spike, pit or bump blends away, a slope stays a slope); `Ramp` sets a
strip along points to their Z (a road bed or ramp, cut and fill, sides blended over `Falloff`; `MaxSlope` refuses a stretch
steeper than that); `RoadBetween` finds a route from a start to an end no steeper than `MaxSlope` (an A* search on a grid of nodes
half the road's width apart: switchbacks or a spiral up a hill), grades it like `Ramp` and returns `Reached`, `LengthMeters`, the built
`MaxSlope`, the `Points` and `Changed` (`Reached` false: it found no route that climbs there within `MaxSlope`, the road goes as
far as it found one); use it for a road up a hill, `Ramp` for a route you choose; `Bumps` (read-only) lists where the ground stands out of its surroundings (`Kind`: Bump, Pit, or a long
Channel / Ridge; `Location`, `Height`, `Radius`, `Length`); `Coverage` (read-only) lists where a paint layer is painted (`Location`, `Min`, `Max`, `Area`,
`Weight`); `Paint` adds a paint layer by name (creating
a weight-blended layer info asset, so painting Rock takes its weight from Grass) and paints it with a soft edge; `PaintWhere`
moves a layer's weight to `Strength` (0..1; 0 erases, and the other layers get the weight back) wherever the ground matches a rule (slope `MinSlope` .. `MaxSlope` degrees, world height `MinHeight` ..
`MaxHeight` cm, defaults `-1e7` .. `1e7`, optionally only inside a `Within` box `[MinX, MinY, MaxX, MaxY]` with `MinX < MaxX`, `MinY < MaxY`; soft edges 2 degrees and 50 cm past the bounds)
and returns `{Vertices, Matched, Ready, State, Seconds}`, the same arguments, meaning and answer as Mesh Terrain's `PaintWhere`: sand on the beaches,
rock on every slope steeper than 30 degrees, after a base layer painted with `Paint` (radius 0); `CreateLayerMaterial` makes a material whose Landscape Layer Blend has one
coloured layer per name; `Delete` removes a whole landscape with every World Partition streaming proxy (`Destroy` on the
landscape actor alone leaves the proxies behind).

A landscape holds ground within a fixed height range around its Z, `Bounds.HeightRange`: +-256 m by default, in 0.78 cm
steps. `Create`'s optional last argument `MaxHeightMeters` (1 to 10000) makes more room by raising the actor's Z scale to
`max(100, ceil(MaxHeightMeters / 2.56))`, so the height steps grow to Z scale / 128 cm (1024 m: Z scale 400, 3.1 cm).
`Sculpt`, `Flatten` and `Carve` refuse an edit that would leave the range and change nothing (the error names the range and
the `MaxHeightMeters` to recreate with); they never flatten a hilltop at the limit silently.

### Edit log

Every shaping and paint call of these operations that changes the landscape (`Sculpt`, `Flatten`, `Carve`, `Smooth`, `Ramp`, `RoadBetween`, `Paint`, `PaintWhere`, and
`CreateRiverBetween` with a `Depth`) appends a record to the landscape's edit log, read with `landscapes | where IsPrimary | select Edits`
(the log is the landscape actor's: `Edits` of a streaming proxy is empty)
(the last 5000 records, newest first: `Index`, `Op`, `Label`, `Points`, `Radius`, `Width`, `Height`, `Layer`, `Rule`, `Time`).
A path keeps the points that hold it within 50 cm of the real one (at most 1000 per record; the whole log keeps at most
100,000 points, dropping its oldest records first), so a road's switchbacks and a river's bends read back as built. Each of these
operations takes an optional argument `Label` that names the edit (the last one; `PaintWhere`: before `Within`; `CreateRiverBetween`'s edit takes the river's label). The log is an editor-only component of the landscape
actor (`AIEditLog`): saved with the level, changed in the operation's own undo step (undo / redo take the record with the
edit), stripped from cooked games. `Bumps` names the logged edit that made each place (`Edit`). Edits made any other way
(Landscape mode, Python, heightmap imports) are not recorded. A level saved with a log and opened without this extension
loses the log (its component class is missing) and nothing else.

On a World Partition map, an editor that opens the map loads only the region around the camera. The operations and
`Bounds` load the landscape's streaming proxies they need first (as the World Partition window's region loading does), so
they see the whole landscape and not only the loaded part; `landscapes`, `waterBodies` and `waterZones` load every actor of
their kind on the map, so a far lake or the streaming proxies a deleted landscape left behind are listed too. Loading
changes and dirties nothing. A streaming proxy in a data layer that is not loaded in the editor cannot be loaded this way: an
operation that needs it fails, names it and changes nothing (it used to run on the rest of the ground); load the data layer
in the editor first.

`Create`, `Sculpt` and `Paint` return with the landscape's edit layers merged: its heights, heightfield collision (line
traces, `GetHeightAtLocation`, actors placed on the ground) and rendered surface match as soon as the call ends. Without
that the engine merges on the GPU over the next editor frames, and a caller that sculpts and then traces or places actors
in the same step reads the old ground. `Surface` reads the ground back at a world X, Y (`Inside`, `Height` in cm,
`Slope` in degrees, `Location` on the ground, `Normal`, `Layers`: each paint layer's weight; read-only, so `rql-query` runs it) and
`Bounds` gives the landscape's world box with its lowest and highest ground, and where they are (`Highest`,
`Lowest`: [X, Y, Z], to a vertex). `Rebuild` merges every edit layer of a
landscape edited elsewhere (Landscape mode, Python) again and rebuilds its collision and, when enabled, its Nanite mesh.
An editor without rendering (`-nullrhi`, commandlets) cannot merge: the edits stay in their edit layer until a rendering
editor opens the landscape, and `Rebuild` fails there.

### Water

`CreateOcean` (surface at a height, filling the water zone: land above it hides it, so one ocean surrounds an island),
`CreateLake` (round, its centre's Z the water surface), `CreateRiver` (through your points, upstream first) and
`CreateRiverBetween` (along the lowest ground of a landscape from a source to a mouth, the water flowing downhill all the
way; Depth > 0 carves its bed so it shows across a rise; on flat ground it winds from side to side, `Meander` 0..1, default 1,
0 for the straight lowest route; when its bed cuts a road of the edit log the answer warns where) place bodies the way the Place Actors panel does (the Water
defaults: material, waves), create the water zone when there is none (`SetZone` resizes it; 0 covers every landscape
plus 25%) and build the water mesh before returning. Each call is bounded work (a river on a 2 km landscape routes and
carves in about a second); the first water material then compiles its shaders for a while, and `Water.Ready` /
`Water.Problem` read back whether a body renders and, when not, what to call. `SetMaterial` assigns a water material
safely (it refuses a material without "Used with Water"; with none it applies the Water defaults of the body's type).
`EnableWater` lists Water in the project file so packaged games include the water actors.

These bodies never edit the landscape through the Water brush. A water body that does (the Water default, e.g. one
placed with `SpawnActor` or Python) makes the Water plugin spawn a `WaterBrushManager` that asks the landscape for a
"Water" edit layer and, when there is none, opens the modal "Insert New Landscape Edit Layer" dialog, which blocks the
editor and every tool call until a human closes it. While the extension is loaded that dialog is off (Landscape
settings, in memory only): the engine still creates the layer where the dialog's OK puts it, and it can be reordered in
Landscape mode. A body spawned without the Water defaults (`SpawnActor`, Python: no material, invisible, and a later
`set_water_material` makes the water zone draw it without its water mesh, an engine ensure) gets the Place Actors
defaults of its type on the next tick, its spline kept.

## Breaking changes

- **`PaintWhere` now works as Mesh Terrain's `PaintWhere`** (next release after 0.3.0). It moves the layer's weight to
  `Strength` where the rule matches, so a lower `Strength` lowers a weight and `Strength` 0 erases the layer (the other
  layers get the weight back); before, it only raised a weight and refused 0. It refuses swapped `Within` corners
  (`MinX < MaxX`, `MinY < MaxY`) instead of sorting them, its height bounds default to `-1e7` .. `1e7` cm (were `-1e9` ..
  `1e9`), and it returns `{Vertices, Matched, Ready, State, Seconds}` instead of the number of changed vertices (read
  `.Vertices`).
- **`LandscapeProxy.Edits`** is the landscape actor's only: a streaming proxy's is empty (it repeated the whole log).

## Releases and development

Each extension has its own versioned source ZIP in [Unreal MCP Releases](https://github.com/IvanMurzak/Unreal-MCP/releases). The shared release index records the latest version, historical releases and archive checksums; unchanged extensions are not copied into other component releases.

See the [monorepository and test guide](../../docs/EXTENSION-MONOREPO.md) for independent publication, isolated compilation and the combined one-editor test suite. See [extension development](../../docs/EXTENSIONS.md) for the RQL module API.

Licensed under [Apache License 2.0](LICENSE).
