// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

using System.IO;
using UnrealBuildTool;

public class UnrealMcpRuntime : ModuleRules
{
	// docs/ARCHITECTURE.md §12.5 + §6.2 / Sidecar/UnrealMcpSidecarManager.cpp::ResolveRid — the .NET RID
	// directory name the C++ resolver looks under (Source/ThirdParty/UnrealMcpBridge/<rid>/ as the
	// Fab-surviving source, staged into Binaries/ThirdParty/UnrealMcpBridge/<rid>/ at package time). Kept in lockstep with ResolveRid:
	// win-x64 / linux-x64 / osx-* (both mac slices). Returns null for a non-desktop platform (console/mobile)
	// — those cannot spawn an external .NET process, so runtime MCP is Desktop-only (Win64/Mac/Linux) and
	// nothing is staged there (matches ResolveRid returning empty on those hosts).
	private static string[] BridgeRidsForPlatform(UnrealTargetPlatform Platform)
	{
		if (Platform == UnrealTargetPlatform.Win64)
			return new string[] { "win-x64" };
		if (Platform == UnrealTargetPlatform.Linux)
			return new string[] { "linux-x64" };
		if (Platform == UnrealTargetPlatform.Mac)
			return new string[] { "osx-arm64", "osx-x64" }; // ResolveRid picks one at runtime from the host CPU
		return null; // non-desktop (console/mobile) — Desktop-only constraint, stage nothing
	}

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
		// Deliberately NO UnrealEd / Slate / EditorSubsystem / AssetRegistry / AssetTools / BlueprintGraph /
		// KismetCompiler / HTTP / FileUtilities / HTTPServer / LiveCoding / ImageWrapper / RenderCore / RHI
		// here — those stay editor-only in UnrealMcpEditor.Build.cs. The runtime module ships only `ping` plus
		// the infra (bridge/registry/dispatcher/sidecar/world-provider/extension-manager), so it pulls no
		// rendering/capture deps; the engine-development tool families (incl. the screenshot family that uses
		// ImageWrapper/RenderCore/RHI) are EDITOR-ONLY. This module must remain GEditor-free (R1 grep gate)
		// and packageable.
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// CoreUObject + Engine are PUBLIC deps (above) — the R3 public UObject headers expose them.
			"Networking",
			"Sockets",
			"JsonUtilities",
			"Projects",
		});

		// R3 (§12.8 #3): the bUnrealMcpAllowShipping Build flag (default false) → UNREAL_MCP_ALLOW_SHIPPING.
		// With 0, UUnrealMcpRuntimeSubsystem::Connect() in a Shipping build logs + returns false — the
		// conservative adopted default (Open question 1). A consumer who deliberately wants runtime MCP in a
		// Shipping build sets bUnrealMcpAllowShipping=true (e.g. via a Target.cs global define or a fork).
		// PublicDefinitions so the guard value is visible to any module that compiles against this one.
		bool bUnrealMcpAllowShipping = false;
		PublicDefinitions.Add("UNREAL_MCP_ALLOW_SHIPPING=" + (bUnrealMcpAllowShipping ? "1" : "0"));

		// R2 (docs/ARCHITECTURE.md §12.5) + Fab readiness (#139): bundle the prebuilt self-contained sidecar
		// payloads into the packaged plugin under Binaries/ThirdParty/UnrealMcpBridge/<rid>/ — the exact path
		// the C++ resolver (FUnrealMcpSidecarManager::ResolveBridgeBinaryPath / ComposeBundledBridgePath) reads
		// at runtime, so a packaged game spawns the bridge with zero install and zero first-run download
		// (§6 BUNDLE model).
		//
		// FAB SOURCE-SUBMISSION CONSTRAINT (#139/#187): Fab accepts a SOURCE plugin and RECOMPILES it on
		// Epic's side per engine version, STRIPPING Binaries/, Intermediate/, and Saved/ from the submitted
		// zip. So the sidecar must NOT live (only) under Binaries/ in the source tree — it would be gone in
		// the Epic-compiled build. Fab review requires redistributed third-party binaries under the
		// engine-canonical Source/ThirdParty/<Lib>/<platform>/ layout, so the prebuilt slices live in the
		// FAB-SURVIVING source folder Source/ThirdParty/UnrealMcpBridge/<rid>/ (declared in
		// Config/FilterPlugin.ini; Source/ also ships in the source zip automatically), and this
		// RuntimeDependencies declaration uses the TWO-ARG (target, source) form so UBT STAGES the surviving
		// source into the package's Binaries/ThirdParty/UnrealMcpBridge/<rid>/ at compile time:
		//   target = $(PluginDir)/Binaries/ThirdParty/UnrealMcpBridge/<rid>/*        (resolver's runtime path)
		//   source = $(PluginDir)/Source/ThirdParty/UnrealMcpBridge/<rid>/*          (Fab-surviving, in the zip)
		// This works for BOTH paths: (a) Epic's Fab recompile sees Source/ThirdParty/UnrealMcpBridge/<rid>/
		// (survived the strip) and stages it; (b) the GitHub-release / release.yml flow stages the SIGNED
		// slices into Source/ThirdParty/UnrealMcpBridge/<rid>/ before BuildPlugin (docs/RELEASING.md), so the
		// same RuntimeDependencies bundles them. The resolver ALSO checks Source/ThirdParty/UnrealMcpBridge/<rid>/
		// directly (ComposeBundledBridgeCandidates) so an Epic-compiled build whose staging differs still
		// resolves from the surviving folder.
		//
		// Why this declaration lives on the RUNTIME module (the whole point of R2): UBT only stages a module's
		// RuntimeDependencies into a build whose target includes that module. UnrealMcpRuntime (Type: Runtime)
		// is part of BOTH the editor target AND a packaged Game target, so its RuntimeDependencies bundle into
		// packaged Development/Shipping GAME builds — not just editor packages. (When it lived on the Editor
		// module it staged into editor packages only; a Game target omits the editor module entirely, so a
		// packaged game shipped without the sidecar — the bug R2 fixes.) The editor BuildPlugin path still
		// bundles because the editor module depends on UnrealMcpRuntime, so UBT honours this module's
		// RuntimeDependencies in the editor package as well — no regression.
		//
		// Conservative gating (adopted defaults, design Open questions 1 & 2):
		//  - Stage ONLY the targeted-platform RID(s) (BridgeRidsForPlatform), not a recursive "..." wildcard
		//    over all four RIDs. Each self-contained slice is ~73-80 MB, so a per-platform package carries
		//    only the slice it can actually run (e.g. a Win64 game stages win-x64 alone).
		//  - DESKTOP-ONLY: console/mobile cannot spawn an external .NET process, so BridgeRidsForPlatform
		//    returns null there and nothing is staged (mirrors ResolveRid returning empty on those hosts).
		//  - SHIPPING is NOT staged by default: a Shipping game omits the sidecar unless the consumer opts in
		//    via bUnrealMcpAllowShipping (the same flag that gates runtime Connect() in Shipping, §12.8 #3).
		//    Development/editor builds always stage. This keeps a default Shipping build lean and closes the
		//    runtime remote-control surface unless deliberately enabled.
		//
		// The binaries are NEVER committed to git (Source/ThirdParty/UnrealMcpBridge/ payload files are
		// gitignored — only the folder's .gitkeep is tracked); the release job stages the signed per-RID
		// slices into Source/ThirdParty/UnrealMcpBridge/<rid>/ before BuildPlugin (docs/RELEASING.md). The
		// "*" wildcard is a no-op on a dev source checkout that has not
		// staged them, so local source builds still compile and resolve the bridge via UNREAL_MCP_BRIDGE_PATH
		// instead. NonUFS = raw (uncooked) files, correct for native binaries.
		bool bStageSidecar = (Target.Configuration != UnrealTargetConfiguration.Shipping) || bUnrealMcpAllowShipping;
		string[] StageRids = BridgeRidsForPlatform(Target.Platform);
		if (bStageSidecar && StageRids != null)
		{
			foreach (string Rid in StageRids)
			{
				RuntimeDependencies.Add(
					Path.Combine(PluginDirectory, "Binaries", "ThirdParty", "UnrealMcpBridge", Rid, "*"),
					Path.Combine(PluginDirectory, "Source", "ThirdParty", "UnrealMcpBridge", Rid, "*"),
					StagedFileType.NonUFS);
			}
		}
	}
}
