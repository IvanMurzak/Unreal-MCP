// Copyright (c) 2026 IvanMurzak/Unreal-AI-GAS. Licensed under the Apache License, Version 2.0.
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
//  (a) registers under its kebab-case id and (b) returns a well-formed result. Read-only tools
//  are exercised for a SUCCESS; the mutating / required-input tools are exercised for a
//  well-formed, DEFENSIVE failure (bad input must yield FUnrealMcpToolResult::Error, never a
//  crash) — the deterministic assertion under a headless `-nullrhi` editor with no project assets.
//
//  The spec discovers THIS extension's live provider through IModularFeatures (the exact path
//  Unreal-MCP uses), registers its tools into a throwaway registry, and exercises them — so it
//  validates the real shipped provider, not a stand-in.
//
//  Run via:  Automation RunTests UnrealAIGAS
// ============================================================================================

namespace
{
	// Spec-unique helper names (the module is unity-built — keep file-local helpers uniquely named).
	IUnrealMcpToolProvider* UnrealAIGAS_FindOwnProvider()
	{
		const TArray<IUnrealMcpToolProvider*> Providers =
			IModularFeatures::Get().GetModularFeatureImplementations<IUnrealMcpToolProvider>(
				IUnrealMcpToolProvider::GetModularFeatureName());
		for (IUnrealMcpToolProvider* Provider : Providers)
		{
			if (Provider && Provider->GetExtensionId() == TEXT("com.ivanmurzak.unreal-ai-gas"))
			{
				return Provider;
			}
		}
		return nullptr;
	}

	// Register the live provider's tools into a throwaway registry (the exact RegisterExtension path
	// Unreal-MCP uses) so a test exercises the real shipped tool bodies.
	bool UnrealAIGAS_BuildRegistry(FAutomationTestBase& Test, FUnrealMcpToolRegistry& OutRegistry)
	{
		IUnrealMcpToolProvider* Provider = UnrealAIGAS_FindOwnProvider();
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

BEGIN_DEFINE_SPEC(FUnrealAIGASSpec, "UnrealAIGAS",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealAIGASSpec)

void FUnrealAIGASSpec::Define()
{
	Describe("provider registration", [this]()
	{
		It("registers this extension as a modular-feature tool provider", [this]()
		{
			IUnrealMcpToolProvider* Provider = UnrealAIGAS_FindOwnProvider();
			TestNotNull(TEXT("extension provider is registered as a modular feature"), Provider);
			if (Provider)
			{
				TestEqual(TEXT("extension id matches the descriptor"),
					Provider->GetExtensionId(), FString(TEXT("com.ivanmurzak.unreal-ai-gas")));
			}
		});
	});

	Describe("tool: gas-list-abilities", [this]()
	{
		It("registers and returns a well-formed { count, abilities } success", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			if (!UnrealAIGAS_BuildRegistry(*this, Registry)) { return; }

			TestTrue(TEXT("gas-list-abilities is registered"), Registry.HasTool(TEXT("gas-list-abilities")));

			const FUnrealMcpToolResult Result =
				Registry.Execute(TEXT("gas-list-abilities"), FUnrealMcpToolCall());

			TestTrue(TEXT("tool reports success"), Result.bSuccess);
			TestFalse(TEXT("tool returned a non-empty message"), Result.Message.IsEmpty());
			TestTrue(TEXT("structured result carries 'count' and 'abilities'"),
				Result.Structured.IsValid()
				&& Result.Structured->HasField(TEXT("count"))
				&& Result.Structured->HasField(TEXT("abilities")));
		});
	});

	Describe("tool: gas-list-effects", [this]()
	{
		It("registers and returns a well-formed { count, effects } success", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			if (!UnrealAIGAS_BuildRegistry(*this, Registry)) { return; }

			TestTrue(TEXT("gas-list-effects is registered"), Registry.HasTool(TEXT("gas-list-effects")));

			const FUnrealMcpToolResult Result =
				Registry.Execute(TEXT("gas-list-effects"), FUnrealMcpToolCall());

			TestTrue(TEXT("tool reports success"), Result.bSuccess);
			TestFalse(TEXT("tool returned a non-empty message"), Result.Message.IsEmpty());
			TestTrue(TEXT("structured result carries 'count' and 'effects'"),
				Result.Structured.IsValid()
				&& Result.Structured->HasField(TEXT("count"))
				&& Result.Structured->HasField(TEXT("effects")));
		});
	});

	Describe("tool: gas-list-attribute-sets", [this]()
	{
		It("registers and returns a well-formed { count, attributeSets } success", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			if (!UnrealAIGAS_BuildRegistry(*this, Registry)) { return; }

			TestTrue(TEXT("gas-list-attribute-sets is registered"), Registry.HasTool(TEXT("gas-list-attribute-sets")));

			const FUnrealMcpToolResult Result =
				Registry.Execute(TEXT("gas-list-attribute-sets"), FUnrealMcpToolCall());

			TestTrue(TEXT("tool reports success"), Result.bSuccess);
			TestFalse(TEXT("tool returned a non-empty message"), Result.Message.IsEmpty());
			TestTrue(TEXT("structured result carries 'count' and 'attributeSets'"),
				Result.Structured.IsValid()
				&& Result.Structured->HasField(TEXT("count"))
				&& Result.Structured->HasField(TEXT("attributeSets")));
		});
	});

	Describe("tool: gas-inspect-ability", [this]()
	{
		It("registers and fails defensively on a missing 'path' (well-formed Error)", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			if (!UnrealAIGAS_BuildRegistry(*this, Registry)) { return; }

			TestTrue(TEXT("gas-inspect-ability is registered"), Registry.HasTool(TEXT("gas-inspect-ability")));

			// No 'path' -> the handler must return a well-formed Error, not crash or succeed.
			const FUnrealMcpToolResult Result =
				Registry.Execute(TEXT("gas-inspect-ability"), FUnrealMcpToolCall());

			TestFalse(TEXT("missing 'path' is reported as a failure"), Result.bSuccess);
			TestFalse(TEXT("failure carries a non-empty message"), Result.Message.IsEmpty());
		});
	});

	Describe("tool: gas-grant-ability", [this]()
	{
		It("registers and fails defensively on a missing 'actor'/'abilityClass' (well-formed Error)", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			if (!UnrealAIGAS_BuildRegistry(*this, Registry)) { return; }

			TestTrue(TEXT("gas-grant-ability is registered"), Registry.HasTool(TEXT("gas-grant-ability")));

			// No 'actor'/'abilityClass' -> the handler must return a well-formed Error before touching the world.
			const FUnrealMcpToolResult Result =
				Registry.Execute(TEXT("gas-grant-ability"), FUnrealMcpToolCall());

			TestFalse(TEXT("missing required inputs are reported as a failure"), Result.bSuccess);
			TestFalse(TEXT("failure carries a non-empty message"), Result.Message.IsEmpty());
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
