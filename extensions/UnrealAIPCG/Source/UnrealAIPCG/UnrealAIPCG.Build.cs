// Copyright (c) 2026 IvanMurzak/Unreal-AI-PCG. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

using UnrealBuildTool;

public class UnrealAIPCG : ModuleRules
{
	public UnrealAIPCG(ReadOnlyTargetRules Target) : base(Target)
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
			// plugin it targets. The PCG plugin's modules are PCG (Runtime), PCGEditor (Editor) and
			// PCGCompute (Runtime) — every type this extension wraps (UPCGGraph, UPCGComponent,
			// UPCGNode) lives in the RUNTIME `PCG` module, so we depend on that ONE module. We do NOT
			// depend on `PCGEditor`: it exists, but no tool here calls an editor-only PCG API (the
			// asset-authoring graph editor), and a bogus editor-module dep fails the UBT build. The
			// matching { "Name": "PCG" } entry is wired into the .uplugin "Plugins" array.
			"PCG",

			// --- Support modules this extension's tools call ----------------------------------------
			// AssetRegistry: enumerate UPCGGraph assets without loading them (pcg-list-graphs).
			// UnrealEd: GEditor + the editor world context for the actor/component tools
			// (pcg-add-component / pcg-get-component / pcg-generate).
			"AssetRegistry",
			"UnrealEd",
		});
	}
}
