// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Features/IModularFeatures.h"

#include "IUnrealMcpToolProvider.h"
#include "UnrealMcpToolRegistry.h"
#include "Extensions/UnrealMcpExtensionManager.h"

/**
 * Extensions mechanism specs (docs/ARCHITECTURE.md §5): discovery + deterministic ordering, duplicate
 * tool-name rejection, invalid-descriptor isolation, late-registration rebuild + revision bump through
 * IModularFeatures, and disabled-extension exclusion with a persistence round-trip.
 */

namespace
{
	/** A test-only tool provider whose RegisterTools delegates to a supplied lambda. */
	class FMockToolProvider : public IUnrealMcpToolProvider
	{
	public:
		FMockToolProvider(const FString& InId, TFunction<void(FUnrealMcpToolRegistry&)> InRegisterFn)
			: Id(InId), Display(FText::FromString(InId)), Version(TEXT("1.0.0")), RegisterFn(MoveTemp(InRegisterFn)) {}

		virtual FString GetExtensionId() const override { return Id; }
		virtual FText GetDisplayName() const override { return Display; }
		virtual FString GetExtensionVersion() const override { return Version; }
		virtual void RegisterTools(FUnrealMcpToolRegistry& Registry) override { if (RegisterFn) RegisterFn(Registry); }

		FString Id;
		FText Display;
		FString Version;
		TFunction<void(FUnrealMcpToolRegistry&)> RegisterFn;
	};

	/** Register a minimal valid tool. */
	void AddSimpleTool(FUnrealMcpToolRegistry& Registry, const FString& Name)
	{
		Registry.Tool(Name)
			.Title(Name)
			.Description(TEXT("test tool"))
			.ParamString(TEXT("arg"), TEXT("an argument"))
			.Handle([](const FUnrealMcpToolCall&) { return FUnrealMcpToolResult::Success(TEXT("ok")); });
	}

	/** Whether the manifest's tools[] array contains a tool with the given name. */
	bool ManifestHasTool(const FUnrealMcpToolRegistry& Registry, const FString& Name)
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

	FString MakeTempConfigPath()
	{
		return FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("UnrealMcpTests"),
			FString::Printf(TEXT("ext-%s.json"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}
}

BEGIN_DEFINE_SPEC(FUnrealMcpExtensionsSpec, "UnrealMcp.Extensions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealMcpExtensionsSpec)

void FUnrealMcpExtensionsSpec::Define()
{
	Describe("discovery and ordering", [this]()
	{
		It("merges tools from multiple providers, recording them in ExtensionId order", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			FUnrealMcpExtensionManager Manager(Registry, nullptr);

			FMockToolProvider A(TEXT("a.ext"), [](FUnrealMcpToolRegistry& R) { AddSimpleTool(R, TEXT("tool-a")); });
			FMockToolProvider B(TEXT("b.ext"), [](FUnrealMcpToolRegistry& R) { AddSimpleTool(R, TEXT("tool-b")); });

			// Pass out of order to prove the manager sorts deterministically.
			Manager.RebuildFromProviders({ &B, &A }, /*bNotify*/ false);

			TestTrue(TEXT("tool-a registered"), Registry.HasTool(TEXT("tool-a")));
			TestTrue(TEXT("tool-b registered"), Registry.HasTool(TEXT("tool-b")));
			TestEqual(TEXT("two extension records"), Manager.GetExtensions().Num(), 2);
			TestEqual(TEXT("record[0] is a.ext"), Manager.GetExtensions()[0].Id, FString(TEXT("a.ext")));
			TestEqual(TEXT("record[1] is b.ext"), Manager.GetExtensions()[1].Id, FString(TEXT("b.ext")));
			TestEqual(TEXT("a.ext tool count"), Manager.GetExtensions()[0].ToolCount, 1);

			// Extension tools carry their owner's ExtensionId on the descriptor.
			TestEqual(TEXT("tool-a owned by a.ext"), Registry.Find(TEXT("tool-a"))->ExtensionId, FString(TEXT("a.ext")));
		});

		It("removes an extension's tools when it disappears from a later rebuild", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			FUnrealMcpExtensionManager Manager(Registry, nullptr);

			FMockToolProvider A(TEXT("a.ext"), [](FUnrealMcpToolRegistry& R) { AddSimpleTool(R, TEXT("tool-a")); });
			FMockToolProvider B(TEXT("b.ext"), [](FUnrealMcpToolRegistry& R) { AddSimpleTool(R, TEXT("tool-b")); });

			Manager.RebuildFromProviders({ &A, &B }, false);
			TestTrue(TEXT("both present"), Registry.HasTool(TEXT("tool-a")) && Registry.HasTool(TEXT("tool-b")));

			// B unloaded -> rebuild with only A.
			Manager.RebuildFromProviders({ &A }, false);
			TestTrue(TEXT("tool-a still present"), Registry.HasTool(TEXT("tool-a")));
			TestFalse(TEXT("tool-b removed"), Registry.HasTool(TEXT("tool-b")));
			TestEqual(TEXT("one extension record"), Manager.GetExtensions().Num(), 1);
		});
	});

	Describe("duplicate tool-name rejection", [this]()
	{
		It("keeps the first-sorted extension's tool and records an error on the later one", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			FUnrealMcpExtensionManager Manager(Registry, nullptr);

			FMockToolProvider A(TEXT("a.ext"), [](FUnrealMcpToolRegistry& R) { AddSimpleTool(R, TEXT("dup")); });
			FMockToolProvider B(TEXT("b.ext"), [](FUnrealMcpToolRegistry& R) { AddSimpleTool(R, TEXT("dup")); });

			Manager.RebuildFromProviders({ &B, &A }, false);

			TestTrue(TEXT("dup present"), Registry.HasTool(TEXT("dup")));
			TestEqual(TEXT("dup owned by first-sorted a.ext"), Registry.Find(TEXT("dup"))->ExtensionId, FString(TEXT("a.ext")));

			const FUnrealMcpExtensionRecord* RecA = Manager.FindExtension(TEXT("a.ext"));
			const FUnrealMcpExtensionRecord* RecB = Manager.FindExtension(TEXT("b.ext"));
			TestTrue(TEXT("a.ext record exists"), RecA != nullptr);
			TestTrue(TEXT("b.ext record exists"), RecB != nullptr);
			TestEqual(TEXT("a.ext registered its tool"), RecA->ToolCount, 1);
			TestFalse(TEXT("a.ext healthy"), RecA->HasError());
			TestEqual(TEXT("b.ext registered nothing"), RecB->ToolCount, 0);
			TestTrue(TEXT("b.ext has a rejection error"), RecB->HasError());
		});
	});

	Describe("invalid-descriptor isolation", [this]()
	{
		It("drops only the invalid entry and leaves other tools and extensions intact", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			FUnrealMcpExtensionManager Manager(Registry, nullptr);

			// A: one valid tool + one invalid (empty-named parameter => malformed schema).
			FMockToolProvider A(TEXT("a.ext"), [](FUnrealMcpToolRegistry& R)
			{
				AddSimpleTool(R, TEXT("good-tool"));
				R.Tool(TEXT("bad-tool"))
					.Title(TEXT("Bad"))
					.Description(TEXT("invalid"))
					.ParamString(TEXT(""), TEXT("empty-named param"))
					.Handle([](const FUnrealMcpToolCall&) { return FUnrealMcpToolResult::Success(TEXT("x")); });
			});
			// B: a healthy neighbour that must be unaffected.
			FMockToolProvider B(TEXT("b.ext"), [](FUnrealMcpToolRegistry& R) { AddSimpleTool(R, TEXT("other-tool")); });

			Manager.RebuildFromProviders({ &A, &B }, false);

			TestTrue(TEXT("good-tool registered"), Registry.HasTool(TEXT("good-tool")));
			TestFalse(TEXT("bad-tool dropped"), Registry.HasTool(TEXT("bad-tool")));
			TestTrue(TEXT("other-tool registered"), Registry.HasTool(TEXT("other-tool")));

			const FUnrealMcpExtensionRecord* RecA = Manager.FindExtension(TEXT("a.ext"));
			const FUnrealMcpExtensionRecord* RecB = Manager.FindExtension(TEXT("b.ext"));
			TestEqual(TEXT("a.ext kept 1 tool"), RecA->ToolCount, 1);
			TestTrue(TEXT("a.ext records the drop"), RecA->HasError());
			TestEqual(TEXT("b.ext kept 1 tool"), RecB->ToolCount, 1);
			TestFalse(TEXT("b.ext healthy"), RecB->HasError());
		});

		It("rejects an invalid tool name", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			FUnrealMcpExtensionManager Manager(Registry, nullptr);

			FMockToolProvider A(TEXT("a.ext"), [](FUnrealMcpToolRegistry& R)
			{
				// Uppercase + underscore is not kebab-case.
				R.Tool(TEXT("Bad_Name"))
					.Description(TEXT("x"))
					.Handle([](const FUnrealMcpToolCall&) { return FUnrealMcpToolResult::Success(TEXT("x")); });
			});

			Manager.RebuildFromProviders({ &A }, false);
			TestFalse(TEXT("invalid-named tool dropped"), Registry.HasTool(TEXT("Bad_Name")));
			TestTrue(TEXT("a.ext records the drop"), Manager.FindExtension(TEXT("a.ext"))->HasError());
		});
	});

	Describe("late registration via IModularFeatures", [this]()
	{
		It("rebuilds the registry and bumps the revision on register and unregister", [this]()
		{
			// A throwaway config path so Startup() does not read the developer's real
			// <ProjectSaved>/Config/UnrealMCP/Extensions.json (whose persisted disabled set could
			// otherwise make this live-discovery spec fail non-locally).
			const FString ConfigPath = MakeTempConfigPath();

			FUnrealMcpToolRegistry Registry;
			int32 OnChangedCount = 0;
			FUnrealMcpExtensionManager Manager(Registry, [&OnChangedCount]() { ++OnChangedCount; }, ConfigPath);

			// Default provider source = live discovery; subscribe to modular-feature events.
			Manager.Startup();

			// A uniquely-named provider so the assertions are robust against any ambient providers.
			FMockToolProvider Late(TEXT("test.late.reg"),
				[](FUnrealMcpToolRegistry& R) { AddSimpleTool(R, TEXT("late-reg-probe-tool")); });

			TestFalse(TEXT("probe tool absent before register"), Registry.HasTool(TEXT("late-reg-probe-tool")));
			const int32 RevBefore = Registry.GetRevision();
			const int32 OnBefore = OnChangedCount;

			IModularFeatures::Get().RegisterModularFeature(IUnrealMcpToolProvider::GetModularFeatureName(), &Late);

			TestTrue(TEXT("probe tool present after register"), Registry.HasTool(TEXT("late-reg-probe-tool")));
			TestTrue(TEXT("revision bumped on register"), Registry.GetRevision() > RevBefore);
			TestTrue(TEXT("OnChanged fired on register"), OnChangedCount > OnBefore);

			const int32 RevAfterReg = Registry.GetRevision();
			const int32 OnAfterReg = OnChangedCount;

			IModularFeatures::Get().UnregisterModularFeature(IUnrealMcpToolProvider::GetModularFeatureName(), &Late);

			TestFalse(TEXT("probe tool absent after unregister"), Registry.HasTool(TEXT("late-reg-probe-tool")));
			TestTrue(TEXT("revision bumped on unregister"), Registry.GetRevision() > RevAfterReg);
			TestTrue(TEXT("OnChanged fired on unregister"), OnChangedCount > OnAfterReg);

			Manager.Shutdown();

			IFileManager::Get().Delete(*ConfigPath, /*RequireExists*/ false, /*EvenReadOnly*/ true);
		});
	});

	Describe("disabled-extension exclusion and persistence", [this]()
	{
		It("excludes a disabled extension's tools and round-trips the disabled set to disk", [this]()
		{
			const FString ConfigPath = MakeTempConfigPath();

			FMockToolProvider P(TEXT("p.ext"), [](FUnrealMcpToolRegistry& R) { AddSimpleTool(R, TEXT("p-tool")); });
			auto Source = [&P]() { return TArray<IUnrealMcpToolProvider*>{ &P }; };

			// Manager 1: discover P (enabled), then disable it.
			{
				FUnrealMcpToolRegistry Registry;
				FUnrealMcpExtensionManager Manager(Registry, nullptr, ConfigPath);
				Manager.SetProviderSourceForTesting(Source);
				Manager.Startup();

				TestTrue(TEXT("p-tool present while enabled"), Registry.HasTool(TEXT("p-tool")));
				TestTrue(TEXT("manifest includes p-tool while enabled"), ManifestHasTool(Registry, TEXT("p-tool")));

				Manager.SetExtensionEnabled(TEXT("p.ext"), false);

				TestFalse(TEXT("p-tool absent when disabled"), Registry.HasTool(TEXT("p-tool")));
				TestFalse(TEXT("manifest excludes p-tool when disabled"), ManifestHasTool(Registry, TEXT("p-tool")));
				const FUnrealMcpExtensionRecord* Rec = Manager.FindExtension(TEXT("p.ext"));
				TestTrue(TEXT("record exists when disabled"), Rec != nullptr);
				TestFalse(TEXT("record marked disabled"), Rec->bEnabled);
				TestEqual(TEXT("disabled tool count is 0"), Rec->ToolCount, 0);
			}

			// Manager 2: a fresh manager + registry reading the SAME config file starts P disabled.
			{
				FUnrealMcpToolRegistry Registry;
				FUnrealMcpExtensionManager Manager(Registry, nullptr, ConfigPath);
				Manager.SetProviderSourceForTesting(Source);
				Manager.Startup();

				TestFalse(TEXT("disabled state persisted across managers"), Registry.HasTool(TEXT("p-tool")));
				TestFalse(TEXT("p.ext starts disabled"), Manager.IsExtensionEnabled(TEXT("p.ext")));

				// Re-enabling restores the tool.
				Manager.SetExtensionEnabled(TEXT("p.ext"), true);
				TestTrue(TEXT("p-tool restored when re-enabled"), Registry.HasTool(TEXT("p-tool")));
			}

			IFileManager::Get().Delete(*ConfigPath, /*RequireExists*/ false, /*EvenReadOnly*/ true);
		});

		It("ignores a corrupt config file and starts every extension enabled", [this]()
		{
			const FString ConfigPath = MakeTempConfigPath();

			// Write malformed JSON to the config path before the manager loads it.
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(ConfigPath), /*Tree*/ true);
			FFileHelper::SaveStringToFile(TEXT("{ this is not valid json"), *ConfigPath);

			FMockToolProvider P(TEXT("p.ext"), [](FUnrealMcpToolRegistry& R) { AddSimpleTool(R, TEXT("p-tool")); });
			auto Source = [&P]() { return TArray<IUnrealMcpToolProvider*>{ &P }; };

			FUnrealMcpToolRegistry Registry;
			FUnrealMcpExtensionManager Manager(Registry, nullptr, ConfigPath);
			Manager.SetProviderSourceForTesting(Source);

			// LoadConfig logs an Error for the malformed file (a deliberately loud signal). Whitelist it
			// so the Automation framework does not treat the expected log as a failure — and so this test
			// also asserts that the loud error actually fired.
			AddExpectedError(TEXT("malformed and was ignored"), EAutomationExpectedErrorFlags::Contains);
			Manager.Startup(); // must not crash on the malformed file

			// A corrupt file is discarded, so the persisted disabled set is empty => P is enabled.
			TestTrue(TEXT("extension enabled despite corrupt config"), Manager.IsExtensionEnabled(TEXT("p.ext")));
			TestTrue(TEXT("p-tool present despite corrupt config"), Registry.HasTool(TEXT("p-tool")));

			IFileManager::Get().Delete(*ConfigPath, /*RequireExists*/ false, /*EvenReadOnly*/ true);
		});
	});

	Describe("extension id validation", [this]()
	{
		It("skips a provider whose id collides with the reserved 'core' scope and preserves core tools", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			FUnrealMcpExtensionManager Manager(Registry, nullptr);

			// A core tool registered on the default (non-extension) path — ExtensionId defaults to "core".
			AddSimpleTool(Registry, TEXT("core-tool"));
			TestTrue(TEXT("core-tool registered"), Registry.HasTool(TEXT("core-tool")));

			// A malicious/buggy provider claiming the reserved "core" id.
			FMockToolProvider Evil(TEXT("core"), [](FUnrealMcpToolRegistry& R) { AddSimpleTool(R, TEXT("evil-tool")); });

			Manager.RebuildFromProviders({ &Evil }, false);

			TestFalse(TEXT("evil-tool not registered"), Registry.HasTool(TEXT("evil-tool")));
			TestTrue(TEXT("core-tool survives the rebuild"), Registry.HasTool(TEXT("core-tool")));
			const FUnrealMcpExtensionRecord* Rec = Manager.FindExtension(TEXT("core"));
			TestTrue(TEXT("core-id record exists"), Rec != nullptr);
			TestTrue(TEXT("core-id record has an error"), Rec->HasError());
			TestEqual(TEXT("core-id record registered nothing"), Rec->ToolCount, 0);

			// Regression guard: a SECOND rebuild must not delete the core tools under the "core" key.
			Manager.RebuildFromProviders({ &Evil }, false);
			TestTrue(TEXT("core-tool still present after a second rebuild"), Registry.HasTool(TEXT("core-tool")));
		});

		It("skips a provider with an empty id and records the failure", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			FUnrealMcpExtensionManager Manager(Registry, nullptr);

			FMockToolProvider Empty(TEXT(""), [](FUnrealMcpToolRegistry& R) { AddSimpleTool(R, TEXT("orphan-tool")); });

			Manager.RebuildFromProviders({ &Empty }, false);

			TestFalse(TEXT("orphan-tool not registered"), Registry.HasTool(TEXT("orphan-tool")));
			const FUnrealMcpExtensionRecord* Rec = Manager.FindExtension(TEXT(""));
			TestTrue(TEXT("empty-id record exists"), Rec != nullptr);
			TestTrue(TEXT("empty-id record has an error"), Rec->HasError());
			TestEqual(TEXT("empty-id record registered nothing"), Rec->ToolCount, 0);
		});

		It("keeps the first provider when two share an ExtensionId and records an error on the later", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			FUnrealMcpExtensionManager Manager(Registry, nullptr);

			// Two providers with the SAME id; each declares a distinct tool.
			FMockToolProvider First(TEXT("dup.ext"), [](FUnrealMcpToolRegistry& R) { AddSimpleTool(R, TEXT("tool-1")); });
			FMockToolProvider Second(TEXT("dup.ext"), [](FUnrealMcpToolRegistry& R) { AddSimpleTool(R, TEXT("tool-2")); });

			// StableSort tie-breaks on input order, so First (passed first) wins deterministically.
			Manager.RebuildFromProviders({ &First, &Second }, false);

			TestTrue(TEXT("first provider's tool registered"), Registry.HasTool(TEXT("tool-1")));
			TestFalse(TEXT("second provider's tool skipped"), Registry.HasTool(TEXT("tool-2")));
			TestEqual(TEXT("both providers produced a record"), Manager.GetExtensions().Num(), 2);

			int32 HealthyCount = 0;
			int32 ErroredCount = 0;
			for (const FUnrealMcpExtensionRecord& Rec : Manager.GetExtensions())
			{
				if (Rec.HasError()) ++ErroredCount; else ++HealthyCount;
			}
			TestEqual(TEXT("exactly one healthy record"), HealthyCount, 1);
			TestEqual(TEXT("exactly one errored record"), ErroredCount, 1);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
