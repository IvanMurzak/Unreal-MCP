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
			// "Json" is needed because the public registry header (UnrealMcpToolRegistry.h) includes
			// Dom/JsonObject.h, and the sample's handler builds a structured result with FJsonObject.
			"Json",
			// The extension contract (IUnrealMcpToolProvider.h) + tool registry (UnrealMcpToolRegistry.h)
			// live in the Unreal-MCP plugin's RUNTIME module (docs/ARCHITECTURE.md §12.1) — extensions are
			// runtime-available (IModularFeatures, §5/§12.9), so the sample depends on the runtime module,
			// not the editor module.
			"UnrealMcpRuntime",
		});
	}
}
