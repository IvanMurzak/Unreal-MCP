// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "HAL/FileManager.h"

#include "IUnrealMcpResourceProvider.h"
#include "UnrealMcpToolRegistry.h"
#include "UnrealMcpResourceRegistry.h"
#include "Extensions/UnrealMcpExtensionManager.h"

/**
 * The §A.2 kind-aware RESOURCE pass of the extension manager: a resource provider's resources merge into the
 * resource registry through the SAME manager that drives tools/prompts, gated by the SAME DisabledExtensions
 * set, and a single OnChanged covers the rebuild. Mirrors the tool/prompt pass contract.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpResourceExtensionPassSpec, "UnrealMcp.Resources.ExtensionPass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealMcpResourceExtensionPassSpec)

namespace
{
	/** A test-only resource provider whose RegisterResources delegates to a supplied lambda (spec-unique name). */
	class FResourcePassMockProvider : public IUnrealMcpResourceProvider
	{
	public:
		FResourcePassMockProvider(const FString& InId, TFunction<void(FUnrealMcpResourceRegistry&)> InRegisterFn)
			: Id(InId), Display(FText::FromString(InId)), Version(TEXT("1.0.0")), RegisterFn(MoveTemp(InRegisterFn)) {}

		virtual FString GetExtensionId() const override { return Id; }
		virtual FText GetDisplayName() const override { return Display; }
		virtual FString GetExtensionVersion() const override { return Version; }
		virtual void RegisterResources(FUnrealMcpResourceRegistry& Registry) override { if (RegisterFn) RegisterFn(Registry); }

		FString Id;
		FText Display;
		FString Version;
		TFunction<void(FUnrealMcpResourceRegistry&)> RegisterFn;
	};

	void ResourcePassAddSimple(FUnrealMcpResourceRegistry& Registry, const FString& Uri)
	{
		Registry.Resource(Uri)
			.Name(TEXT("pass resource"))
			.MimeType(TEXT("text/plain"))
			.Read([](const FString& U) { return FUnrealMcpResourceResult::Text(U, TEXT("ok"), TEXT("text/plain")); });
	}
}

void FUnrealMcpResourceExtensionPassSpec::Define()
{
	It("merges an enabled resource provider into the resource registry and fires OnChanged", [this]()
	{
		FUnrealMcpToolRegistry ToolRegistry;
		FUnrealMcpResourceRegistry ResourceRegistry;
		int32 NotifyCount = 0;

		// Empty config path is fine — no disabled set on disk for this in-memory spec.
		FUnrealMcpExtensionManager Manager(
			ToolRegistry, [&NotifyCount]() { ++NotifyCount; }, FString(),
			/*PromptRegistry*/ nullptr, &ResourceRegistry);

		FResourcePassMockProvider Provider(TEXT("com.spec.res"),
			[](FUnrealMcpResourceRegistry& Reg) { ResourcePassAddSimple(Reg, TEXT("unreal://pass/one")); });
		Manager.SetResourceProviderSourceForTesting([&Provider]() { return TArray<IUnrealMcpResourceProvider*>{ &Provider }; });

		// RebuildFromProviders runs the tool pass (empty here) AND the resource pass under one guard/notify.
		Manager.RebuildFromProviders(TArray<IUnrealMcpToolProvider*>(), /*bNotify*/ true);

		TestTrue(TEXT("resource merged into registry"), ResourceRegistry.HasResource(TEXT("unreal://pass/one")));
		TestEqual(TEXT("OnChanged fired once"), NotifyCount, 1);
		if (const FUnrealMcpRegisteredResource* R = ResourceRegistry.Find(TEXT("unreal://pass/one")))
			TestEqual(TEXT("stamped extension id"), R->ExtensionId, FString(TEXT("com.spec.res")));
	});

	It("a re-run cleanly clears the previous resource contribution (no duplicate / stale entries)", [this]()
	{
		FUnrealMcpToolRegistry ToolRegistry;
		FUnrealMcpResourceRegistry ResourceRegistry;
		FUnrealMcpExtensionManager Manager(ToolRegistry, []() {}, FString(), nullptr, &ResourceRegistry);

		FResourcePassMockProvider Provider(TEXT("com.spec.res"),
			[](FUnrealMcpResourceRegistry& Reg) { ResourcePassAddSimple(Reg, TEXT("unreal://pass/one")); });
		Manager.SetResourceProviderSourceForTesting([&Provider]() { return TArray<IUnrealMcpResourceProvider*>{ &Provider }; });

		Manager.RebuildFromProviders(TArray<IUnrealMcpToolProvider*>(), false);
		Manager.RebuildFromProviders(TArray<IUnrealMcpToolProvider*>(), false);

		TestTrue(TEXT("resource still present after re-run"), ResourceRegistry.HasResource(TEXT("unreal://pass/one")));
		TestEqual(TEXT("exactly one resource (no duplicate)"), ResourceRegistry.Num(), 1);
	});

	It("a disabled extension contributes no resources (gated by the shared DisabledExtensions set)", [this]()
	{
		FUnrealMcpToolRegistry ToolRegistry;
		FUnrealMcpResourceRegistry ResourceRegistry;

		// Use a temp config path so SetExtensionEnabled can persist without touching the project Saved dir.
		const FString TempConfig = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Config"), TEXT("UnrealMCP"),
			FString::Printf(TEXT("ResPassSpec-%s.json"), *FGuid::NewGuid().ToString()));
		FUnrealMcpExtensionManager Manager(ToolRegistry, []() {}, TempConfig, nullptr, &ResourceRegistry);

		FResourcePassMockProvider Provider(TEXT("com.spec.res"),
			[](FUnrealMcpResourceRegistry& Reg) { ResourcePassAddSimple(Reg, TEXT("unreal://pass/one")); });
		Manager.SetResourceProviderSourceForTesting([&Provider]() { return TArray<IUnrealMcpResourceProvider*>{ &Provider }; });

		Manager.RebuildFromProviders(TArray<IUnrealMcpToolProvider*>(), false);
		TestTrue(TEXT("present while enabled"), ResourceRegistry.HasResource(TEXT("unreal://pass/one")));

		Manager.SetExtensionEnabled(TEXT("com.spec.res"), false); // disable -> rebuild
		TestFalse(TEXT("absent once the extension is disabled"), ResourceRegistry.HasResource(TEXT("unreal://pass/one")));

		IFileManager::Get().Delete(*TempConfig, /*RequireExists*/ false, /*EvenReadOnly*/ true);
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
