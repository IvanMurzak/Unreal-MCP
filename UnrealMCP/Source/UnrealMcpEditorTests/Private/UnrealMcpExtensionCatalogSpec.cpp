// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Extensions/UnrealMcpExtensionCatalog.h"
#include "Extensions/UnrealMcpUProjectPlugins.h"

/**
 * Extensions panel — catalog + .uproject helper specs (docs/ARCHITECTURE.md §7 item 10, issue #176). The
 * in-editor install channel (#3) reuses the SAME catalog format + install contract as the CLI (#172); these
 * specs prove the native C++ catalog parser / lookup and the `.uproject`/`.uplugin` edit helpers behave with
 * parity to the TS (`extensions-catalog.ts` / `uproject-plugins.ts`) — all pure, no live download / no editor
 * world. The Slate panel itself is operator-verified (windowed).
 */
BEGIN_DEFINE_SPEC(FUnrealMcpExtensionCatalogSpec, "UnrealMcp.ExtensionCatalog",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	// The real shared catalog JSON shape (mirrors cli/extensions.catalog.json) — one Niagara entry.
	static FString CatalogSpecSampleJson()
	{
		return TEXT(R"({
			"schemaVersion": 1,
			"extensions": [
				{
					"extensionId": "com.ivanmurzak.unreal-ai-niagara",
					"name": "Niagara Tools",
					"description": "AI tools for Unreal's Niagara VFX system.",
					"pluginName": "UnrealAINiagara",
					"repo": "IvanMurzak/Unreal-AI-Niagara",
					"version": "0.1.0",
					"minCoreVersion": "0.5.0",
					"enginePlugins": ["Niagara"],
					"tools": [
						{ "name": "niagara-list-systems", "description": "List Niagara systems." }
					]
				}
			]
		})");
	}

	// Parse a JSON text into an FJsonObject (helper for the .uplugin/.uproject specs).
	static TSharedPtr<FJsonObject> CatalogSpecParseObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		FJsonSerializer::Deserialize(Reader, Root);
		return Root;
	}

END_DEFINE_SPEC(FUnrealMcpExtensionCatalogSpec)

void FUnrealMcpExtensionCatalogSpec::Define()
{
	Describe("ParseCatalogJson", [this]()
	{
		It("parses the shared catalog shape into typed entries", [this]()
		{
			TArray<FUnrealMcpCatalogEntry> Entries;
			FString Error, Warning;
			TestTrue("parse succeeds", FUnrealMcpExtensionCatalog::ParseCatalogJson(CatalogSpecSampleJson(), Entries, Error, Warning));
			TestEqual("one entry", Entries.Num(), 1);
			TestTrue("no warning on supported schema", Warning.IsEmpty());

			const FUnrealMcpCatalogEntry& E = Entries[0];
			TestEqual("extensionId", E.ExtensionId, FString(TEXT("com.ivanmurzak.unreal-ai-niagara")));
			TestEqual("pluginName", E.PluginName, FString(TEXT("UnrealAINiagara")));
			TestEqual("repo", E.Repo, FString(TEXT("IvanMurzak/Unreal-AI-Niagara")));
			TestEqual("version", E.Version, FString(TEXT("0.1.0")));
			TestEqual("minCoreVersion", E.MinCoreVersion, FString(TEXT("0.5.0")));
			TestTrue("has version", E.HasVersion());
			TestEqual("one gating engine plugin", E.EnginePlugins.Num(), 1);
			TestEqual("gating plugin is Niagara", E.EnginePlugins[0], FString(TEXT("Niagara")));
			TestEqual("one tool", E.Tools.Num(), 1);
			TestEqual("tool name", E.Tools[0].Name, FString(TEXT("niagara-list-systems")));
		});

		It("rejects malformed JSON, a missing extensions array, and an entry missing required identity", [this]()
		{
			TArray<FUnrealMcpCatalogEntry> Entries;
			FString Error, Warning;
			TestFalse("malformed JSON fails", FUnrealMcpExtensionCatalog::ParseCatalogJson(TEXT("{ not json"), Entries, Error, Warning));
			TestFalse("no extensions array fails", FUnrealMcpExtensionCatalog::ParseCatalogJson(TEXT("{ \"schemaVersion\": 1 }"), Entries, Error, Warning));

			const FString NoId = TEXT("{ \"extensions\": [ { \"pluginName\": \"X\" } ] }");
			TestFalse("missing extensionId fails", FUnrealMcpExtensionCatalog::ParseCatalogJson(NoId, Entries, Error, Warning));
			const FString NoPlugin = TEXT("{ \"extensions\": [ { \"extensionId\": \"a.b\" } ] }");
			TestFalse("missing pluginName fails", FUnrealMcpExtensionCatalog::ParseCatalogJson(NoPlugin, Entries, Error, Warning));
		});

		It("warns (but still parses) when the schemaVersion is newer than supported", [this]()
		{
			const FString Newer = TEXT("{ \"schemaVersion\": 99, \"extensions\": [ { \"extensionId\": \"a.b\", \"pluginName\": \"AB\" } ] }");
			TArray<FUnrealMcpCatalogEntry> Entries;
			FString Error, Warning;
			TestTrue("still parses best-effort", FUnrealMcpExtensionCatalog::ParseCatalogJson(Newer, Entries, Error, Warning));
			TestEqual("one entry parsed", Entries.Num(), 1);
			TestFalse("a warning is surfaced", Warning.IsEmpty());
		});
	});

	Describe("FindEntry", [this]()
	{
		It("resolves by extensionId, name, and pluginName, case-insensitively", [this]()
		{
			TArray<FUnrealMcpCatalogEntry> Entries;
			FString Error, Warning;
			FUnrealMcpExtensionCatalog::ParseCatalogJson(CatalogSpecSampleJson(), Entries, Error, Warning);

			TestNotNull("by extensionId", FUnrealMcpExtensionCatalog::FindEntry(Entries, TEXT("com.ivanmurzak.unreal-ai-niagara")));
			TestNotNull("by name (caseless)", FUnrealMcpExtensionCatalog::FindEntry(Entries, TEXT("niagara tools")));
			TestNotNull("by pluginName (caseless)", FUnrealMcpExtensionCatalog::FindEntry(Entries, TEXT("unrealainiagara")));
			TestNull("unknown id", FUnrealMcpExtensionCatalog::FindEntry(Entries, TEXT("does.not.exist")));
			TestNull("empty id", FUnrealMcpExtensionCatalog::FindEntry(Entries, TEXT("")));
		});
	});

	Describe("UProject/.uplugin helpers", [this]()
	{
		It("reads a .uplugin's enabled dependencies and VersionName", [this]()
		{
			const FString Uplugin = TEXT(R"({
				"VersionName": "1.2.3",
				"Plugins": [
					{ "Name": "Niagara", "Enabled": true },
					{ "Name": "Disabled", "Enabled": false },
					{ "Name": "DefaultEnabled" }
				]
			})");
			const TSharedPtr<FJsonObject> Obj = CatalogSpecParseObject(Uplugin);
			const TArray<FString> Deps = FUnrealMcpUProjectPlugins::ParseUPluginDependencies(Obj);
			TestTrue("Niagara is a dep", Deps.Contains(TEXT("Niagara")));
			TestTrue("default-enabled (no Enabled field) is a dep", Deps.Contains(TEXT("DefaultEnabled")));
			TestFalse("explicitly disabled is excluded", Deps.Contains(TEXT("Disabled")));
			TestEqual("VersionName", FUnrealMcpUProjectPlugins::ReadUPluginVersionName(Obj), FString(TEXT("1.2.3")));
		});

		It("enables absent plugins, flips disabled ones, and is idempotent", [this]()
		{
			const FString Before = TEXT("{\n\t\"FileVersion\": 3,\n\t\"Plugins\": [\n\t\t{ \"Name\": \"Off\", \"Enabled\": false }\n\t]\n}\n");

			FUnrealMcpEnablePluginsResult Result;
			FString Error;
			TestTrue("enable succeeds", FUnrealMcpUProjectPlugins::EnablePluginsInUProject(Before, { TEXT("UnrealAINiagara"), TEXT("Niagara"), TEXT("Off") }, Result, Error));
			TestTrue("text changed", Result.bChanged);
			TestTrue("appended UnrealAINiagara", Result.AddedOrFlipped.Contains(TEXT("UnrealAINiagara")));
			TestTrue("appended Niagara", Result.AddedOrFlipped.Contains(TEXT("Niagara")));
			TestTrue("flipped Off on", Result.AddedOrFlipped.Contains(TEXT("Off")));

			// Re-parse the output and assert the enabled set (key order is not asserted — UE reorders).
			const TSharedPtr<FJsonObject> After = CatalogSpecParseObject(Result.Text);
			const TArray<FString> Enabled = FUnrealMcpUProjectPlugins::ParseUPluginDependencies(After);
			TestTrue("UnrealAINiagara enabled", Enabled.Contains(TEXT("UnrealAINiagara")));
			TestTrue("Niagara enabled", Enabled.Contains(TEXT("Niagara")));
			TestTrue("Off now enabled", Enabled.Contains(TEXT("Off")));

			// Idempotent: re-running with the same set is a no-op (nothing added/flipped, text unchanged).
			FUnrealMcpEnablePluginsResult Again;
			TestTrue("second run succeeds", FUnrealMcpUProjectPlugins::EnablePluginsInUProject(Result.Text, { TEXT("UnrealAINiagara"), TEXT("Niagara"), TEXT("Off") }, Again, Error));
			TestFalse("no change on the idempotent re-run", Again.bChanged);
			TestEqual("nothing added/flipped on the re-run", Again.AddedOrFlipped.Num(), 0);
		});

		It("returns the input verbatim on a pure no-op (already enabled)", [this]()
		{
			const FString Before = TEXT("{\n\t\"Plugins\": [\n\t\t{ \"Name\": \"Niagara\", \"Enabled\": true }\n\t]\n}\n");
			FUnrealMcpEnablePluginsResult Result;
			FString Error;
			TestTrue("succeeds", FUnrealMcpUProjectPlugins::EnablePluginsInUProject(Before, { TEXT("Niagara") }, Result, Error));
			TestFalse("no change", Result.bChanged);
			TestEqual("text returned verbatim", Result.Text, Before);
		});

		It("fails cleanly on malformed .uproject JSON", [this]()
		{
			FUnrealMcpEnablePluginsResult Result;
			FString Error;
			TestFalse("malformed fails", FUnrealMcpUProjectPlugins::EnablePluginsInUProject(TEXT("{ not json"), { TEXT("X") }, Result, Error));
			TestFalse("error is set", Error.IsEmpty());
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
