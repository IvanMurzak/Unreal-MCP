// Copyright (c) 2026 IvanMurzak/Unreal-AI-GAS. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

using UnrealBuildTool;

public class UnrealAIGAS : ModuleRules
{
	public UnrealAIGAS(ReadOnlyTargetRules Target) : base(Target)
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
			// FJsonValue: the GrantedAbilities / Attributes / OwnedTags / ActiveEffects members.
			"Json",
			// The RQL module seam: IUnrealMcpRqlModule.h (sources, operations, members, guides, the editor-module template)
			// and UnrealMcpRqlTools.h (the specs run plans) live in the Unreal-MCP plugin's UnrealMcpRql module; the
			// matching `UnrealMCP` plugin dependency is in the .uplugin "Plugins" array.
			"UnrealMcpRql",

			// The gating engine plugin's runtime module (its { "Name": "GameplayAbilities" } entry is in the .uplugin
			// "Plugins" array): abilities, effects and their components, attribute sets, the AbilitySystemComponent.
			"GameplayAbilities",
			"GameplayTags",
			"GameplayTasks",
			// The extension's runtime classes (UnrealAIAttributeSet, UnrealAIAbilitySystemComponent, UnrealAIGameplayAbility, ...).
			"UnrealAIGASRuntime",
			// The operations: Blueprint assets and their components (UnrealEd, Kismet utilities), new tags in the tag ini
			// (GameplayTagsEditor plugin), input actions / mapping contexts and keys (EnhancedInput, InputCore), the project
			// file (Projects: EnableGameplayAbilities), new assets (AssetRegistry).
			"UnrealEd",
			"GameplayTagsEditor",
			"EnhancedInput",
			"InputCore",
			"Projects",
			"AssetRegistry",
		});
	}
}
