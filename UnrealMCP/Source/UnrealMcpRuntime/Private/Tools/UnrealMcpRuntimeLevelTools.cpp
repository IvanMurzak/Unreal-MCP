// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRuntimeCoreTools.h"
#include "UnrealMcpToolRegistry.h"
#include "Tools/UnrealMcpObjectRef.h"
#include "Tools/UnrealMcpPropertyJson.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EngineUtils.h"            // TActorIterator
#include "Engine/World.h"
#include "Engine/Level.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

/**
 * The runtime level-data tool (docs/ARCHITECTURE.md §10 "level family" read-only runtime subset, §12.7).
 * A single read-only tool, level-get-data, moved DOWN into the Type=Runtime module so it works over a
 * runtime connection in PIE and packaged Development builds as well as the editor:
 *
 *  - `level-get-data` — an actor-tree snapshot of the world resolved by FUnrealMcpWorldProvider (the
 *    editor world in the editor, the live game world at runtime). Returns each actor's identity,
 *    optionally including reflected data scoped to the dotted `paths` filter (§3.2); pass `actor` to
 *    scope the read to a single actor.
 *
 * RUNTIME-SAFE: it touches only Engine UWorld surface (TActorIterator, IsPartitionedWorld, GetLevels,
 * GetCurrentLevel, PersistentLevel) + FUnrealMcpObjectRef / FUnrealMcpPropertyJson — no GEditor, no
 * UnrealEd. The actor label is read via GetActorNameOrLabel (the friendly label in the editor, the
 * object name at runtime; GetActorLabel itself is WITH_EDITOR-only). All level WRITE tools
 * (level-create/open/save/set-current/unload-sublevel) and the editor read-only level-list-loaded stay
 * editor-only in the editor module's UnrealMcpLevelTools — they need UnrealEd (UEditorLoadingAndSavingUtils,
 * UEditorLevelUtils). The handler runs ON the game thread (the §4 dispatcher).
 */
namespace
{
	// --- Local helpers (family-unique names per the unity-build ODR rule; mirror the editor level family) ---

	/** An `array` of strings — the §3.2 scoped-read `paths` filter. */
	TSharedPtr<FJsonObject> RuntimeLevelMakeStringArraySchema(const FString& Desc)
	{
		TSharedPtr<FJsonObject> Items = MakeShared<FJsonObject>();
		Items->SetStringField(TEXT("type"), TEXT("string"));

		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("array"));
		if (!Desc.IsEmpty())
			Schema->SetStringField(TEXT("description"), Desc);
		Schema->SetObjectField(TEXT("items"), Items);
		return Schema;
	}

	/** Read a string array argument (the scoped-read `paths` filter); a non-string entry fails via OutError. */
	TArray<FString> RuntimeLevelGetStringArray(const FUnrealMcpToolCall& Call, const FString& Key, FString& OutError)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Call.Arguments->TryGetArrayField(Key, Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				FString S;
				if (V.IsValid() && V->TryGetString(S))
				{
					Out.Add(S);
				}
				else
				{
					OutError = FString::Printf(TEXT("'%s' must be an array of strings; a non-string entry was provided."), *Key);
					Out.Reset();
					return Out;
				}
			}
		}
		return Out;
	}

	/** Long package name of a level's outermost package (e.g. /Game/Maps/Arena); empty when null. */
	FString RuntimeLevelPackageName(const ULevel* Level)
	{
		if (!Level)
			return FString();
		const UPackage* Package = Level->GetOutermost();
		return Package ? Package->GetName() : FString();
	}

	/** Short, content-browser-style name of a level (e.g. "Arena"); empty when null. */
	FString RuntimeLevelShortName(const ULevel* Level)
	{
		const FString PackageName = RuntimeLevelPackageName(Level);
		return PackageName.IsEmpty() ? FString() : FPackageName::GetShortName(PackageName);
	}

	/** A `{ name, package, isPersistent, isCurrent }` identity block for a level loaded in @p World. */
	TSharedPtr<FJsonObject> RuntimeLevelIdentity(const ULevel* Level, const UWorld* World)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("name"), RuntimeLevelShortName(Level));
		Out->SetStringField(TEXT("package"), RuntimeLevelPackageName(Level));
		const bool bPersistent = World && Level == World->PersistentLevel;
		Out->SetBoolField(TEXT("isPersistent"), bPersistent);
		Out->SetBoolField(TEXT("isCurrent"), World && Level == World->GetCurrentLevel());
		return Out;
	}
}

namespace UnrealMcpRuntimeLevelTools
{
	UNREALMCPRUNTIME_API void Register(FUnrealMcpToolRegistry& Registry)
	{
		// level-get-data — actor-tree snapshot of the resolved world; scoped reads via 'paths' (§3.2).
		Registry.Tool(TEXT("level-get-data"))
			.Title(TEXT("Get Level Data"))
			.Description(TEXT("Read an actor-tree snapshot of the active world (the editor world in the editor, the "
			                  "live game world over a runtime connection). Returns each actor's identity, optionally "
			                  "including reflected data scoped to the dotted 'paths' filter (§3.2) to save tokens. "
			                  "Pass 'actor' to scope the read to a single actor instead of the whole world."))
			.ParamString(TEXT("actor"), TEXT("Label / name / path of a single actor to read. Omit to snapshot the whole world."))
			.Param(TEXT("paths"), TEXT("array"), TEXT("Dotted property paths to include per actor (scoped read). Identity only when omitted."), EUnrealMcpParamRequirement::Optional, RuntimeLevelMakeStringArraySchema(TEXT("Dotted property paths to include per actor (scoped read).")))
			.ParamInt(TEXT("limit"), TEXT("Maximum number of actors to return for a world snapshot. Defaults to 200; pass 0 or a negative value for no limit."))
			.ReadOnlyHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				UWorld* World = FUnrealMcpObjectRef::GetEditorWorld();
				if (!World)
					return FUnrealMcpToolResult::Error(TEXT("No active world available."));

				FString PathsError;
				const TArray<FString> Paths = RuntimeLevelGetStringArray(Call, TEXT("paths"), PathsError);
				if (!PathsError.IsEmpty())
					return FUnrealMcpToolResult::Error(PathsError);

				// Single-actor scope.
				const FString ActorRef = Call.GetString(TEXT("actor"));
				if (!ActorRef.IsEmpty())
				{
					AActor* Actor = FUnrealMcpObjectRef::ResolveActor(ActorRef, World);
					if (!Actor)
						return FUnrealMcpToolResult::Error(FString::Printf(TEXT("No actor matched '%s' (by label/name/path)."), *ActorRef));

					TSharedPtr<FJsonObject> Entry = FUnrealMcpObjectRef::ActorIdentity(Actor);
					// Attach reflected data only when a scoped 'paths' filter is given — matching the whole-world
					// branch and the "Identity only when omitted" contract (an empty Paths returns the full dump).
					if (Paths.Num() > 0)
						Entry->SetObjectField(TEXT("data"), FUnrealMcpPropertyJson::SerializeObject(Actor, Paths));
					return FUnrealMcpToolResult::Success(
						FString::Printf(TEXT("Read actor '%s'."), *Actor->GetActorNameOrLabel()), Entry);
				}

				// Whole-world snapshot.
				const int32 Limit = static_cast<int32>(Call.GetInt(TEXT("limit"), 200));
				TArray<TSharedPtr<FJsonValue>> Actors;
				int32 Total = 0;
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					AActor* Actor = *It;
					if (!Actor)
						continue;
					++Total;
					if (Limit > 0 && Actors.Num() >= Limit)
						continue; // keep counting the total, stop materializing entries

					TSharedPtr<FJsonObject> Entry = FUnrealMcpObjectRef::ActorIdentity(Actor);
					if (Paths.Num() > 0)
						Entry->SetObjectField(TEXT("data"), FUnrealMcpPropertyJson::SerializeObject(Actor, Paths));
					Actors.Add(MakeShared<FJsonValueObject>(Entry));
				}

				TSharedPtr<FJsonObject> Structured = RuntimeLevelIdentity(World->GetCurrentLevel(), World);
				Structured->SetStringField(TEXT("world"), World->GetName());
				Structured->SetBoolField(TEXT("isPartitionedWorld"), World->IsPartitionedWorld());
				Structured->SetNumberField(TEXT("levelCount"), World->GetLevels().Num());
				Structured->SetNumberField(TEXT("count"), Actors.Num());
				Structured->SetNumberField(TEXT("total"), Total);
				Structured->SetArrayField(TEXT("actors"), Actors);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Snapshot of world '%s': %d actor(s) (returned %d)."), *World->GetName(), Total, Actors.Num()),
					Structured);
			});
	}
}
