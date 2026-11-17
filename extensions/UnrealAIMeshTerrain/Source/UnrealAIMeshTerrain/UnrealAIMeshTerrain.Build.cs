// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

using UnrealBuildTool;

public class UnrealAIMeshTerrain : ModuleRules
{
	public UnrealAIMeshTerrain(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		// The Mesh Partition types live in the UE::MeshPartition namespace (UCLASSes included).
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
			"Chaos",
			// FJsonValue: the computed members (Definition, Sections, Modifier, ...).
			"Json",
			// The RQL module seam: IUnrealMcpRqlModule.h and UnrealMcpRqlTools.h live in the Unreal-MCP plugin's UnrealMcpRql
			// module; the matching `UnrealMCP` plugin dependency is in the .uplugin "Plugins" array.
			"UnrealMcpRql",
			// The gating engine plugin (its { "Name": "MeshPartition" } entry is in the .uplugin "Plugins" array, UE 5.8+):
			// AMeshPartition, UMeshPartitionDefinition, ACompiledSection (runtime module) ...
			"MeshPartition",
			// ... and UMeshPartitionEditorComponent, UMeshPartitionEditorSubsystem, the modifier components, APreviewSection.
			"MeshPartitionEditor",
			// FDynamicMesh3 (the preview sections' meshes, Surface).
			"GeometryCore",
			// Brush strokes (Sculpt / Paint): the Mesh Terrain Mode tools' modifier tool target (UEditableModifierToolTarget,
			// MeshPartitionModelingToolset), its IDynamicMeshCommitter interface (ModelingComponents) and the tool framework.
			"MeshPartitionModelingToolset",
			"ModelingComponents",
			"InteractiveToolsFramework",
			"UnrealEd",
			// IPluginManager / IProjectManager: the plugin guard and EnableMeshTerrain.
			"Projects",
			// The modifier operations: shapes as UDynamicMeshComponent meshes made with GeometryScript primitives (the Shapes
			// tools' generators), splines (USplineComponent, Engine), the lattice's ILatticeStateStorage (ModelingComponents, above).
			"GeometryFramework",
			"GeometryScriptingCore",
			// ImportHeightmap: decode .png heightmaps (any bit depth) and hand Mesh Partition's importer a 16-bit gray .png.
			"ImageWrapper",
			"ImageCore",
		});
	}
}
