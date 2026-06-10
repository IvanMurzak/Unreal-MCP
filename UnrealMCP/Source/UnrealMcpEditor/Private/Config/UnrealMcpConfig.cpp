// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpConfig.h"

#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformMisc.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// --- Env-var names (§8). ---
const TCHAR* FUnrealMcpConfig::EnvConnectionMode = TEXT("UNREAL_MCP_CONNECTION_MODE");
const TCHAR* FUnrealMcpConfig::EnvHost           = TEXT("UNREAL_MCP_HOST");
const TCHAR* FUnrealMcpConfig::EnvCloudUrl       = TEXT("UNREAL_MCP_CLOUD_URL");
const TCHAR* FUnrealMcpConfig::EnvToken          = TEXT("UNREAL_MCP_TOKEN");
const TCHAR* FUnrealMcpConfig::EnvAuthOption     = TEXT("UNREAL_MCP_AUTH_OPTION");
const TCHAR* FUnrealMcpConfig::EnvKeepConnected  = TEXT("UNREAL_MCP_KEEP_CONNECTED");
const TCHAR* FUnrealMcpConfig::EnvTools          = TEXT("UNREAL_MCP_TOOLS");
const TCHAR* FUnrealMcpConfig::EnvStartServer    = TEXT("UNREAL_MCP_START_SERVER");
const TCHAR* FUnrealMcpConfig::EnvTransport      = TEXT("UNREAL_MCP_TRANSPORT");
const TCHAR* FUnrealMcpConfig::EnvLogLevel       = TEXT("UNREAL_MCP_LOG_LEVEL");
const TCHAR* FUnrealMcpConfig::EnvBridgePath     = TEXT("UNREAL_MCP_BRIDGE_PATH");

// --- Defaults. ---
const TCHAR* FUnrealMcpConfig::DefaultCloudBaseUrl = TEXT("https://ai-game.dev");
const TCHAR* FUnrealMcpConfig::DefaultCustomHost   = TEXT("http://localhost:8080");
const TCHAR* FUnrealMcpConfig::DefaultLogLevel     = TEXT("Info");
const TCHAR* FUnrealMcpConfig::DefaultTransport    = TEXT("http");

namespace
{
	// JSON property names (camelCase — System.Text.Json-compatible so the sidecar/cli could share parsing).
	const TCHAR* KeyConnectionMode = TEXT("connectionMode");
	const TCHAR* KeyHost           = TEXT("host");
	const TCHAR* KeyToken          = TEXT("token");
	const TCHAR* KeyCloudToken     = TEXT("cloudToken");
	const TCHAR* KeyCloudUrl       = TEXT("cloudUrl");
	const TCHAR* KeyAuthOption     = TEXT("authOption");
	const TCHAR* KeyKeepConnected  = TEXT("keepConnected");
	const TCHAR* KeyLogLevel       = TEXT("logLevel");
	const TCHAR* KeyTransport      = TEXT("transport");
	const TCHAR* KeyStartServer    = TEXT("startServer");
	const TCHAR* KeyEnabledTools   = TEXT("enabledTools");

	FString TrimSlash(const FString& In)
	{
		FString Out = In;
		while (Out.EndsWith(TEXT("/")))
			Out.LeftChopInline(1);
		return Out;
	}
}

FUnrealMcpConfig::FUnrealMcpConfig()
	: CustomHost(DefaultCustomHost)
	, CloudUrl(DefaultCloudBaseUrl)
	, LogLevel(DefaultLogLevel)
	, Transport(DefaultTransport)
{
}

FString FUnrealMcpConfig::DefaultConfigFilePath()
{
	return FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Config"), TEXT("UnrealMcp"), TEXT("ai-game-developer-config.json")));
}

FString FUnrealMcpConfig::DefaultEnvFilePath()
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT(".env")));
}

FUnrealMcpConfig FUnrealMcpConfig::LoadAndResolve()
{
	return LoadAndResolve(LoadEnvFile(DefaultEnvFilePath()));
}

FUnrealMcpConfig FUnrealMcpConfig::LoadAndResolve(const TMap<FString, FString>& DotEnv)
{
	FUnrealMcpConfig Config;
	Config.LoadFromFile(DefaultConfigFilePath());
	Config.ApplyOverrides(DotEnv, [](const FString& Name, FString& Out) -> bool
	{
		const FString Value = FPlatformMisc::GetEnvironmentVariable(*Name);
		if (Value.IsEmpty())
			return false;
		Out = Value;
		return true;
	});
	return Config;
}

void FUnrealMcpConfig::LoadFromFile(const FString& Path)
{
	TSharedPtr<FJsonObject> Parsed;

	FString Json;
	if (!Path.IsEmpty() && FPaths::FileExists(Path) && FFileHelper::LoadFileToString(Json, *Path) && !Json.IsEmpty())
	{
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Json);
		// A corrupt/partial file is treated as "no persisted config" rather than breaking plugin boot.
		FJsonSerializer::Deserialize(Reader, Parsed);
	}

	LoadFromJson(Parsed);
}

void FUnrealMcpConfig::LoadFromJson(const TSharedPtr<FJsonObject>& Json)
{
	if (Json.IsValid())
	{
		FString Str;
		if (Json->TryGetStringField(KeyConnectionMode, Str))
		{
			EUnrealMcpConnectionMode Mode;
			if (TryParseMode(Str, Mode))
				ConnectionMode = Mode;
		}
		if (Json->TryGetStringField(KeyHost, Str))       CustomHost = Str;
		if (Json->TryGetStringField(KeyToken, Str))      CustomToken = Str;
		if (Json->TryGetStringField(KeyCloudToken, Str)) CloudToken = Str;
		if (Json->TryGetStringField(KeyCloudUrl, Str))   CloudUrl = Str;
		if (Json->TryGetStringField(KeyAuthOption, Str))
		{
			EUnrealMcpAuthOption Option;
			if (TryParseAuthOption(Str, Option))
				AuthOption = Option;
		}
		bool bFlag = false;
		if (Json->TryGetBoolField(KeyKeepConnected, bFlag)) bKeepConnected = bFlag;
		if (Json->TryGetStringField(KeyLogLevel, Str))  LogLevel = Str;
		if (Json->TryGetStringField(KeyTransport, Str)) Transport = Str;
		if (Json->TryGetBoolField(KeyStartServer, bFlag)) bStartServer = bFlag;

		const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
		if (Json->TryGetArrayField(KeyEnabledTools, Tools) && Tools != nullptr)
		{
			EnabledTools.Reset();
			for (const TSharedPtr<FJsonValue>& Value : *Tools)
			{
				FString Tool;
				if (Value.IsValid() && Value->TryGetString(Tool) && !Tool.IsEmpty())
					EnabledTools.Add(Tool);
			}
		}
	}

	// Snapshot the (defaults + disk) baseline AFTER loading — this is what Save() restores for any key an
	// env/.env layer later overrides. ToJson() builds a fresh object, so the baseline is a deep, independent copy.
	OverriddenKeys.Reset();
	DiskBaselineJson = ToJson();
}

void FUnrealMcpConfig::ApplyOverrides(const TMap<FString, FString>& DotEnv, const TFunction<bool(const FString&, FString&)>& EnvReader)
{
	// Effective override value for an env name: process env (highest) else .env file. Both are sanitized;
	// a blank value carries no override (mirrors GodotMcpEnvFile + Unity's env path).
	auto Resolve = [&](const TCHAR* Name, FString& Out) -> bool
	{
		if (EnvReader)
		{
			FString EnvValue;
			if (EnvReader(Name, EnvValue))
			{
				const FString Sanitized = SanitizeValue(EnvValue);
				if (!Sanitized.IsEmpty())
				{
					Out = Sanitized;
					return true;
				}
			}
		}
		if (const FString* FileValue = DotEnv.Find(Name)) // .env values are already sanitized by ParseEnvLines
		{
			if (!FileValue->IsEmpty())
			{
				Out = *FileValue;
				return true;
			}
		}
		return false;
	};

	// Mode must settle BEFORE the token so token routing (Cloud→cloudToken / Custom→token) matches the final
	// mode. The token is therefore applied LAST. (Same ordering as GodotMcpEnvFile.Apply.)
	const TCHAR* OrderedNames[] = {
		EnvConnectionMode, EnvHost, EnvCloudUrl, EnvAuthOption, EnvKeepConnected,
		EnvLogLevel, EnvTransport, EnvStartServer, EnvTools, EnvToken
	};

	for (const TCHAR* Name : OrderedNames)
	{
		FString Value;
		if (Resolve(Name, Value))
			ApplyOne(Name, Value);
	}
}

void FUnrealMcpConfig::ApplyOne(const FString& EnvName, const FString& Source)
{
	if (EnvName == EnvConnectionMode)
	{
		EUnrealMcpConnectionMode Mode;
		if (TryParseMode(Source, Mode))
		{
			ConnectionMode = Mode;
			OverriddenKeys.Add(KeyConnectionMode);
		}
	}
	else if (EnvName == EnvHost)
	{
		CustomHost = Source;
		OverriddenKeys.Add(KeyHost);
	}
	else if (EnvName == EnvCloudUrl)
	{
		CloudUrl = Source;
		OverriddenKeys.Add(KeyCloudUrl);
	}
	else if (EnvName == EnvAuthOption)
	{
		EUnrealMcpAuthOption Option;
		if (TryParseAuthOption(Source, Option))
		{
			AuthOption = Option;
			OverriddenKeys.Add(KeyAuthOption);
		}
	}
	else if (EnvName == EnvKeepConnected)
	{
		bool bValue = false;
		if (TryParseBool(Source, bValue))
		{
			bKeepConnected = bValue;
			OverriddenKeys.Add(KeyKeepConnected);
		}
	}
	else if (EnvName == EnvLogLevel)
	{
		LogLevel = Source;
		OverriddenKeys.Add(KeyLogLevel);
	}
	else if (EnvName == EnvTransport)
	{
		// Only the two named transports are honoured; anything else falls through to the existing value.
		const FString Lower = Source.ToLower();
		if (Lower == TEXT("stdio") || Lower == TEXT("http"))
		{
			Transport = Lower;
			OverriddenKeys.Add(KeyTransport);
		}
	}
	else if (EnvName == EnvStartServer)
	{
		bool bValue = false;
		if (TryParseBool(Source, bValue))
		{
			bStartServer = bValue;
			OverriddenKeys.Add(KeyStartServer);
		}
	}
	else if (EnvName == EnvTools)
	{
		// Accept comma- or semicolon-separated tool ids: normalize ';' → ',' first, then split once so a
		// mixed/single separator can never corrupt an id (e.g. "a," must yield ["a"], not ["a,"]).
		FString Normalized = Source;
		Normalized.ReplaceInline(TEXT(";"), TEXT(","));
		TArray<FString> Parts;
		Normalized.ParseIntoArray(Parts, TEXT(","), true);

		EnabledTools.Reset();
		for (FString& Part : Parts)
		{
			Part.TrimStartAndEndInline();
			if (!Part.IsEmpty())
				EnabledTools.Add(Part);
		}
		OverriddenKeys.Add(KeyEnabledTools);
	}
	else if (EnvName == EnvToken)
	{
		// Route to the active mode's token field (mode has already settled above). Custom-mode token still
		// only goes on the wire when AuthOption==Required (see ResolveEffectiveToken) — storing it here keeps
		// the user's value across an auth toggle without leaking it while auth is None.
		if (ConnectionMode == EUnrealMcpConnectionMode::Cloud)
		{
			CloudToken = Source;
			OverriddenKeys.Add(KeyCloudToken);
		}
		else
		{
			CustomToken = Source;
			OverriddenKeys.Add(KeyToken);
		}
	}
	// EnvBridgePath: dev-only, consumed by FUnrealMcpSidecarManager via the process env — not a config field.
}

TSharedPtr<FJsonObject> FUnrealMcpConfig::ToJson() const
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(KeyConnectionMode, ConnectionMode == EUnrealMcpConnectionMode::Cloud ? TEXT("Cloud") : TEXT("Custom"));
	Obj->SetStringField(KeyHost, CustomHost);
	Obj->SetStringField(KeyToken, CustomToken);
	Obj->SetStringField(KeyCloudToken, CloudToken);
	Obj->SetStringField(KeyCloudUrl, CloudUrl);
	Obj->SetStringField(KeyAuthOption, AuthOption == EUnrealMcpAuthOption::Required ? TEXT("Required") : TEXT("None"));
	Obj->SetBoolField(KeyKeepConnected, bKeepConnected);
	Obj->SetStringField(KeyLogLevel, LogLevel);
	Obj->SetStringField(KeyTransport, Transport);
	Obj->SetBoolField(KeyStartServer, bStartServer);

	TArray<TSharedPtr<FJsonValue>> Tools;
	for (const FString& Tool : EnabledTools)
		Tools.Add(MakeShared<FJsonValueString>(Tool));
	Obj->SetArrayField(KeyEnabledTools, Tools);

	return Obj;
}

bool FUnrealMcpConfig::Save(const FString& Path) const
{
	if (Path.IsEmpty())
		return false;

	TSharedPtr<FJsonObject> Out = ToJson();

	// Round-trip the disk baseline for every env/.env-overridden key so an override is NEVER persisted
	// (Unity OverrideRecord baseline-restore). Non-overridden keys keep their current value.
	if (DiskBaselineJson.IsValid())
	{
		for (const FString& Key : OverriddenKeys)
		{
			if (const TSharedPtr<FJsonValue>* Baseline = DiskBaselineJson->Values.Find(Key))
				Out->SetField(Key, *Baseline);
			else
				Out->RemoveField(Key);
		}
	}

	FString Serialized;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Serialized);
	if (!FJsonSerializer::Serialize(Out.ToSharedRef(), Writer))
		return false;

	const FString Dir = FPaths::GetPath(Path);
	if (!Dir.IsEmpty())
		IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);

	return FFileHelper::SaveStringToFile(Serialized, *Path);
}

FString FUnrealMcpConfig::ResolveCloudBaseUrl() const
{
	const FString Trimmed = TrimSlash(CloudUrl.TrimStartAndEnd());
	return Trimmed.IsEmpty() ? FString(DefaultCloudBaseUrl) : Trimmed;
}

FString FUnrealMcpConfig::ResolveCustomHost() const
{
	const FString Trimmed = TrimSlash(CustomHost.TrimStartAndEnd());
	return Trimmed.IsEmpty() ? FString(DefaultCustomHost) : Trimmed;
}

FString FUnrealMcpConfig::ResolveEffectiveToken() const
{
	if (ConnectionMode == EUnrealMcpConnectionMode::Cloud)
		return CloudToken;

	// Custom mode: an anonymous (None) connection sends NO bearer, regardless of any stored token — so
	// flipping auth back to None drops the token from the wire without discarding the user's stored value.
	return AuthOption == EUnrealMcpAuthOption::Required ? CustomToken : FString();
}

TSharedPtr<FJsonObject> FUnrealMcpConfig::BuildEffectiveConnectionConfig() const
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("mode"), ConnectionMode == EUnrealMcpConnectionMode::Cloud ? TEXT("Cloud") : TEXT("Custom"));
	Obj->SetStringField(TEXT("host"), ResolveCustomHost());
	Obj->SetStringField(TEXT("cloudUrl"), ResolveCloudBaseUrl());
	Obj->SetStringField(TEXT("token"), ResolveEffectiveToken());
	Obj->SetBoolField(TEXT("keepConnected"), bKeepConnected);
	return Obj;
}

TMap<FString, FString> FUnrealMcpConfig::ParseEnvLines(const TArray<FString>& Lines)
{
	static const TCHAR* RecognizedKeys[] = {
		EnvConnectionMode, EnvHost, EnvCloudUrl, EnvToken, EnvAuthOption, EnvKeepConnected,
		EnvTools, EnvStartServer, EnvTransport, EnvLogLevel, EnvBridgePath
	};

	TMap<FString, FString> Result;
	for (const FString& RawLine : Lines)
	{
		FString Line = RawLine;
		Line.TrimStartAndEndInline();
		if (Line.IsEmpty() || Line[0] == TCHAR('#'))
			continue;

		int32 EqIndex = INDEX_NONE;
		if (!Line.FindChar(TCHAR('='), EqIndex) || EqIndex <= 0)
			continue;

		FString Key = Line.Left(EqIndex);
		Key.TrimStartAndEndInline();

		bool bRecognized = false;
		for (const TCHAR* Recognized : RecognizedKeys)
		{
			if (Key.Equals(Recognized, ESearchCase::CaseSensitive))
			{
				bRecognized = true;
				break;
			}
		}
		if (!bRecognized)
			continue;

		const FString Value = SanitizeValue(Line.Mid(EqIndex + 1));
		if (Value.IsEmpty())
			continue;

		Result.Add(Key, Value); // last occurrence wins
	}
	return Result;
}

TMap<FString, FString> FUnrealMcpConfig::LoadEnvFile(const FString& Path)
{
	if (Path.IsEmpty() || !FPaths::FileExists(Path))
		return TMap<FString, FString>();

	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *Path))
		return TMap<FString, FString>();

	return ParseEnvLines(Lines);
}

void FUnrealMcpConfig::ExportDotEnvToProcessEnv(const TMap<FString, FString>& DotEnv)
{
	// Export ONLY the bridge-path key into the process env. That is the single var the editor process itself
	// must read out-of-band (FUnrealMcpSidecarManager::ResolveBridgeBinaryPath, §6), and it is not a secret.
	// HOST/CLOUD_URL/TOKEN are deliberately NOT exported: the sidecar receives them via the authoritative
	// §1.3 `config` push, so exporting them here would only (a) leak the bearer token into EVERY child the
	// editor spawns and (b) pin a now-stale .env value at process-env precedence on any later re-resolve.
	if (const FString* BridgePath = DotEnv.Find(EnvBridgePath))
	{
		// Only set when the process env is currently empty for this key — process env wins over .env (§8).
		if (FPlatformMisc::GetEnvironmentVariable(EnvBridgePath).IsEmpty() && !BridgePath->IsEmpty())
			FPlatformMisc::SetEnvironmentVar(EnvBridgePath, **BridgePath);
	}
}

FString FUnrealMcpConfig::MaskSecret(const FString& Secret)
{
	return Secret.IsEmpty() ? FString(TEXT("<unset>")) : FString(TEXT("***"));
}

FString FUnrealMcpConfig::SanitizeValue(const FString& Raw)
{
	FString Value = Raw;
	Value.TrimStartAndEndInline();
	if (Value.Len() >= 2)
	{
		const TCHAR First = Value[0];
		const TCHAR Last = Value[Value.Len() - 1];
		if ((First == TCHAR('"') && Last == TCHAR('"')) || (First == TCHAR('\'') && Last == TCHAR('\'')))
			Value = Value.Mid(1, Value.Len() - 2);
	}
	return Value;
}

bool FUnrealMcpConfig::TryParseMode(const FString& Raw, EUnrealMcpConnectionMode& OutMode)
{
	const FString Normalized = SanitizeValue(Raw);
	if (Normalized.Equals(TEXT("Cloud"), ESearchCase::IgnoreCase))
	{
		OutMode = EUnrealMcpConnectionMode::Cloud;
		return true;
	}
	if (Normalized.Equals(TEXT("Custom"), ESearchCase::IgnoreCase))
	{
		OutMode = EUnrealMcpConnectionMode::Custom;
		return true;
	}
	return false; // unrecognized/numeric → keep the existing value
}

bool FUnrealMcpConfig::TryParseAuthOption(const FString& Raw, EUnrealMcpAuthOption& OutOption)
{
	const FString Normalized = SanitizeValue(Raw);
	if (Normalized.Equals(TEXT("None"), ESearchCase::IgnoreCase))
	{
		OutOption = EUnrealMcpAuthOption::None;
		return true;
	}
	if (Normalized.Equals(TEXT("Required"), ESearchCase::IgnoreCase))
	{
		OutOption = EUnrealMcpAuthOption::Required;
		return true;
	}
	return false;
}

bool FUnrealMcpConfig::TryParseBool(const FString& Raw, bool& OutValue)
{
	const FString Normalized = SanitizeValue(Raw).ToLower();
	if (Normalized == TEXT("true") || Normalized == TEXT("1") || Normalized == TEXT("yes") || Normalized == TEXT("on"))
	{
		OutValue = true;
		return true;
	}
	if (Normalized == TEXT("false") || Normalized == TEXT("0") || Normalized == TEXT("no") || Normalized == TEXT("off"))
	{
		OutValue = false;
		return true;
	}
	return false;
}
