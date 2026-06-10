// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Extensions/UnrealMcpExtensionManager.h"

#include "IUnrealMcpToolProvider.h"
#include "UnrealMcpToolRegistry.h"
#include "UnrealMcpLog.h"

#include "Features/IModularFeatures.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

FUnrealMcpExtensionManager::FUnrealMcpExtensionManager(
	FUnrealMcpToolRegistry& InRegistry, TFunction<void()> InOnChanged, const FString& InConfigPath)
	: Registry(InRegistry)
	, OnChanged(MoveTemp(InOnChanged))
{
	ConfigPath = InConfigPath.IsEmpty()
		? FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Config"), TEXT("UnrealMCP"), TEXT("Extensions.json"))
		: InConfigPath;

	// Default provider source: the live modular-feature registry. Overridable for deterministic tests.
	ProviderSource = [this]() { return GatherProviders(); };
}

FUnrealMcpExtensionManager::~FUnrealMcpExtensionManager()
{
	Shutdown();
}

void FUnrealMcpExtensionManager::Startup()
{
	LoadConfig();

	if (!bSubscribed)
	{
		IModularFeatures& Features = IModularFeatures::Get();
		RegisteredHandle = Features.OnModularFeatureRegistered().AddRaw(this, &FUnrealMcpExtensionManager::OnFeatureRegistered);
		UnregisteredHandle = Features.OnModularFeatureUnregistered().AddRaw(this, &FUnrealMcpExtensionManager::OnFeatureUnregistered);
		bSubscribed = true;
	}

	// Initial discovery. No notify needed at boot: the bridge has not accepted yet, so the first
	// manifest is read directly on handshake; OnChanged would no-op anyway.
	Rebuild(/*bNotify*/ false);

	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] extension manager started; %d extension(s) discovered."), Records.Num());
}

void FUnrealMcpExtensionManager::Shutdown()
{
	if (bSubscribed)
	{
		IModularFeatures& Features = IModularFeatures::Get();
		Features.OnModularFeatureRegistered().Remove(RegisteredHandle);
		Features.OnModularFeatureUnregistered().Remove(UnregisteredHandle);
		RegisteredHandle.Reset();
		UnregisteredHandle.Reset();
		bSubscribed = false;
	}
}

const FUnrealMcpExtensionRecord* FUnrealMcpExtensionManager::FindExtension(const FString& Id) const
{
	return Records.FindByPredicate([&Id](const FUnrealMcpExtensionRecord& R) { return R.Id == Id; });
}

void FUnrealMcpExtensionManager::SetExtensionEnabled(const FString& Id, bool bEnabled)
{
	const bool bCurrentlyEnabled = IsExtensionEnabled(Id);
	if (bEnabled == bCurrentlyEnabled)
		return; // no-op; avoid a pointless rebuild + manifest churn

	if (bEnabled)
		DisabledExtensions.Remove(Id);
	else
		DisabledExtensions.Add(Id);

	SaveConfig();
	Rebuild(/*bNotify*/ true);

	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] extension '%s' %s."), *Id, bEnabled ? TEXT("enabled") : TEXT("disabled"));
}

TArray<IUnrealMcpToolProvider*> FUnrealMcpExtensionManager::GatherProviders() const
{
	return IModularFeatures::Get().GetModularFeatureImplementations<IUnrealMcpToolProvider>(
		IUnrealMcpToolProvider::GetModularFeatureName());
}

void FUnrealMcpExtensionManager::Rebuild(bool bNotify)
{
	RebuildFromProviders(ProviderSource ? ProviderSource() : GatherProviders(), bNotify);
}

void FUnrealMcpExtensionManager::RebuildFromProviders(const TArray<IUnrealMcpToolProvider*>& Providers, bool bNotify)
{
	// 1. Clear the previous extension contribution (core tools are untouched: only ids we registered).
	for (const FString& Id : RegisteredExtensionIds)
		Registry.RemoveToolsForExtension(Id);
	RegisteredExtensionIds.Reset();
	Records.Reset();

	// 2. Deterministic ordering: providers sorted by ExtensionId (§5). UE's Sort on a pointer array
	//    dereferences elements, so the predicate receives references.
	TArray<IUnrealMcpToolProvider*> Sorted;
	Sorted.Reserve(Providers.Num());
	for (IUnrealMcpToolProvider* P : Providers)
	{
		if (P != nullptr)
			Sorted.Add(P);
	}
	Sorted.Sort([](const IUnrealMcpToolProvider& A, const IUnrealMcpToolProvider& B)
	{
		return A.GetExtensionId() < B.GetExtensionId();
	});

	// 3. Register each enabled provider; build the public record either way.
	Records.Reserve(Sorted.Num());
	for (IUnrealMcpToolProvider* Provider : Sorted)
	{
		FUnrealMcpExtensionRecord Record;
		Record.Id = Provider->GetExtensionId();
		Record.DisplayName = Provider->GetDisplayName();
		Record.Version = Provider->GetExtensionVersion();
		Record.bEnabled = !DisabledExtensions.Contains(Record.Id);

		if (Record.bEnabled)
		{
			const FUnrealMcpExtensionRegistrationResult Result = Registry.RegisterExtension(
				Record.Id, [Provider](FUnrealMcpToolRegistry& Reg) { Provider->RegisterTools(Reg); });

			Record.ToolCount = Result.ToolsRegistered;
			if (Result.Errors.Num() > 0)
				Record.Error = FString::Join(Result.Errors, TEXT("; "));

			// Track the id (even with 0 tools) so the next rebuild removes its tools cleanly.
			RegisteredExtensionIds.AddUnique(Record.Id);
		}

		Records.Add(MoveTemp(Record));
	}

	// 4. Notify the owner to re-push the manifest (§2.2).
	if (bNotify && OnChanged)
		OnChanged();
}

void FUnrealMcpExtensionManager::OnFeatureRegistered(const FName& Type, IModularFeature* /*Feature*/)
{
	if (Type == IUnrealMcpToolProvider::GetModularFeatureName())
	{
		UE_LOG(LogUnrealMcp, Verbose, TEXT("[Unreal-MCP] tool provider registered; rebuilding extensions."));
		Rebuild(/*bNotify*/ true);
	}
}

void FUnrealMcpExtensionManager::OnFeatureUnregistered(const FName& Type, IModularFeature* /*Feature*/)
{
	if (Type == IUnrealMcpToolProvider::GetModularFeatureName())
	{
		UE_LOG(LogUnrealMcp, Verbose, TEXT("[Unreal-MCP] tool provider unregistered; rebuilding extensions."));
		Rebuild(/*bNotify*/ true);
	}
}

void FUnrealMcpExtensionManager::LoadConfig()
{
	DisabledExtensions.Reset();

	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *ConfigPath))
		return; // first run — no file yet

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] extension config at '%s' is malformed; ignoring."), *ConfigPath);
		return;
	}

	const TArray<TSharedPtr<FJsonValue>>* Disabled = nullptr;
	if (Root->TryGetArrayField(TEXT("disabledExtensions"), Disabled))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Disabled)
		{
			FString Id;
			if (Value.IsValid() && Value->TryGetString(Id) && !Id.IsEmpty())
				DisabledExtensions.Add(Id);
		}
	}
}

void FUnrealMcpExtensionManager::SaveConfig() const
{
	TArray<FString> Sorted = DisabledExtensions.Array();
	Sorted.Sort();

	TArray<TSharedPtr<FJsonValue>> Disabled;
	for (const FString& Id : Sorted)
		Disabled.Add(MakeShared<FJsonValueString>(Id));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("disabledExtensions"), Disabled);

	FString Out;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ConfigPath), /*Tree*/ true);
	if (!FFileHelper::SaveStringToFile(Out, *ConfigPath))
		UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] failed to persist extension config to '%s'."), *ConfigPath);
}
