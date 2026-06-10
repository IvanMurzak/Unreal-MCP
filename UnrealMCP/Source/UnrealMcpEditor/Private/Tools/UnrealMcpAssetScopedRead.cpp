// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Tools/UnrealMcpAssetScopedRead.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UnrealMcpAssetScopedRead
{
	/** Deep-clone a JSON value (object/array/leaf) so the filtered result never aliases @p Source. */
	static TSharedPtr<FJsonValue> CloneValue(const TSharedPtr<FJsonValue>& In);

	static TSharedPtr<FJsonObject> CloneObject(const TSharedPtr<FJsonObject>& In)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		if (In.IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : In->Values)
			{
				Out->SetField(Pair.Key, CloneValue(Pair.Value));
			}
		}
		return Out;
	}

	static TSharedPtr<FJsonValue> CloneValue(const TSharedPtr<FJsonValue>& In)
	{
		if (!In.IsValid())
		{
			return MakeShared<FJsonValueNull>();
		}

		switch (In->Type)
		{
		case EJson::Object:
			return MakeShared<FJsonValueObject>(CloneObject(In->AsObject()));
		case EJson::Array:
		{
			TArray<TSharedPtr<FJsonValue>> Cloned;
			for (const TSharedPtr<FJsonValue>& Element : In->AsArray())
			{
				Cloned.Add(CloneValue(Element));
			}
			return MakeShared<FJsonValueArray>(Cloned);
		}
		case EJson::String:
			return MakeShared<FJsonValueString>(In->AsString());
		case EJson::Number:
			return MakeShared<FJsonValueNumber>(In->AsNumber());
		case EJson::Boolean:
			return MakeShared<FJsonValueBoolean>(In->AsBool());
		default:
			// EJson::Null / None — represent as JSON null. Must switch on the concrete Type rather
			// than probing TryGetNumber/TryGetBool/TryGetString in order: FJsonValueString::TryGetBool
			// succeeds for ANY string (yielding false), which would silently corrupt string leaves.
			return MakeShared<FJsonValueNull>();
		}
	}

	/** Insert @p Value into @p Root following the dot-path @p Segments (creating intermediate objects). */
	static void InsertAtPath(const TSharedPtr<FJsonObject>& Root, const TArray<FString>& Segments, const TSharedPtr<FJsonValue>& Value)
	{
		TSharedPtr<FJsonObject> Cursor = Root;
		for (int32 Index = 0; Index < Segments.Num() - 1; ++Index)
		{
			const FString& Segment = Segments[Index];
			const TSharedPtr<FJsonObject>* Existing;
			if (Cursor->TryGetObjectField(Segment, Existing) && Existing->IsValid())
			{
				Cursor = *Existing;
			}
			else
			{
				TSharedPtr<FJsonObject> Child = MakeShared<FJsonObject>();
				Cursor->SetObjectField(Segment, Child);
				Cursor = Child;
			}
		}
		Cursor->SetField(Segments.Last(), Value);
	}

	TSharedPtr<FJsonObject> Apply(const TSharedPtr<FJsonObject>& Source, const TArray<FString>& Paths)
	{
		// No paths requested → return the whole object (deep-copied so callers can mutate freely).
		if (Paths.Num() == 0)
		{
			return CloneObject(Source);
		}
		if (!Source.IsValid())
		{
			return MakeShared<FJsonObject>();
		}

		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		for (const FString& Path : Paths)
		{
			TArray<FString> Segments;
			Path.ParseIntoArray(Segments, TEXT("."), /*InCullEmpty*/ true);
			if (Segments.Num() == 0)
			{
				continue;
			}

			// Walk Source down the path.
			TSharedPtr<FJsonObject> Cursor = Source;
			TSharedPtr<FJsonValue> Resolved;
			bool bResolved = true;
			for (int32 Index = 0; Index < Segments.Num(); ++Index)
			{
				if (!Cursor.IsValid())
				{
					bResolved = false;
					break;
				}
				const TSharedPtr<FJsonValue> Field = Cursor->TryGetField(Segments[Index]);
				if (!Field.IsValid())
				{
					bResolved = false;
					break;
				}
				if (Index == Segments.Num() - 1)
				{
					Resolved = Field;
				}
				else
				{
					Cursor = (Field->Type == EJson::Object) ? Field->AsObject() : nullptr;
				}
			}

			if (bResolved && Resolved.IsValid())
			{
				InsertAtPath(Out, Segments, CloneValue(Resolved));
			}
		}
		return Out;
	}
}
