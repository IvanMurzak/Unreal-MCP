// Copyright (c) 2026 IvanMurzak/Unreal-AI-Niagara. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

using UnrealBuildTool;

public class UnrealAINiagara : ModuleRules
{
	public UnrealAINiagara(ReadOnlyTargetRules Target) : base(Target)
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
			// FJsonValue for the module's JSON member (Emitters).
			"Json",

			// --- Unreal-MCP contract (REQUIRED) ---------------------------------------------------
			// IUnrealMcpRqlModule.h (the RQL module seam: sources, operations, members, guides) and
			// UnrealMcpRqlTools.h (the specs run plans) live in the Unreal-MCP plugin's UnrealMcpRql module. The
			// matching `UnrealMCP` plugin dependency is declared in the .uplugin's "Plugins" array.
			"UnrealMcpRql",

			// --- The feature's engine module (THE GATING) -----------------------------------------
			// The extension won't compile or load without the engine plugin it targets.
			"Niagara",

			// --- Support modules ----------------------------------------------------------------------
			// UnrealEd: GEditor + the editor world and actor labels (SpawnSystem).
			"UnrealEd",
		});
	}
}
