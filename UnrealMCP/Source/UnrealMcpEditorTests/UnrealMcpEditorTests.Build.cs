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
			"Json",
			"UnrealMcpEditor",
			"UnrealEd", // GEditor / editor world for the blueprint-spawn round-trip spec (§10)
		});

		// Reach the sibling editor module's PRIVATE headers (FUnrealMcpNdjsonAccumulator §1.2,
		// FUnrealMcpGameThreadDispatcher §4) so the Automation specs can drive them directly. The
		// types are UNREALMCPEDITOR_API-exported, so symbols link via the module dependency above.
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "UnrealMcpEditor", "Private"));
	}
}
