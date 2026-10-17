// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRqlSession.h"
#include "UnrealMcpRqlSources.h"

#include "Math/Color.h"
#include "Math/IntPoint.h"
#include "Math/IntVector.h"
#include "Math/Quat.h"
#include "Math/Rotator.h"
#include "Math/Vector.h"
#include "Math/Vector2D.h"
#include "Math/Vector4.h"
#include "UObject/Class.h"

#include <cstdlib>
#include <string>
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

	/** Every number in a text such as "(1, 2.5, -3)", "X=1 Y=2 Z=3" or "P=0 Y=90 R=0". */
	std::vector<double> Numbers(const std::string& Text)
	{
		std::vector<double> Out;
		const char* Cursor = Text.c_str();
		while (*Cursor)
		{
			const char C = *Cursor;
			const bool bStarts = (C >= '0' && C <= '9') || C == '-' || C == '+' || C == '.';
			if (!bStarts)
			{
				++Cursor;
				continue;
			}
			char* End = nullptr;
			const double Value = std::strtod(Cursor, &End);
			if (End == Cursor)
			{
				++Cursor;
				continue;
			}
			Out.push_back(Value);
			Cursor = End;
		}
		return Out;
	}

	bool Hex(const std::string& Text, const FMathStruct& Spec, double* Out)
	{
		const FString Trimmed = UnrealMcpRqlFromStd(Text).TrimStartAndEnd();
		if (!Trimmed.StartsWith(TEXT("#")) || (Trimmed.Len() != 7 && Trimmed.Len() != 9))
			return false;
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

bool UnrealMcpRqlIsMathStruct(const UScriptStruct* Struct)
{
	return Find(Struct) != nullptr;
}

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

void UnrealMcpRqlAddStructAdapters(FUnrealMcpRqlSession& Session)
{
	FUnrealMcpRqlTypes* Types = &Session.Types();
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
		Adapter.coerce = [Types, SpecPtr](const rql::Value& Value, const rql::TypeDescriptor&, rql::Value& Result, std::string& Reason) -> rql::AdapterOutcome
		{
			if (!Value.is_string() && !Value.is_list())
				return rql::AdapterOutcome::NotHandled; // structs and {X: 1} literals: RQL's own coercion
			rql::Value Built = Types->WrapStruct(SpecPtr->Struct, nullptr);
			FString Error;
			if (!UnrealMcpRqlParseMathStruct(SpecPtr->Struct, Value, Built.ptr(), Error))
			{
				Reason = UnrealMcpRqlToStd(Error.IsEmpty() ? FString(TEXT("not a ")) + SpecPtr->Struct->GetName() : Error);
				return rql::AdapterOutcome::Failed;
			}
			Result = Built;
			return rql::AdapterOutcome::Ok;
		};
		Session.AddAdapter(std::move(Adapter));
	}
}
