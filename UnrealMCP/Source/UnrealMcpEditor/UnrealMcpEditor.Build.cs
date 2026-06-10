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
			// Blueprint tool family (§10): FKismetEditorUtilities / FBlueprintEditorUtils / FCompilerResultsLog
			// live in UnrealEd; the K2 pin-type schema + event/function K2 nodes live in BlueprintGraph; the
			// compiler backend (EBlueprintCompileOptions) in KismetCompiler. AssetRegistry registers freshly
			// created Blueprint assets so later tools resolve them by path within the session.
			"BlueprintGraph",
			"KismetCompiler",
			"AssetRegistry",
		});
	}
}
