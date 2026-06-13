// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Agents/JsonAiAgentConfig.h"

#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// --- FAiAgentConfig statics (defined here, the only TU that needs them outside headers). ---
const TCHAR* FAiAgentConfig::DefaultMcpServerName = TEXT("unreal-mcp");
const TCHAR* FAiAgentConfig::DefaultBodyPath = TEXT("mcpServers");

const TArray<FString>& FAiAgentConfig::DeprecatedMcpServerNames()
{
	// Earlier generations / sibling engines wrote the server under these names; clean them up on sight so a
	// project migrated from a pre-release plugin (or hand-edited) ends up with exactly one canonical entry.
	static const TArray<FString> Names = { TEXT("Unreal-MCP"), TEXT("ai-game-developer") };
	return Names;
}

FJsonAiAgentConfig::FJsonAiAgentConfig(const FString& InName, const FString& InConfigPath, const FString& InBodyPath)
	: FAiAgentConfig(InName, InConfigPath, InBodyPath)
{
	// Default identity keys mirror Unity's: a sibling entry sharing our command or url is the same server.
	IdentityKeys = { TEXT("command"), TEXT("url") };
}

FJsonAiAgentConfig& FJsonAiAgentConfig::SetProperty(const FString& Key, const TSharedPtr<FJsonValue>& Value, bool bRequired, EUnrealMcpValueComparison Comparison)
{
	if (!Properties.Contains(Key))
		PropertyOrder.Add(Key);
	Properties.Add(Key, FDesiredProperty{ Value, bRequired, Comparison });
	// A property that is being SET cannot also be in the to-remove list (the last call wins).
	PropertiesToRemove.Remove(Key);
	return *this;
}

FJsonAiAgentConfig& FJsonAiAgentConfig::SetStringProperty(const FString& Key, const FString& Value, bool bRequired, EUnrealMcpValueComparison Comparison)
{
	return SetProperty(Key, MakeShared<FJsonValueString>(Value), bRequired, Comparison);
}

FJsonAiAgentConfig& FJsonAiAgentConfig::SetPropertyToRemove(const FString& Key)
{
	if (Properties.Contains(Key))
	{
		Properties.Remove(Key);
		PropertyOrder.Remove(Key);
	}
	PropertiesToRemove.AddUnique(Key);
	return *this;
}

FJsonAiAgentConfig& FJsonAiAgentConfig::AddIdentityKey(const FString& Key)
{
	IdentityKeys.AddUnique(Key);
	return *this;
}

TArray<FString> FJsonAiAgentConfig::BodyPathSegments(const FString& InBodyPath)
{
	TArray<FString> Segments;
	FString Trimmed = InBodyPath;
	Trimmed.TrimStartAndEndInline();
	if (Trimmed.IsEmpty())
		return Segments;
	Trimmed.ParseIntoArray(Segments, TEXT("/"), /*CullEmpty*/ true);
	return Segments;
}

TSharedPtr<FJsonObject> FJsonAiAgentConfig::BuildServerEntry() const
{
	TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
	// Deterministic output: alphabetical key order (matches Unity's OrderBy(Ordinal)).
	TArray<FString> SortedKeys = PropertyOrder;
	SortedKeys.Sort();
	for (const FString& Key : SortedKeys)
	{
		const FDesiredProperty& Prop = Properties[Key];
		Entry->SetField(Key, Prop.Value);
	}
	return Entry;
}

FString FJsonAiAgentConfig::GetExpectedFileContent() const
{
	const TArray<FString> Segments = BodyPathSegments(BodyPath);

	TSharedPtr<FJsonObject> Inner = MakeShared<FJsonObject>();
	Inner->SetObjectField(DefaultMcpServerName, BuildServerEntry());

	// Wrap from innermost to outermost so the body path nests correctly.
	TSharedPtr<FJsonObject> Result = Inner;
	for (int32 i = Segments.Num() - 1; i >= 0; --i)
	{
		TSharedPtr<FJsonObject> Wrapper = MakeShared<FJsonObject>();
		Wrapper->SetObjectField(Segments[i], Result);
		Result = Wrapper;
	}

	FString Serialized;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Serialized);
	FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
	return Serialized;
}

bool FJsonAiAgentConfig::TryReadRoot(TSharedPtr<FJsonObject>& OutRoot) const
{
	OutRoot.Reset();
	if (ConfigPath.IsEmpty() || !FPaths::FileExists(ConfigPath))
		return false;

	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *ConfigPath))
		return false;
	if (Json.TrimStartAndEnd().IsEmpty())
		return false;

	const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Json);
	TSharedPtr<FJsonObject> Parsed;
	if (!FJsonSerializer::Deserialize(Reader, Parsed) || !Parsed.IsValid())
		return false; // invalid / non-object → treated as "no config" by the caller

	OutRoot = Parsed;
	return true;
}

bool FJsonAiAgentConfig::WriteRoot(const TSharedPtr<FJsonObject>& Root) const
{
	if (ConfigPath.IsEmpty() || !Root.IsValid())
		return false;

	FString Serialized;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Serialized);
	if (!FJsonSerializer::Serialize(Root.ToSharedRef(), Writer))
		return false;

	const FString Dir = FPaths::GetPath(ConfigPath);
	if (!Dir.IsEmpty())
		IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);

	return FFileHelper::SaveStringToFile(Serialized, *ConfigPath);
}

TSharedPtr<FJsonObject> FJsonAiAgentConfig::NavigateToBody(const TSharedPtr<FJsonObject>& Root, const TArray<FString>& Segments)
{
	TSharedPtr<FJsonObject> Current = Root;
	for (const FString& Segment : Segments)
	{
		if (!Current.IsValid())
			return nullptr;
		const TSharedPtr<FJsonObject>* Child = nullptr;
		if (!Current->TryGetObjectField(Segment, Child) || Child == nullptr)
			return nullptr;
		Current = *Child;
	}
	return Current;
}

TSharedPtr<FJsonObject> FJsonAiAgentConfig::EnsureBody(const TSharedPtr<FJsonObject>& Root, const TArray<FString>& Segments)
{
	TSharedPtr<FJsonObject> Current = Root;
	for (const FString& Segment : Segments)
	{
		const TSharedPtr<FJsonObject>* Child = nullptr;
		if (Current->TryGetObjectField(Segment, Child) && Child != nullptr)
		{
			Current = *Child;
		}
		else
		{
			// Missing OR present-but-not-an-object: overwrite with a fresh object (Unity's EnsureJsonPathExists).
			TSharedPtr<FJsonObject> NewObj = MakeShared<FJsonObject>();
			Current->SetObjectField(Segment, NewObj);
			Current = NewObj;
		}
	}
	return Current;
}

TArray<FString> FJsonAiAgentConfig::FindDuplicateServerEntryKeys(const TSharedPtr<FJsonObject>& Body) const
{
	TArray<FString> Result;
	if (!Body.IsValid())
		return Result;

	// Our identity values (only those we actually set as desired properties).
	TArray<TPair<FString, FDesiredProperty>> OurIdentities;
	for (const FString& IdKey : IdentityKeys)
	{
		if (const FDesiredProperty* Prop = Properties.Find(IdKey))
			OurIdentities.Add(TPair<FString, FDesiredProperty>(IdKey, *Prop));
	}
	if (OurIdentities.Num() == 0)
		return Result;

	for (const auto& Pair : Body->Values)
	{
		if (Pair.Key == DefaultMcpServerName)
			continue;
		if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object)
			continue;
		const TSharedPtr<FJsonObject> Entry = Pair.Value->AsObject();
		if (!Entry.IsValid())
			continue;

		for (const auto& Identity : OurIdentities)
		{
			const TSharedPtr<FJsonValue> ExistingValue = Entry->TryGetField(Identity.Key);
			if (ExistingValue.IsValid() && AreValuesEquivalent(Identity.Value.Comparison, Identity.Value.Value, ExistingValue))
			{
				Result.AddUnique(Pair.Key);
				break;
			}
		}
	}
	return Result;
}

bool FJsonAiAgentConfig::Configure()
{
	if (ConfigPath.IsEmpty())
		return false;

	const TArray<FString> Segments = BodyPathSegments(BodyPath);

	TSharedPtr<FJsonObject> Root;
	if (!TryReadRoot(Root))
	{
		// Missing / empty / invalid file → write a clean expected-content file (parent dirs created in WriteRoot).
		const FString Dir = FPaths::GetPath(ConfigPath);
		if (!Dir.IsEmpty())
			IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
		if (!FFileHelper::SaveStringToFile(GetExpectedFileContent(), *ConfigPath))
			return false;
		return IsConfigured();
	}

	TSharedPtr<FJsonObject> Body = EnsureBody(Root, Segments);

	// Remove deprecated server entries.
	for (const FString& Deprecated : DeprecatedMcpServerNames())
		Body->RemoveField(Deprecated);

	// Remove sibling duplicates (same server under a different name).
	for (const FString& DupKey : FindDuplicateServerEntryKeys(Body))
		Body->RemoveField(DupKey);

	// Get-or-create the canonical server entry.
	TSharedPtr<FJsonObject> Entry;
	const TSharedPtr<FJsonObject>* ExistingEntry = nullptr;
	if (Body->TryGetObjectField(FString(DefaultMcpServerName), ExistingEntry) && ExistingEntry != nullptr)
		Entry = *ExistingEntry;
	else
		Entry = MakeShared<FJsonObject>();

	// Drop the keys that must not be present (the other transport's keys / stale headers).
	for (const FString& Key : PropertiesToRemove)
		Entry->RemoveField(Key);

	// Set the desired properties (deterministic key order).
	TArray<FString> SortedKeys = PropertyOrder;
	SortedKeys.Sort();
	for (const FString& Key : SortedKeys)
		Entry->SetField(Key, Properties[Key].Value);

	Body->SetObjectField(DefaultMcpServerName, Entry);

	if (!WriteRoot(Root))
		return false;

	return IsConfigured();
}

bool FJsonAiAgentConfig::Remove()
{
	if (ConfigPath.IsEmpty())
		return false;

	const TArray<FString> Segments = BodyPathSegments(BodyPath);

	TSharedPtr<FJsonObject> Root;
	if (!TryReadRoot(Root))
		return false; // nothing to remove from

	TSharedPtr<FJsonObject> Body = NavigateToBody(Root, Segments);
	if (!Body.IsValid())
		return false;

	bool bRemoved = false;

	if (Body->HasField(FString(DefaultMcpServerName)))
	{
		Body->RemoveField(DefaultMcpServerName);
		bRemoved = true;
	}

	for (const FString& Deprecated : DeprecatedMcpServerNames())
	{
		if (Body->HasField(Deprecated))
		{
			Body->RemoveField(Deprecated);
			bRemoved = true;
		}
	}

	for (const FString& DupKey : FindDuplicateServerEntryKeys(Body))
	{
		Body->RemoveField(DupKey);
		bRemoved = true;
	}

	if (!bRemoved)
		return false;

	return WriteRoot(Root);
}

bool FJsonAiAgentConfig::IsDetected() const
{
	const TArray<FString> Segments = BodyPathSegments(BodyPath);

	TSharedPtr<FJsonObject> Root;
	if (!TryReadRoot(Root))
		return false;

	TSharedPtr<FJsonObject> Body = NavigateToBody(Root, Segments);
	if (!Body.IsValid())
		return false;

	if (Body->HasField(FString(DefaultMcpServerName)))
		return true;

	for (const FString& Deprecated : DeprecatedMcpServerNames())
		if (Body->HasField(Deprecated))
			return true;

	return FindDuplicateServerEntryKeys(Body).Num() > 0;
}

bool FJsonAiAgentConfig::IsConfigured() const
{
	const TArray<FString> Segments = BodyPathSegments(BodyPath);

	TSharedPtr<FJsonObject> Root;
	if (!TryReadRoot(Root))
		return false;

	TSharedPtr<FJsonObject> Body = NavigateToBody(Root, Segments);
	if (!Body.IsValid())
		return false;

	const TSharedPtr<FJsonObject>* Entry = nullptr;
	if (!Body->TryGetObjectField(FString(DefaultMcpServerName), Entry) || Entry == nullptr)
		return false;

	return AreRequiredPropertiesMatching(*Entry) && !HasAnyPropertyToRemove(*Entry);
}

bool FJsonAiAgentConfig::AreRequiredPropertiesMatching(const TSharedPtr<FJsonObject>& Entry) const
{
	if (!Entry.IsValid())
		return false;

	for (const FString& Key : PropertyOrder)
	{
		const FDesiredProperty& Prop = Properties[Key];
		if (!Prop.bRequired)
			continue;
		const TSharedPtr<FJsonValue> Existing = Entry->TryGetField(Key);
		if (!Existing.IsValid())
			return false;
		if (!AreValuesEquivalent(Prop.Comparison, Prop.Value, Existing))
			return false;
	}
	return true;
}

bool FJsonAiAgentConfig::HasAnyPropertyToRemove(const TSharedPtr<FJsonObject>& Entry) const
{
	if (!Entry.IsValid() || PropertiesToRemove.Num() == 0)
		return false;
	for (const FString& Key : PropertiesToRemove)
		if (Entry->HasField(Key))
			return true;
	return false;
}

bool FJsonAiAgentConfig::AreValuesEquivalent(EUnrealMcpValueComparison Comparison, const TSharedPtr<FJsonValue>& Expected, const TSharedPtr<FJsonValue>& Actual)
{
	if (!Expected.IsValid() || !Actual.IsValid())
		return false;

	if (Comparison == EUnrealMcpValueComparison::Url)
	{
		FString ExpectedStr, ActualStr;
		if (Expected->TryGetString(ExpectedStr) && Actual->TryGetString(ActualStr))
			return NormalizeUrl(ExpectedStr) == NormalizeUrl(ActualStr);
	}

	return JsonValueToString(Expected) == JsonValueToString(Actual);
}

FString FJsonAiAgentConfig::NormalizeUrl(const FString& Url)
{
	// Lowercase + trim a single trailing slash. Good enough for the http(s)://host:port/mcp shapes we emit
	// (no need for full URI parsing — the server URL is always our own deterministic form).
	FString Normalized = Url.ToLower();
	Normalized.TrimStartAndEndInline();
	while (Normalized.EndsWith(TEXT("/")))
		Normalized.LeftChopInline(1);
	return Normalized;
}

FString FJsonAiAgentConfig::JsonValueToString(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid())
		return FString();

	// Serialize the value into a canonical JSON string so objects/arrays/scalars all compare structurally.
	// Wrap it under a fixed key and serialize the object (the always-available object-serialize overload),
	// sidestepping the single-value writer overload's element-name handling.
	TSharedPtr<FJsonObject> Wrapper = MakeShared<FJsonObject>();
	Wrapper->SetField(TEXT("v"), Value);

	FString Out;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Wrapper.ToSharedRef(), Writer);
	return Out;
}
