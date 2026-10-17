// Copyright (c) 2026 IvanMurzak/Unreal-AI-PCG. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

using UnrealBuildTool;

public class UnrealAIPCG : ModuleRules
{
	public UnrealAIPCG(ReadOnlyTargetRules Target) : base(Target)
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
			// FJsonValue: the Nodes / IsGenerating members.
			"Json",
			// The RQL module seam: IUnrealMcpRqlModule.h (sources, operations, members, guides, the editor-module template)
			// and UnrealMcpRqlTools.h (the specs run plans) live in the Unreal-MCP plugin's UnrealMcpRql module; the
			// matching `UnrealMCP` plugin dependency is in the .uplugin "Plugins" array.
			"UnrealMcpRql",

			// The gating engine plugin's runtime module (its { "Name": "PCG" } entry is in the .uplugin "Plugins" array):
			// UPCGGraph for the pcgGraphs source and Nodes, UPCGComponent for BoundGraph and the AddComponent / Generate
			// operations.
			"PCG",
		});
	}
}
