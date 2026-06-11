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

	// Register one extension-scoped tool under @p ExtensionId, mirroring the §5 RegisterExtension path the
	// ExtensionManager drives on a hot-reload. Used to prove the blocklist survives a re-registration.
	static void RegisterExtensionTool(FUnrealMcpToolRegistry& Registry, const FString& ExtensionId, const FString& ToolName)
	{
		Registry.RegisterExtension(ExtensionId, [&ToolName](FUnrealMcpToolRegistry& Reg)
		{
			Reg.Tool(ToolName)
				.Title(ToolName)
				.Description(TEXT("an extension tool"))
				.Handle([](const FUnrealMcpToolCall&) { return FUnrealMcpToolResult::Success(TEXT("ok")); });
		});
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

	Describe("Extension hot-reload retention (§5)", [this]()
	{
		It("keeps a blocklisted extension tool excluded across an extension rebuild", [this]()
		{
			// The §5 regression: ExtensionManager::RebuildFromProviders removes then re-registers every
			// extension tool FRESH (bEnabled defaults true) and re-pushes the manifest. Before the registry
			// retained the blocklist, a disabled extension tool sprang back enabled and leaked over the wire.
			FUnrealMcpToolRegistry Registry;
			SeedTools(Registry); // core: alpha/beta/gamma
			RegisterExtensionTool(Registry, TEXT("com.example.ext"), TEXT("ext-tool"));
			TestTrue("ext-tool served before disable", ManifestToolNames(Registry).Contains(TEXT("ext-tool")));

			// User disables the extension tool through the §7 blocklist.
			Registry.ApplyDisabledTools({ TEXT("ext-tool") });
			TestFalse("ext-tool excluded after disable", ManifestToolNames(Registry).Contains(TEXT("ext-tool")));

			// Simulate the hot-reload: remove the extension's tools, then re-register them fresh.
			const int32 Removed = Registry.RemoveToolsForExtension(TEXT("com.example.ext"));
			TestEqual("one extension tool removed on unload", Removed, 1);
			RegisterExtensionTool(Registry, TEXT("com.example.ext"), TEXT("ext-tool"));

			// Regression assertion: the rebuilt extension tool MUST still be excluded.
			TArray<FString> AfterRebuild = ManifestToolNames(Registry);
			TestFalse("ext-tool STILL excluded after the extension rebuild", AfterRebuild.Contains(TEXT("ext-tool")));
			TestTrue("core alpha unaffected by the rebuild", AfterRebuild.Contains(TEXT("alpha")));
		});
	});

	Describe("§8 enabledTools whitelist gating", [this]()
	{
		// served iff (whitelist empty OR member) AND NOT blocklisted — the full 3×2 matrix
		// (whitelist empty / member / non-member) × (blocklisted / not).
		It("gates the served manifest by the combined whitelist+blocklist rule", [this]()
		{
			// Row group 1 — EMPTY whitelist (no filter): blocklist alone decides.
			{
				FUnrealMcpToolRegistry Registry;
				SeedTools(Registry);
				Registry.SetEnabledToolsFilter({});             // empty → no whitelist filter
				Registry.ApplyDisabledTools({ TEXT("beta") });  // beta blocklisted
				TArray<FString> Served = ManifestToolNames(Registry);
				TestTrue("empty whitelist + not blocklisted → served (alpha)", Served.Contains(TEXT("alpha")));
				TestTrue("empty whitelist + not blocklisted → served (gamma)", Served.Contains(TEXT("gamma")));
				TestFalse("empty whitelist + blocklisted → excluded (beta)", Served.Contains(TEXT("beta")));
			}

			// Row group 2 — whitelist {alpha,beta}, blocklist {beta}: covers member/not-blocklisted,
			// member/blocklisted, and non-member/not-blocklisted in one shot.
			{
				FUnrealMcpToolRegistry Registry;
				SeedTools(Registry);
				Registry.SetEnabledToolsFilter({ TEXT("alpha"), TEXT("beta") });
				Registry.ApplyDisabledTools({ TEXT("beta") });
				TArray<FString> Served = ManifestToolNames(Registry);
				TestTrue("member + not blocklisted → served (alpha)", Served.Contains(TEXT("alpha")));
				TestFalse("member + blocklisted → excluded (beta)", Served.Contains(TEXT("beta")));
				TestFalse("non-member + not blocklisted → excluded (gamma)", Served.Contains(TEXT("gamma")));
				TestEqual("only alpha survives", Served.Num(), 1);
			}

			// Row group 3 — whitelist {alpha}, blocklist {gamma}: covers non-member/blocklisted.
			{
				FUnrealMcpToolRegistry Registry;
				SeedTools(Registry);
				Registry.SetEnabledToolsFilter({ TEXT("alpha") });
				Registry.ApplyDisabledTools({ TEXT("gamma") });
				TArray<FString> Served = ManifestToolNames(Registry);
				TestTrue("member + not blocklisted → served (alpha)", Served.Contains(TEXT("alpha")));
				TestFalse("non-member + blocklisted → excluded (gamma)", Served.Contains(TEXT("gamma")));
				TestFalse("non-member + not blocklisted → excluded (beta)", Served.Contains(TEXT("beta")));
			}
		});

		It("re-applying the blocklist does not clobber the retained whitelist", [this]()
		{
			// The §8 conflict the [medium] finding called out: ApplyDisabledTools used to set
			// bEnabled = !blocklisted unconditionally. With both filters retained, a blocklist re-apply
			// (even clearing it) must still honor the whitelist gate.
			FUnrealMcpToolRegistry Registry;
			SeedTools(Registry);
			Registry.SetEnabledToolsFilter({ TEXT("alpha") }); // only alpha passes the whitelist
			TestEqual("whitelist alone leaves only alpha", ManifestToolNames(Registry).Num(), 1);

			Registry.ApplyDisabledTools({}); // clear the blocklist entirely
			TArray<FString> Served = ManifestToolNames(Registry);
			TestEqual("whitelist still enforced after a blocklist clear", Served.Num(), 1);
			TestTrue("alpha remains the only served tool", Served.Contains(TEXT("alpha")));
		});

		It("registers extension tools honoring the retained whitelist on a rebuild", [this]()
		{
			// Closes the loop with §5: a non-whitelisted extension tool re-added by a hot-reload stays hidden.
			FUnrealMcpToolRegistry Registry;
			SeedTools(Registry);
			Registry.SetEnabledToolsFilter({ TEXT("alpha") }); // ext-tool is NOT whitelisted
			RegisterExtensionTool(Registry, TEXT("com.example.ext"), TEXT("ext-tool"));
			TestFalse("non-whitelisted ext-tool excluded on first register", ManifestToolNames(Registry).Contains(TEXT("ext-tool")));

			Registry.RemoveToolsForExtension(TEXT("com.example.ext"));
			RegisterExtensionTool(Registry, TEXT("com.example.ext"), TEXT("ext-tool"));
			TestFalse("non-whitelisted ext-tool STILL excluded after rebuild", ManifestToolNames(Registry).Contains(TEXT("ext-tool")));
		});

		It("reports the whitelist gate independently of the blocklist (the §7 window's data source)", [this]()
		{
			// SUnrealMcpToolsWindow snapshots PassesEnabledToolsWhitelist() to render whitelist-gated rows; it is the
			// STATIC env dimension, so it must NOT react to the §7 blocklist (which ApplyDisabledTools drives).
			FUnrealMcpToolRegistry Registry;
			SeedTools(Registry);
			TestTrue("empty whitelist → every tool passes", Registry.PassesEnabledToolsWhitelist(TEXT("beta")));

			Registry.SetEnabledToolsFilter({ TEXT("alpha") });
			TestTrue("whitelisted name passes", Registry.PassesEnabledToolsWhitelist(TEXT("alpha")));
			TestFalse("non-whitelisted name fails", Registry.PassesEnabledToolsWhitelist(TEXT("beta")));

			// Blocklisting alpha must not change its whitelist verdict — the two dimensions are orthogonal.
			Registry.ApplyDisabledTools({ TEXT("alpha") });
			TestTrue("blocklist does not flip the whitelist verdict", Registry.PassesEnabledToolsWhitelist(TEXT("alpha")));
		});
	});

	Describe("Execute enablement gate (§7/§8 execution boundary)", [this]()
	{
		It("refuses to dispatch a blocklisted tool and serves an enabled one", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			SeedTools(Registry);
			Registry.ApplyDisabledTools({ TEXT("beta") }); // beta blocklisted (§7)

			const FUnrealMcpToolResult Disabled = Registry.Execute(TEXT("beta"), FUnrealMcpToolCall{});
			TestFalse("blocklisted tool does not execute", Disabled.bSuccess);
			TestTrue("error names the disabled tool", Disabled.Message.Contains(TEXT("disabled")));

			const FUnrealMcpToolResult Enabled = Registry.Execute(TEXT("alpha"), FUnrealMcpToolCall{});
			TestTrue("enabled tool still executes", Enabled.bSuccess);

			const FUnrealMcpToolResult Unknown = Registry.Execute(TEXT("does-not-exist"), FUnrealMcpToolCall{});
			TestFalse("unknown tool still errors", Unknown.bSuccess);
			TestTrue("unknown-tool error is distinct from the disabled error", Unknown.Message.Contains(TEXT("Unknown")));
		});

		It("refuses to dispatch a non-whitelisted tool (§8 stale-manifest race guard)", [this]()
		{
			// The gate is authoritative even when the manifest exclusion has not yet propagated to the sidecar: a
			// tool the §8 whitelist excludes must never run, regardless of any stale tools/list the caller holds.
			FUnrealMcpToolRegistry Registry;
			SeedTools(Registry);
			Registry.SetEnabledToolsFilter({ TEXT("alpha") }); // only alpha whitelisted; beta/gamma gated off

			const FUnrealMcpToolResult Gated = Registry.Execute(TEXT("gamma"), FUnrealMcpToolCall{});
			TestFalse("non-whitelisted tool does not execute", Gated.bSuccess);
			TestTrue("error names the disabled tool", Gated.Message.Contains(TEXT("disabled")));

			const FUnrealMcpToolResult Served = Registry.Execute(TEXT("alpha"), FUnrealMcpToolCall{});
			TestTrue("whitelisted tool executes", Served.bSuccess);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
