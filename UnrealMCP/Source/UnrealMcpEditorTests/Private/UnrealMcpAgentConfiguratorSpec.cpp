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
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "Agents/AiAgentConfig.h"
#include "Agents/JsonAiAgentConfig.h"
#include "Agents/AiAgentConfigurator.h"
#include "Agents/AiAgentConfiguratorRegistry.h"
#include "Agents/Impl/ClaudeCodeConfigurator.h"
#include "Agents/Impl/CursorConfigurator.h"
#include "Config/UnrealMcpConfig.h"

/**
 * AI Agent Configurators specs (docs/ARCHITECTURE.md §7/§8, issue #44 Phase A). Covers the JSON read-merge-write
 * core (preserve siblings + unrelated keys, robust to missing/empty/invalid files, identity-key dedup, deprecated
 * migration, IsDetected/IsConfigured), the configurator base STDIO/HTTP assembly + auth injection, and the
 * registry ordering/lookups. Pure file/JSON work — no live editor, bridge, or agent.
 *
 * Names are unique under the UnrealMcp. filter prefix; helpers are spec members (the unity-build ODR rule).
 */
BEGIN_DEFINE_SPEC(FUnrealMcpAgentConfiguratorSpec, "UnrealMcp.AgentConfigurators",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	// A unique temp config-file path per call (cleaned up by the spec when done).
	FString MakeTempConfigPath() const
	{
		const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcpAgentSpec"), FGuid::NewGuid().ToString());
		return FPaths::Combine(Dir, TEXT("mcp.json"));
	}

	void DeleteTempDirFor(const FString& ConfigPath) const
	{
		const FString Dir = FPaths::GetPath(ConfigPath);
		if (!Dir.IsEmpty())
			IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists*/ false, /*Tree*/ true);
	}

	// Parse a file into a root object (or null).
	TSharedPtr<FJsonObject> ReadJsonFile(const FString& Path) const
	{
		FString Json;
		if (!FFileHelper::LoadFileToString(Json, *Path))
			return nullptr;
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Json);
		TSharedPtr<FJsonObject> Parsed;
		FJsonSerializer::Deserialize(Reader, Parsed);
		return Parsed;
	}

	// Build a Claude-Code-style HTTP config at a path (type=http, url, no headers).
	TSharedRef<FJsonAiAgentConfig> MakeHttpConfig(const FString& Path, const FString& Url) const
	{
		TSharedRef<FJsonAiAgentConfig> Config = MakeShared<FJsonAiAgentConfig>(TEXT("Test"), Path, TEXT("mcpServers"));
		Config->SetStringProperty(TEXT("type"), TEXT("http"), /*bRequired*/ true);
		Config->SetStringProperty(TEXT("url"), Url, /*bRequired*/ true, EUnrealMcpValueComparison::Url);
		Config->SetPropertyToRemove(TEXT("command"));
		Config->SetPropertyToRemove(TEXT("args"));
		return Config;
	}

END_DEFINE_SPEC(FUnrealMcpAgentConfiguratorSpec)

void FUnrealMcpAgentConfiguratorSpec::Define()
{
	Describe("JSON read-merge-write", [this]()
	{
		It("creates a fresh file (with parent dirs) when none exists, nesting under the body path", [this]()
		{
			const FString Path = MakeTempConfigPath();
			TSharedRef<FJsonAiAgentConfig> Config = MakeHttpConfig(Path, TEXT("http://localhost:8080/mcp"));

			TestTrue(TEXT("Configure succeeds"), Config->Configure());
			TestTrue(TEXT("file exists"), FPaths::FileExists(Path));

			TSharedPtr<FJsonObject> Root = ReadJsonFile(Path);
			TestTrue(TEXT("root parsed"), Root.IsValid());
			const TSharedPtr<FJsonObject>* Body = nullptr;
			TestTrue(TEXT("mcpServers present"), Root.IsValid() && Root->TryGetObjectField(TEXT("mcpServers"), Body) && Body);
			if (Body)
			{
				const TSharedPtr<FJsonObject>* Entry = nullptr;
				TestTrue(TEXT("unreal-mcp entry present"), (*Body)->TryGetObjectField(TEXT("unreal-mcp"), Entry) && Entry);
			}
			TestTrue(TEXT("IsConfigured after write"), Config->IsConfigured());

			DeleteTempDirFor(Path);
		});

		It("preserves sibling servers and unrelated top-level keys on merge", [this]()
		{
			const FString Path = MakeTempConfigPath();
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			const FString Existing = TEXT("{\n  \"someTopLevel\": 42,\n  \"mcpServers\": {\n    \"other-server\": { \"type\": \"http\", \"url\": \"http://elsewhere/mcp\" }\n  }\n}");
			FFileHelper::SaveStringToFile(Existing, *Path);

			TSharedRef<FJsonAiAgentConfig> Config = MakeHttpConfig(Path, TEXT("http://localhost:8080/mcp"));
			TestTrue(TEXT("Configure succeeds"), Config->Configure());

			TSharedPtr<FJsonObject> Root = ReadJsonFile(Path);
			TestTrue(TEXT("unrelated top-level key preserved"), Root.IsValid() && Root->HasField(TEXT("someTopLevel")));
			const TSharedPtr<FJsonObject>* Body = nullptr;
			Root->TryGetObjectField(TEXT("mcpServers"), Body);
			TestTrue(TEXT("sibling server preserved"), Body && (*Body)->HasField(TEXT("other-server")));
			TestTrue(TEXT("our entry added"), Body && (*Body)->HasField(TEXT("unreal-mcp")));

			DeleteTempDirFor(Path);
		});

		It("tolerates an invalid (non-JSON) file by replacing it with clean content", [this]()
		{
			const FString Path = MakeTempConfigPath();
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			FFileHelper::SaveStringToFile(FString(TEXT("this is not json {{{")), *Path);

			TSharedRef<FJsonAiAgentConfig> Config = MakeHttpConfig(Path, TEXT("http://localhost:8080/mcp"));
			TestTrue(TEXT("Configure succeeds over garbage"), Config->Configure());
			TestTrue(TEXT("IsConfigured after replace"), Config->IsConfigured());

			DeleteTempDirFor(Path);
		});

		It("tolerates an empty file", [this]()
		{
			const FString Path = MakeTempConfigPath();
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			FFileHelper::SaveStringToFile(FString(TEXT("")), *Path);

			TSharedRef<FJsonAiAgentConfig> Config = MakeHttpConfig(Path, TEXT("http://localhost:8080/mcp"));
			TestTrue(TEXT("Configure succeeds over empty"), Config->Configure());
			TestTrue(TEXT("IsConfigured after empty"), Config->IsConfigured());

			DeleteTempDirFor(Path);
		});

		It("IsDetected false for a missing file, true once configured", [this]()
		{
			const FString Path = MakeTempConfigPath();
			TSharedRef<FJsonAiAgentConfig> Config = MakeHttpConfig(Path, TEXT("http://localhost:8080/mcp"));
			TestFalse(TEXT("not detected before write"), Config->IsDetected());
			Config->Configure();
			TestTrue(TEXT("detected after write"), Config->IsDetected());

			DeleteTempDirFor(Path);
		});

		It("Remove deletes our entry but preserves siblings", [this]()
		{
			const FString Path = MakeTempConfigPath();
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			const FString Existing = TEXT("{\n  \"mcpServers\": {\n    \"other-server\": { \"type\": \"http\", \"url\": \"http://elsewhere/mcp\" }\n  }\n}");
			FFileHelper::SaveStringToFile(Existing, *Path);

			TSharedRef<FJsonAiAgentConfig> Config = MakeHttpConfig(Path, TEXT("http://localhost:8080/mcp"));
			Config->Configure();
			TestTrue(TEXT("Remove returns true"), Config->Remove());

			TSharedPtr<FJsonObject> Root = ReadJsonFile(Path);
			const TSharedPtr<FJsonObject>* Body = nullptr;
			Root->TryGetObjectField(TEXT("mcpServers"), Body);
			TestFalse(TEXT("our entry removed"), Body && (*Body)->HasField(TEXT("unreal-mcp")));
			TestTrue(TEXT("sibling preserved after remove"), Body && (*Body)->HasField(TEXT("other-server")));

			DeleteTempDirFor(Path);
		});

		It("migrates a deprecated server name to the canonical one", [this]()
		{
			const FString Path = MakeTempConfigPath();
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			const FString Existing = TEXT("{\n  \"mcpServers\": {\n    \"ai-game-developer\": { \"type\": \"http\", \"url\": \"http://old/mcp\" }\n  }\n}");
			FFileHelper::SaveStringToFile(Existing, *Path);

			TSharedRef<FJsonAiAgentConfig> Config = MakeHttpConfig(Path, TEXT("http://localhost:8080/mcp"));
			Config->Configure();

			TSharedPtr<FJsonObject> Root = ReadJsonFile(Path);
			const TSharedPtr<FJsonObject>* Body = nullptr;
			Root->TryGetObjectField(TEXT("mcpServers"), Body);
			TestFalse(TEXT("deprecated name removed"), Body && (*Body)->HasField(TEXT("ai-game-developer")));
			TestTrue(TEXT("canonical name present"), Body && (*Body)->HasField(TEXT("unreal-mcp")));

			DeleteTempDirFor(Path);
		});

		It("collapses a duplicate sibling that shares our url (identity-key dedup)", [this]()
		{
			const FString Path = MakeTempConfigPath();
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			// A differently-named sibling with the SAME url is the same server under another name → removed.
			const FString Existing = TEXT("{\n  \"mcpServers\": {\n    \"my-alias\": { \"type\": \"http\", \"url\": \"http://localhost:8080/mcp\" }\n  }\n}");
			FFileHelper::SaveStringToFile(Existing, *Path);

			TSharedRef<FJsonAiAgentConfig> Config = MakeHttpConfig(Path, TEXT("http://localhost:8080/mcp"));
			Config->Configure();

			TSharedPtr<FJsonObject> Root = ReadJsonFile(Path);
			const TSharedPtr<FJsonObject>* Body = nullptr;
			Root->TryGetObjectField(TEXT("mcpServers"), Body);
			TestFalse(TEXT("duplicate alias removed"), Body && (*Body)->HasField(TEXT("my-alias")));
			TestTrue(TEXT("canonical entry present"), Body && (*Body)->HasField(TEXT("unreal-mcp")));

			DeleteTempDirFor(Path);
		});

		It("IsConfigured tolerates URL trailing-slash / case differences", [this]()
		{
			const FString Path = MakeTempConfigPath();
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			const FString Existing = TEXT("{\n  \"mcpServers\": {\n    \"unreal-mcp\": { \"type\": \"http\", \"url\": \"http://localhost:8080/mcp/\" }\n  }\n}");
			FFileHelper::SaveStringToFile(Existing, *Path);

			TSharedRef<FJsonAiAgentConfig> Config = MakeHttpConfig(Path, TEXT("http://localhost:8080/mcp"));
			TestTrue(TEXT("trailing slash treated as configured"), Config->IsConfigured());

			DeleteTempDirFor(Path);
		});
	});

	Describe("Configurator base (STDIO + HTTP assembly + auth injection)", [this]()
	{
		It("HTTP form points at <host>/mcp and injects the Authorization header when auth required", [this]()
		{
			FUnrealMcpConfig Config;
			Config.ConnectionMode = EUnrealMcpConnectionMode::Custom;
			Config.CustomHost = TEXT("http://localhost:9000");
			Config.AuthOption = EUnrealMcpAuthOption::Required;
			Config.CustomToken = TEXT("secret-token");

			const FAiAgentConnectionInfo Info = FAiAgentConnectionInfo::FromPluginConfig(Config, FString(), 30001);
			TestEqual(TEXT("http url is host + /mcp"), Info.HttpUrl, FString(TEXT("http://localhost:9000/mcp")));
			TestTrue(TEXT("auth required"), Info.bAuthRequired);
			TestEqual(TEXT("token resolved"), Info.Token, FString(TEXT("secret-token")));

			FClaudeCodeConfigurator Configurator;
			Configurator.Initialize(Info, FPaths::ProjectDir());
			const FString Http = Configurator.GetConfigHttp().GetExpectedFileContent();
			TestTrue(TEXT("http snippet has url"), Http.Contains(TEXT("http://localhost:9000/mcp")));
			TestTrue(TEXT("http snippet has Authorization header"), Http.Contains(TEXT("Bearer secret-token")));
		});

		It("HTTP form omits the Authorization header when auth is not required (Custom + None)", [this]()
		{
			FUnrealMcpConfig Config;
			Config.ConnectionMode = EUnrealMcpConnectionMode::Custom;
			Config.CustomHost = TEXT("http://localhost:9000");
			Config.AuthOption = EUnrealMcpAuthOption::None;
			Config.CustomToken = TEXT("stored-but-unused");

			const FAiAgentConnectionInfo Info = FAiAgentConnectionInfo::FromPluginConfig(Config, FString(), 30001);
			TestFalse(TEXT("auth not required"), Info.bAuthRequired);

			FCursorConfigurator Configurator;
			Configurator.Initialize(Info, FPaths::ProjectDir());
			const FString Http = Configurator.GetConfigHttp().GetExpectedFileContent();
			TestFalse(TEXT("no Authorization header"), Http.Contains(TEXT("Authorization")));
			TestFalse(TEXT("token never present"), Http.Contains(TEXT("stored-but-unused")));
		});

		It("Cloud mode always requires auth and uses the cloud base url + /mcp", [this]()
		{
			FUnrealMcpConfig Config;
			Config.ConnectionMode = EUnrealMcpConnectionMode::Cloud;
			Config.CloudToken = TEXT("cloud-token");

			const FAiAgentConnectionInfo Info = FAiAgentConnectionInfo::FromPluginConfig(Config, FString(), 30001);
			TestTrue(TEXT("cloud always auth-required"), Info.bAuthRequired);
			TestTrue(TEXT("cloud url ends with /mcp"), Info.HttpUrl.EndsWith(TEXT("/mcp")));
			TestEqual(TEXT("cloud token resolved"), Info.Token, FString(TEXT("cloud-token")));
		});

		It("STDIO form emits the port, client-transport, authorization and token args", [this]()
		{
			FUnrealMcpConfig Config;
			Config.ConnectionMode = EUnrealMcpConnectionMode::Custom;
			Config.CustomHost = TEXT("http://localhost:9000");
			Config.AuthOption = EUnrealMcpAuthOption::Required;
			Config.CustomToken = TEXT("tok");

			const FAiAgentConnectionInfo Info = FAiAgentConnectionInfo::FromPluginConfig(Config, TEXT("C:/srv/gamedev-mcp-server.exe"), 31234);
			FClaudeCodeConfigurator Configurator;
			Configurator.Initialize(Info, FPaths::ProjectDir());
			const FString Stdio = Configurator.GetConfigStdio().GetExpectedFileContent();
			TestTrue(TEXT("has port arg"), Stdio.Contains(TEXT("port=31234")));
			TestTrue(TEXT("has client-transport arg"), Stdio.Contains(TEXT("client-transport=stdio")));
			TestTrue(TEXT("has authorization=required"), Stdio.Contains(TEXT("authorization=required")));
			TestTrue(TEXT("has token arg"), Stdio.Contains(TEXT("token=tok")));
			TestTrue(TEXT("command uses forward slashes"), Stdio.Contains(TEXT("C:/srv/gamedev-mcp-server.exe")));
		});

		It("Configure writes to the agent's config path and Remove reverts it", [this]()
		{
			const FString ProjectRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcpAgentSpecRoot"), FGuid::NewGuid().ToString());

			FUnrealMcpConfig Config;
			Config.ConnectionMode = EUnrealMcpConnectionMode::Custom;
			Config.CustomHost = TEXT("http://localhost:9000");
			const FAiAgentConnectionInfo Info = FAiAgentConnectionInfo::FromPluginConfig(Config, FString(), 30001);

			FClaudeCodeConfigurator Configurator;
			Configurator.Initialize(Info, ProjectRoot);

			const FString ExpectedPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(ProjectRoot, TEXT(".mcp.json")));
			TestEqual(TEXT("config path is project/.mcp.json"), Configurator.GetConfigFilePath(ProjectRoot), ExpectedPath);

			TestTrue(TEXT("Configure HTTP"), Configurator.Configure(/*bStdio*/ false));
			TestTrue(TEXT("file written"), FPaths::FileExists(ExpectedPath));
			TestTrue(TEXT("detected"), Configurator.IsAnyDetected());

			TestTrue(TEXT("RemoveAll"), Configurator.RemoveAll());
			TestFalse(TEXT("no longer detected"), Configurator.IsAnyDetected());

			IFileManager::Get().DeleteDirectory(*ProjectRoot, false, true);
		});
	});

	Describe("Registry", [this]()
	{
		It("registers the two Phase-A reference agents, sorted by name", [this]()
		{
			FAiAgentConfiguratorRegistry Registry;
			TestEqual(TEXT("two agents"), Registry.Num(), 2);
			const TArray<FString> Names = Registry.GetAgentNames();
			TestEqual(TEXT("first is Claude Code"), Names[0], FString(TEXT("Claude Code")));
			TestEqual(TEXT("second is Cursor"), Names[1], FString(TEXT("Cursor")));
		});

		It("looks up by id and index, returns INDEX_NONE for unknown", [this]()
		{
			FAiAgentConfiguratorRegistry Registry;
			TestTrue(TEXT("claude-code found"), Registry.GetByAgentId(TEXT("claude-code")).IsValid());
			TestTrue(TEXT("cursor found"), Registry.GetByAgentId(TEXT("cursor")).IsValid());
			TestFalse(TEXT("unknown id null"), Registry.GetByAgentId(TEXT("nope")).IsValid());
			TestEqual(TEXT("claude index 0"), Registry.GetIndexByAgentId(TEXT("claude-code")), 0);
			TestEqual(TEXT("unknown index NONE"), Registry.GetIndexByAgentId(TEXT("nope")), (int32)INDEX_NONE);
			TestTrue(TEXT("index 1 valid"), Registry.GetByIndex(1).IsValid());
			TestFalse(TEXT("index 5 null"), Registry.GetByIndex(5).IsValid());
		});
	});

	Describe("Config selectedAgentId persistence", [this]()
	{
		It("defaults to claude-code and round-trips through ToJson/LoadFromJson", [this]()
		{
			FUnrealMcpConfig Config;
			TestEqual(TEXT("default selection"), Config.SelectedAgentId, FString(TEXT("claude-code")));

			Config.SelectedAgentId = TEXT("cursor");
			const TSharedPtr<FJsonObject> Json = Config.ToJson();
			TestTrue(TEXT("json has selectedAgentId"), Json->HasField(TEXT("selectedAgentId")));

			FUnrealMcpConfig Loaded;
			Loaded.LoadFromJson(Json);
			TestEqual(TEXT("round-tripped selection"), Loaded.SelectedAgentId, FString(TEXT("cursor")));
		});

		It("keeps the default when the persisted value is blank/missing", [this]()
		{
			TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("selectedAgentId"), TEXT(""));
			FUnrealMcpConfig Loaded;
			Loaded.LoadFromJson(Json);
			TestEqual(TEXT("blank keeps default"), Loaded.SelectedAgentId, FString(TEXT("claude-code")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
