// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpPropertyJson.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "JsonObjectConverter.h"
#include "GameFramework/Actor.h"
#include "Components/SceneComponent.h"
#include "UObject/UnrealType.h"
#include "UObject/Class.h"

namespace
{
	/** Read a {x,y,z} object into an FVector; returns true only when ≥1 recognized axis key was present
	 *  (an empty or typo'd object must NOT count as an applied no-op write). */
	bool JsonToVector(const TSharedPtr<FJsonValue>& Value, FVector& Out)
	{
		const TSharedPtr<FJsonObject>* Obj;
		if (!Value.IsValid() || !Value->TryGetObject(Obj) || !Obj->IsValid())
			return false;
		bool bAny = false;
		double C;
		if ((*Obj)->TryGetNumberField(TEXT("x"), C)) { Out.X = C; bAny = true; }
		if ((*Obj)->TryGetNumberField(TEXT("y"), C)) { Out.Y = C; bAny = true; }
		if ((*Obj)->TryGetNumberField(TEXT("z"), C)) { Out.Z = C; bAny = true; }
		return bAny;
	}

	/** Read a {pitch,yaw,roll} object into an FRotator; returns true only when ≥1 recognized key was
	 *  present (an empty or typo'd object must NOT count as an applied no-op write). */
	bool JsonToRotator(const TSharedPtr<FJsonValue>& Value, FRotator& Out)
	{
		const TSharedPtr<FJsonObject>* Obj;
		if (!Value.IsValid() || !Value->TryGetObject(Obj) || !Obj->IsValid())
			return false;
		bool bAny = false;
		double C;
		if ((*Obj)->TryGetNumberField(TEXT("pitch"), C)) { Out.Pitch = C; bAny = true; }
		if ((*Obj)->TryGetNumberField(TEXT("yaw"),   C)) { Out.Yaw = C; bAny = true; }
		if ((*Obj)->TryGetNumberField(TEXT("roll"),  C)) { Out.Roll = C; bAny = true; }
		return bAny;
	}

	/** Find a reflected property by its JSON key (case-insensitive — keys are lower-first-cased, §3.2). */
	FProperty* FindPropertyByJsonName(const UStruct* Struct, const FString& Key)
	{
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			if (It->GetName().Equals(Key, ESearchCase::IgnoreCase))
				return *It;
		}
		return nullptr;
	}
}

namespace FUnrealMcpPropertyJson
{
	TSharedPtr<FJsonObject> SerializeObject(const UObject* Object, const TArray<FString>& Paths)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		if (!Object)
			return Json;

		// Skip transient/deprecated and editor-only-noise properties from the schema-facing output.
		const int64 SkipFlags = CPF_Transient | CPF_Deprecated;
		FJsonObjectConverter::UStructToJsonObject(Object->GetClass(), Object, Json.ToSharedRef(), /*CheckFlags*/ 0, SkipFlags);

		return Paths.Num() > 0 ? FilterByPaths(Json, Paths) : Json;
	}

	TSharedPtr<FJsonObject> FilterByPaths(const TSharedPtr<FJsonObject>& Source, const TArray<FString>& Paths)
	{
		if (!Source.IsValid())
			return MakeShared<FJsonObject>();
		if (Paths.Num() == 0)
			return Source;

		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		for (const FString& Path : Paths)
		{
			TArray<FString> Segments;
			Path.ParseIntoArray(Segments, TEXT("."), /*CullEmpty*/ true);
			if (Segments.Num() == 0)
				continue;

			// Walk the source object segment by segment; mirror the structure into Out so the caller gets
			// the requested leaf at the same nesting depth it lives at in the full serialization.
			TSharedPtr<FJsonObject> SrcCursor = Source;
			TSharedPtr<FJsonObject> OutCursor = Out;
			bool bResolved = true;
			for (int32 i = 0; i < Segments.Num(); ++i)
			{
				const FString& Seg = Segments[i];
				const bool bLeaf = (i == Segments.Num() - 1);

				// Case-insensitive field match (paths may be loosely cased like property writes).
				FString MatchedKey;
				TSharedPtr<FJsonValue> MatchedValue;
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : SrcCursor->Values)
				{
					if (Field.Key.Equals(Seg, ESearchCase::IgnoreCase))
					{
						MatchedKey = Field.Key;
						MatchedValue = Field.Value;
						break;
					}
				}
				if (!MatchedValue.IsValid())
				{
					bResolved = false;
					break;
				}

				if (bLeaf)
				{
					OutCursor->SetField(MatchedKey, MatchedValue);
					break;
				}

				const TSharedPtr<FJsonObject>* NextSrc;
				if (!MatchedValue->TryGetObject(NextSrc) || !NextSrc->IsValid())
				{
					bResolved = false;
					break;
				}

				// Reuse an existing nested object in Out (so two paths under the same parent merge).
				TSharedPtr<FJsonObject> NextOut;
				const TSharedPtr<FJsonObject>* ExistingOut;
				if (OutCursor->TryGetObjectField(MatchedKey, ExistingOut) && ExistingOut->IsValid())
				{
					NextOut = *ExistingOut;
				}
				else
				{
					NextOut = MakeShared<FJsonObject>();
					OutCursor->SetObjectField(MatchedKey, NextOut);
				}
				SrcCursor = *NextSrc;
				OutCursor = NextOut;
			}
			(void)bResolved; // an unresolved path simply contributes nothing — not an error.
		}
		return Out;
	}

	int32 ApplyProperties(UObject* Object, const TSharedPtr<FJsonObject>& Properties, TArray<FString>& OutErrors)
	{
		if (!Object || !Properties.IsValid())
			return 0;

		AActor* AsActor = Cast<AActor>(Object);
		USceneComponent* AsScene = Cast<USceneComponent>(Object);

		// Snapshot the pre-change state for the transaction buffer BEFORE mutating. Calling Modify() after
		// the writes (as before) records the already-changed values, making undo a no-op. PreEditChange(nullptr)
		// completes the standard editor edit protocol (PreEditChange -> write -> PostEditChange at line ~256):
		// properties whose edit hooks tear down state up front (component render-state/registration guards,
		// cached-data invalidation) can misbehave when raw memory is written without the pre-notify.
		// Modify()/PreEditChange()/PostEditChange() are WITH_EDITOR-only on UObject (transaction buffer +
		// editor edit-protocol hooks), so they cannot be called from a Type=Runtime module that BuildPlugin
		// compiles in a non-editor configuration. Guard them: in the editor the undo/edit-protocol behaviour is
		// preserved byte-for-byte; in a packaged game the raw FProperty writes below still apply (no transaction
		// buffer or edit hooks exist there to drive). MarkPackageDirty() is available in both configurations.
#if WITH_EDITOR
		Object->Modify();
		Object->PreEditChange(nullptr);
#endif

		int32 Applied = 0;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Properties->Values)
		{
			const FString& Key = Field.Key;
			const TSharedPtr<FJsonValue>& Value = Field.Value;

			// --- Actor transform special-cases (not single FProperties) ---
			// Each transform key is handled TERMINALLY: on a parse success we apply and count it; on a
			// parse failure (empty {} or all-typo'd keys) we record an error and `continue`. Falling through
			// to the generic FProperty path would either count an empty-object no-op as applied (the scene
			// component RelativeLocation/etc. FProperties exist and JsonObjectToUStruct returns true for {})
			// or emit a misleading "unknown property" error for the actor pseudo-keys.
			if (AsActor)
			{
				if (Key.Equals(TEXT("location"), ESearchCase::IgnoreCase))
				{
					FVector V = AsActor->GetActorLocation();
					if (JsonToVector(Value, V)) { AsActor->SetActorLocation(V); ++Applied; }
					else OutErrors.Add(FString::Printf(TEXT("'%s' expects a {x,y,z} object with at least one numeric axis"), *Key));
					continue;
				}
				if (Key.Equals(TEXT("rotation"), ESearchCase::IgnoreCase))
				{
					FRotator R = AsActor->GetActorRotation();
					if (JsonToRotator(Value, R)) { AsActor->SetActorRotation(R); ++Applied; }
					else OutErrors.Add(FString::Printf(TEXT("'%s' expects a {pitch,yaw,roll} object with at least one numeric key"), *Key));
					continue;
				}
				if (Key.Equals(TEXT("scale"), ESearchCase::IgnoreCase))
				{
					FVector S = AsActor->GetActorScale3D();
					if (JsonToVector(Value, S)) { AsActor->SetActorScale3D(S); ++Applied; }
					else OutErrors.Add(FString::Printf(TEXT("'%s' expects a {x,y,z} object with at least one numeric axis"), *Key));
					continue;
				}
			}

			// --- Scene-component relative-transform special-cases (also handled terminally) ---
			if (AsScene)
			{
				if (Key.Equals(TEXT("relativeLocation"), ESearchCase::IgnoreCase))
				{
					FVector V = AsScene->GetRelativeLocation();
					if (JsonToVector(Value, V)) { AsScene->SetRelativeLocation(V); ++Applied; }
					else OutErrors.Add(FString::Printf(TEXT("'%s' expects a {x,y,z} object with at least one numeric axis"), *Key));
					continue;
				}
				if (Key.Equals(TEXT("relativeRotation"), ESearchCase::IgnoreCase))
				{
					FRotator R = AsScene->GetRelativeRotation();
					if (JsonToRotator(Value, R)) { AsScene->SetRelativeRotation(R); ++Applied; }
					else OutErrors.Add(FString::Printf(TEXT("'%s' expects a {pitch,yaw,roll} object with at least one numeric key"), *Key));
					continue;
				}
				if (Key.Equals(TEXT("relativeScale3D"), ESearchCase::IgnoreCase)
					|| Key.Equals(TEXT("relativeScale"), ESearchCase::IgnoreCase))
				{
					FVector S = AsScene->GetRelativeScale3D();
					if (JsonToVector(Value, S)) { AsScene->SetRelativeScale3D(S); ++Applied; }
					else OutErrors.Add(FString::Printf(TEXT("'%s' expects a {x,y,z} object with at least one numeric axis"), *Key));
					continue;
				}
			}

			// --- Generic reflected FProperty write ---
			FProperty* Prop = FindPropertyByJsonName(Object->GetClass(), Key);
			if (!Prop)
			{
				OutErrors.Add(FString::Printf(TEXT("unknown property '%s'"), *Key));
				continue;
			}
			// Writable iff editable-and-not-const, OR blueprint-visible-and-not-blueprint-read-only. The old
			// gate admitted BlueprintReadOnly props (CPF_BlueprintVisible set, no CPF_Edit, no CPF_EditConst).
			const bool bEditable = Prop->HasAnyPropertyFlags(CPF_Edit) && !Prop->HasAnyPropertyFlags(CPF_EditConst);
			const bool bBlueprintWritable = Prop->HasAnyPropertyFlags(CPF_BlueprintVisible) && !Prop->HasAnyPropertyFlags(CPF_BlueprintReadOnly);
			if (!bEditable && !bBlueprintWritable)
			{
				// Reject read-only properties explicitly so the caller gets a clear reason, not a silent no-op.
				OutErrors.Add(FString::Printf(TEXT("property '%s' is not writable"), *Key));
				continue;
			}

			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Object);
			if (FJsonObjectConverter::JsonValueToUProperty(Value, Prop, ValuePtr, /*CheckFlags*/ 0, /*SkipFlags*/ 0))
			{
				++Applied;
			}
			else
			{
				OutErrors.Add(FString::Printf(TEXT("could not set property '%s' from the provided value"), *Key));
			}
		}

		// PostEditChange MUST pair the PreEditChange(nullptr) above on every path (it re-registers a component
		// and re-runs the edit hooks that the pre-notify tore down) — calling it only when Applied>0 would
		// leave a zero-applied object stranded mid-edit (e.g. an unregistered component). Only dirty the
		// package when something actually changed.
#if WITH_EDITOR
		Object->PostEditChange();
#endif
		if (Applied > 0)
			Object->MarkPackageDirty();
		return Applied;
	}
}
