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
			"Projects",
			// "Json" is needed because the public registry header (UnrealMcpToolRegistry.h) includes
			// Dom/JsonObject.h, and the sample handler builds a structured result with FJsonObject.
			"Json",

			// --- Unreal-MCP contract (REQUIRED) ---------------------------------------------------
			// The extension contract (IUnrealMcpToolProvider.h) + tool registry (UnrealMcpToolRegistry.h)
			// live in the Unreal-MCP plugin's RUNTIME module. UnrealMcpEditor re-exports those headers
			// and gives editor-only API access (most tools touch the editor). Keep both — they are the
			// spine of every extension. The matching `UnrealMCP` plugin dependency is declared in the
			// .uplugin's "Plugins" array.
			"UnrealMcpRuntime",
			"UnrealMcpEditor",

			// --- Your feature's engine modules (THE GATING) ---------------------------------------
			// This dependency IS the "gating": the extension won't compile or load without the
			// ControlRig engine plugin it targets (the matching { "Name": "ControlRig" } entry is in
			// the .uplugin "Plugins" array). The ControlRig plugin's real modules are `ControlRig`
			// (Runtime — URigHierarchy / UControlRigComponent / UControlRig) and `ControlRigDeveloper`
			// (UncookedOnly — UControlRigBlueprint, the authoring asset that owns the rig hierarchy this
			// read-only family inspects). NO editor-only ControlRig API is used, so `ControlRigEditor`
			// is intentionally NOT a dependency.
			"ControlRig",
			"ControlRigDeveloper",

			// --- Support modules this extension's tools call ----------------------------------------
			// AssetRegistry: enumerate UControlRigBlueprint assets without loading them (control-rig-list).
			// UnrealEd: GEditor + the editor world context to resolve an actor (control-rig-get-component).
			"AssetRegistry",
			"UnrealEd",
		});
	}
}
