// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

using UnrealBuildTool;

public class UnrealAITemplate : ModuleRules
{
	public UnrealAITemplate(ReadOnlyTargetRules Target) : base(Target)
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
			// FJsonValue for the sample's JSON member.
			"Json",
			// The RQL module seam (IUnrealMcpRqlModule.h) lives in the Unreal-MCP plugin's UnrealMcpRql module; the
			// matching `UnrealMCP` plugin dependency is declared in the .uplugin "Plugins" array. Add your feature's
			// engine modules here (e.g. "Niagara") — that dependency gates the extension on the engine plugin.
			"UnrealMcpRql",
			// GEditor / the editor world for operations that touch it.
			"UnrealEd",
		});
	}
}
