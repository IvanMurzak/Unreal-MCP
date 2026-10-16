// Copyright (c) 2026 IvanMurzak/Unreal-AI-Landscape. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Features/IModularFeatures.h"
#include "Dom/JsonObject.h"

#include "IUnrealMcpToolProvider.h"
#include "UnrealMcpToolRegistry.h"

// ============================================================================================
//  UE Automation spec — ONE-TEST-PER-TOOL convention.
//
//  Every tool this extension contributes gets a focused Automation spec asserting it
//  (a) registers under its kebab-case id and (b) returns a well-formed result. The read-only
//  LIST tools are exercised for a SUCCESS (deterministic under a headless `-nullrhi` editor with
//  an empty world — the active editor world is available, so they return count 0). The required-
//  input GET tools are exercised for a DEFENSIVE failure (a missing 'name' must yield
//  FUnrealMcpToolResult::Error, never a crash) — the deterministic assertion that needs no seeded
//  actor or world state.
//
//  The spec discovers THIS extension's live provider through IModularFeatures (the exact path
//  Unreal-MCP uses), registers its tools into a throwaway registry, and exercises them — so it
//  validates the real shipped provider, not a stand-in.
//
//  Run via:  Automation RunTests UnrealAILandscape
// ============================================================================================

namespace
{
	// Module-unique helper names (the module is unity-built — keep file-local helpers uniquely named).
	IUnrealMcpToolProvider* UnrealAILandscape_FindOwnProvider()
	{
		const TArray<IUnrealMcpToolProvider*> Providers =
			IModularFeatures::Get().GetModularFeatureImplementations<IUnrealMcpToolProvider>(
				IUnrealMcpToolProvider::GetModularFeatureName());
		for (IUnrealMcpToolProvider* Provider : Providers)
		{
			if (Provider && Provider->GetExtensionId() == TEXT("com.ivanmurzak.unreal-ai-landscape"))
			{
				return Provider;
			}
		}
		return nullptr;
	}

	// Register the live provider's tools into a throwaway registry (the exact RegisterExtension path
	// Unreal-MCP uses) so a test exercises the real shipped tool bodies.
	bool UnrealAILandscape_BuildRegistry(FAutomationTestBase& Test, FUnrealMcpToolRegistry& OutRegistry)
	{
		IUnrealMcpToolProvider* Provider = UnrealAILandscape_FindOwnProvider();
		if (!Provider)
		{
			Test.AddError(TEXT("extension provider not registered — cannot exercise its tools"));
			return false;
		}
		OutRegistry.RegisterExtension(Provider->GetExtensionId(),
			[Provider](FUnrealMcpToolRegistry& R) { Provider->RegisterTools(R); });
		return true;
	}
}

BEGIN_DEFINE_SPEC(FUnrealAILandscapeSpec, "UnrealAILandscape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealAILandscapeSpec)

void FUnrealAILandscapeSpec::Define()
{
	Describe("provider registration", [this]()
	{
		It("registers this extension as a modular-feature tool provider", [this]()
		{
			IUnrealMcpToolProvider* Provider = UnrealAILandscape_FindOwnProvider();
			TestNotNull(TEXT("extension provider is registered as a modular feature"), Provider);
			if (Provider)
			{
				TestEqual(TEXT("extension id matches the descriptor"),
					Provider->GetExtensionId(), FString(TEXT("com.ivanmurzak.unreal-ai-landscape")));
			}
		});
	});

	Describe("tool: landscape-list-actors", [this]()
	{
		It("registers and returns a well-formed { count, actors } success", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			if (!UnrealAILandscape_BuildRegistry(*this, Registry)) { return; }

			TestTrue(TEXT("landscape-list-actors is registered"), Registry.HasTool(TEXT("landscape-list-actors")));

			const FUnrealMcpToolResult Result =
				Registry.Execute(TEXT("landscape-list-actors"), FUnrealMcpToolCall());

			TestTrue(TEXT("tool reports success"), Result.bSuccess);
			TestFalse(TEXT("tool returned a non-empty message"), Result.Message.IsEmpty());
			TestTrue(TEXT("structured result carries 'count' and 'actors'"),
				Result.Structured.IsValid()
				&& Result.Structured->HasField(TEXT("count"))
				&& Result.Structured->HasField(TEXT("actors")));
		});
	});

	Describe("tool: landscape-get-actor", [this]()
	{
		It("registers and fails defensively on a missing 'name' (well-formed Error)", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			if (!UnrealAILandscape_BuildRegistry(*this, Registry)) { return; }

			TestTrue(TEXT("landscape-get-actor is registered"), Registry.HasTool(TEXT("landscape-get-actor")));

			// No 'name' -> the handler must return a well-formed Error, not crash or succeed.
			const FUnrealMcpToolResult Result =
				Registry.Execute(TEXT("landscape-get-actor"), FUnrealMcpToolCall());

			TestFalse(TEXT("missing 'name' is reported as a failure"), Result.bSuccess);
			TestFalse(TEXT("failure carries a non-empty message"), Result.Message.IsEmpty());
		});
	});

	Describe("tool: landscape-list-water-bodies", [this]()
	{
		It("registers and returns a well-formed { count, waterBodies } success", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			if (!UnrealAILandscape_BuildRegistry(*this, Registry)) { return; }

			TestTrue(TEXT("landscape-list-water-bodies is registered"), Registry.HasTool(TEXT("landscape-list-water-bodies")));

			const FUnrealMcpToolResult Result =
				Registry.Execute(TEXT("landscape-list-water-bodies"), FUnrealMcpToolCall());

			TestTrue(TEXT("tool reports success"), Result.bSuccess);
			TestFalse(TEXT("tool returned a non-empty message"), Result.Message.IsEmpty());
			TestTrue(TEXT("structured result carries 'count' and 'waterBodies'"),
				Result.Structured.IsValid()
				&& Result.Structured->HasField(TEXT("count"))
				&& Result.Structured->HasField(TEXT("waterBodies")));
		});
	});

	Describe("tool: landscape-get-water-body", [this]()
	{
		It("registers and fails defensively on a missing 'name' (well-formed Error)", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			if (!UnrealAILandscape_BuildRegistry(*this, Registry)) { return; }

			TestTrue(TEXT("landscape-get-water-body is registered"), Registry.HasTool(TEXT("landscape-get-water-body")));

			// No 'name' -> the handler must return a well-formed Error before touching the world.
			const FUnrealMcpToolResult Result =
				Registry.Execute(TEXT("landscape-get-water-body"), FUnrealMcpToolCall());

			TestFalse(TEXT("missing 'name' is reported as a failure"), Result.bSuccess);
			TestFalse(TEXT("failure carries a non-empty message"), Result.Message.IsEmpty());
		});
	});

	Describe("tool: landscape-list-water-zones", [this]()
	{
		It("registers and returns a well-formed { count, waterZones } success", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			if (!UnrealAILandscape_BuildRegistry(*this, Registry)) { return; }

			TestTrue(TEXT("landscape-list-water-zones is registered"), Registry.HasTool(TEXT("landscape-list-water-zones")));

			const FUnrealMcpToolResult Result =
				Registry.Execute(TEXT("landscape-list-water-zones"), FUnrealMcpToolCall());

			TestTrue(TEXT("tool reports success"), Result.bSuccess);
			TestFalse(TEXT("tool returned a non-empty message"), Result.Message.IsEmpty());
			TestTrue(TEXT("structured result carries 'count' and 'waterZones'"),
				Result.Structured.IsValid()
				&& Result.Structured->HasField(TEXT("count"))
				&& Result.Structured->HasField(TEXT("waterZones")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
