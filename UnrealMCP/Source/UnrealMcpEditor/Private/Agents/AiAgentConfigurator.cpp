// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Agents/AiAgentConfigurator.h"
#include "Agents/JsonAiAgentConfig.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"

FAiAgentConnectionInfo FAiAgentConnectionInfo::FromPluginConfig(const FUnrealMcpConfig& Config, const FString& InServerPath, int32 InPort)
{
	FAiAgentConnectionInfo Info;
	Info.ServerPath = InServerPath;
	Info.Port = InPort;

	const bool bCloud = Config.ConnectionMode == EUnrealMcpConnectionMode::Cloud;

	// The MCP-client URL is the effective host + "/mcp" (mirrors the cli's `${url}/mcp`). Cloud uses the cloud
	// base URL; Custom uses the user's host. ResolveCloudBaseUrl/ResolveCustomHost already trim a trailing slash.
	const FString BaseUrl = bCloud ? Config.ResolveCloudBaseUrl() : Config.ResolveCustomHost();
	Info.HttpUrl = BaseUrl + TEXT("/mcp");

	// Auth: Cloud always requires it (the cloud enforces it); Custom follows AuthOption. The token is the
	// mode-resolved effective bearer (Cloud→cloudToken, Custom+Required→token, else empty).
	Info.bAuthRequired = bCloud || Config.AuthOption == EUnrealMcpAuthOption::Required;
	Info.Token = Config.ResolveEffectiveToken();

	return Info;
}

bool FAiAgentConfigurator::SupportsSkills(const FString& InProjectRoot) const
{
	return !GetSkillsPath(InProjectRoot).IsEmpty();
}

FString FAiAgentConfigurator::ResolveAbsoluteSkillsPath(const FString& InProjectRoot) const
{
	const FString Folder = GetSkillsPath(InProjectRoot);
	if (Folder.IsEmpty())
		return FString();

	// Already-absolute path: normalize separators only. Otherwise resolve under the project root. Forward
	// slashes throughout so the panel display and the writer agree regardless of host path conventions.
	const FString Absolute = FPaths::IsRelative(Folder)
		? FPaths::ConvertRelativePathToFull(FPaths::Combine(InProjectRoot, Folder))
		: FPaths::ConvertRelativePathToFull(Folder);
	return Absolute.Replace(TEXT("\\"), TEXT("/"));
}

void FAiAgentConfigurator::Initialize(const FAiAgentConnectionInfo& InConnection, const FString& InProjectRoot)
{
	Connection = InConnection;
	ProjectRoot = InProjectRoot;
	Invalidate();
}

void FAiAgentConfigurator::Invalidate()
{
	ConfigStdio.Reset();
	ConfigHttp.Reset();
}

FAiAgentConfig& FAiAgentConfigurator::GetConfigStdio()
{
	if (!ConfigStdio.IsValid())
		ConfigStdio = BuildStdio();
	return *ConfigStdio;
}

FAiAgentConfig& FAiAgentConfigurator::GetConfigHttp()
{
	if (!ConfigHttp.IsValid())
		ConfigHttp = BuildHttp();
	return *ConfigHttp;
}

FString FAiAgentConfigurator::GetStdioCommand() const
{
	return Connection.ServerPath.Replace(TEXT("\\"), TEXT("/"));
}

TArray<FString> FAiAgentConfigurator::GetStdioArgs() const
{
	// The shared GameDev-MCP-Server CLI contract (identical to the cli's buildServerEntry args, sans plugin-timeout
	// which the Unreal cli does not emit). The token arg is empty unless auth is required.
	TArray<FString> Args;
	Args.Add(FString::Printf(TEXT("port=%d"), Connection.Port));
	Args.Add(TEXT("client-transport=stdio"));
	Args.Add(FString::Printf(TEXT("authorization=%s"), Connection.bAuthRequired ? TEXT("required") : TEXT("none")));
	Args.Add(FString::Printf(TEXT("token=%s"), Connection.bAuthRequired ? *Connection.Token : TEXT("")));
	return Args;
}

TSharedRef<FAiAgentConfig> FAiAgentConfigurator::BuildStdio() const
{
	// STDIO form (the shared GameDev-MCP-Server CLI contract, identical to the cli's buildServerEntry):
	//   { "type": "stdio", "command": <serverPath>, "args": ["port=N", "client-transport=stdio",
	//     "authorization=required|none", "token=<token>"] }
	// command is an identity key (ValueComparison::Path-equivalent → Exact is fine since we always emit the
	// same normalized path). The HTTP-only keys (type=http url headers) are explicitly removed so a transport
	// switch in the same file leaves no stale HTTP entry.
	const FString Command = GetStdioCommand();

	TArray<TSharedPtr<FJsonValue>> Args;
	for (const FString& Arg : GetStdioArgs())
		Args.Add(MakeShared<FJsonValueString>(Arg));

	TSharedRef<FJsonAiAgentConfig> Config = MakeShared<FJsonAiAgentConfig>(
		GetAgentName(),
		GetConfigFilePath(ProjectRoot),
		GetBodyPath());

	Config->SetStringProperty(TEXT("type"), TEXT("stdio"), /*bRequired*/ true);
	Config->SetStringProperty(TEXT("command"), Command, /*bRequired*/ true);
	Config->SetProperty(TEXT("args"), MakeShared<FJsonValueArray>(Args), /*bRequired*/ true);
	// Drop the HTTP-only keys so a stdio write over a prior http entry is clean.
	Config->SetPropertyToRemove(TEXT("url"));
	Config->SetPropertyToRemove(TEXT("headers"));

	// Per-agent additions (e.g. Rider/KiloCode/ZooCode `disabled`, GitHubCopilotCli `tools`).
	CustomizeStdio(*Config);

	return Config;
}

TSharedRef<FAiAgentConfig> FAiAgentConfigurator::BuildHttp() const
{
	// HTTP form (identical to the cli's buildServerEntry):
	//   { "type": "http", "url": "<host>/mcp", "headers": { "Authorization": "Bearer <token>" }? }
	// url is compared with URL semantics (trailing-slash / case tolerant). The header is added ONLY when auth is
	// required AND a token exists; otherwise it is explicitly removed so toggling auth off scrubs a stale header.
	TSharedRef<FJsonAiAgentConfig> Config = MakeShared<FJsonAiAgentConfig>(
		GetAgentName(),
		GetConfigFilePath(ProjectRoot),
		GetBodyPath());

	Config->SetStringProperty(TEXT("type"), TEXT("http"), /*bRequired*/ true);
	Config->SetStringProperty(TEXT("url"), Connection.HttpUrl, /*bRequired*/ true, EUnrealMcpValueComparison::Url);

	if (Connection.bAuthRequired && !Connection.Token.IsEmpty())
	{
		TSharedPtr<FJsonObject> Headers = MakeShared<FJsonObject>();
		Headers->SetStringField(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Connection.Token));
		Config->SetProperty(TEXT("headers"), MakeShared<FJsonValueObject>(Headers), /*bRequired*/ true);
	}
	else
	{
		Config->SetPropertyToRemove(TEXT("headers"));
	}

	// Drop the STDIO-only keys so an http write over a prior stdio entry is clean.
	Config->SetPropertyToRemove(TEXT("command"));
	Config->SetPropertyToRemove(TEXT("args"));

	// Per-agent additions (e.g. Rider/KiloCode/ZooCode `disabled`).
	CustomizeHttp(*Config);

	return Config;
}

bool FAiAgentConfigurator::IsAnyDetected()
{
	return GetConfigStdio().IsDetected() || GetConfigHttp().IsDetected();
}

bool FAiAgentConfigurator::IsConfigured(bool bStdio)
{
	return bStdio ? GetConfigStdio().IsConfigured() : GetConfigHttp().IsConfigured();
}

bool FAiAgentConfigurator::Configure(bool bStdio)
{
	const bool bResult = bStdio ? GetConfigStdio().Configure() : GetConfigHttp().Configure();
	// A write changes IsConfigured/IsDetected for BOTH cached configs (same file); rebuild on next access.
	Invalidate();
	return bResult;
}

TArray<FAiAgentRichContentSection> FAiAgentConfigurator::BuildRichContent(bool bStdio) const
{
	// Generic default (issue #59): a single "Configuration details" foldout so every built-in agent shows
	// richer-than-bare content even without a bespoke override. Describes which transport the config targets, the
	// resolved endpoint (HTTP url or the stdio server command + port), and whether a bearer is included. The token
	// is NEVER inlined here — the snippet preview is the single place a (masked/revealed) token surfaces (§8).
	using FItem = FAiAgentRichContentItem;

	FAiAgentRichContentSection Section;
	Section.Heading = TEXT("Configuration details");
	Section.bExpandedFirst = true;

	if (bStdio)
	{
		Section.Items.Add(FItem::Description(TEXT("This agent launches the MCP server over stdio. The config writes the server command and arguments below.")));
		const FString Command = Connection.ServerPath.IsEmpty()
			? TEXT("<gamedev-mcp-server>")
			: Connection.ServerPath.Replace(TEXT("\\"), TEXT("/"));
		Section.Items.Add(FItem::ReadOnlyField(FString::Printf(TEXT("%s port=%d client-transport=stdio"), *Command, Connection.Port)));
		if (Connection.ServerPath.IsEmpty())
			Section.Items.Add(FItem::Warning(TEXT("The local MCP server binary is not present yet; the snippet is still a valid template and will resolve once the server is installed.")));
	}
	else
	{
		Section.Items.Add(FItem::Description(TEXT("This agent connects to a running MCP server over HTTP at the URL below.")));
		Section.Items.Add(FItem::ReadOnlyField(Connection.HttpUrl));
	}

	if (Connection.bAuthRequired)
		Section.Items.Add(FItem::Description(TEXT("Authorization is required — a bearer token is included in the snippet (masked above unless revealed).")));
	else
		Section.Items.Add(FItem::Description(TEXT("Authorization is not required — no bearer token is sent.")));

	return { MoveTemp(Section) };
}

bool FAiAgentConfigurator::RemoveAll()
{
	// Remove via either transport's config — both share the same file/body/identity, and Remove() clears the
	// canonical entry + deprecated + duplicate aliases regardless of which transport object calls it.
	const bool bResult = GetConfigStdio().Remove();
	Invalidate();
	return bResult;
}
