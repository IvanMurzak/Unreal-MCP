// Copyright (c) 2026 IvanMurzak/Unreal-AI-Niagara. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

using UnrealBuildTool;

public class UnrealAINiagara : ModuleRules
{
	public UnrealAINiagara(ReadOnlyTargetRules Target) : base(Target)
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
			// Uncomment + rename these to the engine plugin/module(s) your tools wrap. This dependency
			// IS the "gating": the extension won't compile or load without the engine plugin it targets.
			// `commands/init.ps1 -FeaturePlugin <Name>` wires the matching { "Name": "<Feature>" } entry
			// into the .uplugin "Plugins" array; uncomment the lines below to take a real code dependency.
			//   e.g. for Niagara: "Niagara", "NiagaraEditor"
			"Niagara",
			"NiagaraEditor",

			// --- Support modules this extension's tools call ----------------------------------------
			// AssetRegistry: enumerate UNiagaraSystem assets without loading them (niagara-list-systems).
			// UnrealEd: GEditor + the editor world context for spawning a component (niagara-spawn-component).
			"AssetRegistry",
			"UnrealEd",
		});
	}
}
