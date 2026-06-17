// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

using System.IO;
using UnrealBuildTool;

public class UnrealMcpEditor : ModuleRules
{
	public UnrealMcpEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
		});

		// The dependency set the architecture needs (docs/ARCHITECTURE.md):
		//  - Networking/Sockets  -> FUnrealMcpBridgeServer (localhost TCP listener, §1)
		//  - Json/JsonUtilities  -> NDJSON framing + FProperty<->JSON schema/serialization (§1.2, §3)
		//  - UnrealEd/EditorSubsystem -> editor operations from tool bodies (§3.3, §10)
		//  - Slate/SlateCore     -> main window + aux tabs (§7)
		//  - Projects            -> IPluginManager (plugin version/paths, bundled-sidecar resolution, §6)
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"UnrealEd",
			"Slate",
			"SlateCore",
			"Networking",
			"Sockets",
			// DEV-ONLY inject/control bridge over the live Slate dock (FUnrealMcpDevControlServer): UE's
			// in-process HTTP server. Env-gated (UNREAL_MCP_DEV_CONTROL=1) + loopback-only — never listens
			// in a shipped plugin. The module is always linked; the listener is only started when gated on.
			"HTTPServer",
			"Json",
			"JsonUtilities",
			"Projects",
			"EditorSubsystem",
			// Local gamedev-mcp-server launch/supervision (FUnrealMcpServerManager, docs/ARCHITECTURE.md §7):
			//  - HTTP          -> download the pinned server release zip on a cache miss (override/cache first)
			//  - FileUtilities -> FZipArchiveReader (libzip-backed) to unpack the downloaded release zip
			"HTTP",
			"FileUtilities",
			// Slate main window (docs/ARCHITECTURE.md §7): nomad-tab registration via FGlobalTabmanager
			// needs WorkspaceMenuStructure (the "AI Game Developer" Window-menu group, applied via
			// RegisterNomadTabSpawner().SetGroup); the masked-token field reads keystrokes via InputCore;
			// the copyable user code / token uses FPlatformApplicationMisc::ClipboardCopy (ApplicationCore).
			"WorkspaceMenuStructure",
			"InputCore",
			"ApplicationCore",
			// Asset / Content-Browser tool family (docs/ARCHITECTURE.md §10, issue #10):
			//  - AssetRegistry            -> asset-find / asset-get-data / asset-refresh queries
			//  - AssetTools               -> CreateAsset (material instance) + ImportAssetTasks
			//  - MaterialEditor           -> UMaterialEditingLibrary (instance param read/write)
			//  - EditorScriptingUtilities -> UEditorAssetLibrary (copy/move/delete/folder/load)
			"AssetRegistry",
			"AssetTools",
			"MaterialEditor",
			"EditorScriptingUtilities",
			// Blueprint tool family (§10): FKismetEditorUtilities / FBlueprintEditorUtils / FCompilerResultsLog
			// live in UnrealEd; the K2 pin-type schema + event/function K2 nodes live in BlueprintGraph; the
			// compiler backend (EBlueprintCompileOptions) in KismetCompiler. (AssetRegistry shared above.)
			"BlueprintGraph",
			"KismetCompiler",
			// Screenshot / viewport-capture tool family (docs/ARCHITECTURE.md §10, issue #17):
			//  - ImageWrapper -> PNG encode of the captured FColor buffer (FImageUtils::PNGCompressImageArray)
			//  - RenderCore / RHI -> FViewport::ReadPixels + render-target read-back for SceneCapture2D
			"ImageWrapper",
			"RenderCore",
			"RHI",
		});

		// C++ source / script tool family (docs/ARCHITECTURE.md §10, issue #18): source-compile prefers
		// Live Coding to patch a running editor when interactive. The LiveCoding module is Windows-only
		// (Engine/Source/Developer/Windows/LiveCoding); gate the dependency + the WITH_UNREAL_MCP_LIVE_CODING
		// guard so non-Windows builds (and the UBT fallback path) compile cleanly without it.
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDependencyModuleNames.Add("LiveCoding");
			PublicDefinitions.Add("WITH_UNREAL_MCP_LIVE_CODING=1");
		}
		else
		{
			PublicDefinitions.Add("WITH_UNREAL_MCP_LIVE_CODING=0");
		}

		// Bundle the prebuilt self-contained sidecar payloads (docs/ARCHITECTURE.md §6 BUNDLE model).
		// Staged into the packaged plugin under Binaries/ThirdParty/UnrealMcpBridge/<rid>/ so the editor
		// can spawn the bridge with zero install (FUnrealMcpSidecarManager::ResolveBridgeBinaryPath). The
		// binaries are NOT committed to git; the release job stages them before BuildPlugin (task T4). The
		// recursive "..." wildcard is a no-op on a dev checkout that has not staged them, so local source
		// builds still compile and resolve the bridge via UNREAL_MCP_BRIDGE_PATH instead. NonUFS = raw
		// (not cooked) files, correct for native runtime binaries.
		RuntimeDependencies.Add(
			Path.Combine(PluginDirectory, "Binaries", "ThirdParty", "UnrealMcpBridge", "...", "*"),
			StagedFileType.NonUFS);
	}
}
