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
			"Projects",
			// FJsonValue for the module's JSON members.
			"Json",

			// --- Unreal-MCP contract (REQUIRED) ---------------------------------------------------
			// IUnrealMcpRqlModule.h (the RQL module seam: sources, operations, members, guides) and
			// UnrealMcpRqlTools.h (the specs run plans) live in the Unreal-MCP plugin's UnrealMcpRql module. The
			// matching `UnrealMCP` plugin dependency is declared in the .uplugin's "Plugins" array.
			"UnrealMcpRql",

			// --- Your feature's engine modules (THE GATING) ---------------------------------------
			// This dependency IS the "gating": the extension won't compile or load without the engine
			// plugin it targets. The GATING plugin (GameplayAbilities) is declared in the .uplugin
			// "Plugins" array; here we depend on its RUNTIME module, which carries every type the tools
			// touch (UGameplayAbility, UGameplayEffect, UAttributeSet, UAbilitySystemComponent,
			// FGameplayAbilitySpec). The plugin's editor module is "GameplayAbilitiesEditor"
			// (Type=UncookedOnly), but NONE of these tools call an editor-only GAS API, so it is NOT a
			// dependency here (the iter-01 scaffold seeded it; dropped per the verify-real-module-names rule).
			"GameplayAbilities",

			// --- Support modules this extension's tools call ----------------------------------------
			// GameplayTags : FGameplayTagContainer / FGameplayTag string-ify in gas-inspect-ability
			//                (a public dep of GameplayAbilities, declared explicitly since we use it directly).
			// AssetRegistry: enumerate UGameplayAbility / UGameplayEffect / UAttributeSet subclasses
			//                without loading them (the three gas-list-* tools, via GetDerivedClassNames).
			// UnrealEd     : GEditor + the editor world context for gas-grant-ability.
			"GameplayTags",
			"AssetRegistry",
			"UnrealEd",
		});
	}
}
