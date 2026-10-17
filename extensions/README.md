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

RQL module inventories (`rql`) and version/dependency metadata live in each `extension.json`.
See [the monorepo guide](../docs/EXTENSION-MONOREPO.md) for the shared test runner, independent
release workflow, online index and migration compatibility. Original commits/licenses are preserved
in `migration-sources.json` and each plugin's `LICENSE`.
