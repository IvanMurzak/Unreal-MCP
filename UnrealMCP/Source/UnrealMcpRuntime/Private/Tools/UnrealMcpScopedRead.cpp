// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Tools/UnrealMcpScopedRead.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	// Unity-build ODR (conventions.md): the runtime module is unity-built, so an anonymous namespace does NOT
	// make these helpers file-private. Give each a file-unique `ScopedRead` prefix so a future same-named helper
	// in another runtime TU cannot collide with these.

	// --- Deep-clone machinery (ported verbatim from the asset family's UnrealMcpAssetScopedRead, which deep-
	//     cloned copied values so the filtered result never aliases the source). Used only on the bDeepClone path.

	TSharedPtr<FJsonValue> ScopedReadCloneValue(const TSharedPtr<FJsonValue>& In);

	TSharedPtr<FJsonObject> ScopedReadCloneObject(const TSharedPtr<FJsonObject>& In)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		if (In.IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : In->Values)
			{
				Out->SetField(Pair.Key, ScopedReadCloneValue(Pair.Value));
			}
		}
		return Out;
	}

	TSharedPtr<FJsonValue> ScopedReadCloneValue(const TSharedPtr<FJsonValue>& In)
	{
		if (!In.IsValid())
		{
			return MakeShared<FJsonValueNull>();
		}

		switch (In->Type)
		{
		case EJson::Object:
			return MakeShared<FJsonValueObject>(ScopedReadCloneObject(In->AsObject()));
		case EJson::Array:
		{
			TArray<TSharedPtr<FJsonValue>> Cloned;
			for (const TSharedPtr<FJsonValue>& Element : In->AsArray())
			{
				Cloned.Add(ScopedReadCloneValue(Element));
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
			// EJson::Null / None — represent as JSON null. Must switch on the concrete Type rather than probing
			// TryGetNumber/TryGetBool/TryGetString in order: FJsonValueString::TryGetBool succeeds for ANY string
			// (yielding false), which would silently corrupt string leaves.
			return MakeShared<FJsonValueNull>();
		}
	}

	/** Materialize the matched value into the result: deep-cloned (asset) or aliased (actor/level). */
	TSharedPtr<FJsonValue> ScopedReadMaterialize(const TSharedPtr<FJsonValue>& Value, bool bDeepClone)
	{
		return bDeepClone ? ScopedReadCloneValue(Value) : Value;
	}

	/**
	 * Find a path segment in @p SrcObj, returning the matched source key + value. When @p bCaseInsensitive,
	 * scans the object's fields for the first case-insensitive key match (and returns that key's actual casing,
	 * so the result mirrors the source's casing — the actor/level behavior). Otherwise an exact TryGetField
	 * lookup (the matched key equals the requested segment — the asset behavior). Returns false if no match.
	 */
	bool ScopedReadMatchSegment(const TSharedPtr<FJsonObject>& SrcObj, const FString& Segment, bool bCaseInsensitive,
		FString& OutMatchedKey, TSharedPtr<FJsonValue>& OutMatchedValue)
	{
		if (bCaseInsensitive)
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : SrcObj->Values)
			{
				if (Field.Key.Equals(Segment, ESearchCase::IgnoreCase))
				{
					OutMatchedKey = Field.Key;
					OutMatchedValue = Field.Value;
					return true;
				}
			}
			return false;
		}

		const TSharedPtr<FJsonValue> Field = SrcObj->TryGetField(Segment);
		if (!Field.IsValid())
			return false;
		OutMatchedKey = Segment;
		OutMatchedValue = Field;
		return true;
	}

	/** Insert @p Value into @p Root following @p Segments, creating intermediate objects (asset InsertAtPath). */
	void ScopedReadInsertAtPath(const TSharedPtr<FJsonObject>& Root, const TArray<FString>& Segments, const TSharedPtr<FJsonValue>& Value)
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
}

namespace FUnrealMcpScopedRead
{
	TSharedPtr<FJsonObject> Filter(
		const TSharedPtr<FJsonObject>& Source, const TArray<FString>& Paths, const FScopedReadOptions& Options)
	{
		// A null/invalid source always yields a valid empty object (never an aliased null) — matching each prior
		// impl's leading guard. Checked BEFORE the no-paths branch so the no-source + no-paths combination cannot
		// return the null alias on the bDeepClone==false (actor/level) path.
		if (!Source.IsValid())
		{
			return MakeShared<FJsonObject>();
		}
		// No paths requested → the whole object. Deep-clone it (asset) so the caller can mutate freely, or return
		// the source alias (actor/level — the caller must not mutate it). Each prior impl's no-paths branch.
		if (Paths.Num() == 0)
		{
			return Options.bDeepClone ? ScopedReadCloneObject(Source) : Source;
		}

		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		for (const FString& Path : Paths)
		{
			TArray<FString> Segments;
			Path.ParseIntoArray(Segments, TEXT("."), /*CullEmpty*/ true);
			if (Segments.Num() == 0)
				continue;

			if (Options.bLeavePartialBranches)
			{
				// ACTOR/LEVEL parity: build the result DURING the walk, mirroring the structure into Out so the
				// caller gets the requested leaf at the same nesting depth. A path that fails a deeper segment LEAVES
				// the partial intermediate branch already created (the historical FilterByPaths residue).
				TSharedPtr<FJsonObject> SrcCursor = Source;
				TSharedPtr<FJsonObject> OutCursor = Out;
				for (int32 i = 0; i < Segments.Num(); ++i)
				{
					const bool bLeaf = (i == Segments.Num() - 1);

					FString MatchedKey;
					TSharedPtr<FJsonValue> MatchedValue;
					if (!ScopedReadMatchSegment(SrcCursor, Segments[i], Options.bCaseInsensitive, MatchedKey, MatchedValue))
						break; // unresolved — the partial branch built so far remains in Out

					if (bLeaf)
					{
						OutCursor->SetField(MatchedKey, ScopedReadMaterialize(MatchedValue, Options.bDeepClone));
						break;
					}

					const TSharedPtr<FJsonObject>* NextSrc;
					if (!MatchedValue->TryGetObject(NextSrc) || !NextSrc->IsValid())
						break; // a non-object intermediate — partial branch remains

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
			}
			else
			{
				// ASSET parity: resolve the leaf value by walking SOURCE only (no Out mutation), then insert into Out
				// at the requested path ONLY on full resolution — so an unresolved path (including a partial one)
				// contributes NOTHING. Intermediate descent requires an object, else the path is unresolved.
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
					FString MatchedKey;
					TSharedPtr<FJsonValue> MatchedValue;
					if (!ScopedReadMatchSegment(Cursor, Segments[Index], Options.bCaseInsensitive, MatchedKey, MatchedValue))
					{
						bResolved = false;
						break;
					}
					if (Index == Segments.Num() - 1)
					{
						Resolved = MatchedValue;
					}
					else
					{
						Cursor = (MatchedValue->Type == EJson::Object) ? MatchedValue->AsObject() : nullptr;
					}
				}

				if (bResolved && Resolved.IsValid())
				{
					ScopedReadInsertAtPath(Out, Segments, ScopedReadMaterialize(Resolved, Options.bDeepClone));
				}
			}
		}
		return Out;
	}
}
