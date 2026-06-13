// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Agents/TomlAiAgentConfig.h"

#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"

FTomlAiAgentConfig::FTomlAiAgentConfig(const FString& InName, const FString& InConfigPath, const FString& InBodyPath)
	: FAiAgentConfig(InName, InConfigPath, InBodyPath)
{
	// Default identity keys mirror the JSON config: a sibling section sharing our command or url is the same server.
	IdentityKeys = { TEXT("command"), TEXT("url") };
}

FTomlAiAgentConfig& FTomlAiAgentConfig::SetStringProperty(const FString& Key, const FString& Value, bool bRequired, EUnrealMcpValueComparison Comparison)
{
	if (!Properties.Contains(Key))
		PropertyOrder.Add(Key);
	FDesiredValue V;
	V.Kind = EValueKind::String;
	V.StringValue = Value;
	V.bRequired = bRequired;
	V.Comparison = Comparison;
	Properties.Add(Key, V);
	PropertiesToRemove.Remove(Key);
	return *this;
}

FTomlAiAgentConfig& FTomlAiAgentConfig::SetStringArrayProperty(const FString& Key, const TArray<FString>& Values, bool bRequired)
{
	if (!Properties.Contains(Key))
		PropertyOrder.Add(Key);
	FDesiredValue V;
	V.Kind = EValueKind::StringArray;
	V.ArrayValue = Values;
	V.bRequired = bRequired;
	Properties.Add(Key, V);
	PropertiesToRemove.Remove(Key);
	return *this;
}

FTomlAiAgentConfig& FTomlAiAgentConfig::SetIntProperty(const FString& Key, int32 Value, bool bRequired)
{
	if (!Properties.Contains(Key))
		PropertyOrder.Add(Key);
	FDesiredValue V;
	V.Kind = EValueKind::Int;
	V.IntValue = Value;
	V.bRequired = bRequired;
	Properties.Add(Key, V);
	PropertiesToRemove.Remove(Key);
	return *this;
}

FTomlAiAgentConfig& FTomlAiAgentConfig::SetBoolProperty(const FString& Key, bool Value, bool bRequired)
{
	if (!Properties.Contains(Key))
		PropertyOrder.Add(Key);
	FDesiredValue V;
	V.Kind = EValueKind::Bool;
	V.BoolValue = Value;
	V.bRequired = bRequired;
	Properties.Add(Key, V);
	PropertiesToRemove.Remove(Key);
	return *this;
}

FTomlAiAgentConfig& FTomlAiAgentConfig::SetPropertyToRemove(const FString& Key)
{
	if (Properties.Contains(Key))
	{
		Properties.Remove(Key);
		PropertyOrder.Remove(Key);
	}
	PropertiesToRemove.AddUnique(Key);
	return *this;
}

FTomlAiAgentConfig& FTomlAiAgentConfig::AddIdentityKey(const FString& Key)
{
	IdentityKeys.AddUnique(Key);
	return *this;
}

FString FTomlAiAgentConfig::SectionName() const
{
	return FString::Printf(TEXT("%s.%s"), *BodyPath, DefaultMcpServerName);
}

FString FTomlAiAgentConfig::FormatProperty(const FString& Key, const FDesiredValue& Value)
{
	switch (Value.Kind)
	{
		case EValueKind::String:
		{
			FString Escaped = Value.StringValue.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
			return FString::Printf(TEXT("%s = \"%s\""), *Key, *Escaped);
		}
		case EValueKind::StringArray:
		{
			TArray<FString> Quoted;
			for (const FString& E : Value.ArrayValue)
			{
				FString Escaped = E.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
				Quoted.Add(FString::Printf(TEXT("\"%s\""), *Escaped));
			}
			return FString::Printf(TEXT("%s = [%s]"), *Key, *FString::Join(Quoted, TEXT(",")));
		}
		case EValueKind::Int:
			return FString::Printf(TEXT("%s = %d"), *Key, Value.IntValue);
		case EValueKind::Bool:
			return FString::Printf(TEXT("%s = %s"), *Key, Value.BoolValue ? TEXT("true") : TEXT("false"));
		default:
			return FString();
	}
}

FString FTomlAiAgentConfig::FormatRawProperty(const FString& Key, const FString& RawValue)
{
	return FString::Printf(TEXT("%s = %s"), *Key, *RawValue);
}

TMap<FString, FString> FTomlAiAgentConfig::DesiredRawProperties() const
{
	TMap<FString, FString> Raw;
	for (const FString& Key : PropertyOrder)
	{
		const FDesiredValue& V = Properties[Key];
		// Build the raw TOML literal (right-hand side of "key = ") by formatting then trimming the "key = " prefix.
		const FString Formatted = FormatProperty(Key, V);
		const FString Prefix = Key + TEXT(" = ");
		Raw.Add(Key, Formatted.RightChop(Prefix.Len()));
	}
	return Raw;
}

FString FTomlAiAgentConfig::BuildSection(const FString& InSectionName, const TMap<FString, FString>& Props) const
{
	TArray<FString> Keys;
	Props.GetKeys(Keys);
	Keys.Sort(); // deterministic alphabetical order (matches Unity's OrderBy(Ordinal))

	TArray<FString> Lines;
	Lines.Add(FString::Printf(TEXT("[%s]"), *InSectionName));
	for (const FString& Key : Keys)
		Lines.Add(FormatRawProperty(Key, Props[Key]));
	return FString::Join(Lines, TEXT("\n"));
}

FString FTomlAiAgentConfig::GetExpectedFileContent() const
{
	return BuildSection(SectionName(), DesiredRawProperties()) + TEXT("\n");
}

bool FTomlAiAgentConfig::TryReadLines(const FString& Path, TArray<FString>& OutLines)
{
	OutLines.Reset();
	if (Path.IsEmpty() || !FPaths::FileExists(Path))
		return false;
	return FFileHelper::LoadFileToStringArray(OutLines, *Path);
}

bool FTomlAiAgentConfig::WriteLines(const TArray<FString>& Lines) const
{
	if (ConfigPath.IsEmpty())
		return false;
	const FString Dir = FPaths::GetPath(ConfigPath);
	if (!Dir.IsEmpty())
		IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
	// Force UTF-8 without a BOM (the TOML parsers in agent CLIs expect plain UTF-8). Join with "\n" lines.
	const FString Content = FString::Join(Lines, TEXT("\n")) + TEXT("\n");
	return FFileHelper::SaveStringToFile(Content, *ConfigPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

int32 FTomlAiAgentConfig::FindSection(const TArray<FString>& Lines, const FString& InSectionName)
{
	const FString Header = FString::Printf(TEXT("[%s]"), *InSectionName);
	for (int32 i = 0; i < Lines.Num(); ++i)
		if (Lines[i].TrimStartAndEnd() == Header)
			return i;
	return INDEX_NONE;
}

int32 FTomlAiAgentConfig::FindSectionEnd(const TArray<FString>& Lines, int32 SectionStart)
{
	for (int32 i = SectionStart + 1; i < Lines.Num(); ++i)
	{
		const FString Trimmed = Lines[i].TrimStartAndEnd();
		if (Trimmed.StartsWith(TEXT("[")) && Trimmed.EndsWith(TEXT("]")))
			return i;
	}
	return Lines.Num();
}

TMap<FString, FString> FTomlAiAgentConfig::ParseSectionProps(const TArray<FString>& Lines, int32 Start, int32 End)
{
	TMap<FString, FString> Props;
	for (int32 i = Start; i < End; ++i)
	{
		const FString Trimmed = Lines[i].TrimStartAndEnd();
		if (Trimmed.IsEmpty() || Trimmed.StartsWith(TEXT("#")))
			continue;
		int32 EqIndex;
		if (!Trimmed.FindChar(TEXT('='), EqIndex))
			continue;
		const FString Key = Trimmed.Left(EqIndex).TrimStartAndEnd();
		const FString RawValue = Trimmed.RightChop(EqIndex + 1).TrimStartAndEnd();
		if (!Key.IsEmpty())
			Props.Add(Key, RawValue); // store the raw TOML literal verbatim (round-trips opaque values)
	}
	return Props;
}

TArray<TPair<int32, int32>> FTomlAiAgentConfig::FindDuplicateSectionRanges(const TArray<FString>& Lines, const FString& OwnSection) const
{
	TArray<TPair<int32, int32>> Result;

	// Our identity values (only those we set as desired string properties).
	TArray<TPair<FString, FDesiredValue>> OurIdentities;
	for (const FString& IdKey : IdentityKeys)
	{
		if (const FDesiredValue* V = Properties.Find(IdKey))
			if (V->Kind == EValueKind::String)
				OurIdentities.Add(TPair<FString, FDesiredValue>(IdKey, *V));
	}
	if (OurIdentities.Num() == 0)
		return Result;

	const FString BodyPrefix = FString::Printf(TEXT("[%s."), *BodyPath);
	const FString OwnHeader = FString::Printf(TEXT("[%s]"), *OwnSection);

	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		const FString Trimmed = Lines[i].TrimStartAndEnd();
		if (!Trimmed.StartsWith(BodyPrefix) || !Trimmed.EndsWith(TEXT("]")))
			continue;
		if (Trimmed == OwnHeader)
			continue;

		const int32 SectionEnd = FindSectionEnd(Lines, i);
		const TMap<FString, FString> Props = ParseSectionProps(Lines, i + 1, SectionEnd);

		for (const auto& Identity : OurIdentities)
		{
			if (const FString* Existing = Props.Find(Identity.Key))
			{
				if (RawMatchesDesired(Identity.Value, *Existing))
				{
					Result.Add(TPair<int32, int32>(i, SectionEnd));
					break;
				}
			}
		}
	}
	return Result;
}

bool FTomlAiAgentConfig::Configure()
{
	if (ConfigPath.IsEmpty())
		return false;

	const FString Section = SectionName();

	TArray<FString> Lines;
	if (!TryReadLines(ConfigPath, Lines))
	{
		// Missing / unreadable file → write a clean expected-content file.
		const FString Dir = FPaths::GetPath(ConfigPath);
		if (!Dir.IsEmpty())
			IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
		if (!FFileHelper::SaveStringToFile(GetExpectedFileContent(), *ConfigPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			return false;
		return IsConfigured();
	}

	// Remove deprecated sections.
	for (const FString& Deprecated : DeprecatedMcpServerNames())
	{
		const FString DeprecatedSection = FString::Printf(TEXT("%s.%s"), *BodyPath, *Deprecated);
		const int32 DepIndex = FindSection(Lines, DeprecatedSection);
		if (DepIndex != INDEX_NONE)
		{
			const int32 DepEnd = FindSectionEnd(Lines, DepIndex);
			Lines.RemoveAt(DepIndex, DepEnd - DepIndex);
		}
	}

	// Remove duplicate sibling sections (same server under a different name). Remove bottom-up to keep indices valid.
	TArray<TPair<int32, int32>> Dups = FindDuplicateSectionRanges(Lines, Section);
	for (int32 d = Dups.Num() - 1; d >= 0; --d)
		Lines.RemoveAt(Dups[d].Key, Dups[d].Value - Dups[d].Key);

	// Merge desired props onto the existing section (or append a fresh one).
	const int32 SectionIndex = FindSection(Lines, Section);
	if (SectionIndex != INDEX_NONE)
	{
		const int32 SectionEnd = FindSectionEnd(Lines, SectionIndex);
		TMap<FString, FString> Merged = ParseSectionProps(Lines, SectionIndex + 1, SectionEnd);

		for (const FString& Key : PropertiesToRemove)
			Merged.Remove(Key);

		const TMap<FString, FString> Desired = DesiredRawProperties();
		for (const auto& Pair : Desired)
			Merged.Add(Pair.Key, Pair.Value);

		// Replace the old section lines with the rebuilt section.
		Lines.RemoveAt(SectionIndex, SectionEnd - SectionIndex);
		TArray<FString> NewSectionLines;
		BuildSection(Section, Merged).ParseIntoArray(NewSectionLines, TEXT("\n"), /*CullEmpty*/ false);
		Lines.Insert(NewSectionLines, SectionIndex);
	}
	else
	{
		// Append, separated by a blank line if the file does not already end on one.
		if (Lines.Num() > 0 && !Lines.Last().TrimStartAndEnd().IsEmpty())
			Lines.Add(TEXT(""));
		TArray<FString> NewSectionLines;
		BuildSection(Section, DesiredRawProperties()).ParseIntoArray(NewSectionLines, TEXT("\n"), /*CullEmpty*/ false);
		Lines.Append(NewSectionLines);
	}

	if (!WriteLines(Lines))
		return false;

	return IsConfigured();
}

bool FTomlAiAgentConfig::Remove()
{
	if (ConfigPath.IsEmpty())
		return false;

	TArray<FString> Lines;
	if (!TryReadLines(ConfigPath, Lines))
		return false;

	const FString Section = SectionName();
	bool bRemoved = false;

	const int32 SectionIndex = FindSection(Lines, Section);
	if (SectionIndex != INDEX_NONE)
	{
		const int32 SectionEnd = FindSectionEnd(Lines, SectionIndex);
		Lines.RemoveAt(SectionIndex, SectionEnd - SectionIndex);
		bRemoved = true;
	}

	for (const FString& Deprecated : DeprecatedMcpServerNames())
	{
		const FString DeprecatedSection = FString::Printf(TEXT("%s.%s"), *BodyPath, *Deprecated);
		const int32 DepIndex = FindSection(Lines, DeprecatedSection);
		if (DepIndex != INDEX_NONE)
		{
			const int32 DepEnd = FindSectionEnd(Lines, DepIndex);
			Lines.RemoveAt(DepIndex, DepEnd - DepIndex);
			bRemoved = true;
		}
	}

	TArray<TPair<int32, int32>> Dups = FindDuplicateSectionRanges(Lines, Section);
	for (int32 d = Dups.Num() - 1; d >= 0; --d)
	{
		Lines.RemoveAt(Dups[d].Key, Dups[d].Value - Dups[d].Key);
		bRemoved = true;
	}

	if (!bRemoved)
		return false;

	return WriteLines(Lines);
}

bool FTomlAiAgentConfig::IsDetected() const
{
	TArray<FString> Lines;
	if (!TryReadLines(ConfigPath, Lines))
		return false;

	if (FindSection(Lines, SectionName()) != INDEX_NONE)
		return true;

	for (const FString& Deprecated : DeprecatedMcpServerNames())
		if (FindSection(Lines, FString::Printf(TEXT("%s.%s"), *BodyPath, *Deprecated)) != INDEX_NONE)
			return true;

	return FindDuplicateSectionRanges(Lines, SectionName()).Num() > 0;
}

bool FTomlAiAgentConfig::IsConfigured() const
{
	TArray<FString> Lines;
	if (!TryReadLines(ConfigPath, Lines))
		return false;

	const int32 SectionIndex = FindSection(Lines, SectionName());
	if (SectionIndex == INDEX_NONE)
		return false;

	const int32 SectionEnd = FindSectionEnd(Lines, SectionIndex);
	const TMap<FString, FString> Existing = ParseSectionProps(Lines, SectionIndex + 1, SectionEnd);

	return AreRequiredPropertiesMatching(Existing) && !HasAnyPropertyToRemove(Existing);
}

bool FTomlAiAgentConfig::AreRequiredPropertiesMatching(const TMap<FString, FString>& Existing) const
{
	for (const FString& Key : PropertyOrder)
	{
		const FDesiredValue& V = Properties[Key];
		if (!V.bRequired)
			continue;
		const FString* RawExisting = Existing.Find(Key);
		if (!RawExisting)
			return false;
		if (!RawMatchesDesired(V, *RawExisting))
			return false;
	}
	return true;
}

bool FTomlAiAgentConfig::HasAnyPropertyToRemove(const TMap<FString, FString>& Existing) const
{
	for (const FString& Key : PropertiesToRemove)
		if (Existing.Contains(Key))
			return true;
	return false;
}

bool FTomlAiAgentConfig::RawMatchesDesired(const FDesiredValue& Desired, const FString& RawExisting)
{
	const FString Trimmed = RawExisting.TrimStartAndEnd();
	switch (Desired.Kind)
	{
		case EValueKind::String:
		{
			const FString Existing = StripQuotes(Trimmed);
			if (Desired.Comparison == EUnrealMcpValueComparison::Url)
				return NormalizeUrl(Desired.StringValue) == NormalizeUrl(Existing);
			if (Desired.Comparison == EUnrealMcpValueComparison::Path)
				return NormalizePath(Desired.StringValue) == NormalizePath(Existing);
			return Desired.StringValue == Existing;
		}
		case EValueKind::StringArray:
		{
			// Compare the raw existing array literal against a freshly-formatted desired literal (both deterministic).
			FDesiredValue Tmp = Desired;
			const FString DesiredLiteral = FormatProperty(TEXT("k"), Tmp).RightChop(FString(TEXT("k = ")).Len());
			return DesiredLiteral == Trimmed;
		}
		case EValueKind::Int:
		{
			int32 ExistingInt = 0;
			return LexTryParseString(ExistingInt, *Trimmed) && ExistingInt == Desired.IntValue;
		}
		case EValueKind::Bool:
		{
			const FString Lower = Trimmed.ToLower();
			const bool bExisting = (Lower == TEXT("true"));
			const bool bValidBool = (Lower == TEXT("true") || Lower == TEXT("false"));
			return bValidBool && bExisting == Desired.BoolValue;
		}
		default:
			return false;
	}
}

FString FTomlAiAgentConfig::StripQuotes(const FString& Raw)
{
	FString S = Raw.TrimStartAndEnd();
	if (S.Len() >= 2 && S.StartsWith(TEXT("\"")) && S.EndsWith(TEXT("\"")))
	{
		S = S.Mid(1, S.Len() - 2);
		// Unescape the two escapes our writer emits.
		S = S.Replace(TEXT("\\\""), TEXT("\"")).Replace(TEXT("\\\\"), TEXT("\\"));
	}
	return S;
}

FString FTomlAiAgentConfig::NormalizeUrl(const FString& Url)
{
	FString Normalized = Url.ToLower();
	Normalized.TrimStartAndEndInline();
	while (Normalized.EndsWith(TEXT("/")))
		Normalized.LeftChopInline(1);
	return Normalized;
}

FString FTomlAiAgentConfig::NormalizePath(const FString& Path)
{
	FString Normalized = Path.Replace(TEXT("\\"), TEXT("/"));
	while (Normalized.EndsWith(TEXT("/")))
		Normalized.LeftChopInline(1);
	return Normalized;
}
