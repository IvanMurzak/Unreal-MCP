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
	/** Read a {x,y,z} object into an FVector; returns false when the value is not such an object. */
	bool JsonToVector(const TSharedPtr<FJsonValue>& Value, FVector& Out)
	{
		const TSharedPtr<FJsonObject>* Obj;
		if (!Value.IsValid() || !Value->TryGetObject(Obj) || !Obj->IsValid())
			return false;
		double C;
		if ((*Obj)->TryGetNumberField(TEXT("x"), C)) Out.X = C;
		if ((*Obj)->TryGetNumberField(TEXT("y"), C)) Out.Y = C;
		if ((*Obj)->TryGetNumberField(TEXT("z"), C)) Out.Z = C;
		return true;
	}

	/** Read a {pitch,yaw,roll} object into an FRotator; returns false when not such an object. */
	bool JsonToRotator(const TSharedPtr<FJsonValue>& Value, FRotator& Out)
	{
		const TSharedPtr<FJsonObject>* Obj;
		if (!Value.IsValid() || !Value->TryGetObject(Obj) || !Obj->IsValid())
			return false;
		double C;
		if ((*Obj)->TryGetNumberField(TEXT("pitch"), C)) Out.Pitch = C;
		if ((*Obj)->TryGetNumberField(TEXT("yaw"),   C)) Out.Yaw = C;
		if ((*Obj)->TryGetNumberField(TEXT("roll"),  C)) Out.Roll = C;
		return true;
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

		int32 Applied = 0;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Properties->Values)
		{
			const FString& Key = Field.Key;
			const TSharedPtr<FJsonValue>& Value = Field.Value;

			// --- Actor transform special-cases (not single FProperties) ---
			if (AsActor)
			{
				if (Key.Equals(TEXT("location"), ESearchCase::IgnoreCase))
				{
					FVector V = AsActor->GetActorLocation();
					if (JsonToVector(Value, V)) { AsActor->SetActorLocation(V); ++Applied; continue; }
				}
				else if (Key.Equals(TEXT("rotation"), ESearchCase::IgnoreCase))
				{
					FRotator R = AsActor->GetActorRotation();
					if (JsonToRotator(Value, R)) { AsActor->SetActorRotation(R); ++Applied; continue; }
				}
				else if (Key.Equals(TEXT("scale"), ESearchCase::IgnoreCase))
				{
					FVector S = AsActor->GetActorScale3D();
					if (JsonToVector(Value, S)) { AsActor->SetActorScale3D(S); ++Applied; continue; }
				}
			}

			// --- Scene-component relative-transform special-cases ---
			if (AsScene)
			{
				if (Key.Equals(TEXT("relativeLocation"), ESearchCase::IgnoreCase))
				{
					FVector V = AsScene->GetRelativeLocation();
					if (JsonToVector(Value, V)) { AsScene->SetRelativeLocation(V); ++Applied; continue; }
				}
				else if (Key.Equals(TEXT("relativeRotation"), ESearchCase::IgnoreCase))
				{
					FRotator R = AsScene->GetRelativeRotation();
					if (JsonToRotator(Value, R)) { AsScene->SetRelativeRotation(R); ++Applied; continue; }
				}
				else if (Key.Equals(TEXT("relativeScale3D"), ESearchCase::IgnoreCase)
					|| Key.Equals(TEXT("relativeScale"), ESearchCase::IgnoreCase))
				{
					FVector S = AsScene->GetRelativeScale3D();
					if (JsonToVector(Value, S)) { AsScene->SetRelativeScale3D(S); ++Applied; continue; }
				}
			}

			// --- Generic reflected FProperty write ---
			FProperty* Prop = FindPropertyByJsonName(Object->GetClass(), Key);
			if (!Prop)
			{
				OutErrors.Add(FString::Printf(TEXT("unknown property '%s'"), *Key));
				continue;
			}
			if (Prop->HasAnyPropertyFlags(CPF_EditConst) || !Prop->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
			{
				// Allow editable / blueprint-visible properties only; reject read-only ones explicitly so
				// the caller gets a clear reason rather than a silent no-op.
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

		if (Applied > 0)
		{
			Object->Modify();
			Object->PostEditChange();
			Object->MarkPackageDirty();
		}
		return Applied;
	}
}
