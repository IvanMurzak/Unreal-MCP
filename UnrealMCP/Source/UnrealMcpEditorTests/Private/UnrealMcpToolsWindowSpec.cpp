// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "UnrealMcpToolRegistry.h"
#include "UI/UnrealMcpEditorViewModel.h"
#include "Config/UnrealMcpConfig.h"

/**
 * MCP Tools window specs (docs/ARCHITECTURE.md §7, issue #26): the per-tool enable-map. Covers the three
 * acceptance criteria that ARE headless-provable — (1) the §8 config-store round-trip of the `disabledTools`
 * blocklist survives a save/reload (persistence across an editor restart), (2) a disabled tool is EXCLUDED
 * from the registry's served manifest and re-enabling restores it (the manifest-exclusion the sidecar mirrors
 * over the wire), and (3) the view-model toggle drives persistence + the manifest re-apply end to end, plus the
 * boot path that re-applies the persisted blocklist to a fresh registry. All without Slate, a live bridge, or a
 * real editor world. (The windowed open/dock + dedup/focus is operator-verified — not reachable headless.)
 */
BEGIN_DEFINE_SPEC(FUnrealMcpToolsWindowSpec, "UnrealMcp.ToolsWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	// Register a handful of no-op tools so manifest/exclusion assertions have a stable, named set.
	static void SeedTools(FUnrealMcpToolRegistry& Registry)
	{
		const TCHAR* Names[] = { TEXT("alpha"), TEXT("beta"), TEXT("gamma") };
		for (const TCHAR* Name : Names)
		{
			Registry.Tool(Name)
				.Title(FString(Name))
				.Description(FString::Printf(TEXT("the %s tool"), Name))
				.Handle([](const FUnrealMcpToolCall&) { return FUnrealMcpToolResult::Success(TEXT("ok")); });
		}
	}

	// The set of tool names present in the registry's served manifest (the §2.2 tools[] array).
	static TArray<FString> ManifestToolNames(const FUnrealMcpToolRegistry& Registry)
	{
		TArray<FString> Out;
		const TSharedPtr<FJsonObject> Manifest = Registry.BuildManifestJson();
		const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
		if (Manifest->TryGetArrayField(TEXT("tools"), Tools) && Tools)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Tools)
			{
				const TSharedPtr<FJsonObject>* Obj = nullptr;
				FString Name;
				if (Value.IsValid() && Value->TryGetObject(Obj) && Obj && (*Obj)->TryGetStringField(TEXT("name"), Name))
					Out.Add(Name);
			}
		}
		return Out;
	}

END_DEFINE_SPEC(FUnrealMcpToolsWindowSpec)

void FUnrealMcpToolsWindowSpec::Define()
{
	Describe("§8 disabledTools persistence", [this]()
	{
		It("round-trips the blocklist through ToJson/LoadFromJson", [this]()
		{
			FUnrealMcpConfig Config;
			Config.DisabledTools = { TEXT("beta"), TEXT("gamma") };

			FUnrealMcpConfig Reloaded;
			Reloaded.LoadFromJson(Config.ToJson());

			TestEqual("blocklist size preserved", Reloaded.DisabledTools.Num(), 2);
			TestTrue("beta preserved", Reloaded.DisabledTools.Contains(TEXT("beta")));
			TestTrue("gamma preserved", Reloaded.DisabledTools.Contains(TEXT("gamma")));
		});

		It("survives a real Save() / LoadFromFile() reload (editor-restart proof)", [this]()
		{
			const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcpTests"),
				FString::Printf(TEXT("tools-%s.json"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));

			FUnrealMcpConfig Config;
			Config.DisabledTools = { TEXT("alpha") };
			TestTrue("save succeeds", Config.Save(Path));

			FUnrealMcpConfig Reloaded;
			Reloaded.LoadFromFile(Path);
			TestTrue("alpha disabled after reload", Reloaded.DisabledTools.Contains(TEXT("alpha")));
			TestFalse("beta not disabled after reload", Reloaded.DisabledTools.Contains(TEXT("beta")));

			IFileManager::Get().Delete(*Path);
		});
	});

	Describe("Registry manifest exclusion", [this]()
	{
		It("excludes a disabled tool from the served manifest and restores it on re-enable", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			SeedTools(Registry);
			TestEqual("all three served initially", ManifestToolNames(Registry).Num(), 3);

			TestTrue("disable bumps revision", Registry.SetToolEnabled(TEXT("beta"), false));
			TArray<FString> AfterDisable = ManifestToolNames(Registry);
			TestEqual("two served after disable", AfterDisable.Num(), 2);
			TestFalse("beta excluded from manifest", AfterDisable.Contains(TEXT("beta")));
			TestTrue("alpha still served", AfterDisable.Contains(TEXT("alpha")));
			TestEqual("NumEnabled reflects exclusion", Registry.NumEnabled(), 2);

			// A no-op toggle does not churn the revision.
			TestFalse("re-disabling is a no-op", Registry.SetToolEnabled(TEXT("beta"), false));

			TestTrue("re-enable bumps revision", Registry.SetToolEnabled(TEXT("beta"), true));
			TArray<FString> AfterEnable = ManifestToolNames(Registry);
			TestEqual("all three served after re-enable", AfterEnable.Num(), 3);
			TestTrue("beta restored to manifest", AfterEnable.Contains(TEXT("beta")));
		});

		It("applies a whole disabled-name set at once (the boot path)", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			SeedTools(Registry);

			Registry.ApplyDisabledTools({ TEXT("alpha"), TEXT("gamma") });
			TArray<FString> Served = ManifestToolNames(Registry);
			TestEqual("only beta served", Served.Num(), 1);
			TestTrue("beta is the survivor", Served.Contains(TEXT("beta")));

			// Re-applying an empty set re-enables everything.
			Registry.ApplyDisabledTools({});
			TestEqual("all re-enabled", ManifestToolNames(Registry).Num(), 3);
		});
	});

	Describe("View-model toggle round-trip", [this]()
	{
		It("persists the choice and re-applies the manifest exclusion through the registry", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			SeedTools(Registry);

			int32 PersistCount = 0;
			FUnrealMcpEditorViewModel VM;
			VM.OnPersistConfig = [&PersistCount](const FUnrealMcpConfig&) { ++PersistCount; };
			// Wire the enablement sink exactly as the runtime does: re-apply the blocklist to the live registry.
			VM.OnToolEnablementChanged = [&Registry](const TArray<FString>& Disabled) { Registry.ApplyDisabledTools(Disabled); };

			// Disable "gamma" through the view-model (the §7 Tools-window checkbox path).
			VM.SetToolEnabled(TEXT("gamma"), false);
			TestEqual("persist fired once", PersistCount, 1);
			TestTrue("view-model reports gamma disabled", VM.IsToolDisabled(TEXT("gamma")));
			TestTrue("blocklist carries gamma", VM.GetDisabledTools().Contains(TEXT("gamma")));
			TestFalse("gamma excluded from registry manifest", ManifestToolNames(Registry).Contains(TEXT("gamma")));

			// A no-op toggle (already enabled) does nothing.
			VM.SetToolEnabled(TEXT("alpha"), true);
			TestEqual("no extra persist on no-op", PersistCount, 1);

			// Re-enable restores the manifest + clears the blocklist.
			VM.SetToolEnabled(TEXT("gamma"), true);
			TestEqual("persist fired again on re-enable", PersistCount, 2);
			TestFalse("view-model reports gamma enabled", VM.IsToolDisabled(TEXT("gamma")));
			TestTrue("gamma restored to registry manifest", ManifestToolNames(Registry).Contains(TEXT("gamma")));
		});

		It("re-applies a persisted blocklist to a fresh registry on boot", [this]()
		{
			// Persist a blocklist via one view-model/config…
			FUnrealMcpConfig Saved;
			Saved.DisabledTools = { TEXT("beta") };
			const TSharedPtr<FJsonObject> Json = Saved.ToJson();

			// …then simulate a fresh editor boot: reload the config, seed a fresh registry, apply the blocklist.
			FUnrealMcpConfig Booted;
			Booted.LoadFromJson(Json);
			FUnrealMcpToolRegistry Registry;
			SeedTools(Registry);
			Registry.ApplyDisabledTools(Booted.DisabledTools);

			TArray<FString> Served = ManifestToolNames(Registry);
			TestEqual("two served after boot-apply", Served.Num(), 2);
			TestFalse("beta stays disabled across the restart", Served.Contains(TEXT("beta")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
