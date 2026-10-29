// Copyright (c) 2026 IvanMurzak/Unreal-AI-Landscape. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

using UnrealBuildTool;

public class UnrealAILandscape : ModuleRules
{
	public UnrealAILandscape(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			// FJsonValue: the Sections / PaintLayers / Water members.
			"Json",
			// The RQL module seam: IUnrealMcpRqlModule.h (sources, operations, members, guides, the editor-module template)
			// and UnrealMcpRqlTools.h (the specs run plans) live in the Unreal-MCP plugin's UnrealMcpRql module; the
			// matching `UnrealMCP` plugin dependency is in the .uplugin "Plugins" array.
			"UnrealMcpRql",

			// The gating engine plugin's runtime module (its { "Name": "Water" } entry is in the .uplugin "Plugins"
			// array): AWaterBody / AWaterZone for the waterBodies / waterZones sources and the Water members.
			"Water",
			// A built-in engine module (no .uplugin entry): ALandscapeProxy / ULandscapeInfo for the landscapes source
			// and the Sections / PaintLayers members.
			"Landscape",
			// The `landscape` operations: UMaterialEditingLibrary (CreateLayerMaterial's Landscape Layer Blend graph).
			"MaterialEditor",
			// LandscapeEdit.h (the height / weight accessors) includes InstancedFoliageActor.h.
			"Foliage",
			"UnrealEd",
		});
	}
}
