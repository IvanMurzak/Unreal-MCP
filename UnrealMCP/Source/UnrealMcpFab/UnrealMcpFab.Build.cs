// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

using UnrealBuildTool;

/// <summary>
/// The Fab sign-in companion of UnrealMcpEditor: restores the Epic sign-in the engine's Fab plugin remembers when the editor
/// starts, and tells the Fab tools whether Epic's sign-in service runs. It links EOSShared / EOSSDK, so it stays a separate
/// module that UnrealMcpEditor loads only when the Fab plugin is enabled (LoadingPhase None in UnrealMCP.uplugin).
/// </summary>
public class UnrealMcpFab : ModuleRules
{
	public UnrealMcpFab(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"EOSSDK",
			"EOSShared",
			"UnrealMcpEditor",
			"UnrealMcpRuntime", // LogUnrealMcp
		});
	}
}
