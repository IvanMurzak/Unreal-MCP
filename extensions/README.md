# Optional Unreal MCP plugins

Each folder is a separately installable UE plugin with an independent source-ZIP release.
All depend on the UnrealMCP core provider contracts; none requires a sibling extension.

| Plugin | Engine subsystem |
| --- | --- |
| UnrealAINiagara | Niagara VFX |
| UnrealAIEnhancedInput | Enhanced Input |
| UnrealAIPCG | Procedural Content Generation |
| UnrealAIGAS | Gameplay Abilities |
| UnrealAILandscape | Landscape / Water |
| UnrealAIControlRig | Control Rig |

Tool inventories and version/dependency metadata live in each `extension.json`.
See [the monorepo guide](../docs/EXTENSION-MONOREPO.md) for the shared test runner, independent
release workflow, online index and migration compatibility. Original commits/licenses are preserved
in `migration-sources.json` and each plugin's `LICENSE`.
