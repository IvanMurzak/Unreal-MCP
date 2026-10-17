// Copyright (c) 2026 IvanMurzak/Unreal-AI-EnhancedInput. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

using UnrealBuildTool;

public class UnrealAIEnhancedInput : ModuleRules
{
	public UnrealAIEnhancedInput(ReadOnlyTargetRules Target) : base(Target)
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

			// --- The gating engine module (THE GATING) --------------------------------------------
			// This dependency IS the "gating": the extension won't compile or load without the engine
			// plugin it targets. The Enhanced Input asset classes the tools author/inspect — UInputAction,
			// UInputMappingContext, FEnhancedActionKeyMapping, EInputActionValueType — all live in the
			// plugin's RUNTIME module `EnhancedInput`. NOTE: the EnhancedInput plugin's EDITOR module is
			// named `InputEditor` (NOT `EnhancedInputEditor`, which does not exist); the asset-authoring
			// tools here need none of it (they create/save via AssetTools / EditorScriptingUtilities /
			// AssetRegistry), so only the runtime module is taken. `commands/init.ps1 -FeaturePlugin
			// EnhancedInput` wired the matching { "Name": "EnhancedInput" } entry into the .uplugin
			// "Plugins" array.
			"EnhancedInput",

			// --- Support modules this extension's tools call ----------------------------------------
			// InputCore: FKey + EKeys (resolve/validate a key name in enhanced-input-add-mapping).
			// AssetRegistry: enumerate Input Action / Mapping Context assets (enhanced-input-list) +
			//   FAssetRegistryModule::AssetCreated for the create tools.
			// EditorScriptingUtilities: UEditorAssetLibrary (DoesAssetExist / LoadAsset / SaveLoadedAsset).
			// UnrealEd: editor context the create/save path runs under.
			"InputCore",
			"AssetRegistry",
			"EditorScriptingUtilities",
			"UnrealEd",
		});
	}
}
