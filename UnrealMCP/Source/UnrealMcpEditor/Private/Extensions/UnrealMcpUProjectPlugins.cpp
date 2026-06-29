// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Extensions/UnrealMcpUProjectPlugins.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/PrettyJsonPrintPolicy.h"

TArray<FString> FUnrealMcpUProjectPlugins::ParseUPluginDependencies(const TSharedPtr<FJsonObject>& UpluginJson)
{
	TArray<FString> Names;
	if (!UpluginJson.IsValid())
		return Names;

	const TArray<TSharedPtr<FJsonValue>>* Plugins = nullptr;
	if (!UpluginJson->TryGetArrayField(TEXT("Plugins"), Plugins) || Plugins == nullptr)
		return Names;

	for (const TSharedPtr<FJsonValue>& Value : *Plugins)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Obj) || Obj == nullptr || !(*Obj).IsValid())
			continue;
		FString Name;
		if (!(*Obj)->TryGetStringField(TEXT("Name"), Name) || Name.TrimStartAndEnd().IsEmpty())
			continue;
		// Enabled defaults true: only an explicit `Enabled: false` excludes the dependency.
		bool bEnabled = true;
		(*Obj)->TryGetBoolField(TEXT("Enabled"), bEnabled);
		if (bEnabled)
			Names.Add(Name.TrimStartAndEnd());
	}
	return Names;
}

FString FUnrealMcpUProjectPlugins::ReadUPluginVersionName(const TSharedPtr<FJsonObject>& UpluginJson)
{
	if (!UpluginJson.IsValid())
		return FString();
	FString Version;
	if (UpluginJson->TryGetStringField(TEXT("VersionName"), Version) && !Version.TrimStartAndEnd().IsEmpty())
		return Version.TrimStartAndEnd();
	return FString();
}

bool FUnrealMcpUProjectPlugins::EnablePluginsInUProject(
	const FString& UprojectText, const TArray<FString>& PluginNames,
	FUnrealMcpEnablePluginsResult& OutResult, FString& OutError)
{
	OutResult = FUnrealMcpEnablePluginsResult();
	OutResult.Text = UprojectText;
	OutError.Reset();

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(UprojectText);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = TEXT("the .uproject JSON is malformed");
		return false;
	}

	// Existing Plugins[] (may be absent → start empty).
	TArray<TSharedPtr<FJsonValue>> Plugins;
	const TArray<TSharedPtr<FJsonValue>>* Existing = nullptr;
	if (Root->TryGetArrayField(TEXT("Plugins"), Existing) && Existing)
		Plugins = *Existing;

	// De-dupe requested names case-insensitively, preserving first-seen order.
	TArray<FString> Wanted;
	TSet<FString> Seen;
	for (const FString& Raw : PluginNames)
	{
		const FString Name = Raw.TrimStartAndEnd();
		if (Name.IsEmpty())
			continue;
		const FString Key = Name.ToLower();
		if (Seen.Contains(Key))
			continue;
		Seen.Add(Key);
		Wanted.Add(Name);
	}

	auto FindPluginIndex = [&Plugins](const FString& Name) -> int32
	{
		for (int32 i = 0; i < Plugins.Num(); ++i)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			FString ExistingName;
			if (Plugins[i].IsValid() && Plugins[i]->TryGetObject(Obj) && Obj && (*Obj).IsValid()
				&& (*Obj)->TryGetStringField(TEXT("Name"), ExistingName)
				&& ExistingName.ToLower() == Name.ToLower())
				return i;
		}
		return INDEX_NONE;
	};

	for (const FString& Name : Wanted)
	{
		const int32 Idx = FindPluginIndex(Name);
		if (Idx == INDEX_NONE)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("Name"), Name);
			Entry->SetBoolField(TEXT("Enabled"), true);
			Plugins.Add(MakeShared<FJsonValueObject>(Entry));
			OutResult.AddedOrFlipped.Add(Name);
		}
		else
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			Plugins[Idx]->TryGetObject(Obj);
			bool bEnabled = true;
			(*Obj)->TryGetBoolField(TEXT("Enabled"), bEnabled);
			if (!bEnabled)
			{
				(*Obj)->SetBoolField(TEXT("Enabled"), true);
				OutResult.AddedOrFlipped.Add(Name);
			}
		}
	}

	// Compute the full enabled set after the operation.
	for (const TSharedPtr<FJsonValue>& Value : Plugins)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		FString Name;
		if (Value.IsValid() && Value->TryGetObject(Obj) && Obj && (*Obj).IsValid()
			&& (*Obj)->TryGetStringField(TEXT("Name"), Name) && !Name.IsEmpty())
		{
			bool bEnabled = true;
			(*Obj)->TryGetBoolField(TEXT("Enabled"), bEnabled);
			if (bEnabled)
				OutResult.EnabledPlugins.Add(Name);
		}
	}

	if (OutResult.AddedOrFlipped.Num() == 0)
	{
		// Idempotent no-op: nothing appended or flipped → return the input text verbatim (no reserialize churn).
		OutResult.bChanged = false;
		OutResult.Text = UprojectText;
		return true;
	}

	Root->SetArrayField(TEXT("Plugins"), Plugins);

	FString Out;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Out);
	if (!FJsonSerializer::Serialize(Root.ToSharedRef(), Writer))
	{
		OutError = TEXT("failed to re-serialize the .uproject JSON");
		return false;
	}

	// Preserve a trailing newline iff the input had one (UE descriptors conventionally end with one).
	if (UprojectText.EndsWith(TEXT("\n")) && !Out.EndsWith(TEXT("\n")))
		Out += TEXT("\n");

	OutResult.bChanged = true;
	OutResult.Text = Out;
	return true;
}
