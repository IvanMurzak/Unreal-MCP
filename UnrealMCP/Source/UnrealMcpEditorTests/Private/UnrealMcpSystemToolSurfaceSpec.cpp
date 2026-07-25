// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "UnrealMcpCoreTools.h"
#include "UnrealMcpRuntimeCoreTools.h"
#include "UnrealMcpToolRegistry.h"
#include "Tools/UnrealMcpSkillTools.h"

/**
 * The §2.4 STANDARD-vs-SYSTEM tool surface.
 *
 * The load-bearing group is "ping is a SYSTEM tool": the owner ruling (2026-07-25) is that `ping` must be a
 * system tool on all three engines, matching Unity's `[AiTool(..., ToolType = McpToolType.System)]`. These
 * assertions fail the moment `ping` regresses to the standard surface — which is what would silently break the
 * CLI probe and the desktop app's connectivity check again.
 *
 * The complementary bridge-side guard (that a system-flagged descriptor is registered on the system manager and
 * NOT the MCP tool manager) lives in bridge/tests/SystemToolSurfaceTests.cs.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpSystemToolSurfaceSpec, "UnrealMcp.Tools.SystemSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	/** Descriptor of @p Name from a freshly-built manifest, or null when the tool is absent. */
	TSharedPtr<FJsonObject> SurfaceFindDescriptor(const FUnrealMcpToolRegistry& Registry, const FString& Name) const;
END_DEFINE_SPEC(FUnrealMcpSystemToolSurfaceSpec)

TSharedPtr<FJsonObject> FUnrealMcpSystemToolSurfaceSpec::SurfaceFindDescriptor(
	const FUnrealMcpToolRegistry& Registry, const FString& Name) const
{
	const TSharedPtr<FJsonObject> Manifest = Registry.BuildManifestJson();
	const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
	if (!Manifest.IsValid() || !Manifest->TryGetArrayField(TEXT("tools"), Tools) || Tools == nullptr)
		return nullptr;

	for (const TSharedPtr<FJsonValue>& Value : *Tools)
	{
		const TSharedPtr<FJsonObject> Object = Value.IsValid() ? Value->AsObject() : nullptr;
		if (Object.IsValid() && Object->GetStringField(TEXT("name")) == Name)
			return Object;
	}
	return nullptr;
}

void FUnrealMcpSystemToolSurfaceSpec::Define()
{
	Describe("tool type defaults", [this]()
	{
		It("defaults a declared tool to the standard surface", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			Registry.Tool(TEXT("surface-default-probe"))
				.Title(TEXT("Probe"))
				.Description(TEXT("A plain tool."))
				.Handle([](const FUnrealMcpToolCall&) { return FUnrealMcpToolResult::Success(TEXT("ok")); });

			const FUnrealMcpRegisteredTool* Tool = Registry.Find(TEXT("surface-default-probe"));
			TestTrue(TEXT("registered"), Tool != nullptr);
			if (Tool != nullptr)
				TestTrue(TEXT("standard by default"), Tool->ToolType == EUnrealMcpToolType::Standard);
		});

		It("round-trips the wire token both ways", [this]()
		{
			TestEqual(TEXT("standard token"),
				FString(UnrealMcpToolTypeToString(EUnrealMcpToolType::Standard)), FString(TEXT("standard")));
			TestEqual(TEXT("system token"),
				FString(UnrealMcpToolTypeToString(EUnrealMcpToolType::System)), FString(TEXT("system")));
			TestTrue(TEXT("parses system"),
				UnrealMcpToolTypeFromString(TEXT("system")) == EUnrealMcpToolType::System);
			TestTrue(TEXT("parses SYSTEM case-insensitively"),
				UnrealMcpToolTypeFromString(TEXT("SYSTEM")) == EUnrealMcpToolType::System);
			// Lenient: an unknown/absent token must never hide a tool from every AI agent.
			TestTrue(TEXT("unknown token -> standard"),
				UnrealMcpToolTypeFromString(TEXT("sytsem")) == EUnrealMcpToolType::Standard);
			TestTrue(TEXT("empty token -> standard"),
				UnrealMcpToolTypeFromString(FString()) == EUnrealMcpToolType::Standard);
		});

		It("ships the toolType field on every manifest descriptor", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			Registry.Tool(TEXT("surface-manifest-probe"))
				.Title(TEXT("Probe"))
				.Description(TEXT("A plain tool."))
				.Handle([](const FUnrealMcpToolCall&) { return FUnrealMcpToolResult::Success(TEXT("ok")); });

			const TSharedPtr<FJsonObject> Descriptor = SurfaceFindDescriptor(Registry, TEXT("surface-manifest-probe"));
			TestTrue(TEXT("descriptor present"), Descriptor.IsValid());
			if (Descriptor.IsValid())
			{
				TestEqual(TEXT("standard toolType emitted"),
					Descriptor->GetStringField(TEXT("toolType")), FString(TEXT("standard")));
			}
		});

		It("changes the schema hash when the surface changes", [this]()
		{
			// The bridge diffs manifests by schemaHash, so a surface move MUST perturb the hash — otherwise the
			// sidecar would keep serving the tool from the manager it first landed on.
			FUnrealMcpToolRegistry Standard, System;
			Standard.Tool(TEXT("surface-hash-probe"))
				.Description(TEXT("A plain tool."))
				.Handle([](const FUnrealMcpToolCall&) { return FUnrealMcpToolResult::Success(TEXT("ok")); });
			System.Tool(TEXT("surface-hash-probe"))
				.Description(TEXT("A plain tool."))
				.ToolType(EUnrealMcpToolType::System)
				.Handle([](const FUnrealMcpToolCall&) { return FUnrealMcpToolResult::Success(TEXT("ok")); });

			const FUnrealMcpRegisteredTool* A = Standard.Find(TEXT("surface-hash-probe"));
			const FUnrealMcpRegisteredTool* B = System.Find(TEXT("surface-hash-probe"));
			TestTrue(TEXT("both registered"), A != nullptr && B != nullptr);
			if (A != nullptr && B != nullptr)
				TestNotEqual(TEXT("hash differs with the surface"), A->SchemaHash, B->SchemaHash);
		});
	});

	Describe("ping is a SYSTEM tool", [this]()
	{
		It("declares ping on the system surface", [this]()
		{
			// REGRESSION GUARD (owner ruling 2026-07-25). If this fails, `ping` has drifted back onto the
			// standard surface and the CLI/desktop connectivity probes will break again.
			FUnrealMcpToolRegistry Registry;
			UnrealMcpPingTool::Register(Registry);

			const FUnrealMcpRegisteredTool* Ping = Registry.Find(TEXT("ping"));
			TestTrue(TEXT("ping registered"), Ping != nullptr);
			if (Ping != nullptr)
				TestTrue(TEXT("ping is System"), Ping->ToolType == EUnrealMcpToolType::System);
		});

		It("ships toolType=system to the sidecar in the manifest", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpPingTool::Register(Registry);

			const TSharedPtr<FJsonObject> Descriptor = SurfaceFindDescriptor(Registry, TEXT("ping"));
			TestTrue(TEXT("ping in manifest"), Descriptor.IsValid());
			if (Descriptor.IsValid())
			{
				TestEqual(TEXT("system toolType"),
					Descriptor->GetStringField(TEXT("toolType")), FString(TEXT("system")));
			}
		});

		It("still executes normally on the system surface", [this]()
		{
			// The surface controls WHERE the tool is reachable, never WHETHER it works.
			FUnrealMcpToolRegistry Registry;
			UnrealMcpPingTool::Register(Registry);

			const FUnrealMcpToolResult Result = Registry.Execute(TEXT("ping"), FUnrealMcpToolCall());
			TestTrue(TEXT("success"), Result.bSuccess);
			TestEqual(TEXT("pong"), Result.Message, FString(TEXT("pong")));
		});
	});

	Describe("the editor tool families stay on the standard surface", [this]()
	{
		It("leaves every actor-family tool standard", [this]()
		{
			// Guards the opposite regression: the new flag must not leak onto the authoring families, which an
			// AI agent absolutely must keep seeing in tools/list.
			FUnrealMcpToolRegistry Registry;
			UnrealMcpActorTools::Register(Registry);

			int32 SystemCount = 0;
			for (const FString& Name : Registry.GetToolNamesSorted())
			{
				const FUnrealMcpRegisteredTool* Tool = Registry.Find(Name);
				if (Tool != nullptr && Tool->ToolType == EUnrealMcpToolType::System)
					++SystemCount;
			}
			TestTrue(TEXT("family registered something"), Registry.Num() > 0);
			TestEqual(TEXT("no system tools in the actor family"), SystemCount, 0);
		});

		It("declares unreal-skill-create on the system surface", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpSkillTools::Register(Registry);

			const FUnrealMcpRegisteredTool* Create = Registry.Find(FString(UnrealMcpSkillTools::SkillCreateToolId));
			TestTrue(TEXT("unreal-skill-create registered"), Create != nullptr);
			if (Create != nullptr)
				TestTrue(TEXT("is System"), Create->ToolType == EUnrealMcpToolType::System);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
