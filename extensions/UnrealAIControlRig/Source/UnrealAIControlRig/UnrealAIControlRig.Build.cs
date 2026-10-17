// Copyright (c) 2026 IvanMurzak/Unreal-AI-ControlRig. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

using UnrealBuildTool;

public class UnrealAIControlRig : ModuleRules
{
	public UnrealAIControlRig(ReadOnlyTargetRules Target) : base(Target)
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
			// FJsonValue: the Elements / Hierarchy / Controls / Bones members.
			"Json",
			// The RQL module seam: IUnrealMcpRqlModule.h (sources, operations, members, guides, the editor-module template)
			// and UnrealMcpRqlTools.h (the specs run plans) live in the Unreal-MCP plugin's UnrealMcpRql module; the
			// matching `UnrealMCP` plugin dependency is in the .uplugin "Plugins" array.
			"UnrealMcpRql",

			// The gating engine plugin (its { "Name": "ControlRig" } entry is in the .uplugin "Plugins" array):
			// ControlRig (Runtime) has URigHierarchy and UControlRigComponent (the BoundRig member); ControlRigDeveloper
			// (UncookedOnly) has UControlRigBlueprint, the controlRigs source and the owner of the rig hierarchy.
			"ControlRig",
			"ControlRigDeveloper",
		});
	}
}
