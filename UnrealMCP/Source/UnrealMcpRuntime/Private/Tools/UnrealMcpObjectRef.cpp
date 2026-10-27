// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpObjectRef.h"
#include "UnrealMcpWorldProvider.h"

#include "Engine/Blueprint.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"            // TActorIterator
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	// AActor::GetActorLabel() is WITH_EDITOR-only (it lives behind the editor-only label storage), so it
	// cannot be called from a Type=Runtime module that BuildPlugin compiles in a non-editor configuration.
	// In the editor we return the friendly label byte-for-byte as before; outside the editor (packaged game,
	// where labels do not exist) we fall back to the object name, which is the actor's only stable identifier
	// there. This preserves today's editor behaviour exactly while keeping the module packageable (§12).
	FString ObjectRefActorLabel(const AActor* Actor)
	{
#if WITH_EDITOR
		return Actor->GetActorLabel();
#else
		return Actor->GetName();
#endif
	}

	/** A level loaded in the active world by its map name ("Sub_A"), package ("/Game/Maps/Sub_A") or object path. */
	ULevel* ObjectRefLoadedLevel(const FString& Text)
	{
		UWorld* World = FUnrealMcpWorldProvider::GetActiveWorld();
		if (!World)
			return nullptr;
		// A package or object path names one level; a short map name only when exactly one loaded level has it.
		const FString Package = FPackageName::ObjectPathToPackageName(Text);
		ULevel* ByShortName = nullptr;
		int32 ShortMatches = 0;
		for (ULevel* Level : World->GetLevels())
		{
			if (!Level)
				continue;
			const FString LevelPackage = Level->GetOutermost()->GetName();
			if (LevelPackage.Equals(Package, ESearchCase::IgnoreCase) || Level->GetPathName().Equals(Text, ESearchCase::IgnoreCase))
				return Level;
			if (FPackageName::GetShortName(LevelPackage).Equals(Text, ESearchCase::IgnoreCase) && ShortMatches++ == 0)
				ByShortName = Level;
		}
		return ShortMatches == 1 ? ByShortName : nullptr;
	}
}

namespace FUnrealMcpObjectRef
{
	UWorld* GetEditorWorld()
	{
		// §12.6: the runtime module owns no GEditor reference. The world is resolved via the injected
		// FUnrealMcpWorldProvider resolver — the editor coordinator installs one returning the editor
		// world (`GEditor->GetEditorWorldContext().World()`), preserving today's behaviour byte-for-byte;
		// the runtime subsystem (R3) installs a game-world resolver. Null when no resolver is set, exactly
		// matching the old `!GEditor -> nullptr` contract outside the editor.
		return FUnrealMcpWorldProvider::GetActiveWorld();
	}

	UObject* LoadObjectQuiet(UClass* Class, const FString& InPath)
	{
		FString Path = InPath.TrimStartAndEnd();
		if (Path.IsEmpty() || !Class)
			return nullptr;
		// "/Game/Materials/M_Red" names a package: its main asset is "/Game/Materials/M_Red.M_Red".
		if (FPackageName::IsValidLongPackageName(Path))
			Path += TEXT(".") + FPackageName::GetShortName(Path);
		if (UObject* Found = StaticFindObject(Class, nullptr, *Path))
			return Found;
		// Probe first: loading a missing package logs an engine error (an Automation failure) even with LOAD_NoWarn.
		const FString Package = FPackageName::ObjectPathToPackageName(Path);
		if (!FPackageName::IsValidLongPackageName(Package) || !FPackageName::DoesPackageExist(Package))
			return nullptr;
		return StaticLoadObject(Class, nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	UClass* ResolveClass(const FString& InRef)
	{
		const FString Ref = InRef.TrimStartAndEnd();
		if (Ref.IsEmpty())
			return nullptr;
		if (Ref.Contains(TEXT("/")))
		{
			// A class path (native or a generated `_C` class), else a Blueprint asset whose generated class is meant.
			if (UClass* ByPath = Cast<UClass>(LoadObjectQuiet(UClass::StaticClass(), Ref)))
				return ByPath;
			const UBlueprint* Blueprint = Cast<UBlueprint>(LoadObjectQuiet(UBlueprint::StaticClass(), Ref));
			return Blueprint ? Blueprint->GeneratedClass.Get() : nullptr;
		}
		// A short name, without the engine's "use a path" warning (logging off). C++ spellings (AStaticMeshActor,
		// UStaticMeshComponent) name the class without its prefix.
		if (UClass* Found = FindFirstObject<UClass>(*Ref, EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging))
			return Found;
		if (Ref.Len() > 1 && (Ref[0] == TEXT('A') || Ref[0] == TEXT('U')) && FChar::IsUpper(Ref[1]))
			return FindFirstObject<UClass>(*Ref.Mid(1), EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging);
		return nullptr;
	}

	AActor* ResolveActor(const FString& ActorRef, UWorld* World)
	{
		if (ActorRef.IsEmpty())
			return nullptr;
		if (!World)
			World = GetEditorWorld();
		if (!World)
			return nullptr;

		// Exact (case-SENSITIVE) match on label / name / full path first (deterministic). FString::operator==
		// is case-INSENSITIVE in UE, so an explicit ESearchCase::CaseSensitive Equals is required to make the
		// exact-first preference real and keep the case-insensitive fallback below reachable. A single sweep:
		// any exact match returns immediately; otherwise we remember the FIRST case-insensitive label match
		// (LLM-supplied labels are often loosely cased) and return it after the sweep — same result the old
		// second pass produced (iteration order is identical), without walking every actor twice.
		AActor* IgnoreCaseFallback = nullptr;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (ObjectRefActorLabel(Actor).Equals(ActorRef, ESearchCase::CaseSensitive)
				|| Actor->GetName().Equals(ActorRef, ESearchCase::CaseSensitive)
				|| Actor->GetPathName().Equals(ActorRef, ESearchCase::CaseSensitive))
			{
				return Actor;
			}
			if (IgnoreCaseFallback == nullptr && ObjectRefActorLabel(Actor).Equals(ActorRef, ESearchCase::IgnoreCase))
				IgnoreCaseFallback = Actor;
		}

		return IgnoreCaseFallback;
	}

	UObject* ResolveReference(UClass* Wanted, const FString& InText)
	{
		const FString Text = InText.TrimStartAndEnd();
		if (!Wanted || Text.IsEmpty())
			return nullptr;
		UObject* Found = nullptr;
		if (Wanted->IsChildOf(UClass::StaticClass()))
			Found = ResolveClass(Text);
		else if (Wanted->IsChildOf(AActor::StaticClass()))
			Found = ResolveActor(Text);
		else if (Wanted->IsChildOf(ULevel::StaticClass()))
			Found = ObjectRefLoadedLevel(Text);
		else
			Found = LoadObjectQuiet(Wanted, Text);
		return Found && Found->IsA(Wanted) ? Found : nullptr;
	}

	UObject* ResolveObject(const FString& ObjectRef, UWorld* World)
	{
		if (ObjectRef.IsEmpty())
			return nullptr;

		// A live actor (by label/name/path) takes precedence so `object-*` can target scene actors.
		if (AActor* Actor = ResolveActor(ObjectRef, World))
			return Actor;

		return LoadObjectQuiet(UObject::StaticClass(), ObjectRef);
	}
}
