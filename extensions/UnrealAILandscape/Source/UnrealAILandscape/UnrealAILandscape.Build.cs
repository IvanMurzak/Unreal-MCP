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
			"Projects",
			// FJsonValue for the module's JSON members.
			"Json",

			// --- Unreal-MCP contract (REQUIRED) ---------------------------------------------------
			// IUnrealMcpRqlModule.h (the RQL module seam: sources, operations, members, guides) and
			// UnrealMcpRqlTools.h (the specs run plans) live in the Unreal-MCP plugin's UnrealMcpRql module. The
			// matching `UnrealMCP` plugin dependency is declared in the .uplugin's "Plugins" array.
			"UnrealMcpRql",

			// --- Editor support ------------------------------------------------------------------
			// UnrealEd: GEditor + GetEditorWorldContext().World() — every tool inspects the active
			// editor world. (Editor.h lives in UnrealEd.)
			"UnrealEd",

			// --- Feature engine modules (THE GATING) ---------------------------------------------
			// "Water" is the gating ENGINE PLUGIN (its runtime module). This dependency IS the gate:
			// the extension won't compile or load without the Water plugin present in the host project.
			// The matching { "Name": "Water" } entry is declared in the .uplugin "Plugins" array.
			// (No "WaterEditor": every tool here reads runtime Water classes — AWaterBody /
			// UWaterBodyComponent / AWaterZone / UWaterSplineComponent — so no editor-only Water API is
			// touched. "Landscape" below is a BUILT-IN engine module, NOT a plugin, so it takes a plain
			// module dependency and NO .uplugin "Plugins" entry.)
			"Water",
			"Landscape",
		});
	}
}
