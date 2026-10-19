// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRqlSession.h"
#include "UnrealMcpRqlSources.h"
#include "UnrealMcpRqlUtil.h"

#include "Engine/Blueprint.h"
#include "Math/Color.h"
#include "Math/IntPoint.h"
#include "Math/IntVector.h"
#include "Math/Quat.h"
#include "Math/Rotator.h"
#include "Math/Vector.h"
#include "Math/Vector2D.h"
#include "Math/Vector4.h"
#include "UObject/Class.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/Object.h"

#include <exception>
#include <string>
#include <string_view>
#include <vector>

namespace UnrealMcpRqlStructsLocal
{
	/** One math struct with a text form: N components read / written as doubles. */
	struct FMathStruct
	{
		UScriptStruct* Struct = nullptr;
		int32 Count = 0;
		bool bColor = false;   // "#rrggbb[aa]" accepted, alpha optional (defaults to opaque)
		bool bSingle = false;  // float components: printed in single precision
		const char* Shape = "";
		TFunction<void(const void*, double*)> Get;
		TFunction<void(void*, const double*)> Set;
	};

	const TArray<FMathStruct>& MathStructs()
	{
		static TArray<FMathStruct> Specs;
		if (Specs.Num() > 0)
			return Specs;
		auto Add = [](UScriptStruct* Struct, int32 Count, bool bColor, bool bSingle, const char* Shape,
			TFunction<void(const void*, double*)> Get, TFunction<void(void*, const double*)> Set)
		{
			FMathStruct Spec;
			Spec.Struct = Struct;
			Spec.Count = Count;
			Spec.bColor = bColor;
			Spec.bSingle = bSingle;
			Spec.Shape = Shape;
			Spec.Get = MoveTemp(Get);
			Spec.Set = MoveTemp(Set);
			Specs.Add(MoveTemp(Spec));
		};
		Add(TBaseStructure<FVector>::Get(), 3, false, false, "(x, y, z)",
			[](const void* P, double* O) { const FVector& V = *static_cast<const FVector*>(P); O[0] = V.X; O[1] = V.Y; O[2] = V.Z; },
			[](void* P, const double* I) { *static_cast<FVector*>(P) = FVector(I[0], I[1], I[2]); });
		Add(TBaseStructure<FVector2D>::Get(), 2, false, false, "(x, y)",
			[](const void* P, double* O) { const FVector2D& V = *static_cast<const FVector2D*>(P); O[0] = V.X; O[1] = V.Y; },
			[](void* P, const double* I) { *static_cast<FVector2D*>(P) = FVector2D(I[0], I[1]); });
		Add(TBaseStructure<FVector4>::Get(), 4, false, false, "(x, y, z, w)",
			[](const void* P, double* O) { const FVector4& V = *static_cast<const FVector4*>(P); O[0] = V.X; O[1] = V.Y; O[2] = V.Z; O[3] = V.W; },
			[](void* P, const double* I) { *static_cast<FVector4*>(P) = FVector4(I[0], I[1], I[2], I[3]); });
		Add(TBaseStructure<FRotator>::Get(), 3, false, false, "(pitch, yaw, roll) in degrees",
			[](const void* P, double* O) { const FRotator& R = *static_cast<const FRotator*>(P); O[0] = R.Pitch; O[1] = R.Yaw; O[2] = R.Roll; },
			[](void* P, const double* I) { *static_cast<FRotator*>(P) = FRotator(I[0], I[1], I[2]); });
		Add(TBaseStructure<FQuat>::Get(), 4, false, false, "(x, y, z, w)",
			[](const void* P, double* O) { const FQuat& Q = *static_cast<const FQuat*>(P); O[0] = Q.X; O[1] = Q.Y; O[2] = Q.Z; O[3] = Q.W; },
			[](void* P, const double* I) { *static_cast<FQuat*>(P) = FQuat(I[0], I[1], I[2], I[3]); });
		Add(TBaseStructure<FLinearColor>::Get(), 4, true, true, "(r, g, b, a) linear 0..1 or \"#rrggbb\"",
			[](const void* P, double* O) { const FLinearColor& C = *static_cast<const FLinearColor*>(P); O[0] = C.R; O[1] = C.G; O[2] = C.B; O[3] = C.A; },
			[](void* P, const double* I) { *static_cast<FLinearColor*>(P) = FLinearColor(static_cast<float>(I[0]), static_cast<float>(I[1]), static_cast<float>(I[2]), static_cast<float>(I[3])); });
		Add(TBaseStructure<FColor>::Get(), 4, true, false, "(r, g, b, a) 0..255 or \"#rrggbb\"",
			[](const void* P, double* O) { const FColor& C = *static_cast<const FColor*>(P); O[0] = C.R; O[1] = C.G; O[2] = C.B; O[3] = C.A; },
			[](void* P, const double* I)
			{
				auto Byte = [](double V) { return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(V), 0, 255)); };
				*static_cast<FColor*>(P) = FColor(Byte(I[0]), Byte(I[1]), Byte(I[2]), Byte(I[3]));
			});
		Add(TBaseStructure<FIntPoint>::Get(), 2, false, false, "(x, y)",
			[](const void* P, double* O) { const FIntPoint& V = *static_cast<const FIntPoint*>(P); O[0] = V.X; O[1] = V.Y; },
			[](void* P, const double* I) { *static_cast<FIntPoint*>(P) = FIntPoint(FMath::RoundToInt(I[0]), FMath::RoundToInt(I[1])); });
		Add(TBaseStructure<FIntVector>::Get(), 3, false, false, "(x, y, z)",
			[](const void* P, double* O) { const FIntVector& V = *static_cast<const FIntVector*>(P); O[0] = V.X; O[1] = V.Y; O[2] = V.Z; },
			[](void* P, const double* I) { *static_cast<FIntVector*>(P) = FIntVector(FMath::RoundToInt(I[0]), FMath::RoundToInt(I[1]), FMath::RoundToInt(I[2])); });
		return Specs;
	}

	const FMathStruct* Find(const UScriptStruct* Struct)
	{
		for (const FMathStruct& Spec : MathStructs())
		{
			if (Spec.Struct == Struct)
				return &Spec;
		}
		return nullptr;
	}

	/** Every number in a text such as "(1, 2.5, -3)", "X=1 Y=2 Z=3" or "P=0 Y=90 R=0" (locale-independent). */
	std::vector<double> Numbers(const std::string& Text)
	{
		auto Starts = [](char C) { return (C >= '0' && C <= '9') || C == '-' || C == '+' || C == '.'; };
		auto Continues = [&Starts](char C) { return Starts(C) || C == 'e' || C == 'E'; };
		std::vector<double> Out;
		size_t Cursor = 0;
		while (Cursor < Text.size())
		{
			if (!Starts(Text[Cursor]))
			{
				++Cursor;
				continue;
			}
			size_t End = Cursor + 1;
			while (End < Text.size() && Continues(Text[End]))
				++End;
			double Value = 0.0;
			if (rql::parse_double(std::string_view(Text).substr(Cursor, End - Cursor), Value))
			{
				Out.push_back(Value);
				Cursor = End;
			}
			else
			{
				++Cursor; // a lone sign or dot
			}
		}
		return Out;
	}

	bool Hex(const std::string& Text, const FMathStruct& Spec, double* Out)
	{
		const FString Trimmed = UnrealMcpRqlFromStd(Text).TrimStartAndEnd();
		if (!Trimmed.StartsWith(TEXT("#")) || (Trimmed.Len() != 7 && Trimmed.Len() != 9))
			return false;
		for (int32 Index = 1; Index < Trimmed.Len(); ++Index)
		{
			if (!FChar::IsHexDigit(Trimmed[Index]))
				return false; // not a color: the number parse below reports the malformed literal
		}
		const FColor Color = FColor::FromHex(Trimmed);
		if (Spec.Struct == TBaseStructure<FColor>::Get())
		{
			Out[0] = Color.R; Out[1] = Color.G; Out[2] = Color.B; Out[3] = Color.A;
		}
		else
		{
			const FLinearColor Linear(Color); // "#rrggbb" is sRGB, as in color pickers
			Out[0] = Linear.R; Out[1] = Linear.G; Out[2] = Linear.B; Out[3] = Linear.A;
		}
		return true;
	}

	bool Components(const FMathStruct& Spec, const rql::Value& Value, double* Out, FString& OutError)
	{
		std::vector<double> Parts;
		if (Value.is_string())
		{
			if (Spec.bColor && Hex(Value.as_string(), Spec, Out))
				return true;
			Parts = Numbers(Value.as_string());
		}
		else if (Value.is_list())
		{
			for (size_t Index = 0, Count = Value.list().size(); Index < Count; ++Index)
			{
				const rql::Value Item = Value.list().at(Index);
				if (!Item.is_number())
				{
					OutError = TEXT("list elements must be numbers");
					return false;
				}
				Parts.push_back(Item.as_number());
			}
		}
		else
		{
			return false;
		}
		const size_t Wanted = static_cast<size_t>(Spec.Count);
		if (Spec.bColor && Parts.size() == Wanted - 1)
			Parts.push_back(Spec.Struct == TBaseStructure<FColor>::Get() ? 255.0 : 1.0); // alpha
		if (Parts.size() != Wanted)
		{
			OutError = FString::Printf(TEXT("%s expects %d numbers %s"), *Spec.Struct->GetName(), Spec.Count, *UnrealMcpRqlFromStd(Spec.Shape));
			return false;
		}
		for (size_t Index = 0; Index < Wanted; ++Index)
			Out[Index] = Parts[Index];
		return true;
	}

	std::string Format(const FMathStruct& Spec, const void* Data)
	{
		double Parts[4] = { 0, 0, 0, 0 };
		Spec.Get(Data, Parts);
		std::string Out = "(";
		for (int32 Index = 0; Index < Spec.Count; ++Index)
		{
			if (Index > 0)
				Out += ", ";
			Out += rql::to_text(rql::Value::number(Parts[Index], Spec.bSingle ? rql::builtin::single_type() : rql::builtin::double_type()));
		}
		return Out + ")";
	}
}

using namespace UnrealMcpRqlStructsLocal;

bool UnrealMcpRqlParseMathStruct(UScriptStruct* Struct, const rql::Value& Value, void* Out, FString& OutError)
{
	const FMathStruct* Spec = Find(Struct);
	if (!Spec)
		return false;
	double Parts[4] = { 0, 0, 0, 0 };
	if (!Components(*Spec, Value, Parts, OutError))
		return false;
	Spec->Set(Out, Parts);
	return true;
}

std::function<rql::AdapterOutcome(const rql::Value&, const rql::TypeDescriptor&, rql::Value&, std::string&)>
	UnrealMcpRqlStructCoerce(FUnrealMcpRqlTypes& Types, UScriptStruct* Struct, std::function<bool(const rql::Value&)> Accepts)
{
	FUnrealMcpRqlTypes* TypesPtr = &Types;
	return [TypesPtr, Struct, Accepts](const rql::Value& Value, const rql::TypeDescriptor&, rql::Value& Result, std::string& Reason)
	{
		if (!Accepts(Value))
			return rql::AdapterOutcome::NotHandled;
		rql::Value Built = TypesPtr->WrapStruct(Struct, nullptr);
		try
		{
			TypesPtr->CoerceStruct(Struct, Value, Built.ptr());
		}
		catch (const std::exception& Error)
		{
			Reason = Error.what();
			return rql::AdapterOutcome::Failed;
		}
		Result = Built;
		return rql::AdapterOutcome::Ok;
	};
}

namespace UnrealMcpRqlStructsLocal
{
	/**
	 * Soft object / class paths (FSoftObjectPath, FSoftClassPath: GameDefaultMap, a class to spawn ...) as text:
	 * "/Game/Maps/Arena.Arena" out, a path, an object or "" in. Paths are not checked for an existing asset (they are
	 * soft: the asset may not be loaded or not exist yet), the same rule as soft object properties.
	 */
	void AddSoftPathAdapter(FUnrealMcpRqlSession& Session, UScriptStruct* Struct)
	{
		FUnrealMcpRqlTypes* Types = &Session.Types();
		const rql::TypeDescriptor* Descriptor = Types->ForStruct(Struct);
		if (!Descriptor)
			return;
		// Text is the whole value: no raw members (AssetPath / SubPathString) to write past the adapter.
		Types->AddStructHook(Struct, [](FUnrealMcpRqlTypes&, rql::TypeDescriptor& D)
		{
			D.members.clear();
			FUnrealMcpRqlTypes::SetMember(D, UnrealMcpRqlUtil::Member("Path", rql::builtin::string_type(), [](void* Self)
			{
				return UnrealMcpRqlUtil::Str(static_cast<const FSoftObjectPath*>(Self)->ToString());
			}, {}, "the path text"));
			FUnrealMcpRqlTypes::SetMember(D, UnrealMcpRqlUtil::Member("Package", rql::builtin::string_type(), [](void* Self)
			{
				return UnrealMcpRqlUtil::Str(static_cast<const FSoftObjectPath*>(Self)->GetLongPackageName());
			}, {}, "the package (\"/Game/Maps/Arena\")"));
		});
		const bool bClassPath = Struct->GetFName() == FName(TEXT("SoftClassPath"));
		Types->AddStructParser(Struct, [bClassPath](FUnrealMcpRqlTypes& T, const rql::Value& Value, void* Out) -> bool
		{
			FSoftObjectPath& Path = *static_cast<FSoftObjectPath*>(Out);
			if (Value.is_null() || (Value.is_string() && Value.as_string().empty()))
			{
				Path.Reset();
				return true;
			}
			if (UObject* Object = T.AsObject(Value))
			{
				// A class path holds a class: a Blueprint stands for its generated class, other objects do not fit.
				if (bClassPath && !Object->IsA<UClass>())
				{
					const UBlueprint* Blueprint = Cast<UBlueprint>(Object);
					if (!Blueprint || !Blueprint->GeneratedClass)
						UnrealMcpRqlUtil::Throw(FString::Printf(TEXT("%s is not a class (a class path takes a class or a Blueprint)"), *Object->GetName()));
					Object = Blueprint->GeneratedClass;
				}
				Path = FSoftObjectPath(Object);
				return true;
			}
			if (!Value.is_string())
				return false;
			// SetPath silently resets text that is not an object path ("Arena", "Game/Maps/Arena"): refuse it instead.
			const FString Text = UnrealMcpRqlFromStd(Value.as_string()).TrimStartAndEnd();
			Path.SetPath(Text);
			if (Path.IsNull() && !Text.IsEmpty() && !Text.Equals(TEXT("None")))
				UnrealMcpRqlUtil::Throw(FString::Printf(TEXT("'%s' is not an object path (\"/Game/Maps/Arena.Arena\")"), *Text));
			return true;
		});
		rql::TypeAdapter Adapter;
		Adapter.name = UnrealMcpRqlToStd(Struct->GetName());
		Adapter.target = Descriptor;
		Adapter.description = UnrealMcpRqlToStd(Struct->GetName()) + ": an asset path as text (\"/Game/Maps/Arena.Arena\"), an object, or \"\" for none";
		Adapter.encode = [](const rql::Value& Value, const rql::EncodeContext&) -> std::optional<rql::Json>
		{
			if (!Value.is_struct() || !Value.ptr())
				return std::nullopt;
			const FSoftObjectPath& Path = *static_cast<const FSoftObjectPath*>(Value.ptr());
			return Path.IsNull() ? rql::Json() : rql::Json(UnrealMcpRqlToStd(Path.ToString()));
		};
		Adapter.coerce = UnrealMcpRqlStructCoerce(*Types, Struct,
			[](const rql::Value& Value) { return Value.is_string() || Value.is_null() || Value.is_object(); });
		Adapter.equals = [](const rql::Value& Value, const rql::Value& Other) -> std::optional<bool>
		{
			if (!Value.is_struct() || !Value.ptr())
				return std::nullopt;
			const FSoftObjectPath& Path = *static_cast<const FSoftObjectPath*>(Value.ptr());
			if (Other.is_null())
				return Path.IsNull();
			if (!Other.is_string())
				return std::nullopt;
			const FString Text = UnrealMcpRqlFromStd(Other.as_string()).TrimStartAndEnd();
			// "/Game/Maps/Arena" matches "/Game/Maps/Arena.Arena" (the package path an edit field shows).
			return Path.ToString().Equals(Text, ESearchCase::IgnoreCase) || Path.GetLongPackageName().Equals(Text, ESearchCase::IgnoreCase);
		};
		Session.AddAdapter(std::move(Adapter));
	}
}

void UnrealMcpRqlAddStructAdapters(FUnrealMcpRqlSession& Session)
{
	FUnrealMcpRqlTypes* Types = &Session.Types();
	// The reflected structs of FSoftObjectPath / FSoftClassPath (no TBaseStructure for them).
	for (const TCHAR* Path : { TEXT("/Script/CoreUObject.SoftObjectPath"), TEXT("/Script/CoreUObject.SoftClassPath") })
	{
		if (UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, Path))
			AddSoftPathAdapter(Session, Struct);
	}
	for (const FMathStruct& Spec : MathStructs())
	{
		const rql::TypeDescriptor* Descriptor = Types->ForStruct(Spec.Struct);
		if (!Descriptor)
			continue;
		const FMathStruct* SpecPtr = &Spec; // MathStructs() is a function-local static: stable for the process
		rql::TypeAdapter Adapter;
		Adapter.name = UnrealMcpRqlToStd(Spec.Struct->GetName());
		Adapter.target = Descriptor;
		Adapter.description = UnrealMcpRqlToStd(Spec.Struct->GetName()) + ": write \"" + Spec.Shape + "\", a list of numbers or {member: value}";
		Adapter.encode = [SpecPtr](const rql::Value& Value, const rql::EncodeContext&) -> std::optional<rql::Json>
		{
			if (!Value.is_struct() || !Value.ptr())
				return std::nullopt;
			return rql::Json(Format(*SpecPtr, Value.ptr()));
		};
		// Structs and {X: 1} literals: RQL's own coercion.
		Adapter.coerce = UnrealMcpRqlStructCoerce(*Types, Spec.Struct, [](const rql::Value& Value) { return Value.is_string() || Value.is_list(); });
		// Equality with the text and list forms (where Location == "(0, 0, 100)"), approximate like RQL's .NET tuple
		// adapters: components within 1e-5.
		Adapter.equals = [SpecPtr](const rql::Value& Value, const rql::Value& Other) -> std::optional<bool>
		{
			if (!Value.is_struct() || !Value.ptr())
				return std::nullopt;
			double Mine[4] = { 0, 0, 0, 0 };
			double Theirs[4] = { 0, 0, 0, 0 };
			SpecPtr->Get(Value.ptr(), Mine);
			if (Other.is_struct() && Other.type() == Value.type() && Other.ptr())
				SpecPtr->Get(Other.ptr(), Theirs);
			else if (Other.is_string() || Other.is_list())
			{
				FString Ignored;
				if (!Components(*SpecPtr, Other, Theirs, Ignored))
					return false; // not this struct's form: not equal
			}
			else
				return std::nullopt;
			for (int32 Index = 0; Index < SpecPtr->Count; ++Index)
			{
				if (!(FMath::Abs(Mine[Index] - Theirs[Index]) <= 1e-5))
					return false;
			}
			return true;
		};
		Session.AddAdapter(std::move(Adapter));
	}
}
