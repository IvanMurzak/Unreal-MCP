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
#include "Agents/TomlAiAgentConfig.h"
#include "Agents/AiAgentConfigurator.h"
#include "Agents/AiAgentConfiguratorRegistry.h"
#include "Agents/Impl/ClaudeCodeConfigurator.h"
#include "Agents/Impl/CursorConfigurator.h"
#include "Agents/Impl/ClaudeDesktopConfigurator.h"
#include "Agents/Impl/VisualStudioCodeCopilotConfigurator.h"
#include "Agents/Impl/VisualStudioCopilotConfigurator.h"
#include "Agents/Impl/RiderConfigurator.h"
#include "Agents/Impl/GitHubCopilotCliConfigurator.h"
#include "Agents/Impl/GeminiConfigurator.h"
#include "Agents/Impl/AntigravityConfigurator.h"
#include "Agents/Impl/ClineConfigurator.h"
#include "Agents/Impl/OpenCodeConfigurator.h"
#include "Agents/Impl/CodexConfigurator.h"
#include "Agents/Impl/KiloCodeConfigurator.h"
#include "Agents/Impl/UnityAiConfigurator.h"
#include "Agents/Impl/ZooCodeConfigurator.h"
#include "Agents/Impl/CustomConfigurator.h"
#include "Config/UnrealMcpConfig.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/PlatformFileManager.h"

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

	// Build a STDIO config at a path (type=stdio, command, args) that scrubs the HTTP-only keys (url, headers) —
	// mirrors FAiAgentConfigurator::BuildStdio so a transport switch over a prior HTTP entry leaves no stale keys.
	TSharedRef<FJsonAiAgentConfig> MakeStdioConfig(const FString& Path, const FString& Command) const
	{
		TSharedRef<FJsonAiAgentConfig> Config = MakeShared<FJsonAiAgentConfig>(TEXT("Test"), Path, TEXT("mcpServers"));
		TArray<TSharedPtr<FJsonValue>> Args;
		Args.Add(MakeShared<FJsonValueString>(TEXT("port=8080")));
		Args.Add(MakeShared<FJsonValueString>(TEXT("client-transport=stdio")));
		Config->SetStringProperty(TEXT("type"), TEXT("stdio"), /*bRequired*/ true);
		Config->SetStringProperty(TEXT("command"), Command, /*bRequired*/ true);
		Config->SetProperty(TEXT("args"), MakeShared<FJsonValueArray>(Args), /*bRequired*/ true);
		Config->SetPropertyToRemove(TEXT("url"));
		Config->SetPropertyToRemove(TEXT("headers"));
		return Config;
	}

	// A unique temp TOML config-file path per call.
	FString MakeTempTomlPath() const
	{
		const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcpAgentSpecToml"), FGuid::NewGuid().ToString());
		return FPaths::Combine(Dir, TEXT("config.toml"));
	}

	// Build a Codex-style HTTP TOML config (enabled + url) under "mcp_servers".
	TSharedRef<FTomlAiAgentConfig> MakeTomlHttpConfig(const FString& Path, const FString& Url) const
	{
		TSharedRef<FTomlAiAgentConfig> Config = MakeShared<FTomlAiAgentConfig>(TEXT("Test"), Path, TEXT("mcp_servers"));
		Config->SetBoolProperty(TEXT("enabled"), true, /*bRequired*/ true);
		Config->SetStringProperty(TEXT("url"), Url, /*bRequired*/ true, EUnrealMcpValueComparison::Url);
		Config->SetPropertyToRemove(TEXT("command"));
		Config->SetPropertyToRemove(TEXT("args"));
		return Config;
	}

	// Build a Codex-style STDIO TOML config (enabled + command + args) that scrubs url.
	TSharedRef<FTomlAiAgentConfig> MakeTomlStdioConfig(const FString& Path, const FString& Command) const
	{
		TSharedRef<FTomlAiAgentConfig> Config = MakeShared<FTomlAiAgentConfig>(TEXT("Test"), Path, TEXT("mcp_servers"));
		Config->SetBoolProperty(TEXT("enabled"), true, /*bRequired*/ true);
		Config->SetStringProperty(TEXT("command"), Command, /*bRequired*/ true, EUnrealMcpValueComparison::Path);
		Config->SetStringArrayProperty(TEXT("args"), { TEXT("port=8080"), TEXT("client-transport=stdio") }, /*bRequired*/ true);
		Config->SetPropertyToRemove(TEXT("url"));
		return Config;
	}

	// A connection info with auth required (a real token) for configurator config-shape assertions.
	FAiAgentConnectionInfo MakeAuthConnection(int32 Port = 31000) const
	{
		FUnrealMcpConfig Config;
		Config.ConnectionMode = EUnrealMcpConnectionMode::Custom;
		Config.CustomHost = TEXT("http://localhost:9000");
		Config.AuthOption = EUnrealMcpAuthOption::Required;
		Config.CustomToken = TEXT("secret-token");
		return FAiAgentConnectionInfo::FromPluginConfig(Config, TEXT("C:/srv/gamedev-mcp-server.exe"), Port);
	}

	// Read a whole file into a string (for TOML content assertions).
	FString ReadAllText(const FString& Path) const
	{
		FString Out;
		FFileHelper::LoadFileToString(Out, *Path);
		return Out;
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

		It("switches an existing HTTP entry to STDIO cleanly (drops url/headers, adds command/args)", [this]()
		{
			const FString Path = MakeTempConfigPath();
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			// A pre-existing HTTP "unreal-mcp" entry, complete with an Authorization header.
			const FString Existing = TEXT("{\n  \"mcpServers\": {\n    \"unreal-mcp\": { \"type\": \"http\", \"url\": \"http://localhost:8080/mcp\", \"headers\": { \"Authorization\": \"Bearer stale\" } }\n  }\n}");
			FFileHelper::SaveStringToFile(Existing, *Path);

			TSharedRef<FJsonAiAgentConfig> Config = MakeStdioConfig(Path, TEXT("C:/srv/gamedev-mcp-server.exe"));
			TestTrue(TEXT("STDIO Configure over HTTP succeeds"), Config->Configure());

			TSharedPtr<FJsonObject> Root = ReadJsonFile(Path);
			const TSharedPtr<FJsonObject>* Body = nullptr;
			Root->TryGetObjectField(TEXT("mcpServers"), Body);
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			TestTrue(TEXT("unreal-mcp entry present"), Body && (*Body)->TryGetObjectField(TEXT("unreal-mcp"), Entry) && Entry);
			if (Entry)
			{
				TestFalse(TEXT("stale url removed"), (*Entry)->HasField(TEXT("url")));
				TestFalse(TEXT("stale headers removed"), (*Entry)->HasField(TEXT("headers")));
				TestTrue(TEXT("command present"), (*Entry)->HasField(TEXT("command")));
				TestTrue(TEXT("args present"), (*Entry)->HasField(TEXT("args")));
			}
			TestTrue(TEXT("IsConfigured after switch"), Config->IsConfigured());

			DeleteTempDirFor(Path);
		});

		It("IsConfigured is false when a to-remove key still survives in the entry", [this]()
		{
			const FString Path = MakeTempConfigPath();
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			// An entry that already matches our desired STDIO properties BUT still carries a stale "url" key —
			// the very key the STDIO config asks to remove. HasAnyPropertyToRemove must veto IsConfigured.
			const FString Existing = TEXT("{\n  \"mcpServers\": {\n    \"unreal-mcp\": { \"type\": \"stdio\", \"command\": \"C:/srv/gamedev-mcp-server.exe\", \"args\": [\"port=8080\", \"client-transport=stdio\"], \"url\": \"http://localhost:8080/mcp\" }\n  }\n}");
			FFileHelper::SaveStringToFile(Existing, *Path);

			TSharedRef<FJsonAiAgentConfig> Config = MakeStdioConfig(Path, TEXT("C:/srv/gamedev-mcp-server.exe"));
			TestFalse(TEXT("surviving to-remove key vetoes IsConfigured"), Config->IsConfigured());

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
		It("registers the full Phase-B roster (15 agents + Custom = 16), sorted by name with Custom last", [this]()
		{
			FAiAgentConfiguratorRegistry Registry;
			TestEqual(TEXT("16 entries (15 agents + Custom)"), Registry.Num(), 16);

			const TArray<FString> Names = Registry.GetAgentNames();
			// Sorted alphabetically by display name; Custom ("Other - Custom") is forced LAST regardless of sort.
			TestEqual(TEXT("first is Antigravity"), Names[0], FString(TEXT("Antigravity")));
			TestEqual(TEXT("Custom entry is last"), Names.Last(), FString(TEXT("Other - Custom")));

			// The non-Custom prefix must be strictly alphabetically sorted.
			for (int32 i = 1; i < Names.Num() - 1; ++i)
				TestTrue(FString::Printf(TEXT("sorted at %d (%s <= %s)"), i, *Names[i - 1], *Names[i]), Names[i - 1] <= Names[i]);
		});

		It("contains every required agent id, each unique", [this]()
		{
			FAiAgentConfiguratorRegistry Registry;
			const TArray<FString> ExpectedIds = {
				TEXT("antigravity"), TEXT("claude-code"), TEXT("claude-desktop"), TEXT("cline"), TEXT("codex"),
				TEXT("cursor"), TEXT("gemini"), TEXT("github-copilot-cli"), TEXT("kilo-code"), TEXT("open-code"),
				TEXT("rider-junie"), TEXT("unity-ai"), TEXT("vscode-copilot"), TEXT("vs-copilot"), TEXT("zoo-code"),
				TEXT("other-custom")
			};
			for (const FString& Id : ExpectedIds)
				TestTrue(FString::Printf(TEXT("id present: %s"), *Id), Registry.GetByAgentId(Id).IsValid());

			// No duplicate ids.
			const TArray<FString> Ids = Registry.GetAgentIds();
			TSet<FString> Unique(Ids);
			TestEqual(TEXT("ids are unique"), Unique.Num(), Ids.Num());
		});

		It("Custom is the LAST index, found by id, and is snippet-only (empty config path)", [this]()
		{
			FAiAgentConfiguratorRegistry Registry;
			const int32 CustomIndex = Registry.GetIndexByAgentId(TEXT("other-custom"));
			TestEqual(TEXT("custom is last index"), CustomIndex, Registry.Num() - 1);

			TSharedPtr<FAiAgentConfigurator> Custom = Registry.GetByAgentId(TEXT("other-custom"));
			TestTrue(TEXT("custom found"), Custom.IsValid());
			if (Custom.IsValid())
				TestTrue(TEXT("custom config path empty"), Custom->GetConfigFilePath(FPaths::ProjectDir()).IsEmpty());
		});

		It("looks up by id and index, returns INDEX_NONE for unknown", [this]()
		{
			FAiAgentConfiguratorRegistry Registry;
			TestTrue(TEXT("claude-code found"), Registry.GetByAgentId(TEXT("claude-code")).IsValid());
			TestFalse(TEXT("unknown id null"), Registry.GetByAgentId(TEXT("nope")).IsValid());
			TestEqual(TEXT("unknown index NONE"), Registry.GetIndexByAgentId(TEXT("nope")), (int32)INDEX_NONE);
			TestTrue(TEXT("index 0 valid"), Registry.GetByIndex(0).IsValid());
			TestFalse(TEXT("index 999 null"), Registry.GetByIndex(999).IsValid());
		});
	});

	Describe("Per-agent path + body-path resolution", [this]()
	{
		It("project-local agents resolve their config path under the project root with the right body key", [this]()
		{
			const FString Root = TEXT("C:/proj");

			FVisualStudioCodeCopilotConfigurator VsCode;
			TestTrue(TEXT("vscode path"), VsCode.GetConfigFilePath(Root).Replace(TEXT("\\"), TEXT("/")).EndsWith(TEXT(".vscode/mcp.json")));
			TestEqual(TEXT("vscode body=servers"), VsCode.GetBodyPath(), FString(TEXT("servers")));

			FVisualStudioCopilotConfigurator Vs;
			TestTrue(TEXT("vs path"), Vs.GetConfigFilePath(Root).Replace(TEXT("\\"), TEXT("/")).EndsWith(TEXT(".vs/mcp.json")));
			TestEqual(TEXT("vs body=servers"), Vs.GetBodyPath(), FString(TEXT("servers")));

			FRiderConfigurator Rider;
			TestTrue(TEXT("rider path"), Rider.GetConfigFilePath(Root).Replace(TEXT("\\"), TEXT("/")).EndsWith(TEXT(".junie/mcp/mcp.json")));

			FGeminiConfigurator Gemini;
			TestTrue(TEXT("gemini path"), Gemini.GetConfigFilePath(Root).Replace(TEXT("\\"), TEXT("/")).EndsWith(TEXT(".gemini/settings.json")));

			FGitHubCopilotCliConfigurator Copilot;
			TestTrue(TEXT("copilot-cli path"), Copilot.GetConfigFilePath(Root).Replace(TEXT("\\"), TEXT("/")).EndsWith(TEXT(".mcp.json")));

			FKiloCodeConfigurator Kilo;
			TestTrue(TEXT("kilo path"), Kilo.GetConfigFilePath(Root).Replace(TEXT("\\"), TEXT("/")).EndsWith(TEXT(".kilocode/mcp.json")));

			FZooCodeConfigurator Zoo;
			TestTrue(TEXT("zoo path"), Zoo.GetConfigFilePath(Root).Replace(TEXT("\\"), TEXT("/")).EndsWith(TEXT(".roo/mcp.json")));

			FOpenCodeConfigurator OpenCode;
			TestTrue(TEXT("opencode path"), OpenCode.GetConfigFilePath(Root).Replace(TEXT("\\"), TEXT("/")).EndsWith(TEXT("opencode.json")));
			TestEqual(TEXT("opencode body=mcp"), OpenCode.GetBodyPath(), FString(TEXT("mcp")));

			FCodexConfigurator Codex;
			TestTrue(TEXT("codex path"), Codex.GetConfigFilePath(Root).Replace(TEXT("\\"), TEXT("/")).EndsWith(TEXT(".codex/config.toml")));
			TestEqual(TEXT("codex body=mcp_servers"), Codex.GetBodyPath(), FString(TEXT("mcp_servers")));

			FUnityAiConfigurator UnityAi;
			TestTrue(TEXT("unity-ai path"), UnityAi.GetConfigFilePath(Root).Replace(TEXT("\\"), TEXT("/")).EndsWith(TEXT("UserSettings/mcp.json")));
		});

		It("global agents resolve a non-empty machine-global path that ignores the project root", [this]()
		{
			FClaudeDesktopConfigurator ClaudeDesktop;
			const FString P1 = ClaudeDesktop.GetConfigFilePath(TEXT("C:/proj-a"));
			const FString P2 = ClaudeDesktop.GetConfigFilePath(TEXT("C:/proj-b"));
			TestTrue(TEXT("claude-desktop non-empty"), !P1.IsEmpty());
			TestEqual(TEXT("claude-desktop path is project-independent"), P1, P2);
			TestTrue(TEXT("claude-desktop ends with claude_desktop_config.json"), P1.Replace(TEXT("\\"), TEXT("/")).EndsWith(TEXT("Claude/claude_desktop_config.json")));

			FClineConfigurator Cline;
			const FString C1 = Cline.GetConfigFilePath(TEXT("C:/proj-a"));
			const FString C2 = Cline.GetConfigFilePath(TEXT("C:/proj-b"));
			TestTrue(TEXT("cline non-empty"), !C1.IsEmpty());
			TestEqual(TEXT("cline path is project-independent"), C1, C2);
			TestTrue(TEXT("cline ends with cline_mcp_settings.json"), C1.Replace(TEXT("\\"), TEXT("/")).EndsWith(TEXT("cline_mcp_settings.json")));

			FAntigravityConfigurator Antigravity;
			const FString A1 = Antigravity.GetConfigFilePath(TEXT("C:/proj-a"));
			TestTrue(TEXT("antigravity non-empty"), !A1.IsEmpty());
			TestTrue(TEXT("antigravity ends with .gemini/config/mcp_config.json"), A1.Replace(TEXT("\\"), TEXT("/")).EndsWith(TEXT(".gemini/config/mcp_config.json")));
		});
	});

	Describe("Per-agent config-shape quirks", [this]()
	{
		It("VS Code Copilot nests servers under the 'servers' body key", [this]()
		{
			FVisualStudioCodeCopilotConfigurator Configurator;
			Configurator.Initialize(MakeAuthConnection(), FPaths::ProjectDir());
			const FString Http = Configurator.GetConfigHttp().GetExpectedFileContent();
			TestTrue(TEXT("has \"servers\" body key"), Http.Contains(TEXT("\"servers\"")));
			TestFalse(TEXT("no mcpServers body key"), Http.Contains(TEXT("\"mcpServers\"")));
		});

		It("Rider/Kilo/Zoo carry the enabled/disabled flags they require", [this]()
		{
			FRiderConfigurator Rider;
			Rider.Initialize(MakeAuthConnection(), FPaths::ProjectDir());
			TestTrue(TEXT("rider stdio enabled:true"), Rider.GetConfigStdio().GetExpectedFileContent().Contains(TEXT("\"enabled\": true")));

			FKiloCodeConfigurator Kilo;
			Kilo.Initialize(MakeAuthConnection(), FPaths::ProjectDir());
			TestTrue(TEXT("kilo stdio disabled:false"), Kilo.GetConfigStdio().GetExpectedFileContent().Contains(TEXT("\"disabled\": false")));
			TestTrue(TEXT("kilo http streamable-http"), Kilo.GetConfigHttp().GetExpectedFileContent().Contains(TEXT("streamable-http")));

			FZooCodeConfigurator Zoo;
			Zoo.Initialize(MakeAuthConnection(), FPaths::ProjectDir());
			TestTrue(TEXT("zoo http streamable-http"), Zoo.GetConfigHttp().GetExpectedFileContent().Contains(TEXT("streamable-http")));
		});

		It("Cline uses the streamableHttp transport type", [this]()
		{
			FClineConfigurator Cline;
			Cline.Initialize(MakeAuthConnection(), FPaths::ProjectDir());
			TestTrue(TEXT("cline http streamableHttp"), Cline.GetConfigHttp().GetExpectedFileContent().Contains(TEXT("streamableHttp")));
		});

		It("GitHub Copilot CLI drops stdio 'type' and adds a tools allowlist", [this]()
		{
			FGitHubCopilotCliConfigurator Copilot;
			Copilot.Initialize(MakeAuthConnection(), FPaths::ProjectDir());
			const FString Stdio = Copilot.GetConfigStdio().GetExpectedFileContent();
			TestFalse(TEXT("no \"type\" key in stdio"), Stdio.Contains(TEXT("\"type\"")));
			TestTrue(TEXT("has tools allowlist"), Stdio.Contains(TEXT("\"tools\"")) && Stdio.Contains(TEXT("\"*\"")));
		});

		It("Antigravity stores the HTTP url under serverUrl (not url)", [this]()
		{
			FAntigravityConfigurator Antigravity;
			Antigravity.Initialize(MakeAuthConnection(), FPaths::ProjectDir());
			const FString Http = Antigravity.GetConfigHttp().GetExpectedFileContent();
			TestTrue(TEXT("has serverUrl"), Http.Contains(TEXT("\"serverUrl\"")));
			TestTrue(TEXT("auth header present"), Http.Contains(TEXT("Bearer secret-token")));
		});

		It("Open Code packs binary + args into a single command array (stdio) and uses remote (http)", [this]()
		{
			FOpenCodeConfigurator OpenCode;
			OpenCode.Initialize(MakeAuthConnection(), FPaths::ProjectDir());
			const FString Stdio = OpenCode.GetConfigStdio().GetExpectedFileContent();
			TestTrue(TEXT("type local"), Stdio.Contains(TEXT("\"local\"")));
			TestTrue(TEXT("command array has binary"), Stdio.Contains(TEXT("gamedev-mcp-server.exe")));
			TestTrue(TEXT("command array has port arg"), Stdio.Contains(TEXT("port=31000")));
			TestFalse(TEXT("no separate args key"), Stdio.Contains(TEXT("\"args\"")));

			const FString Http = OpenCode.GetConfigHttp().GetExpectedFileContent();
			TestTrue(TEXT("type remote"), Http.Contains(TEXT("\"remote\"")));
		});

		It("Custom is snippet-only: still renders a snippet but never writes/detects a file", [this]()
		{
			FCustomConfigurator Custom;
			Custom.Initialize(MakeAuthConnection(), FPaths::ProjectDir());
			const FString Snippet = Custom.GetConfigHttp().GetExpectedFileContent();
			TestTrue(TEXT("snippet not empty"), !Snippet.IsEmpty());
			TestFalse(TEXT("Configure is a no-op (false)"), Custom.Configure(/*bStdio*/ false));
			TestFalse(TEXT("never detected"), Custom.IsAnyDetected());
		});
	});

	Describe("Codex TOML configurator (token discipline)", [this]()
	{
		It("STDIO emits a TOML section with command/args and never writes the raw token", [this]()
		{
			FCodexConfigurator Codex;
			Codex.Initialize(MakeAuthConnection(), FPaths::ProjectDir());
			const FString Stdio = Codex.GetConfigStdio().GetExpectedFileContent();
			TestTrue(TEXT("dotted-table header"), Stdio.Contains(TEXT("[mcp_servers.unreal-mcp]")));
			TestTrue(TEXT("enabled true"), Stdio.Contains(TEXT("enabled = true")));
			TestTrue(TEXT("command present"), Stdio.Contains(TEXT("command = ")));
			TestTrue(TEXT("args present"), Stdio.Contains(TEXT("args = [")));
			TestFalse(TEXT("raw token NEVER in toml"), Stdio.Contains(TEXT("secret-token")));
			TestTrue(TEXT("bearer env-var name (not value)"), Stdio.Contains(TEXT("bearer_token_env_var")) && Stdio.Contains(TEXT("GAME_DEV_AUTH_TOKEN")));
		});

		It("HTTP emits a TOML url + timeouts and never writes the raw token", [this]()
		{
			FCodexConfigurator Codex;
			Codex.Initialize(MakeAuthConnection(), FPaths::ProjectDir());
			const FString Http = Codex.GetConfigHttp().GetExpectedFileContent();
			TestTrue(TEXT("url present"), Http.Contains(TEXT("url = \"http://localhost:9000/mcp\"")));
			TestTrue(TEXT("tool_timeout_sec present"), Http.Contains(TEXT("tool_timeout_sec = 300")));
			TestFalse(TEXT("raw token NEVER in toml http"), Http.Contains(TEXT("secret-token")));
		});

		It("omits the bearer env-var indirection when auth is not required", [this]()
		{
			FUnrealMcpConfig Config;
			Config.ConnectionMode = EUnrealMcpConnectionMode::Custom;
			Config.CustomHost = TEXT("http://localhost:9000");
			Config.AuthOption = EUnrealMcpAuthOption::None;
			const FAiAgentConnectionInfo Info = FAiAgentConnectionInfo::FromPluginConfig(Config, TEXT("C:/srv/gamedev-mcp-server.exe"), 31000);

			FCodexConfigurator Codex;
			Codex.Initialize(Info, FPaths::ProjectDir());
			TestFalse(TEXT("no bearer env-var when auth none"), Codex.GetConfigStdio().GetExpectedFileContent().Contains(TEXT("bearer_token_env_var")));
		});
	});

	Describe("TOML read-merge-write", [this]()
	{
		It("creates a fresh TOML file (with parent dirs) when none exists", [this]()
		{
			const FString Path = MakeTempTomlPath();
			TSharedRef<FTomlAiAgentConfig> Config = MakeTomlHttpConfig(Path, TEXT("http://localhost:8080/mcp"));

			TestTrue(TEXT("Configure succeeds"), Config->Configure());
			TestTrue(TEXT("file exists"), FPaths::FileExists(Path));
			const FString Content = ReadAllText(Path);
			TestTrue(TEXT("has section"), Content.Contains(TEXT("[mcp_servers.unreal-mcp]")));
			TestTrue(TEXT("IsConfigured after write"), Config->IsConfigured());

			DeleteTempDirFor(Path);
		});

		It("preserves a sibling section and unrelated lines on merge", [this]()
		{
			const FString Path = MakeTempTomlPath();
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			const FString Existing = TEXT("# top comment\n[mcp_servers.other]\nurl = \"http://elsewhere/mcp\"\n");
			FFileHelper::SaveStringToFile(Existing, *Path);

			TSharedRef<FTomlAiAgentConfig> Config = MakeTomlHttpConfig(Path, TEXT("http://localhost:8080/mcp"));
			TestTrue(TEXT("Configure succeeds"), Config->Configure());

			const FString Content = ReadAllText(Path);
			TestTrue(TEXT("comment preserved"), Content.Contains(TEXT("# top comment")));
			TestTrue(TEXT("sibling preserved"), Content.Contains(TEXT("[mcp_servers.other]")));
			TestTrue(TEXT("our section added"), Content.Contains(TEXT("[mcp_servers.unreal-mcp]")));

			DeleteTempDirFor(Path);
		});

		It("tolerates an invalid/garbage TOML file by appending a clean section", [this]()
		{
			const FString Path = MakeTempTomlPath();
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			FFileHelper::SaveStringToFile(FString(TEXT("not = toml = at = all")), *Path);

			TSharedRef<FTomlAiAgentConfig> Config = MakeTomlHttpConfig(Path, TEXT("http://localhost:8080/mcp"));
			TestTrue(TEXT("Configure succeeds over garbage"), Config->Configure());
			TestTrue(TEXT("IsConfigured after append"), Config->IsConfigured());

			DeleteTempDirFor(Path);
		});

		It("IsDetected false for a missing file, true once configured; Remove reverts it", [this]()
		{
			const FString Path = MakeTempTomlPath();
			TSharedRef<FTomlAiAgentConfig> Config = MakeTomlHttpConfig(Path, TEXT("http://localhost:8080/mcp"));
			TestFalse(TEXT("not detected before write"), Config->IsDetected());
			Config->Configure();
			TestTrue(TEXT("detected after write"), Config->IsDetected());
			TestTrue(TEXT("Remove returns true"), Config->Remove());
			TestFalse(TEXT("not detected after remove"), Config->IsDetected());

			DeleteTempDirFor(Path);
		});

		It("Remove deletes our section but preserves siblings", [this]()
		{
			const FString Path = MakeTempTomlPath();
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			const FString Existing = TEXT("[mcp_servers.other]\nurl = \"http://elsewhere/mcp\"\n");
			FFileHelper::SaveStringToFile(Existing, *Path);

			TSharedRef<FTomlAiAgentConfig> Config = MakeTomlHttpConfig(Path, TEXT("http://localhost:8080/mcp"));
			Config->Configure();
			TestTrue(TEXT("Remove returns true"), Config->Remove());

			const FString Content = ReadAllText(Path);
			TestFalse(TEXT("our section removed"), Content.Contains(TEXT("[mcp_servers.unreal-mcp]")));
			TestTrue(TEXT("sibling preserved"), Content.Contains(TEXT("[mcp_servers.other]")));

			DeleteTempDirFor(Path);
		});

		It("migrates a deprecated TOML section name to the canonical one", [this]()
		{
			const FString Path = MakeTempTomlPath();
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			const FString Existing = TEXT("[mcp_servers.ai-game-developer]\nurl = \"http://old/mcp\"\n");
			FFileHelper::SaveStringToFile(Existing, *Path);

			TSharedRef<FTomlAiAgentConfig> Config = MakeTomlHttpConfig(Path, TEXT("http://localhost:8080/mcp"));
			Config->Configure();

			const FString Content = ReadAllText(Path);
			TestFalse(TEXT("deprecated section removed"), Content.Contains(TEXT("[mcp_servers.ai-game-developer]")));
			TestTrue(TEXT("canonical section present"), Content.Contains(TEXT("[mcp_servers.unreal-mcp]")));

			DeleteTempDirFor(Path);
		});

		It("collapses a duplicate sibling section that shares our url (identity-key dedup)", [this]()
		{
			const FString Path = MakeTempTomlPath();
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			const FString Existing = TEXT("[mcp_servers.my-alias]\nurl = \"http://localhost:8080/mcp\"\n");
			FFileHelper::SaveStringToFile(Existing, *Path);

			TSharedRef<FTomlAiAgentConfig> Config = MakeTomlHttpConfig(Path, TEXT("http://localhost:8080/mcp"));
			Config->Configure();

			const FString Content = ReadAllText(Path);
			TestFalse(TEXT("duplicate alias removed"), Content.Contains(TEXT("[mcp_servers.my-alias]")));
			TestTrue(TEXT("canonical section present"), Content.Contains(TEXT("[mcp_servers.unreal-mcp]")));

			DeleteTempDirFor(Path);
		});

		It("IsConfigured tolerates URL trailing-slash differences", [this]()
		{
			const FString Path = MakeTempTomlPath();
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			const FString Existing = TEXT("[mcp_servers.unreal-mcp]\nenabled = true\nurl = \"http://localhost:8080/mcp/\"\n");
			FFileHelper::SaveStringToFile(Existing, *Path);

			TSharedRef<FTomlAiAgentConfig> Config = MakeTomlHttpConfig(Path, TEXT("http://localhost:8080/mcp"));
			TestTrue(TEXT("trailing slash treated as configured"), Config->IsConfigured());

			DeleteTempDirFor(Path);
		});

		It("switches an existing HTTP section to STDIO cleanly (drops url, adds command/args)", [this]()
		{
			const FString Path = MakeTempTomlPath();
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			const FString Existing = TEXT("[mcp_servers.unreal-mcp]\nenabled = true\nurl = \"http://localhost:8080/mcp\"\n");
			FFileHelper::SaveStringToFile(Existing, *Path);

			TSharedRef<FTomlAiAgentConfig> Config = MakeTomlStdioConfig(Path, TEXT("C:/srv/gamedev-mcp-server.exe"));
			TestTrue(TEXT("STDIO Configure over HTTP succeeds"), Config->Configure());

			const FString Content = ReadAllText(Path);
			TestFalse(TEXT("stale url removed"), Content.Contains(TEXT("url = ")));
			TestTrue(TEXT("command present"), Content.Contains(TEXT("command = ")));
			TestTrue(TEXT("args present"), Content.Contains(TEXT("args = [")));
			TestTrue(TEXT("IsConfigured after switch"), Config->IsConfigured());

			DeleteTempDirFor(Path);
		});

		It("IsConfigured is false when a to-remove key still survives in the section", [this]()
		{
			const FString Path = MakeTempTomlPath();
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			// Matches our desired STDIO props but still carries a stale `url` — the STDIO config asks to remove it.
			const FString Existing = TEXT("[mcp_servers.unreal-mcp]\nenabled = true\ncommand = \"C:/srv/gamedev-mcp-server.exe\"\nargs = [\"port=8080\",\"client-transport=stdio\"]\nurl = \"http://localhost:8080/mcp\"\n");
			FFileHelper::SaveStringToFile(Existing, *Path);

			TSharedRef<FTomlAiAgentConfig> Config = MakeTomlStdioConfig(Path, TEXT("C:/srv/gamedev-mcp-server.exe"));
			TestFalse(TEXT("surviving to-remove key vetoes IsConfigured"), Config->IsConfigured());

			DeleteTempDirFor(Path);
		});

		It("absorbs a nested dotted sub-table into our section so [mcp_servers.unreal-mcp.env] round-trips on merge", [this]()
		{
			const FString Path = MakeTempTomlPath();
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			// Our section already carries a nested env sub-table; a sibling section follows it. The merge must keep the
			// nested sub-table glued to our section (not orphan/flatten it) AND must not swallow the sibling.
			const FString Existing = TEXT("[mcp_servers.unreal-mcp]\nenabled = true\nurl = \"http://localhost:8080/mcp\"\n[mcp_servers.unreal-mcp.env]\nGAME_DEV_AUTH_TOKEN = \"placeholder\"\n[mcp_servers.other]\nurl = \"http://elsewhere/mcp\"\n");
			FFileHelper::SaveStringToFile(Existing, *Path);

			TSharedRef<FTomlAiAgentConfig> Config = MakeTomlHttpConfig(Path, TEXT("http://localhost:8080/mcp"));
			TestTrue(TEXT("Configure succeeds"), Config->Configure());

			const FString Content = ReadAllText(Path);
			TestTrue(TEXT("our section present"), Content.Contains(TEXT("[mcp_servers.unreal-mcp]")));
			TestTrue(TEXT("nested env sub-table survives"), Content.Contains(TEXT("[mcp_servers.unreal-mcp.env]")));
			TestTrue(TEXT("nested env key survives"), Content.Contains(TEXT("GAME_DEV_AUTH_TOKEN = \"placeholder\"")));
			TestTrue(TEXT("sibling section preserved"), Content.Contains(TEXT("[mcp_servers.other]")));
			TestTrue(TEXT("sibling url preserved"), Content.Contains(TEXT("http://elsewhere/mcp")));
			// The nested env key must NOT have been flattened up into our flat section as a scalar line.
			TestTrue(TEXT("env key only inside its sub-table"), Content.Find(TEXT("GAME_DEV_AUTH_TOKEN")) > Content.Find(TEXT("[mcp_servers.unreal-mcp.env]")));

			DeleteTempDirFor(Path);
		});

		It("does NOT clobber an existing-but-unreadable file (read failure must not take the clean-write path)", [this]()
		{
			const FString Path = MakeTempTomlPath();
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			const FString Existing = TEXT("[mcp_servers.other]\nurl = \"http://elsewhere/mcp\"\n");
			FFileHelper::SaveStringToFile(Existing, *Path);

			// Hold an EXCLUSIVE write handle (bAllowRead = false) so the config's LoadFileToStringArray fails while
			// the file still EXISTS — the exact missing-vs-unreadable distinction the guard protects. On platforms
			// whose filesystem grants the read anyway (the exclusive open is advisory), the lock acquire is skipped
			// and the spec degrades to asserting the merge path simply preserves the sibling.
			IFileHandle* Locked = FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*Path, /*bAppend*/ true, /*bAllowRead*/ false);
			TestNotNull(TEXT("write handle opened"), Locked);

			TArray<FString> Probe;
			const bool bUnreadableWhileLocked = !FFileHelper::LoadFileToStringArray(Probe, *Path);

			TSharedRef<FTomlAiAgentConfig> Config = MakeTomlHttpConfig(Path, TEXT("http://localhost:8080/mcp"));
			const bool bConfigured = Config->Configure();

			if (Locked)
				delete Locked; // releases the exclusive lock

			if (bUnreadableWhileLocked)
			{
				// The file existed but could not be read → Configure must refuse rather than clobber.
				TestFalse(TEXT("Configure refuses to clobber an unreadable existing file"), bConfigured);
				const FString After = ReadAllText(Path);
				TestTrue(TEXT("original sibling content untouched"), After.Contains(TEXT("[mcp_servers.other]")));
				TestFalse(TEXT("our section was NOT written over the locked file"), After.Contains(TEXT("[mcp_servers.unreal-mcp]")));
			}
			else
			{
				// Platform allowed the read despite the lock → this isn't the read-failure path; just assert the
				// normal merge preserved the sibling so the spec still has a meaningful invariant.
				const FString After = ReadAllText(Path);
				TestTrue(TEXT("sibling preserved on readable merge"), After.Contains(TEXT("[mcp_servers.other]")));
			}

			DeleteTempDirFor(Path);
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

	Describe("Per-agent rich content (§7 templates — issue #59)", [this]()
	{
		It("Claude Code emits Start / Manual / Troubleshooting foldouts with transport-specific commands", [this]()
		{
			FClaudeCodeConfigurator Agent;
			FUnrealMcpConfig Config;
			Config.ConnectionMode = EUnrealMcpConnectionMode::Custom;
			Config.AuthOption = EUnrealMcpAuthOption::Required;
			Config.CustomToken = TEXT("rich-token-xyz");
			const FAiAgentConnectionInfo Conn = FAiAgentConnectionInfo::FromPluginConfig(Config, TEXT("C:/srv/gamedev-mcp-server.exe"), 5123);
			Agent.Initialize(Conn, TEXT("C:/proj"));

			// STDIO rich content: a `claude mcp add ... client-transport=stdio` command with the bearer.
			const TArray<FAiAgentRichContentSection> Stdio = Agent.BuildRichContent(/*bStdio*/ true);
			TestEqual(TEXT("three stdio foldouts"), Stdio.Num(), 3);
			TestEqual(TEXT("first foldout is Start"), Stdio[0].Heading, FString(TEXT("Start")));
			TestTrue(TEXT("Start opens first"), Stdio[0].bExpandedFirst);

			FString StdioCommand;
			for (const FAiAgentRichContentItem& Item : Stdio[1].Items)
				if (Item.Kind == FAiAgentRichContentItem::EKind::ReadOnlyField && Item.Text.Contains(TEXT("claude mcp add")))
					StdioCommand = Item.Text;
			TestTrue(TEXT("stdio command present"), StdioCommand.Contains(TEXT("client-transport=stdio")));
			TestTrue(TEXT("stdio command carries the token when auth required"), StdioCommand.Contains(TEXT("rich-token-xyz")));

			// HTTP rich content: a `claude mcp add --transport http` command with the Bearer header.
			const TArray<FAiAgentRichContentSection> Http = Agent.BuildRichContent(/*bStdio*/ false);
			FString HttpCommand;
			for (const FAiAgentRichContentItem& Item : Http[1].Items)
				if (Item.Kind == FAiAgentRichContentItem::EKind::ReadOnlyField && Item.Text.Contains(TEXT("claude mcp add")))
					HttpCommand = Item.Text;
			TestTrue(TEXT("http command uses --transport http"), HttpCommand.Contains(TEXT("--transport http")));
			TestTrue(TEXT("http command carries the Bearer header"), HttpCommand.Contains(TEXT("Authorization: Bearer rich-token-xyz")));
		});

		It("Claude Code omits the bearer from commands when auth is not required", [this]()
		{
			FClaudeCodeConfigurator Agent;
			FUnrealMcpConfig Config;
			Config.ConnectionMode = EUnrealMcpConnectionMode::Custom;
			Config.AuthOption = EUnrealMcpAuthOption::None;
			Config.CustomToken = TEXT("should-not-appear");
			const FAiAgentConnectionInfo Conn = FAiAgentConnectionInfo::FromPluginConfig(Config, TEXT("C:/srv/server.exe"), 5123);
			Agent.Initialize(Conn, TEXT("C:/proj"));

			for (const FAiAgentRichContentSection& Section : Agent.BuildRichContent(/*bStdio*/ true))
				for (const FAiAgentRichContentItem& Item : Section.Items)
					TestFalse(TEXT("no token leaks when auth is None"), Item.Text.Contains(TEXT("should-not-appear")));
		});

		It("the generic base configurator supplies a Configuration-details foldout for plain agents", [this]()
		{
			FCursorConfigurator Agent; // Cursor uses the base BuildRichContent (no override).
			FUnrealMcpConfig Config;
			Config.ConnectionMode = EUnrealMcpConnectionMode::Custom;
			const FAiAgentConnectionInfo Conn = FAiAgentConnectionInfo::FromPluginConfig(Config, TEXT("C:/srv/server.exe"), 5123);
			Agent.Initialize(Conn, TEXT("C:/proj"));

			const TArray<FAiAgentRichContentSection> Sections = Agent.BuildRichContent(/*bStdio*/ false);
			TestEqual(TEXT("one generic section"), Sections.Num(), 1);
			TestEqual(TEXT("generic heading"), Sections[0].Heading, FString(TEXT("Configuration details")));
			TestTrue(TEXT("generic section has items"), Sections[0].Items.Num() > 0);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
