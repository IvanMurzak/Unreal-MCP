// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Features/IModularFeatures.h"
#include "Engine/GameInstance.h" // a transient UGameInstance as the subsystem's Outer (ClassWithin=UGameInstance)

#include "IUnrealMcpToolProvider.h"
#include "UnrealMcpToolRegistry.h"
#include "Extensions/UnrealMcpExtensionManager.h"
#include "UnrealMcpRuntimeSubsystem.h"

/**
 * Specs for the R5 runtime extension bus (docs/ARCHITECTURE.md §12.9) — the PRIMARY Unity-parity runtime
 * use case: a game registers a CUSTOM tool provider at runtime and Unreal-MCP rebuilds the registry +
 * re-pushes the manifest so a connected sidecar's tools/list gains/loses the tool.
 *
 * The R5 deliverable adds the ergonomic UUnrealMcpRuntimeSubsystem::RegisterToolProvider /
 * UnregisterToolProvider wrappers (so a game need not touch IModularFeatures directly). These wrappers are
 * thin pass-throughs to IModularFeatures::Register/UnregisterModularFeature; they do NOT touch the
 * subsystem's PImpl, so a bare (un-Initialized) NewObject subsystem is enough to exercise them — exactly
 * as UnrealMcpRuntimeSubsystemSpec exercises the pre-Initialize security gates. The §5 extension manager
 * (wired to its own registry here) is the observer that proves the register/unregister actually rebuilt
 * the registry and fired the manifest re-push callback.
 *
 * The live sidecar connect → server → tools/list wire path is the live-e2e / PIE runbook (test.md Suite 7);
 * here we assert the registry+manifest contract headlessly, which is what makes "a runtime-registered
 * provider's tool appears/disappears in the manifest" a deterministic CI gate.
 */
namespace
{
	/** A test-only runtime tool provider whose RegisterTools registers one gameplay-style tool. */
	class FRuntimeExtSpecProvider : public IUnrealMcpToolProvider
	{
	public:
		explicit FRuntimeExtSpecProvider(const FString& InId, const FString& InToolName)
			: Id(InId), ToolName(InToolName), Display(FText::FromString(InId)) {}

		virtual FString GetExtensionId() const override { return Id; }
		virtual FText GetDisplayName() const override { return Display; }
		virtual FString GetExtensionVersion() const override { return TEXT("1.0.0"); }
		virtual void RegisterTools(FUnrealMcpToolRegistry& Registry) override
		{
			const FString Name = ToolName;
			Registry.Tool(Name)
				.Title(TEXT("Runtime sample tool"))
				.Description(TEXT("A runtime-registered gameplay tool (R5 §12.9 spec fixture)."))
				.ParamNumber(TEXT("value"), TEXT("an optional value"))
				.Handle([](const FUnrealMcpToolCall&) { return FUnrealMcpToolResult::Success(TEXT("ok")); });
		}

		FString Id;
		FString ToolName;
		FText Display;
	};

	/** Whether the manifest's tools[] array contains a tool with the given name (mirrors the §5 spec helper). */
	bool RuntimeExtManifestHasTool(const FUnrealMcpToolRegistry& Registry, const FString& Name)
	{
		const TSharedPtr<FJsonObject> Manifest = Registry.BuildManifestJson();
		const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
		if (!Manifest->TryGetArrayField(TEXT("tools"), Tools))
			return false;
		for (const TSharedPtr<FJsonValue>& Value : *Tools)
		{
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			if (Obj.IsValid() && Obj->GetStringField(TEXT("name")) == Name)
				return true;
		}
		return false;
	}

	FString RuntimeExtMakeTempConfigPath()
	{
		return FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("UnrealMcpTests"),
			FString::Printf(TEXT("runtime-ext-%s.json"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	/** A bare (un-Initialize'd) runtime subsystem — enough for the IModularFeatures pass-through wrappers,
	 *  which do not touch the subsystem's PImpl. ClassWithin=UGameInstance requires a UGameInstance Outer. */
	UUnrealMcpRuntimeSubsystem* RuntimeExtMakeSubsystem()
	{
		UGameInstance* OuterGI = NewObject<UGameInstance>(GetTransientPackage());
		return NewObject<UUnrealMcpRuntimeSubsystem>(OuterGI);
	}
}

BEGIN_DEFINE_SPEC(FUnrealMcpRuntimeExtensionProviderSpec, "UnrealMcp.RuntimeExtensionProvider",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealMcpRuntimeExtensionProviderSpec)

void FUnrealMcpRuntimeExtensionProviderSpec::Define()
{
	Describe("RegisterToolProvider / UnregisterToolProvider (§12.9 ergonomic API)", [this]()
	{
		It("re-pushes the manifest when a provider is registered then unregistered via the subsystem helper", [this]()
		{
			// A throwaway config path so Startup() does not read the developer's real Extensions.json.
			const FString ConfigPath = RuntimeExtMakeTempConfigPath();

			FUnrealMcpToolRegistry Registry;
			int32 OnChangedCount = 0;
			FUnrealMcpExtensionManager Manager(Registry, [&OnChangedCount]() { ++OnChangedCount; }, ConfigPath);

			// Default provider source = live IModularFeatures discovery; subscribe to register/unregister events.
			Manager.Startup();

			UUnrealMcpRuntimeSubsystem* Subsystem = RuntimeExtMakeSubsystem();

			// A uniquely-named provider/tool so the assertions are robust against any ambient providers.
			FRuntimeExtSpecProvider Provider(TEXT("test.runtime.ext"), TEXT("runtime-ext-probe-tool"));

			TestFalse(TEXT("probe tool absent before register"), Registry.HasTool(TEXT("runtime-ext-probe-tool")));
			const int32 RevBefore = Registry.GetRevision();
			const int32 OnBefore = OnChangedCount;

			// --- The R5 ergonomic register path (NOT a direct IModularFeatures call). ---
			Subsystem->RegisterToolProvider(&Provider);

			TestTrue(TEXT("probe tool present after RegisterToolProvider"), Registry.HasTool(TEXT("runtime-ext-probe-tool")));
			TestTrue(TEXT("probe tool in the manifest after register"), RuntimeExtManifestHasTool(Registry, TEXT("runtime-ext-probe-tool")));
			TestTrue(TEXT("revision bumped on register"), Registry.GetRevision() > RevBefore);
			TestTrue(TEXT("manifest re-push (OnChanged) fired on register"), OnChangedCount > OnBefore);

			const int32 RevAfterReg = Registry.GetRevision();
			const int32 OnAfterReg = OnChangedCount;

			// --- The R5 ergonomic unregister path. ---
			Subsystem->UnregisterToolProvider(&Provider);

			TestFalse(TEXT("probe tool absent after UnregisterToolProvider"), Registry.HasTool(TEXT("runtime-ext-probe-tool")));
			TestFalse(TEXT("probe tool gone from the manifest after unregister"), RuntimeExtManifestHasTool(Registry, TEXT("runtime-ext-probe-tool")));
			TestTrue(TEXT("revision bumped on unregister"), Registry.GetRevision() > RevAfterReg);
			TestTrue(TEXT("manifest re-push (OnChanged) fired on unregister"), OnChangedCount > OnAfterReg);

			Manager.Shutdown();
			IFileManager::Get().Delete(*ConfigPath, /*RequireExists*/ false, /*EvenReadOnly*/ true);
		});

		It("treats a null provider as a harmless no-op (no rebuild, no manifest churn)", [this]()
		{
			const FString ConfigPath = RuntimeExtMakeTempConfigPath();

			FUnrealMcpToolRegistry Registry;
			int32 OnChangedCount = 0;
			FUnrealMcpExtensionManager Manager(Registry, [&OnChangedCount]() { ++OnChangedCount; }, ConfigPath);
			Manager.Startup();

			UUnrealMcpRuntimeSubsystem* Subsystem = RuntimeExtMakeSubsystem();

			const int32 RevBefore = Registry.GetRevision();
			const int32 OnBefore = OnChangedCount;

			// RegisterToolProvider logs a Warning on null; whitelist it so the framework does not fail the spec
			// and so this test also asserts the loud signal actually fired.
			AddExpectedError(TEXT("null provider ignored"), EAutomationExpectedErrorFlags::Contains);
			Subsystem->RegisterToolProvider(nullptr);
			Subsystem->UnregisterToolProvider(nullptr); // silent no-op

			TestEqual(TEXT("revision unchanged on null register/unregister"), Registry.GetRevision(), RevBefore);
			TestEqual(TEXT("no manifest re-push on null register/unregister"), OnChangedCount, OnBefore);

			Manager.Shutdown();
			IFileManager::Get().Delete(*ConfigPath, /*RequireExists*/ false, /*EvenReadOnly*/ true);
		});
	});

	Describe("re-push parity: subsystem helper vs direct IModularFeatures", [this]()
	{
		It("a provider registered via the helper is observed identically to a direct modular-feature registration", [this]()
		{
			const FString ConfigPath = RuntimeExtMakeTempConfigPath();

			FUnrealMcpToolRegistry Registry;
			int32 OnChangedCount = 0;
			FUnrealMcpExtensionManager Manager(Registry, [&OnChangedCount]() { ++OnChangedCount; }, ConfigPath);
			Manager.Startup();

			UUnrealMcpRuntimeSubsystem* Subsystem = RuntimeExtMakeSubsystem();

			FRuntimeExtSpecProvider ViaHelper(TEXT("test.runtime.helper"), TEXT("runtime-helper-tool"));
			FRuntimeExtSpecProvider ViaDirect(TEXT("test.runtime.direct"), TEXT("runtime-direct-tool"));

			// Register one through the ergonomic helper, the other straight through IModularFeatures.
			Subsystem->RegisterToolProvider(&ViaHelper);
			IModularFeatures::Get().RegisterModularFeature(IUnrealMcpToolProvider::GetModularFeatureName(), &ViaDirect);

			TestTrue(TEXT("helper-registered tool present"), Registry.HasTool(TEXT("runtime-helper-tool")));
			TestTrue(TEXT("direct-registered tool present"), Registry.HasTool(TEXT("runtime-direct-tool")));
			TestTrue(TEXT("both records discovered"),
				Manager.FindExtension(TEXT("test.runtime.helper")) != nullptr
				&& Manager.FindExtension(TEXT("test.runtime.direct")) != nullptr);

			// Clean up both registrations regardless of which path created them.
			Subsystem->UnregisterToolProvider(&ViaHelper);
			IModularFeatures::Get().UnregisterModularFeature(IUnrealMcpToolProvider::GetModularFeatureName(), &ViaDirect);

			TestFalse(TEXT("helper tool removed"), Registry.HasTool(TEXT("runtime-helper-tool")));
			TestFalse(TEXT("direct tool removed"), Registry.HasTool(TEXT("runtime-direct-tool")));

			Manager.Shutdown();
			IFileManager::Get().Delete(*ConfigPath, /*RequireExists*/ false, /*EvenReadOnly*/ true);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
