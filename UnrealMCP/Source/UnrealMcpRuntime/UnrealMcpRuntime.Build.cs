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
			// Json: the UNREALMCPRUNTIME_API headers Tools/UnrealMcpObjectRef.h + Tools/UnrealMcpPropertyJson.h
			// expose TSharedPtr<FJsonObject> in their signatures, so FJsonObject must be visible to any module
			// that includes them across the .dll boundary (BuildPlugin compiles this module without the editor's
			// shared PCH, which previously supplied the transitive include). PUBLIC, not Private, for that reason.
			"Json",
			// R3 (§12.4): the public headers UnrealMcpRuntimeSubsystem.h / UnrealMcpRuntimeSettings.h are
			// UObject types. CoreUObject (UCLASS/GENERATED_BODY), Engine (UGameInstanceSubsystem in
			// Subsystems/GameInstanceSubsystem.h) and DeveloperSettings (UDeveloperSettings, the kill-switch
			// base) must be PUBLIC so any module including these headers across the .dll boundary (e.g. the
			// editor module, which depends on UnrealMcpRuntime publicly) compiles + links them.
			"CoreUObject",
			"Engine",
			"DeveloperSettings",
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
			// CoreUObject + Engine are PUBLIC deps (above) — the R3 public UObject headers expose them.
			"Networking",
			"Sockets",
			"JsonUtilities",
			"Projects",
			"ImageWrapper",
			"RenderCore",
			"RHI",
		});

		// R3 (§12.8 #3): the bUnrealMcpAllowShipping Build flag (default false) → UNREAL_MCP_ALLOW_SHIPPING.
		// With 0, UUnrealMcpRuntimeSubsystem::Connect() in a Shipping build logs + returns false — the
		// conservative adopted default (Open question 1). A consumer who deliberately wants runtime MCP in a
		// Shipping build sets bUnrealMcpAllowShipping=true (e.g. via a Target.cs global define or a fork).
		// PublicDefinitions so the guard value is visible to any module that compiles against this one.
		bool bUnrealMcpAllowShipping = false;
		PublicDefinitions.Add("UNREAL_MCP_ALLOW_SHIPPING=" + (bUnrealMcpAllowShipping ? "1" : "0"));

		// NOTE (R2): RuntimeDependencies.Add(.../UnrealMcpBridge/<rid>/*) deliberately stays in
		// UnrealMcpEditor.Build.cs for now. Moving it here (so the sidecar bundles into packaged GAMES) is
		// the explicit scope of task R2 (unreal-mcp-runtime-sidecar-packaging) — out of scope for R1.
	}
}
