// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

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
		//  - Slate/SlateCore     -> main window + 4 aux tabs (§7)
		//  - Projects            -> IPluginManager (plugin version/paths, sidecar download, §6)
		//  - DeveloperSettings   -> ISettingsModule-backed settings page (§7)
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"UnrealEd",
			"Slate",
			"SlateCore",
			"Networking",
			"Sockets",
			"Json",
			"JsonUtilities",
			"Projects",
			"EditorSubsystem",
			"DeveloperSettings",
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
	}
}
