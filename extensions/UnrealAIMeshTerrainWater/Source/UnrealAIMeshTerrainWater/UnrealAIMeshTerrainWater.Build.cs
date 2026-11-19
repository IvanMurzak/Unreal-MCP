// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

using UnrealBuildTool;

public class UnrealAIMeshTerrainWater : ModuleRules
{
	public UnrealAIMeshTerrainWater(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		// The Mesh Partition types (the water modifiers included) live in the UE::MeshPartition namespace.
		bAllowUETypesInNamespaces = true;
		// MeshPartitionCompiledSection.h (included by MeshPartitionEditorComponent.h) includes the Engine module's Internal
		// MaterialCache/MaterialCacheVirtualTexture.h, which UBT only exposes to engine modules.
		PrivateIncludePaths.Add(System.IO.Path.Combine(EngineDirectory, "Source/Runtime/Engine/Internal"));

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			// FJsonValue: the computed members.
			"Json",
			// The RQL module seam: IUnrealMcpRqlModule.h and UnrealMcpRqlTools.h live in the Unreal-MCP plugin's UnrealMcpRql
			// module; the matching `UnrealMCP` plugin dependency is in the .uplugin "Plugins" array.
			"UnrealMcpRql",
			// The gating engine plugins (their entries are in the .uplugin "Plugins" array, UE 5.8+): AMeshPartition (runtime
			// module), UMeshPartitionEditorComponent / APreviewSection / UModifierComponent (editor module) ...
			"MeshPartition",
			"MeshPartitionEditor",
			// ... the water modifiers (UE::MeshPartition::UWaterModifier and its Ocean / Lake / River kinds) ...
			"MeshPartitionWater",
			// ... and the Water plugin: AWaterBody / AWaterZone (runtime) and its editor module (the actor factories).
			"Water",
			"WaterEditor",
			// FDynamicMesh3 (the preview sections' meshes; the spec's rectangle generator holds them).
			"GeometryCore",
			// A built-in engine module (no .uplugin entry): ULandscapeSettings, the Water plugin's edit-layer dialog switch.
			"Landscape",
			"UnrealEd",
			// IPluginManager / IProjectManager: the plugin guard and EnableMeshTerrainWater.
			"Projects",
		});
	}
}
