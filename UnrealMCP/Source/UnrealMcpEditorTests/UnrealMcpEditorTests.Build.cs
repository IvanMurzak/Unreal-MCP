// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

using System.IO;
using UnrealBuildTool;

public class UnrealMcpEditorTests : ModuleRules
{
	public UnrealMcpEditorTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UnrealEd",       // GEditor / editor world for actor-family + blueprint-spawn round-trip specs (§10)
			"Json",
			"UnrealMcpEditor",
			// docs/ARCHITECTURE.md §12.3: the infra the specs drive directly (Ndjson §1.2, dispatcher §4,
			// config §8, sidecar §6, bridge §1, extensions §5, AssetScopedRead/PropertyJson/LogCollector)
			// moved to the runtime module. Depend on it so those UNREALMCPRUNTIME_API symbols link.
			"UnrealMcpRuntime",
			"BlueprintGraph", // UK2Node_Event — asserting add-event produced an ENABLED node (§10)
			"HTTPServer",     // the dev-control server header (FUnrealMcpDevControlServer) includes HttpRouteHandle.h
		});

		// Reach BOTH modules' PRIVATE headers so the Automation specs can drive their internals directly.
		// The runtime path resolves the moved-down reach-ins (Bridge/, Dispatch/, Config/, Sidecar/,
		// Extensions/, Tools/UnrealMcpAssetScopedRead.h, Tools/UnrealMcpPropertyJson.h,
		// Tools/UnrealMcpLogCollector.h); the editor path keeps the editor-staying ones (DevControl/,
		// Server/, UI/, Tools/UnrealMcpSourceTools.h). Each type is *_API-exported, so symbols link via
		// the module dependencies above; these only open the include paths.
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "UnrealMcpEditor", "Private"));
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "UnrealMcpRuntime", "Private"));
	}
}
