// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpObjectRef.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "EngineUtils.h"            // TActorIterator
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

namespace FUnrealMcpObjectRef
{
	UWorld* GetEditorWorld()
	{
		if (!GEditor)
			return nullptr;
		return GEditor->GetEditorWorldContext().World();
	}

	UClass* ResolveClass(const FString& ClassRef)
	{
		if (ClassRef.IsEmpty())
			return nullptr;

		// 1. Soft class path — handles native (`/Script/Engine.PointLight`) and generated Blueprint
		//    classes (`/Game/BP/BP_Foo.BP_Foo_C`) without forcing a synchronous asset scan first.
		if (UClass* Loaded = FSoftClassPath(ClassRef).TryLoadClass<UObject>())
			return Loaded;

		// 2. Load as a generic object: the ref may point at a UClass directly, or at a UBlueprint asset
		//    (`/Game/BP/BP_Foo.BP_Foo`) whose GeneratedClass is what callers actually want to spawn.
		//    LOAD_NoWarn | LOAD_Quiet: short, unqualified names (`PointLight`) reach here and fail this load
		//    before succeeding at step 3 — without the flags each emits a spurious LogUObjectGlobals warning.
		if (UObject* Loaded = StaticLoadObject(UObject::StaticClass(), nullptr, *ClassRef, nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			if (UClass* AsClass = Cast<UClass>(Loaded))
				return AsClass;
			if (const UBlueprint* AsBlueprint = Cast<UBlueprint>(Loaded))
				return AsBlueprint->GeneratedClass;
		}

		// 3. Short, unqualified native type name (`StaticMeshActor`, `PointLightComponent`).
		if (UClass* Found = UClass::TryFindTypeSlow<UClass>(ClassRef))
			return Found;

		return FindFirstObject<UClass>(*ClassRef, EFindFirstObjectOptions::None);
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
		// exact-first preference real and keep the case-insensitive fallback below reachable.
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (Actor->GetActorLabel().Equals(ActorRef, ESearchCase::CaseSensitive)
				|| Actor->GetName().Equals(ActorRef, ESearchCase::CaseSensitive)
				|| Actor->GetPathName().Equals(ActorRef, ESearchCase::CaseSensitive))
			{
				return Actor;
			}
		}

		// Case-insensitive label fallback (LLM-supplied labels are often loosely cased).
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->GetActorLabel().Equals(ActorRef, ESearchCase::IgnoreCase))
				return *It;
		}

		return nullptr;
	}

	UActorComponent* ResolveComponent(AActor* Actor, const FString& ComponentRef)
	{
		if (!Actor || ComponentRef.IsEmpty())
			return nullptr;

		for (UActorComponent* Component : Actor->GetComponents())
		{
			if (!Component)
				continue;
			// FString::operator== is case-insensitive in UE, so this already matches loosely-cased component
			// names — no separate IgnoreCase clause is needed.
			if (Component->GetName() == ComponentRef
				|| Component->GetReadableName() == ComponentRef)
			{
				return Component;
			}
		}
		return nullptr;
	}

	UObject* ResolveObject(const FString& ObjectRef, UWorld* World)
	{
		if (ObjectRef.IsEmpty())
			return nullptr;

		// A live actor (by label/name/path) takes precedence so `object-*` can target scene actors.
		if (AActor* Actor = ResolveActor(ObjectRef, World))
			return Actor;

		// Already-loaded object by path.
		if (UObject* Found = FindObject<UObject>(nullptr, *ObjectRef))
			return Found;

		// Soft path load (asset on disk not yet in memory).
		if (UObject* Loaded = FSoftObjectPath(ObjectRef).TryLoad())
			return Loaded;

		// Last-resort fallback for refs that FSoftObjectPath's strict parsing rejects (e.g. legacy
		// `Package.Object` short forms). Overlaps TryLoad for well-formed paths; kept for that long tail.
		return StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectRef);
	}

	TSharedPtr<FJsonObject> ActorIdentity(const AActor* Actor)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		if (!Actor)
			return Json;
		Json->SetStringField(TEXT("name"), Actor->GetName());
		Json->SetStringField(TEXT("label"), Actor->GetActorLabel());
		Json->SetStringField(TEXT("class"), Actor->GetClass() ? Actor->GetClass()->GetPathName() : FString());
		Json->SetStringField(TEXT("path"), Actor->GetPathName());
		return Json;
	}

	TSharedPtr<FJsonObject> ComponentIdentity(const UActorComponent* Component)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		if (!Component)
			return Json;
		Json->SetStringField(TEXT("name"), Component->GetName());
		Json->SetStringField(TEXT("class"), Component->GetClass() ? Component->GetClass()->GetPathName() : FString());
		Json->SetStringField(TEXT("path"), Component->GetPathName());
		return Json;
	}
}
