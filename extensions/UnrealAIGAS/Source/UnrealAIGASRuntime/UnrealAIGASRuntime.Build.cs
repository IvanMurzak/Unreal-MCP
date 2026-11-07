// Copyright (c) 2026 IvanMurzak/Unreal-AI-GAS. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

using UnrealBuildTool;

/**
 * The extension's runtime module: generic, data-driven Gameplay Ability System classes that projects use from
 * Blueprints and data (an attribute set with common attributes, an ability system component that creates and
 * initializes it, grants abilities and binds them to Enhanced Input, a configurable ability and projectile, an attribute
 * display), so a typical "health, mana, fireball" task needs no project C++. It does not depend on Unreal MCP.
 */
public class UnrealAIGASRuntime : ModuleRules
{
	public UnrealAIGASRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			// The public classes derive from UAttributeSet / UAbilitySystemComponent / UGameplayAbility and use gameplay tags.
			"GameplayAbilities",
			"GameplayTags",
			"GameplayTasks",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// Attribute replication (DOREPLIFETIME).
			"NetCore",
			// The component binds abilities to Enhanced Input actions (the matching plugin is in the .uplugin "Plugins" array).
			"EnhancedInput",
		});
	}
}
