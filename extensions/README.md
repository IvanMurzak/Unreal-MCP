# Optional Unreal MCP plugins

Each folder is a separately installable UE plugin with an independent source-ZIP release.
Each is an RQL module ([`docs/rql.md`](../docs/rql.md)): it adds sources, operations, members and a guide to the
editor's `rql-query` / `rql-mutate` / `rql-schema` tools instead of tools of its own. All depend on the UnrealMCP core
`UnrealMcpRql` module; none requires a sibling extension.

| Plugin | Engine subsystem |
| --- | --- |
| UnrealAINiagara | Niagara VFX |
| UnrealAIEnhancedInput | Enhanced Input |
| UnrealAIPCG | Procedural Content Generation |
| UnrealAIGAS | Gameplay Abilities |
| UnrealAILandscape | Landscape / Water |
| UnrealAIControlRig | Control Rig |
| UnrealAIMeshTerrain | Mesh Terrain / Mesh Partition (Experimental, UE 5.8+) |
| UnrealAIMeshTerrainWater | Water bodies on Mesh Terrain / MeshPartitionWater (Experimental, UE 5.8+) |

Version and dependency metadata live in each `extension.json` (`minEngineVersion`: the oldest engine it builds on); the RQL module itself (sources, operations, members) is listed by `rql-schema` and each README.
See [the monorepo guide](../docs/EXTENSION-MONOREPO.md) for the shared test runner, independent
release workflow, online index and migration compatibility. Original commits/licenses are preserved
in `migration-sources.json` and each plugin's `LICENSE`.
