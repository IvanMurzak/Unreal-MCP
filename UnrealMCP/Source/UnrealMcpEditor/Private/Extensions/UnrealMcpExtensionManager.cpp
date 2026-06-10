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

namespace
{
	/** The reserved core scope: every non-extension tool defaults to this id (UnrealMcpToolRegistry.h). */
	const FString ReservedCoreExtensionId = TEXT("core");

	/**
	 * Validate a provider-supplied extension id before it is used as a registry scope / removal key (§5).
	 * A provider returning the reserved "core" id (or an empty/garbage id) would otherwise have its tools
	 * removed under that id on the NEXT rebuild's cleanup pass — for "core" that silently deletes every
	 * core tool. Ids are reverse-DNS-style: lowercase letters/digits separated by '.' or '-', no leading/
	 * trailing/empty separators. Returns false + a human-readable reason on the first problem.
	 */
	bool IsValidExtensionId(const FString& Id, FString& OutError)
	{
		if (Id.IsEmpty())
		{
			OutError = TEXT("extension id is empty");
			return false;
		}
		if (Id == ReservedCoreExtensionId)
		{
			OutError = FString::Printf(TEXT("extension id '%s' is reserved for built-in tools"), *ReservedCoreExtensionId);
			return false;
		}

		bool bPrevSeparator = false;
		const int32 Len = Id.Len();
		for (int32 i = 0; i < Len; ++i)
		{
			const TCHAR C = Id[i];
			const bool bLower = (C >= TEXT('a') && C <= TEXT('z'));
			const bool bDigit = (C >= TEXT('0') && C <= TEXT('9'));
			const bool bSeparator = (C == TEXT('.') || C == TEXT('-'));
			if (!bLower && !bDigit && !bSeparator)
			{
				OutError = FString::Printf(
					TEXT("extension id '%s' has an invalid character (allowed: lowercase letters, digits, '.', '-')"), *Id);
				return false;
			}
			if (bSeparator && (i == 0 || i == Len - 1 || bPrevSeparator))
			{
				OutError = FString::Printf(TEXT("extension id '%s' has a leading, trailing, or doubled '.'/'-' separator"), *Id);
				return false;
			}
			bPrevSeparator = bSeparator;
		}
		return true;
	}
}

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
	// 0. Re-entrancy guard (§5): if a provider's RegisterTools synchronously (un)registers a modular
	//    feature, OnFeatureRegistered re-enters here mid-rebuild. Defer rather than recurse — recursing
	//    would re-open the registry's extension scope (tripping its no-nested-scope check) and could free
	//    a provider the outer Sorted loop still holds. Record the request and let the outer pass re-run.
	if (bRebuilding)
	{
		bPendingRebuild = true;
		bPendingNotify = bPendingNotify || bNotify;
		return;
	}
	bRebuilding = true;

	// 1. Clear the previous extension contribution (core tools are untouched: only ids we registered).
	for (const FString& Id : RegisteredExtensionIds)
		Registry.RemoveToolsForExtension(Id);
	RegisteredExtensionIds.Reset();
	Records.Reset();

	// 2. Deterministic ordering: providers sorted by ExtensionId (§5). StableSort keeps registration
	//    order as the tie-break so two providers sharing an id get a stable relative order run-to-run.
	//    UE's Sort on a pointer array dereferences elements, so the predicate receives references.
	TArray<IUnrealMcpToolProvider*> Sorted;
	Sorted.Reserve(Providers.Num());
	for (IUnrealMcpToolProvider* P : Providers)
	{
		if (P != nullptr)
			Sorted.Add(P);
	}
	Sorted.StableSort([](const IUnrealMcpToolProvider& A, const IUnrealMcpToolProvider& B)
	{
		return A.GetExtensionId() < B.GetExtensionId();
	});

	// 3. Register each enabled provider; build the public record either way.
	TSet<FString> SeenIds;
	Records.Reserve(Sorted.Num());
	for (IUnrealMcpToolProvider* Provider : Sorted)
	{
		FUnrealMcpExtensionRecord Record;
		Record.Id = Provider->GetExtensionId();
		Record.DisplayName = Provider->GetDisplayName();
		Record.Version = Provider->GetExtensionVersion();

		// 3a. Validate the provider-supplied id before it becomes a registry scope / removal key. An
		//     invalid or reserved ("core") id is recorded on the record and the provider contributes
		//     nothing — never register under it (that would let the next rebuild's cleanup delete the
		//     core tools, or a malformed id leak into the manifest).
		FString IdError;
		if (!IsValidExtensionId(Record.Id, IdError))
		{
			Record.bEnabled = false;
			Record.ToolCount = 0;
			Record.Error = IdError;
			UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] extension rejected: %s — its tools were skipped."), *IdError);
			Records.Add(MoveTemp(Record));
			continue;
		}

		// 3b. Detect a duplicate extension id (an authoring error, §5). The first-sorted provider keeps
		//     the id; later providers sharing it are skipped with an error so the manifest stays
		//     deterministic and a second provider cannot silently shadow the first's removal key.
		if (SeenIds.Contains(Record.Id))
		{
			Record.bEnabled = false;
			Record.ToolCount = 0;
			Record.Error = FString::Printf(
				TEXT("duplicate extension id '%s' — another provider already registered it; this provider's tools were skipped"),
				*Record.Id);
			UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] %s"), *Record.Error);
			Records.Add(MoveTemp(Record));
			continue;
		}
		SeenIds.Add(Record.Id);

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

	// 5. Release the guard. If a provider re-entered while we were rebuilding, the feature set changed
	//    under us — re-run once against the FRESH source so the manifest reflects the new reality.
	bRebuilding = false;
	if (bPendingRebuild)
	{
		bPendingRebuild = false;
		const bool bDeferredNotify = bPendingNotify;
		bPendingNotify = false;
		Rebuild(bDeferredNotify);
	}
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
		// Loud (Error, not Warning): a malformed file means the persisted disabled set is being silently
		// dropped, so disabled extensions will spring back enabled until the file is rewritten.
		UE_LOG(LogUnrealMcp, Error,
			TEXT("[Unreal-MCP] extension config at '%s' is malformed and was ignored; disabled-extension state is lost until it is next saved."),
			*ConfigPath);
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

	IFileManager& FileManager = IFileManager::Get();
	FileManager.MakeDirectory(*FPaths::GetPath(ConfigPath), /*Tree*/ true);

	// Crash-safe write: serialize to a sibling temp file, then atomically Move it over the target. A
	// crash mid-write leaves the temp file (discarded next run), never a half-written Extensions.json
	// that LoadConfig would reject as malformed.
	const FString TempPath = ConfigPath + TEXT(".tmp");
	if (!FFileHelper::SaveStringToFile(Out, *TempPath))
	{
		UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] failed to write temp extension config '%s'."), *TempPath);
		return;
	}
	if (!FileManager.Move(*ConfigPath, *TempPath, /*bReplace*/ true, /*bEvenIfReadOnly*/ true))
	{
		UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] failed to persist extension config to '%s'."), *ConfigPath);
		FileManager.Delete(*TempPath, /*RequireExists*/ false, /*EvenReadOnly*/ true);
	}
}
