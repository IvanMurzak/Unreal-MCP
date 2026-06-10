// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "HAL/FileManager.h"
#include "Config/UnrealMcpConfig.h"
#include "UnrealMcpLog.h"

namespace
{
	// Captures everything routed through GLog while installed, so a test can assert what reached the log.
	class FUnrealMcpLogCapture : public FOutputDevice
	{
	public:
		FString Captured;
		virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type, const FName&) override
		{
			Captured += Message;
			Captured += TEXT("\n");
		}
	};
}

/**
 * Config store specs (docs/ARCHITECTURE.md §8): the .env parser rules, the process-env > .env > file >
 * default precedence matrix, the OverrideRecord baseline-restore on Save(), the token-never-logged guard,
 * and the effective connection-config routing the plugin pushes to the sidecar (§1.3).
 */
BEGIN_DEFINE_SPEC(FUnrealMcpConfigSpec, "UnrealMcp.Config",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	// An injectable process-env reader backed by an in-memory map (no real process env touched).
	static TFunction<bool(const FString&, FString&)> MakeEnvReader(const TMap<FString, FString>& Env)
	{
		return [Env](const FString& Name, FString& Out) -> bool
		{
			if (const FString* Value = Env.Find(Name))
			{
				Out = *Value;
				return true;
			}
			return false;
		};
	}

	// A config seeded from a JSON disk baseline (the "file" precedence layer).
	static FUnrealMcpConfig FromDiskJson(const TSharedPtr<FJsonObject>& Json)
	{
		FUnrealMcpConfig Config;
		Config.LoadFromJson(Json);
		return Config;
	}

END_DEFINE_SPEC(FUnrealMcpConfigSpec)

void FUnrealMcpConfigSpec::Define()
{
	Describe(".env parsing", [this]()
	{
		It("keeps only recognized keys, skips blanks/comments, splits on first '=', last wins, strips quotes", [this]()
		{
			TArray<FString> Lines = {
				TEXT(""),
				TEXT("   "),
				TEXT("# a comment"),
				TEXT("   # indented comment"),
				TEXT("UNKNOWN_KEY=ignored"),
				TEXT("no_equals_here"),
				TEXT("=leading-equals-skipped"),
				TEXT("UNREAL_MCP_HOST=http://first:1"),
				TEXT("UNREAL_MCP_HOST=http://second:2"),       // last wins
				TEXT("UNREAL_MCP_TOKEN=\"quoted-token\""),       // double quotes stripped
				TEXT("UNREAL_MCP_CLOUD_URL='https://cloud'"),    // single quotes stripped
				TEXT("UNREAL_MCP_AUTH_OPTION = Required "),      // surrounding whitespace trimmed
				TEXT("UNREAL_MCP_HOST_WITH_VALUE=x=y=z"),        // unknown key with multiple '='
			};

			const TMap<FString, FString> Parsed = FUnrealMcpConfig::ParseEnvLines(Lines);

			TestEqual(TEXT("recognized key count"), Parsed.Num(), 4);
			TestEqual(TEXT("host last-wins"), Parsed.FindRef(FUnrealMcpConfig::EnvHost), FString(TEXT("http://second:2")));
			TestEqual(TEXT("token unquoted"), Parsed.FindRef(FUnrealMcpConfig::EnvToken), FString(TEXT("quoted-token")));
			TestEqual(TEXT("cloud url unquoted"), Parsed.FindRef(FUnrealMcpConfig::EnvCloudUrl), FString(TEXT("https://cloud")));
			TestEqual(TEXT("auth option trimmed"), Parsed.FindRef(FUnrealMcpConfig::EnvAuthOption), FString(TEXT("Required")));
			TestFalse(TEXT("unknown key dropped"), Parsed.Contains(TEXT("UNKNOWN_KEY")));
		});

		It("preserves a value containing '=' after the first split", [this]()
		{
			const TMap<FString, FString> Parsed = FUnrealMcpConfig::ParseEnvLines({ TEXT("UNREAL_MCP_TOKEN=a=b=c") });
			TestEqual(TEXT("value keeps later '='"), Parsed.FindRef(FUnrealMcpConfig::EnvToken), FString(TEXT("a=b=c")));
		});
	});

	Describe("precedence (process env > .env > file > default)", [this]()
	{
		It("uses the built-in default when nothing else is set", [this]()
		{
			FUnrealMcpConfig Config; // fresh, no disk, no overrides
			TestEqual(TEXT("default custom host"), Config.ResolveCustomHost(), FString(FUnrealMcpConfig::DefaultCustomHost));
			TestEqual(TEXT("default cloud base"), Config.ResolveCloudBaseUrl(), FString(FUnrealMcpConfig::DefaultCloudBaseUrl));
		});

		It("file value beats the default", [this]()
		{
			TSharedPtr<FJsonObject> Disk = MakeShared<FJsonObject>();
			Disk->SetStringField(TEXT("host"), TEXT("http://file-host:10"));
			FUnrealMcpConfig Config = FromDiskJson(Disk);
			Config.ApplyOverrides({}, MakeEnvReader({}));
			TestEqual(TEXT("file host"), Config.CustomHost, FString(TEXT("http://file-host:10")));
		});

		It(".env value beats the file value", [this]()
		{
			TSharedPtr<FJsonObject> Disk = MakeShared<FJsonObject>();
			Disk->SetStringField(TEXT("host"), TEXT("http://file-host:10"));
			FUnrealMcpConfig Config = FromDiskJson(Disk);

			TMap<FString, FString> DotEnv;
			DotEnv.Add(FUnrealMcpConfig::EnvHost, TEXT("http://dotenv-host:20"));
			Config.ApplyOverrides(DotEnv, MakeEnvReader({}));

			TestEqual(TEXT("dotenv host"), Config.CustomHost, FString(TEXT("http://dotenv-host:20")));
			TestTrue(TEXT("host overridden"), Config.IsOverridden(TEXT("host")));
		});

		It("process env beats the .env value", [this]()
		{
			TSharedPtr<FJsonObject> Disk = MakeShared<FJsonObject>();
			Disk->SetStringField(TEXT("host"), TEXT("http://file-host:10"));
			FUnrealMcpConfig Config = FromDiskJson(Disk);

			TMap<FString, FString> DotEnv;
			DotEnv.Add(FUnrealMcpConfig::EnvHost, TEXT("http://dotenv-host:20"));

			TMap<FString, FString> ProcEnv;
			ProcEnv.Add(FUnrealMcpConfig::EnvHost, TEXT("http://proc-host:30"));

			Config.ApplyOverrides(DotEnv, MakeEnvReader(ProcEnv));
			TestEqual(TEXT("process env host wins"), Config.CustomHost, FString(TEXT("http://proc-host:30")));
		});
	});

	Describe("token routing + effective config", [this]()
	{
		It("Custom + None sends no bearer; Custom + Required sends the custom token", [this]()
		{
			FUnrealMcpConfig Config;
			Config.ConnectionMode = EUnrealMcpConnectionMode::Custom;

			TMap<FString, FString> ProcEnv;
			ProcEnv.Add(FUnrealMcpConfig::EnvConnectionMode, TEXT("Custom"));
			ProcEnv.Add(FUnrealMcpConfig::EnvToken, TEXT("the-custom-token"));
			Config.ApplyOverrides({}, MakeEnvReader(ProcEnv));

			// Auth defaults to None → no bearer on the wire even though a token is stored.
			TestEqual(TEXT("custom token stored"), Config.CustomToken, FString(TEXT("the-custom-token")));
			TestTrue(TEXT("effective token empty under None"), Config.ResolveEffectiveToken().IsEmpty());

			Config.AuthOption = EUnrealMcpAuthOption::Required;
			TestEqual(TEXT("effective token under Required"), Config.ResolveEffectiveToken(), FString(TEXT("the-custom-token")));
		});

		It("routes the token to the cloud field in Cloud mode", [this]()
		{
			FUnrealMcpConfig Config;
			TMap<FString, FString> ProcEnv;
			ProcEnv.Add(FUnrealMcpConfig::EnvConnectionMode, TEXT("Cloud"));
			ProcEnv.Add(FUnrealMcpConfig::EnvToken, TEXT("the-cloud-token"));
			Config.ApplyOverrides({}, MakeEnvReader(ProcEnv));

			TestEqual(TEXT("cloud token stored"), Config.CloudToken, FString(TEXT("the-cloud-token")));
			TestTrue(TEXT("custom token untouched"), Config.CustomToken.IsEmpty());
			TestEqual(TEXT("effective token = cloud token"), Config.ResolveEffectiveToken(), FString(TEXT("the-cloud-token")));
		});

		It("builds the effective connection config for the §1.3 config message (mode-aware)", [this]()
		{
			FUnrealMcpConfig Config;
			Config.ConnectionMode = EUnrealMcpConnectionMode::Custom;
			Config.CustomHost = TEXT("http://localhost:5200/");

			const TSharedPtr<FJsonObject> Eff = Config.BuildEffectiveConnectionConfig();
			TestEqual(TEXT("mode"), Eff->GetStringField(TEXT("mode")), FString(TEXT("Custom")));
			TestEqual(TEXT("host trailing slash trimmed"), Eff->GetStringField(TEXT("host")), FString(TEXT("http://localhost:5200")));
			TestTrue(TEXT("token empty (Custom+None)"), Eff->GetStringField(TEXT("token")).IsEmpty());
			TestTrue(TEXT("keepConnected true by default"), Eff->GetBoolField(TEXT("keepConnected")));
		});
	});

	Describe("Save() baseline-restore (env/.env never persisted)", [this]()
	{
		It("round-trips the disk baseline while the in-memory config keeps the override", [this]()
		{
			const FString Path = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("UnrealMcpConfigSpec"), TEXT("config.json"));
			IFileManager::Get().Delete(*Path, false, true, true);

			// Disk baseline: Custom host = file-host.
			TSharedPtr<FJsonObject> Disk = MakeShared<FJsonObject>();
			Disk->SetStringField(TEXT("connectionMode"), TEXT("Custom"));
			Disk->SetStringField(TEXT("host"), TEXT("http://file-host:10"));
			FUnrealMcpConfig Config = FromDiskJson(Disk);

			// Override host via the process env.
			TMap<FString, FString> ProcEnv;
			ProcEnv.Add(FUnrealMcpConfig::EnvHost, TEXT("http://env-host:99"));
			Config.ApplyOverrides({}, MakeEnvReader(ProcEnv));
			TestEqual(TEXT("in-memory host is the override"), Config.CustomHost, FString(TEXT("http://env-host:99")));

			TestTrue(TEXT("save ok"), Config.Save(Path));

			// Reload from disk: the persisted host must be the BASELINE, not the env override.
			FUnrealMcpConfig Reloaded;
			Reloaded.LoadFromFile(Path);
			TestEqual(TEXT("persisted host is the baseline"), Reloaded.CustomHost, FString(TEXT("http://file-host:10")));

			// And the in-memory override is untouched by Save().
			TestEqual(TEXT("in-memory host still the override after save"), Config.CustomHost, FString(TEXT("http://env-host:99")));

			IFileManager::Get().Delete(*Path, false, true, true);
		});

		It("persists a non-overridden field normally", [this]()
		{
			const FString Path = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("UnrealMcpConfigSpec"), TEXT("config2.json"));
			IFileManager::Get().Delete(*Path, false, true, true);

			FUnrealMcpConfig Config;
			Config.CustomHost = TEXT("http://user-edited:7");   // a user edit, not an env override
			TestTrue(TEXT("save ok"), Config.Save(Path));

			FUnrealMcpConfig Reloaded;
			Reloaded.LoadFromFile(Path);
			TestEqual(TEXT("user edit persists"), Reloaded.CustomHost, FString(TEXT("http://user-edited:7")));

			IFileManager::Get().Delete(*Path, false, true, true);
		});
	});

	Describe("token never logged (§8)", [this]()
	{
		It("MaskSecret never reveals the token value", [this]()
		{
			const FString Secret = TEXT("super-secret-bearer-value");
			const FString Masked = FUnrealMcpConfig::MaskSecret(Secret);
			TestNotEqual(TEXT("masked != raw"), Masked, Secret);
			TestFalse(TEXT("masked does not contain the secret"), Masked.Contains(Secret));
			TestEqual(TEXT("unset marker"), FUnrealMcpConfig::MaskSecret(TEXT("")), FString(TEXT("<unset>")));
		});

		It("a representative resolved-config log line never emits the raw token", [this]()
		{
			const FString RawToken = TEXT("super-secret-bearer-value");

			// Resolve a Cloud-mode config so ResolveEffectiveToken() returns the raw bearer.
			FUnrealMcpConfig Config;
			Config.ApplyOverrides({}, MakeEnvReader({
				{ TEXT("UNREAL_MCP_CONNECTION_MODE"), TEXT("Cloud") },
				{ TEXT("UNREAL_MCP_TOKEN"), RawToken },
			}));
			TestEqual(TEXT("token resolved for the test"), Config.ResolveEffectiveToken(), RawToken);

			// Emit the SAME masked-config log shape the runtime uses, captured through GLog.
			FUnrealMcpLogCapture Capture;
			GLog->AddOutputDevice(&Capture);
			UE_LOG(LogUnrealMcp, Log,
				TEXT("[Unreal-MCP] connection config resolved (mode=%s, host=%s, cloudUrl=%s, token=%s, keepConnected=%s)."),
				TEXT("Cloud"), *Config.ResolveCustomHost(), *Config.ResolveCloudBaseUrl(),
				*FUnrealMcpConfig::MaskSecret(Config.ResolveEffectiveToken()),
				Config.bKeepConnected ? TEXT("true") : TEXT("false"));
			GLog->Flush();
			GLog->RemoveOutputDevice(&Capture);

			TestTrue(TEXT("masked marker reached the log"), Capture.Captured.Contains(TEXT("***")));
			TestFalse(TEXT("raw token never reached the log"), Capture.Captured.Contains(RawToken));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
