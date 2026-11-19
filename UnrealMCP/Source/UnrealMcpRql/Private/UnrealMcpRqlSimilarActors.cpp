// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRqlSources.h"

#include "UnrealMcpRqlEditorApi.h"
#include "UnrealMcpRqlUtil.h"
#include "UnrealMcpRqlWorldPartition.h"
#include "Tools/UnrealMcpObjectRef.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

namespace UnrealMcpRqlSimilarActorsLocal
{
	/** Labels read at most, loaded and not loaded together: a typo in a huge level must not cost a full sweep. */
	constexpr int32 MaxScanned = 20000;
	/** Longer labels are not compared (the distance is quadratic in the length). */
	constexpr int32 MaxLabelLength = 128;
	constexpr int32 MaxSuggestions = 3;

	/** @p Text lower-cased without whitespace: "Island ", "island" and "Is land" all read "island". */
	FString Folded(const FString& Text)
	{
		FString Out;
		Out.Reserve(Text.Len());
		for (const TCHAR Char : Text)
		{
			if (!FChar::IsWhitespace(Char))
				Out.AppendChar(FChar::ToLower(Char));
		}
		return Out;
	}

	/**
	 * Edit distance from @p Wanted to @p Label (@p OutFull) and to the closest prefix of @p Label (@p OutPrefix: "islnd" is 1
	 * from "island_old"). Both are Limit + 1 once they exceed @p Limit.
	 */
	void Distances(const FString& Wanted, const FString& Label, int32 Limit, int32& OutFull, int32& OutPrefix)
	{
		// Rows over the label, one per character of the wanted text; Row[j] = distance from the wanted text so far to Label[0, j).
		TArray<int32, TInlineAllocator<MaxLabelLength + 1>> Row;
		Row.SetNumUninitialized(Label.Len() + 1);
		for (int32 J = 0; J <= Label.Len(); ++J)
			Row[J] = J;
		for (int32 I = 1; I <= Wanted.Len(); ++I)
		{
			int32 Diagonal = Row[0];
			Row[0] = I;
			int32 Best = Row[0];
			for (int32 J = 1; J <= Label.Len(); ++J)
			{
				const int32 Above = Row[J];
				Row[J] = FMath::Min3(Above + 1, Row[J - 1] + 1, Diagonal + (Wanted[I - 1] == Label[J - 1] ? 0 : 1));
				Diagonal = Above;
				Best = FMath::Min(Best, Row[J]);
			}
			if (Best > Limit)
			{
				OutFull = OutPrefix = Limit + 1;
				return;
			}
		}
		OutFull = FMath::Min(Row[Label.Len()], Limit + 1);
		OutPrefix = Limit + 1;
		for (int32 J = 1; J <= Label.Len(); ++J) // a non-empty prefix: the empty one is Wanted.Len() from any label
			OutPrefix = FMath::Min(OutPrefix, Row[J]);
	}

	/** Keys shorter than this match only whole labels: a short key is close to the start of too many. */
	constexpr int32 MinPartialKey = 4;

	struct FCandidate
	{
		FString Label;
		FString Class;
		bool bLoaded = true;
		bool bPreferred = false;
		int32 Score = 0;
	};
}

static FString UnrealMcpRqlSimilarActorsScan(UWorld* World, const FString& Wanted, UClass* Preferred, const TArray<FUnrealMcpUnloadedActor>* InUnloaded);

/** The answers of the running request (RQL calls a function again for every row: a misspelt unreal.find in a per-row
 * predicate must not scan the level once per row). Emptied when a request starts. */
static TMap<FString, FString> GUnrealMcpRqlSimilarActorsCache;

void UnrealMcpRqlForgetSimilarActors()
{
	GUnrealMcpRqlSimilarActorsCache.Reset();
	UnrealMcpRqlWorldPartition::ForgetUnloaded(); // the unloaded rows it scans are kept for one request too
}

FString UnrealMcpRqlSimilarActors(const FString& Text, UClass* Preferred, const TArray<FUnrealMcpUnloadedActor>* InUnloaded)
{
	using namespace UnrealMcpRqlSimilarActorsLocal;
	const FString Wanted = Folded(Text);
	UWorld* World = UnrealMcpRqlUtil::EditorWorld();
	if (Wanted.IsEmpty() || Wanted.Len() > MaxLabelLength || !World)
		return FString();
	const FString CacheKey = Wanted + TEXT("|") + (Preferred ? Preferred->GetPathName() : FString()) + TEXT("|") + World->GetPathName();
	if (const FString* Hit = GUnrealMcpRqlSimilarActorsCache.Find(CacheKey))
		return *Hit;
	return GUnrealMcpRqlSimilarActorsCache.Add(CacheKey, UnrealMcpRqlSimilarActorsScan(World, Wanted, Preferred, InUnloaded));
}

static FString UnrealMcpRqlSimilarActorsScan(UWorld* World, const FString& Wanted, UClass* Preferred, const TArray<FUnrealMcpUnloadedActor>* InUnloaded)
{
	using namespace UnrealMcpRqlSimilarActorsLocal;
	// Tolerated typos: 1 up to 5 characters, 2 up to 8, then 3.
	const int32 Limit = FMath::Clamp(Wanted.Len() / 3, 1, 3);
	TArray<FCandidate> Hits;
	int32 Scanned = 0;
	// Score, lower first, by tier then distance: the same text but for case or whitespace, a whole-label typo, a typo of
	// the label's start, the text inside the label.
	auto Consider = [&](const FString& Label, const FString& Class, bool bLoaded, bool bPreferred)
	{
		++Scanned;
		const FString Key = Folded(Label);
		if (Key.IsEmpty() || Key.Len() > MaxLabelLength)
			return;
		int32 Score = 0;
		if (Key != Wanted)
		{
			int32 Full = 0, Prefix = 0;
			Distances(Wanted, Key, Limit, Full, Prefix);
			if (Full <= Limit)
				Score = 10 + Full;
			else if (Wanted.Len() >= MinPartialKey && Prefix <= Limit)
				Score = 20 + Prefix;
			else if (Wanted.Len() >= MinPartialKey && Key.Contains(Wanted))
				Score = 30;
			else
				return;
		}
		Hits.Add({ Label, Class, bLoaded, bPreferred, Score });
	};
	for (TActorIterator<AActor> It(World); It && Scanned < MaxScanned; ++It)
		Consider(It->GetActorLabel(), It->GetClass()->GetName(), true, Preferred && It->IsA(Preferred));
	const TArray<FUnrealMcpUnloadedActor>& Unloaded = InUnloaded ? *InUnloaded : UnrealMcpRqlWorldPartition::UnloadedForRequest(World);
	TMap<FString, bool> PreferredClasses; // descriptor class name -> is a Preferred
	for (const FUnrealMcpUnloadedActor& Row : Unloaded)
	{
		if (Scanned >= MaxScanned)
			break;
		bool bPreferred = false;
		if (Preferred)
		{
			if (const bool* Known = PreferredClasses.Find(Row.Class))
				bPreferred = *Known;
			else
			{
				const UClass* Class = FUnrealMcpObjectRef::ResolveClass(Row.Class);
				bPreferred = PreferredClasses.Add(Row.Class, Class && Class->IsChildOf(Preferred));
			}
		}
		Consider(Row.Label, Row.Class, false, bPreferred);
	}
	if (Hits.IsEmpty())
		return FString();
	Hits.Sort([](const FCandidate& A, const FCandidate& B)
	{
		if (A.bPreferred != B.bPreferred)
			return A.bPreferred;
		if (A.Score != B.Score)
			return A.Score < B.Score;
		if (A.bLoaded != B.bLoaded)
			return A.bLoaded;
		return A.Label < B.Label;
	});
	TArray<FString> Named;
	for (const FCandidate& Hit : Hits)
	{
		const FString Entry = FString::Printf(TEXT("'%s' (%s%s)"), *Hit.Label, *Hit.Class, Hit.bLoaded ? TEXT("") : TEXT(", not loaded"));
		Named.AddUnique(Entry);
		if (Named.Num() == MaxSuggestions)
			break;
	}
	return FString::Printf(TEXT("did you mean %s?"), *FString::Join(Named, TEXT(", ")));
}
