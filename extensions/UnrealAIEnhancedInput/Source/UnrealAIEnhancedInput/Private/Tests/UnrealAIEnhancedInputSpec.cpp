// Copyright (c) 2026 IvanMurzak/Unreal-AI-EnhancedInput. Licensed under the Apache License, Version 2.0.
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
//  well-formed, DEFENSIVE failure (a missing required input must yield FUnrealMcpToolResult::Error,
//  never a crash or a silent success) — the deterministic assertion under a headless `-nullrhi`
//  editor with no project assets (the missing-input guard returns BEFORE any asset load).
//
//  The spec discovers THIS extension's live provider through IModularFeatures (the exact path
//  Unreal-MCP uses), registers its tools into a throwaway registry, and exercises them — so it
//  validates the real shipped provider, not a stand-in.
//
//  Run via:  Automation RunTests UnrealAIEnhancedInput
// ============================================================================================

namespace
{
	// Spec-unique helper names (the module is unity-built — keep file-local helpers uniquely named).
	IUnrealMcpToolProvider* UnrealAIEnhancedInput_FindOwnProvider()
	{
		const TArray<IUnrealMcpToolProvider*> Providers =
			IModularFeatures::Get().GetModularFeatureImplementations<IUnrealMcpToolProvider>(
				IUnrealMcpToolProvider::GetModularFeatureName());
		for (IUnrealMcpToolProvider* Provider : Providers)
		{
			if (Provider && Provider->GetExtensionId() == TEXT("com.ivanmurzak.unreal-ai-enhanced-input"))
			{
				return Provider;
			}
		}
		return nullptr;
	}

	// Register the live provider's tools into a throwaway registry (the exact RegisterExtension path
	// Unreal-MCP uses) so a test exercises the real shipped tool bodies.
	bool UnrealAIEnhancedInput_BuildRegistry(FAutomationTestBase& Test, FUnrealMcpToolRegistry& OutRegistry)
	{
		IUnrealMcpToolProvider* Provider = UnrealAIEnhancedInput_FindOwnProvider();
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

BEGIN_DEFINE_SPEC(FUnrealAIEnhancedInputSpec, "UnrealAIEnhancedInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealAIEnhancedInputSpec)

void FUnrealAIEnhancedInputSpec::Define()
{
	Describe("provider registration", [this]()
	{
		It("registers this extension as a modular-feature tool provider", [this]()
		{
			IUnrealMcpToolProvider* Provider = UnrealAIEnhancedInput_FindOwnProvider();
			TestNotNull(TEXT("extension provider is registered as a modular feature"), Provider);
			if (Provider)
			{
				TestEqual(TEXT("extension id matches the descriptor"),
					Provider->GetExtensionId(), FString(TEXT("com.ivanmurzak.unreal-ai-enhanced-input")));
			}
		});
	});

	Describe("tool: enhanced-input-list", [this]()
	{
		It("registers and returns a well-formed { actionCount, contextCount, actions, contexts } success", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			if (!UnrealAIEnhancedInput_BuildRegistry(*this, Registry)) { return; }

			TestTrue(TEXT("enhanced-input-list is registered"), Registry.HasTool(TEXT("enhanced-input-list")));

			const FUnrealMcpToolResult Result =
				Registry.Execute(TEXT("enhanced-input-list"), FUnrealMcpToolCall());

			TestTrue(TEXT("tool reports success"), Result.bSuccess);
			TestFalse(TEXT("tool returned a non-empty message"), Result.Message.IsEmpty());
			TestTrue(TEXT("structured result carries 'actionCount', 'contextCount', 'actions' and 'contexts'"),
				Result.Structured.IsValid()
				&& Result.Structured->HasField(TEXT("actionCount"))
				&& Result.Structured->HasField(TEXT("contextCount"))
				&& Result.Structured->HasField(TEXT("actions"))
				&& Result.Structured->HasField(TEXT("contexts")));
		});
	});

	Describe("tool: enhanced-input-get", [this]()
	{
		It("registers and fails defensively on a missing 'path' (well-formed Error)", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			if (!UnrealAIEnhancedInput_BuildRegistry(*this, Registry)) { return; }

			TestTrue(TEXT("enhanced-input-get is registered"), Registry.HasTool(TEXT("enhanced-input-get")));

			// No 'path' -> the handler must return a well-formed Error, not crash or succeed.
			const FUnrealMcpToolResult Result =
				Registry.Execute(TEXT("enhanced-input-get"), FUnrealMcpToolCall());

			TestFalse(TEXT("missing 'path' is reported as a failure"), Result.bSuccess);
			TestFalse(TEXT("failure carries a non-empty message"), Result.Message.IsEmpty());
		});
	});

	Describe("tool: enhanced-input-action-create", [this]()
	{
		It("registers and fails defensively on a missing 'path' (well-formed Error)", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			if (!UnrealAIEnhancedInput_BuildRegistry(*this, Registry)) { return; }

			TestTrue(TEXT("enhanced-input-action-create is registered"), Registry.HasTool(TEXT("enhanced-input-action-create")));

			// No 'path' -> the handler must return a well-formed Error before creating anything.
			const FUnrealMcpToolResult Result =
				Registry.Execute(TEXT("enhanced-input-action-create"), FUnrealMcpToolCall());

			TestFalse(TEXT("missing 'path' is reported as a failure"), Result.bSuccess);
			TestFalse(TEXT("failure carries a non-empty message"), Result.Message.IsEmpty());
		});
	});

	Describe("tool: enhanced-input-context-create", [this]()
	{
		It("registers and fails defensively on a missing 'path' (well-formed Error)", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			if (!UnrealAIEnhancedInput_BuildRegistry(*this, Registry)) { return; }

			TestTrue(TEXT("enhanced-input-context-create is registered"), Registry.HasTool(TEXT("enhanced-input-context-create")));

			// No 'path' -> the handler must return a well-formed Error before creating anything.
			const FUnrealMcpToolResult Result =
				Registry.Execute(TEXT("enhanced-input-context-create"), FUnrealMcpToolCall());

			TestFalse(TEXT("missing 'path' is reported as a failure"), Result.bSuccess);
			TestFalse(TEXT("failure carries a non-empty message"), Result.Message.IsEmpty());
		});
	});

	Describe("tool: enhanced-input-add-mapping", [this]()
	{
		It("registers and fails defensively on a missing 'contextPath' (well-formed Error)", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			if (!UnrealAIEnhancedInput_BuildRegistry(*this, Registry)) { return; }

			TestTrue(TEXT("enhanced-input-add-mapping is registered"), Registry.HasTool(TEXT("enhanced-input-add-mapping")));

			// No required inputs -> the handler must return a well-formed Error before loading any asset.
			const FUnrealMcpToolResult Result =
				Registry.Execute(TEXT("enhanced-input-add-mapping"), FUnrealMcpToolCall());

			TestFalse(TEXT("missing 'contextPath' is reported as a failure"), Result.bSuccess);
			TestFalse(TEXT("failure carries a non-empty message"), Result.Message.IsEmpty());
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
