// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

using UnrealBuildTool;

public class UnrealMcpRuntime : ModuleRules
{
	public UnrealMcpRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
		});

		// The engine-agnostic, NON-editor machinery the runtime module owns (docs/ARCHITECTURE.md §12.1):
		//  - Networking/Sockets  -> FUnrealMcpBridgeServer (localhost TCP listener, §1)
		//  - Json/JsonUtilities  -> NDJSON framing + FProperty<->JSON schema/serialization (§1.2, §3)
		//  - Projects            -> IPluginManager (plugin version/paths, bundled-sidecar resolution, §6)
		//  - ImageWrapper/RenderCore/RHI -> the runtime-screenshot subset (kept here for the §12.7 R4
		//    families that will land later; harmless in an editor build, runtime-available in a packaged game)
		// Deliberately NO UnrealEd / Slate / EditorSubsystem / AssetRegistry / AssetTools / BlueprintGraph /
		// KismetCompiler / HTTP / FileUtilities / HTTPServer / LiveCoding here — those stay editor-only in
		// UnrealMcpEditor.Build.cs. This module must remain GEditor-free (R1 grep gate) and packageable.
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"Networking",
			"Sockets",
			"Json",
			"JsonUtilities",
			"Projects",
			"ImageWrapper",
			"RenderCore",
			"RHI",
		});

		// NOTE (R2): RuntimeDependencies.Add(.../UnrealMcpBridge/<rid>/*) deliberately stays in
		// UnrealMcpEditor.Build.cs for now. Moving it here (so the sidecar bundles into packaged GAMES) is
		// the explicit scope of task R2 (unreal-mcp-runtime-sidecar-packaging) — out of scope for R1.
	}
}
