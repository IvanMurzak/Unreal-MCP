// Copyright (c) 2026 IvanMurzak/Unreal-AI-Niagara. Licensed under the Apache License, Version 2.0.
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
//  Run via:  Automation RunTests UnrealAINiagara
// ============================================================================================

namespace
{
	// Spec-unique helper names (the module is unity-built — keep file-local helpers uniquely named).
	IUnrealMcpToolProvider* UnrealAINiagara_FindOwnProvider()
	{
		const TArray<IUnrealMcpToolProvider*> Providers =
			IModularFeatures::Get().GetModularFeatureImplementations<IUnrealMcpToolProvider>(
				IUnrealMcpToolProvider::GetModularFeatureName());
		for (IUnrealMcpToolProvider* Provider : Providers)
		{
			if (Provider && Provider->GetExtensionId() == TEXT("com.ivanmurzak.unreal-ai-niagara"))
			{
				return Provider;
			}
		}
		return nullptr;
	}

	// Register the live provider's tools into a throwaway registry (the exact RegisterExtension path
	// Unreal-MCP uses) so a test exercises the real shipped tool bodies.
	bool UnrealAINiagara_BuildRegistry(FAutomationTestBase& Test, FUnrealMcpToolRegistry& OutRegistry)
	{
		IUnrealMcpToolProvider* Provider = UnrealAINiagara_FindOwnProvider();
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

BEGIN_DEFINE_SPEC(FUnrealAINiagaraSpec, "UnrealAINiagara",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealAINiagaraSpec)

void FUnrealAINiagaraSpec::Define()
{
	Describe("provider registration", [this]()
	{
		It("registers this extension as a modular-feature tool provider", [this]()
		{
			IUnrealMcpToolProvider* Provider = UnrealAINiagara_FindOwnProvider();
			TestNotNull(TEXT("extension provider is registered as a modular feature"), Provider);
			if (Provider)
			{
				TestEqual(TEXT("extension id matches the descriptor"),
					Provider->GetExtensionId(), FString(TEXT("com.ivanmurzak.unreal-ai-niagara")));
			}
		});
	});

	Describe("tool: niagara-list-systems", [this]()
	{
		It("registers and returns a well-formed { count, systems } success", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			if (!UnrealAINiagara_BuildRegistry(*this, Registry)) { return; }

			TestTrue(TEXT("niagara-list-systems is registered"), Registry.HasTool(TEXT("niagara-list-systems")));

			const FUnrealMcpToolResult Result =
				Registry.Execute(TEXT("niagara-list-systems"), FUnrealMcpToolCall());

			TestTrue(TEXT("tool reports success"), Result.bSuccess);
			TestFalse(TEXT("tool returned a non-empty message"), Result.Message.IsEmpty());
			TestTrue(TEXT("structured result carries 'count' and 'systems'"),
				Result.Structured.IsValid()
				&& Result.Structured->HasField(TEXT("count"))
				&& Result.Structured->HasField(TEXT("systems")));
		});
	});

	Describe("tool: niagara-get-system", [this]()
	{
		It("registers and fails defensively on a missing 'path' (well-formed Error)", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			if (!UnrealAINiagara_BuildRegistry(*this, Registry)) { return; }

			TestTrue(TEXT("niagara-get-system is registered"), Registry.HasTool(TEXT("niagara-get-system")));

			// No 'path' -> the handler must return a well-formed Error, not crash or succeed.
			const FUnrealMcpToolResult Result =
				Registry.Execute(TEXT("niagara-get-system"), FUnrealMcpToolCall());

			TestFalse(TEXT("missing 'path' is reported as a failure"), Result.bSuccess);
			TestFalse(TEXT("failure carries a non-empty message"), Result.Message.IsEmpty());
		});
	});

	Describe("tool: niagara-spawn-component", [this]()
	{
		It("registers and fails defensively on a missing 'systemPath' (well-formed Error)", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			if (!UnrealAINiagara_BuildRegistry(*this, Registry)) { return; }

			TestTrue(TEXT("niagara-spawn-component is registered"), Registry.HasTool(TEXT("niagara-spawn-component")));

			// No 'systemPath' -> the handler must return a well-formed Error before touching the world.
			const FUnrealMcpToolResult Result =
				Registry.Execute(TEXT("niagara-spawn-component"), FUnrealMcpToolCall());

			TestFalse(TEXT("missing 'systemPath' is reported as a failure"), Result.bSuccess);
			TestFalse(TEXT("failure carries a non-empty message"), Result.Message.IsEmpty());
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
