# Mesh Terrain Water RQL module (Experimental)

**Experimental, Unreal Engine 5.8+ only.** RQL module for Water plugin bodies on Unreal's experimental Mesh Terrain
(the MeshPartitionWater plugin): create lakes and rivers whose water modifiers carve their bed and shores into a Mesh
Terrain, create the ocean around an island, attach a body placed another way to a terrain, detach or delete it, size the
water zone, and read whether each body shapes its terrain and renders (`Ready`) or what to call (`Problem`).

An optional editor plugin in the [Unreal MCP monorepository](../../README.md), with its own module, version and release
archive. It is separate from the [Mesh Terrain extension](../UnrealAIMeshTerrain/README.md), so Mesh Terrain does not
depend on the Water plugin; the two work together (create and shape the terrain with Mesh Terrain, add the water with this
one) but neither needs the other to build or load. MeshPartitionWater is experimental in Unreal Engine 5.8 and changes
between engine versions; so does this extension.

## Requirements

- Unreal Engine 5.8 or later. MeshPartitionWater does not exist before 5.8: on UE 5.5 - 5.7 `unreal-mcp-cli
  install-extension` and the editor's Extensions panel refuse it (`extension.json` `minEngineVersion`), and the shared
  extension suite skips it.
- Unreal MCP 0.31.3 or later, installed in the same project.
- Engine plugins: `MeshPartitionWater`, `Water` and `MeshPartition` (the installer enables them;
  `meshTerrainWater | call EnableMeshTerrainWater()` keeps them in the project file). A Mesh Terrain to put the water on
  (Mesh Terrain Mode, or the Mesh Terrain extension's `meshTerrainCreate | call Create(...)`).
- A C++ toolchain for your Unreal Engine installation. Extension releases contain source files and must be compiled for
  your project and engine.

## Install

With the Unreal Editor closed:

```sh
npm install -g unreal-mcp-cli
unreal-mcp-cli install-plugin "<UEProject>"
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-mesh-terrain-water "<UEProject>"
```

Replace `<UEProject>` with the project directory. Use `--version X.Y.Z` to select a published historical version, or
`--build` to compile after installation. Otherwise rebuild the project before opening the editor.

## RQL module

The extension adds its feature to the editor's RQL data layer (`rql-query` / `rql-mutate` / `rql-schema`,
[docs/rql.md](../../docs/rql.md)) instead of separate tools; its guide is part of the `unreal-rql` skill
(`rql-schema {"guide": "mesh-terrain-water"}`).

| Kind | Names |
| --- | --- |
| Sources | `meshTerrainWaterBodies`, `meshTerrainWater` |
| Operations (`call`) | `meshTerrainWater`: `CreateLake`, `CreateRiver`, `CreateOcean`, `Attach`, `Detach`, `Delete`, `WaitForBuild`, `SetZone`, `SetMaterial`, `EnableMeshTerrainWater` |
| Members | `WaterBody.MeshTerrainWater`, `WaterBody.TerrainModifier`, `MeshPartition.WaterBodies` |

```
meshTerrainWater | call CreateLake(unreal.find("Island"), "Lake", [12000, -8000, 300], 3000)            Z: the water surface
meshTerrainWater | call CreateLake(unreal.find("Island"), "Pond", [0, 0, 150], 1500, 400)               Depth 4 m
meshTerrainWater | call CreateRiver(unreal.find("Island"), "River", [[-20000, 0, 600], [0, 3000, 300], [20000, 0, 0]], 1500, 300)
meshTerrainWater | call CreateOcean("Ocean", 0)
meshTerrainWater | call Attach(unreal.find("WaterBodyLake"), unreal.find("Island"))
meshTerrainWater | call Detach(unreal.find("Ocean"))
meshTerrainWater | call Delete(unreal.find("Pond"))
meshTerrainWaterBodies | select Label MeshTerrainWater
meshTerrains | first | get WaterBodies | select Label
```

How it works: a Water plugin lake or river carries a Mesh Partition water modifier of its kind (`LakeModifier`,
`RiverModifier`, from MeshPartitionWater) bound to the terrain. The modifier shapes the terrain from the body's spline and
its Terrain settings (`CurveSettings.ChannelDepth`, `WaterHeightmapSettings`): the bed `ChannelDepth` below the water
surface, the ground around the water blended to the surface height. It works only while the body's `bAffectsLandscape` is
on. The operations place bodies the way the Place Actors panel does (material, waves), bind the modifier, and return when
the terrain is rebuilt: `{Body, Label, Type, Terrain, Modifier, Location, Ready, Seconds, State, Problem}`.

The ocean needs no modifier (Epic's Mesh Terrain documentation: no setup for oceans; its spline only crops the ocean water
under the terrain): ground above its height hides it. The engine's experimental `OceanModifier` can override the ground
other modifiers shaped under it, so the operations never add one, `MeshTerrainWater.Problem` reports one and `Detach`
removes it.

- `MeshTerrainWater` (member): `Type`, `Terrain`, `Modifier`, `Modifiers`, `AffectsTerrain`, `SurfaceZ`, `ChannelDepth`,
  `SplinePoints`, `Material`, `Zone`, `Ready` and `Problem` (no modifier, bound to no terrain, disabled, Affects Landscape
  off, an ocean with a modifier, no or a non-water material, no water zone, the terrain still building, `-nullrhi`, the water
  mesh not built, shaders compiling), each with the call that repairs it.
- `CreateLake` / `CreateRiver` warn (`call-warning`) where the water surface is well above the ground (the modifier raises a
  plateau or a causeway there) or far below it (a trench).
- Every call is one undo step; on a World Partition map the operations load the terrain's water and modifier actors, and
  `meshTerrainWaterBodies` loads every water body of the map.
- A level that also has a Landscape: water bodies that affect the ground also edit it through the Water brush. While this
  extension is loaded the Water plugin's modal "Insert New Landscape Edit Layer" dialog is off (in memory only), so a tool
  call never blocks on it.

## Test

```shell
py -3 scripts/extension_suite.py --engine "C:/Program Files/Epic Games/UE_5.8" --extensions UnrealAIMeshTerrainWater --mode full
```

The Automation spec (`UnrealAIMeshTerrainWater`) makes its own flat Mesh Terrain far from the origin with the engine's
rectangle generator and deletes it, its water and the water zone it made; the E2E checks under `Tests/e2e/tools` read the
schema, the guide and the refusals through the live MCP server.
