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
#include "UnrealMcpToolRegistry.h"
#include "UI/UnrealMcpEditorViewModel.h"
#include "UI/SUnrealMcpToolsWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SCheckBox.h"
#include "Config/UnrealMcpConfig.h"

/**
 * MCP Tools window specs (docs/ARCHITECTURE.md §7, issue #26): the per-tool Visible toggles. Visible only decides
 * whether a tool is listed in tools/list; a hidden tool stays in the served manifest (flagged `enabled: false`, the
 * wire key) and still runs when called by name. Covers what IS headless-provable — (1) the §8 config-store round-trip
 * of the `hiddenTools` list survives a save/reload (and the pre-rename `disabledTools` key is still read, never
 * written), (2) a hidden tool is flagged hidden in the registry's manifest (what the sidecar mirrors into tools/list)
 * yet still executes, (3) the view-model toggle drives persistence + the manifest re-apply end to end, plus the boot
 * path that re-applies the persisted list to a fresh registry, and (4) the §8 `enabledTools` whitelist still decides
 * whether a tool is served at all, and (5) the Tools window widget labels the toggle Visible. All without a live
 * bridge or a real editor world (the widget is constructed, not opened in a tab). (The windowed open/dock
 * + dedup/focus is operator-verified — not reachable headless.)
 */
BEGIN_DEFINE_SPEC(FUnrealMcpToolsWindowSpec, "UnrealMcp.ToolsWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	// Register a handful of no-op tools so manifest assertions have a stable, named set.
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
	// ExtensionManager drives on a hot-reload. Used to prove the hidden list survives a re-registration.
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

	// The descriptor objects of the registry's served manifest (the §2.2 tools[] array).
	static TArray<TSharedPtr<FJsonObject>> ManifestDescriptors(const FUnrealMcpToolRegistry& Registry)
	{
		TArray<TSharedPtr<FJsonObject>> Out;
		const TSharedPtr<FJsonObject> Manifest = Registry.BuildManifestJson();
		const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
		if (Manifest->TryGetArrayField(TEXT("tools"), Tools) && Tools)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Tools)
			{
				const TSharedPtr<FJsonObject>* Obj = nullptr;
				if (Value.IsValid() && Value->TryGetObject(Obj) && Obj)
					Out.Add(*Obj);
			}
		}
		return Out;
	}

	// Tool names in the served manifest. With @p bVisibleOnly, only the entries flagged visible (wire key
	// `enabled: true`) — what the sidecar lists in tools/list.
	static TArray<FString> ManifestToolNames(const FUnrealMcpToolRegistry& Registry, bool bVisibleOnly = false)
	{
		TArray<FString> Out;
		for (const TSharedPtr<FJsonObject>& Desc : ManifestDescriptors(Registry))
		{
			bool bListed = true;
			Desc->TryGetBoolField(TEXT("enabled"), bListed);
			if (!bVisibleOnly || bListed)
				Out.Add(Desc->GetStringField(TEXT("name")));
		}
		return Out;
	}

	static TArray<FString> VisibleToolNames(const FUnrealMcpToolRegistry& Registry)
	{
		return ManifestToolNames(Registry, /*bVisibleOnly*/ true);
	}

	// The schema hash the manifest carries for @p ToolName (empty when absent).
	static FString ManifestSchemaHash(const FUnrealMcpToolRegistry& Registry, const FString& ToolName)
	{
		for (const TSharedPtr<FJsonObject>& Desc : ManifestDescriptors(Registry))
		{
			if (Desc->GetStringField(TEXT("name")) == ToolName)
				return Desc->GetStringField(TEXT("schemaHash"));
		}
		return FString();
	}

	// Walk a widget tree collecting every STextBlock's current text and every SCheckBox (in tree order).
	static void ToolsWindowCollectWidgets(const TSharedRef<SWidget>& Widget, TArray<FString>& OutTexts, TArray<TSharedRef<SCheckBox>>& OutChecks)
	{
		if (Widget->GetType() == TEXT("STextBlock"))
			OutTexts.Add(StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString());
		else if (Widget->GetType() == TEXT("SCheckBox"))
			OutChecks.Add(StaticCastSharedRef<SCheckBox>(Widget));
		FChildren* Children = Widget->GetChildren();
		for (int32 Index = 0; Children != nullptr && Index < Children->Num(); ++Index)
			ToolsWindowCollectWidgets(Children->GetChildAt(Index), OutTexts, OutChecks);
	}

	static TArray<TSharedPtr<FJsonValue>> ToolsWindowJsonNames(std::initializer_list<const TCHAR*> Names)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const TCHAR* Name : Names)
			Out.Add(MakeShared<FJsonValueString>(Name));
		return Out;
	}

END_DEFINE_SPEC(FUnrealMcpToolsWindowSpec)

void FUnrealMcpToolsWindowSpec::Define()
{
	Describe("§8 hiddenTools persistence", [this]()
	{
		It("round-trips the hidden list through ToJson/LoadFromJson under the hiddenTools key", [this]()
		{
			FUnrealMcpConfig Config;
			Config.HiddenTools = { TEXT("beta"), TEXT("gamma") };

			const TSharedPtr<FJsonObject> Json = Config.ToJson();
			TestTrue("written under hiddenTools", Json->HasField(TEXT("hiddenTools")));
			TestFalse("the pre-rename disabledTools key is not written", Json->HasField(TEXT("disabledTools")));

			FUnrealMcpConfig Reloaded;
			Reloaded.LoadFromJson(Json);

			TestEqual("list size preserved", Reloaded.HiddenTools.Num(), 2);
			TestTrue("beta preserved", Reloaded.HiddenTools.Contains(TEXT("beta")));
			TestTrue("gamma preserved", Reloaded.HiddenTools.Contains(TEXT("gamma")));
		});

		It("reads the pre-rename disabledTools key and merges it with hiddenTools", [this]()
		{
			const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetArrayField(TEXT("disabledTools"), ToolsWindowJsonNames({ TEXT("alpha"), TEXT("beta") }));
			Json->SetArrayField(TEXT("hiddenTools"), ToolsWindowJsonNames({ TEXT("beta"), TEXT("gamma") }));

			FUnrealMcpConfig Loaded;
			Loaded.LoadFromJson(Json);
			TestEqual("union without duplicates", Loaded.HiddenTools.Num(), 3);
			TestTrue("alpha from the old key", Loaded.HiddenTools.Contains(TEXT("alpha")));
			TestTrue("gamma from the new key", Loaded.HiddenTools.Contains(TEXT("gamma")));
		});

		It("survives a real Save() / LoadFromFile() reload and migrates an old file to hiddenTools", [this]()
		{
			const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcpTests"),
				FString::Printf(TEXT("tools-%s.json"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));

			// A config file written by a plugin from before the rename.
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree*/ true);
			TestTrue("old file written", FFileHelper::SaveStringToFile(TEXT("{ \"disabledTools\": [ \"alpha\" ] }"), *Path));

			FUnrealMcpConfig Old;
			Old.LoadFromFile(Path);
			TestTrue("alpha hidden from the old key", Old.HiddenTools.Contains(TEXT("alpha")));
			TestTrue("save succeeds", Old.Save(Path));

			FString Written;
			FFileHelper::LoadFileToString(Written, *Path);
			TestTrue("the saved file uses hiddenTools", Written.Contains(TEXT("\"hiddenTools\"")));
			TestFalse("the saved file drops disabledTools", Written.Contains(TEXT("\"disabledTools\"")));

			FUnrealMcpConfig Reloaded;
			Reloaded.LoadFromFile(Path);
			TestTrue("alpha hidden after reload", Reloaded.HiddenTools.Contains(TEXT("alpha")));
			TestFalse("beta not hidden after reload", Reloaded.HiddenTools.Contains(TEXT("beta")));

			IFileManager::Get().Delete(*Path);
		});
	});

	Describe("Registry manifest visibility", [this]()
	{
		It("flags a hidden tool in the served manifest and lists it again on re-show", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			SeedTools(Registry);
			TestEqual("all three visible initially", VisibleToolNames(Registry).Num(), 3);
			const FString HashBefore = ManifestSchemaHash(Registry, TEXT("beta"));
			const int32 RevisionBefore = Registry.GetRevision();

			TestTrue("hide reports a change", Registry.SetToolVisible(TEXT("beta"), false));
			TestTrue("hide bumps the revision", Registry.GetRevision() > RevisionBefore);
			TestEqual("all three still in the manifest", ManifestToolNames(Registry).Num(), 3);
			TArray<FString> Listed = VisibleToolNames(Registry);
			TestFalse("beta not listed", Listed.Contains(TEXT("beta")));
			TestTrue("alpha still listed", Listed.Contains(TEXT("alpha")));
			TestEqual("the schema hash ignores visibility", ManifestSchemaHash(Registry, TEXT("beta")), HashBefore);
			TestEqual("NumVisibleTools reflects the hidden tool", Registry.NumVisibleTools(), 2);
			TestEqual("a hidden tool is still served", Registry.NumEnabled(), 3);

			// A no-op toggle does not churn the revision.
			TestFalse("re-hiding is a no-op", Registry.SetToolVisible(TEXT("beta"), false));

			TestTrue("re-show reports a change", Registry.SetToolVisible(TEXT("beta"), true));
			TestTrue("beta listed again", VisibleToolNames(Registry).Contains(TEXT("beta")));
		});

		It("applies a whole hidden-name set at once (the boot path)", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			SeedTools(Registry);

			Registry.ApplyHiddenTools({ TEXT("alpha"), TEXT("gamma") });
			TArray<FString> Listed = VisibleToolNames(Registry);
			TestEqual("only beta listed", Listed.Num(), 1);
			TestTrue("beta is the listed one", Listed.Contains(TEXT("beta")));
			TestEqual("all three still served", ManifestToolNames(Registry).Num(), 3);

			// Re-applying an empty set shows everything.
			Registry.ApplyHiddenTools({});
			TestEqual("all visible again", VisibleToolNames(Registry).Num(), 3);
		});
	});

	Describe("View-model toggle round-trip", [this]()
	{
		It("persists the choice and re-applies the visibility through the registry", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			SeedTools(Registry);

			int32 PersistCount = 0;
			FUnrealMcpEditorViewModel VM;
			VM.OnPersistConfig = [&PersistCount](const FUnrealMcpConfig&) { ++PersistCount; };
			// Wire the visibility sink exactly as the runtime does: re-apply the hidden list to the live registry.
			VM.OnToolVisibilityChanged = [&Registry](const TArray<FString>& Hidden) { Registry.ApplyHiddenTools(Hidden); };

			// Hide "gamma" through the view-model (the §7 Tools-window checkbox path).
			VM.SetToolVisible(TEXT("gamma"), false);
			TestEqual("persist fired once", PersistCount, 1);
			TestTrue("view-model reports gamma hidden", VM.IsToolHidden(TEXT("gamma")));
			TestTrue("hidden list carries gamma", VM.GetHiddenTools().Contains(TEXT("gamma")));
			TestFalse("gamma not listed", VisibleToolNames(Registry).Contains(TEXT("gamma")));
			TestTrue("gamma still runs", Registry.Execute(TEXT("gamma"), FUnrealMcpToolCall{}).bSuccess);

			// A no-op toggle (already visible) does nothing.
			VM.SetToolVisible(TEXT("alpha"), true);
			TestEqual("no extra persist on no-op", PersistCount, 1);

			// Re-show lists it again + clears the hidden list.
			VM.SetToolVisible(TEXT("gamma"), true);
			TestEqual("persist fired again on re-show", PersistCount, 2);
			TestFalse("view-model reports gamma visible", VM.IsToolHidden(TEXT("gamma")));
			TestTrue("gamma listed again", VisibleToolNames(Registry).Contains(TEXT("gamma")));
		});

		It("re-applies a persisted hidden list to a fresh registry on boot", [this]()
		{
			// Persist a hidden list via one config…
			FUnrealMcpConfig Saved;
			Saved.HiddenTools = { TEXT("beta") };
			const TSharedPtr<FJsonObject> Json = Saved.ToJson();

			// …then simulate a fresh editor boot: reload the config, seed a fresh registry, apply the list.
			FUnrealMcpConfig Booted;
			Booted.LoadFromJson(Json);
			FUnrealMcpToolRegistry Registry;
			SeedTools(Registry);
			Registry.ApplyHiddenTools(Booted.HiddenTools);

			TArray<FString> Listed = VisibleToolNames(Registry);
			TestEqual("two listed after boot-apply", Listed.Num(), 2);
			TestFalse("beta stays hidden across the restart", Listed.Contains(TEXT("beta")));
		});
	});

	Describe("Extension hot-reload retention (§5)", [this]()
	{
		It("keeps a hidden extension tool hidden across an extension rebuild", [this]()
		{
			// The §5 regression: ExtensionManager::RebuildFromProviders removes then re-registers every
			// extension tool FRESH (flags default true) and re-pushes the manifest. The registry retains the
			// hidden list, so a hidden extension tool must not come back listed.
			FUnrealMcpToolRegistry Registry;
			SeedTools(Registry); // core: alpha/beta/gamma
			RegisterExtensionTool(Registry, TEXT("com.example.ext"), TEXT("ext-tool"));
			TestTrue("ext-tool listed before hide", VisibleToolNames(Registry).Contains(TEXT("ext-tool")));

			Registry.ApplyHiddenTools({ TEXT("ext-tool") });
			TestFalse("ext-tool not listed after hide", VisibleToolNames(Registry).Contains(TEXT("ext-tool")));

			// Simulate the hot-reload: remove the extension's tools, then re-register them fresh.
			const int32 Removed = Registry.RemoveToolsForExtension(TEXT("com.example.ext"));
			TestEqual("one extension tool removed on unload", Removed, 1);
			RegisterExtensionTool(Registry, TEXT("com.example.ext"), TEXT("ext-tool"));

			TArray<FString> AfterRebuild = VisibleToolNames(Registry);
			TestFalse("ext-tool STILL hidden after the extension rebuild", AfterRebuild.Contains(TEXT("ext-tool")));
			TestTrue("ext-tool still served after the rebuild", ManifestToolNames(Registry).Contains(TEXT("ext-tool")));
			TestTrue("core alpha unaffected by the rebuild", AfterRebuild.Contains(TEXT("alpha")));
		});
	});

	Describe("§8 enabledTools whitelist gating", [this]()
	{
		// served iff (whitelist empty OR member); listed iff served AND NOT hidden — the 3×2 matrix
		// (whitelist empty / member / non-member) × (hidden / not).
		It("serves by the whitelist and lists by the Visible toggle", [this]()
		{
			// Row group 1 — EMPTY whitelist (no filter): every tool served, the hidden list decides the listing.
			{
				FUnrealMcpToolRegistry Registry;
				SeedTools(Registry);
				Registry.SetEnabledToolsFilter({});           // empty → no whitelist filter
				Registry.ApplyHiddenTools({ TEXT("beta") });  // beta hidden
				TArray<FString> Listed = VisibleToolNames(Registry);
				TestTrue("empty whitelist + visible → listed (alpha)", Listed.Contains(TEXT("alpha")));
				TestTrue("empty whitelist + visible → listed (gamma)", Listed.Contains(TEXT("gamma")));
				TestFalse("empty whitelist + hidden → not listed (beta)", Listed.Contains(TEXT("beta")));
				TestTrue("empty whitelist + hidden → still served (beta)", ManifestToolNames(Registry).Contains(TEXT("beta")));
			}

			// Row group 2 — whitelist {alpha,beta}, hidden {beta}: member/visible, member/hidden, non-member/visible.
			{
				FUnrealMcpToolRegistry Registry;
				SeedTools(Registry);
				Registry.SetEnabledToolsFilter({ TEXT("alpha"), TEXT("beta") });
				Registry.ApplyHiddenTools({ TEXT("beta") });
				TArray<FString> Served = ManifestToolNames(Registry);
				TArray<FString> Listed = VisibleToolNames(Registry);
				TestTrue("member + visible → listed (alpha)", Listed.Contains(TEXT("alpha")));
				TestFalse("member + hidden → not listed (beta)", Listed.Contains(TEXT("beta")));
				TestTrue("member + hidden → served (beta)", Served.Contains(TEXT("beta")));
				TestFalse("non-member + visible → not served (gamma)", Served.Contains(TEXT("gamma")));
				TestEqual("alpha and beta served", Served.Num(), 2);
			}

			// Row group 3 — whitelist {alpha}, hidden {gamma}: covers non-member/hidden.
			{
				FUnrealMcpToolRegistry Registry;
				SeedTools(Registry);
				Registry.SetEnabledToolsFilter({ TEXT("alpha") });
				Registry.ApplyHiddenTools({ TEXT("gamma") });
				TArray<FString> Served = ManifestToolNames(Registry);
				TestTrue("member + visible → served (alpha)", Served.Contains(TEXT("alpha")));
				TestFalse("non-member + hidden → not served (gamma)", Served.Contains(TEXT("gamma")));
				TestFalse("non-member + visible → not served (beta)", Served.Contains(TEXT("beta")));
			}
		});

		It("re-applying the hidden list does not clobber the retained whitelist", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			SeedTools(Registry);
			Registry.SetEnabledToolsFilter({ TEXT("alpha") }); // only alpha passes the whitelist
			TestEqual("whitelist alone leaves only alpha", ManifestToolNames(Registry).Num(), 1);

			Registry.ApplyHiddenTools({}); // clear the hidden list entirely
			TArray<FString> Served = ManifestToolNames(Registry);
			TestEqual("whitelist still enforced after a hidden-list clear", Served.Num(), 1);
			TestTrue("alpha remains the only served tool", Served.Contains(TEXT("alpha")));
		});

		It("registers extension tools honoring the retained whitelist on a rebuild", [this]()
		{
			// Closes the loop with §5: a non-whitelisted extension tool re-added by a hot-reload stays unserved.
			FUnrealMcpToolRegistry Registry;
			SeedTools(Registry);
			Registry.SetEnabledToolsFilter({ TEXT("alpha") }); // ext-tool is NOT whitelisted
			RegisterExtensionTool(Registry, TEXT("com.example.ext"), TEXT("ext-tool"));
			TestFalse("non-whitelisted ext-tool excluded on first register", ManifestToolNames(Registry).Contains(TEXT("ext-tool")));

			Registry.RemoveToolsForExtension(TEXT("com.example.ext"));
			RegisterExtensionTool(Registry, TEXT("com.example.ext"), TEXT("ext-tool"));
			TestFalse("non-whitelisted ext-tool STILL excluded after rebuild", ManifestToolNames(Registry).Contains(TEXT("ext-tool")));
		});

		It("reports the whitelist gate independently of the hidden list (the §7 window's data source)", [this]()
		{
			// SUnrealMcpToolsWindow snapshots PassesEnabledToolsWhitelist() to render whitelist-gated rows; it is the
			// STATIC env dimension, so it must NOT react to the §7 Visible toggle (which ApplyHiddenTools drives).
			FUnrealMcpToolRegistry Registry;
			SeedTools(Registry);
			TestTrue("empty whitelist → every tool passes", Registry.PassesEnabledToolsWhitelist(TEXT("beta")));

			Registry.SetEnabledToolsFilter({ TEXT("alpha") });
			TestTrue("whitelisted name passes", Registry.PassesEnabledToolsWhitelist(TEXT("alpha")));
			TestFalse("non-whitelisted name fails", Registry.PassesEnabledToolsWhitelist(TEXT("beta")));

			// Hiding alpha must not change its whitelist verdict — the two dimensions are orthogonal.
			Registry.ApplyHiddenTools({ TEXT("alpha") });
			TestTrue("hiding does not flip the whitelist verdict", Registry.PassesEnabledToolsWhitelist(TEXT("alpha")));
		});
	});

	Describe("Tools window (Slate)", [this]()
	{
		It("labels the per-tool toggle Visible and counts what tools/list shows", [this]()
		{
			const TSharedPtr<FUnrealMcpEditorViewModel> VM = MakeShared<FUnrealMcpEditorViewModel>();
			VM->SetToolVisible(TEXT("beta"), false);

			const TSharedRef<SUnrealMcpToolsWindow> Window = SNew(SUnrealMcpToolsWindow)
				.ViewModel(VM)
				.ToolListProvider([]()
				{
					return TArray<FUnrealMcpToolListEntry>{
						{ TEXT("alpha"), TEXT("Alpha"), TEXT("the alpha tool"), TEXT("core"), true },
						{ TEXT("beta"), TEXT("Beta"), TEXT("the beta tool"), TEXT("core"), true },
						{ TEXT("gamma"), TEXT("Gamma"), TEXT("the gamma tool"), TEXT("core"), false } };
				});

			TArray<FString> Texts;
			TArray<TSharedRef<SCheckBox>> Checks;
			ToolsWindowCollectWidgets(Window, Texts, Checks);

			TestTrue("summary counts the visible tools", Texts.Contains(TEXT("1 / 3 tools visible")));
			TestTrue("the window explains Visible", Texts.ContainsByPredicate([](const FString& Text)
			{
				return Text.StartsWith(TEXT("Visible: listed in tools/list")) && Text.Contains(TEXT("still runs when called by name"));
			}));
			TestFalse("no 'enabled' wording left for the toggle", Texts.ContainsByPredicate([](const FString& Text) { return Text.Contains(TEXT("tools enabled")); }));
			if (!TestEqual("one Visible checkbox per tool", Checks.Num(), 3))
				return;

			TestTrue("alpha checked (visible)", Checks[0]->IsChecked());
			TestFalse("beta unchecked (hidden)", Checks[1]->IsChecked());
			TestFalse("a whitelist-gated row cannot be toggled", Checks[2]->IsEnabled());

			// Clicking a checkbox drives the view-model like the user would.
			Checks[0]->ToggleCheckedState();
			TestTrue("unchecking alpha hides it", VM->IsToolHidden(TEXT("alpha")));
			Checks[1]->ToggleCheckedState();
			TestFalse("checking beta shows it", VM->IsToolHidden(TEXT("beta")));
		});
	});

	Describe("Execute gate (§8 execution boundary)", [this]()
	{
		It("runs a hidden tool when called by name (Visible is not an access control)", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			SeedTools(Registry);
			Registry.ApplyHiddenTools({ TEXT("beta") }); // beta hidden (§7)

			const FUnrealMcpToolResult Hidden = Registry.Execute(TEXT("beta"), FUnrealMcpToolCall{});
			TestTrue("hidden tool executes", Hidden.bSuccess);

			const FUnrealMcpToolResult Visible = Registry.Execute(TEXT("alpha"), FUnrealMcpToolCall{});
			TestTrue("visible tool executes", Visible.bSuccess);

			const FUnrealMcpToolResult Unknown = Registry.Execute(TEXT("does-not-exist"), FUnrealMcpToolCall{});
			TestFalse("unknown tool still errors", Unknown.bSuccess);
			TestTrue("unknown-tool error names it unknown", Unknown.Message.Contains(TEXT("Unknown")));
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
