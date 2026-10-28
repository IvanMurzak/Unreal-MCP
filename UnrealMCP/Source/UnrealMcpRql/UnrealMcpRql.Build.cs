// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

using System.IO;
using UnrealBuildTool;

/// <summary>
/// The RQL data layer of the editor (docs/rql.md): the vendored RQL C++ runtime (Source/ThirdParty/RQL, PolyForm
/// Shield 1.0.0), the Unreal reflection provider (UClass / UScriptStruct / UEnum / FProperty / UFunction descriptors),
/// the `editor` operations, the rql-query / rql-mutate / rql-schema tools and the IUnrealMcpRqlModule extension seam.
///
/// The RQL runtime reports failures with C++ exceptions inside its own code (its public API is noexcept), so this
/// module enables exceptions. It is a separate module because exceptions and the no-unity build below apply per
/// module: the vendored sources keep file-local helpers that a unity build would merge into one translation unit.
/// RQL types never cross this module's boundary: extensions use the UE-typed API in IUnrealMcpRqlModule.h.
/// </summary>
public class UnrealMcpRql : ModuleRules
{
	public UnrealMcpRql(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.NoPCHs;
		bUseUnity = false;
		bEnableExceptions = true;

		string RqlRoot = Path.Combine(PluginDirectory, "Source", "ThirdParty", "RQL");
		PrivateIncludePaths.Add(Path.Combine(RqlRoot, "include"));
		PrivateIncludePaths.Add(RqlRoot); // the vendor wrappers include "src/<file>.cpp"
		// The runtime module's private headers (Tools/UnrealMcpObjectRef.h, Tools/UnrealMcpLogCollector.h): the types
		// are UNREALMCPRUNTIME_API-exported, this only opens the include path (same as UnrealMcpEditor).
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "UnrealMcpRuntime", "Private"));

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UnrealMcpRuntime",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Json",
			"Projects", // IPluginManager: UnrealMcpRql::PluginVersion
			"UnrealEd",
			"AssetRegistry",
			"AssetTools",
			"MaterialEditor",
			"EditorScriptingUtilities",
			// Data models reflection does not reach (docs/rql.md): DataTable rows (FDataTableEditorUtils lives in UnrealEd),
			// gameplay tags (GameplayTags; the tag ini editing of the GameplayTagsEditor plugin), Project Settings
			// sections (Settings; SettingsEditor registers the auto-discovered UDeveloperSettings ones), Blueprint graphs (BlueprintGraph / Kismet: K2 nodes, schema, compile), UMG widget trees
			// (UMG) and Level Sequences (LevelSequence / MovieScene / MovieSceneTracks).
			"GameplayTags",
			"GameplayTagsEditor",
			"Settings",
			"SettingsEditor",
			"BlueprintGraph",
			"KismetCompiler",
			"InputCore",
			"UMG",
			"UMGEditor",
			"LevelSequence",
			"MovieScene",
			"MovieSceneTracks",
		});
	}
}
