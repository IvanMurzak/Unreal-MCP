// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpPinTypes.h"

#include "EdGraphSchema_K2.h"
#include "Tools/UnrealMcpObjectRef.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

bool UnrealMcpPinTypes::Make(const FString& InType, bool bIsArray, FEdGraphPinType& OutPinType, FString& OutError)
{
	const FString Type = InType.TrimStartAndEnd();
	const FString Lower = Type.ToLower();

	auto Set = [&OutPinType](FName Category, FName SubCategory, UObject* SubObject)
	{
		OutPinType = FEdGraphPinType();
		OutPinType.PinCategory = Category;
		OutPinType.PinSubCategory = SubCategory;
		OutPinType.PinSubCategoryObject = SubObject;
	};

	if (Lower == TEXT("bool") || Lower == TEXT("boolean"))
		Set(UEdGraphSchema_K2::PC_Boolean, NAME_None, nullptr);
	else if (Lower == TEXT("int") || Lower == TEXT("integer") || Lower == TEXT("int32"))
		Set(UEdGraphSchema_K2::PC_Int, NAME_None, nullptr);
	else if (Lower == TEXT("int64"))
		Set(UEdGraphSchema_K2::PC_Int64, NAME_None, nullptr);
	else if (Lower == TEXT("byte"))
		Set(UEdGraphSchema_K2::PC_Byte, NAME_None, nullptr);
	else if (Lower == TEXT("float") || Lower == TEXT("double") || Lower == TEXT("number") || Lower == TEXT("real"))
		Set(UEdGraphSchema_K2::PC_Real, UEdGraphSchema_K2::PC_Double, nullptr);
	else if (Lower == TEXT("string"))
		Set(UEdGraphSchema_K2::PC_String, NAME_None, nullptr);
	else if (Lower == TEXT("name"))
		Set(UEdGraphSchema_K2::PC_Name, NAME_None, nullptr);
	else if (Lower == TEXT("text"))
		Set(UEdGraphSchema_K2::PC_Text, NAME_None, nullptr);
	else if (Lower == TEXT("vector"))
		Set(UEdGraphSchema_K2::PC_Struct, NAME_None, TBaseStructure<FVector>::Get());
	else if (Lower == TEXT("vector2d"))
		Set(UEdGraphSchema_K2::PC_Struct, NAME_None, TBaseStructure<FVector2D>::Get());
	else if (Lower == TEXT("rotator"))
		Set(UEdGraphSchema_K2::PC_Struct, NAME_None, TBaseStructure<FRotator>::Get());
	else if (Lower == TEXT("transform"))
		Set(UEdGraphSchema_K2::PC_Struct, NAME_None, TBaseStructure<FTransform>::Get());
	else if (Lower == TEXT("color") || Lower == TEXT("linearcolor"))
		Set(UEdGraphSchema_K2::PC_Struct, NAME_None, TBaseStructure<FLinearColor>::Get());
	else
	{
		// Object / class / struct: a class (object reference), else a struct path, else a struct name ("GameplayTag",
		// "FGameplayTag").
		UScriptStruct* Struct = nullptr;
		if (UClass* Class = FUnrealMcpObjectRef::ResolveClass(Type))
		{
			Set(UEdGraphSchema_K2::PC_Object, NAME_None, Class);
		}
		else if (Type.Contains(TEXT("/")) && (Struct = LoadObject<UScriptStruct>(nullptr, *Type, nullptr, LOAD_NoWarn | LOAD_Quiet)) != nullptr)
		{
			Set(UEdGraphSchema_K2::PC_Struct, NAME_None, Struct);
		}
		else if (!Type.Contains(TEXT("/"))
			&& ((Struct = FindFirstObject<UScriptStruct>(*Type, EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging)) != nullptr
				|| (Type.Len() > 1 && Type[0] == TEXT('F') && FChar::IsUpper(Type[1])
					&& (Struct = FindFirstObject<UScriptStruct>(*Type.Mid(1), EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging)) != nullptr)))
		{
			Set(UEdGraphSchema_K2::PC_Struct, NAME_None, Struct);
		}
		else
		{
			OutError = FString::Printf(TEXT("unknown variable type '%s' (expected a primitive, a math struct, or a resolvable object/struct path)"), *Type);
			return false;
		}
	}

	OutPinType.ContainerType = bIsArray ? EPinContainerType::Array : EPinContainerType::None;
	return true;
}

FString UnrealMcpPinTypes::ToString(const FEdGraphPinType& PinType)
{
	const FName Cat = PinType.PinCategory;
	FString Base;
	if (Cat == UEdGraphSchema_K2::PC_Boolean) Base = TEXT("bool");
	else if (Cat == UEdGraphSchema_K2::PC_Int) Base = TEXT("int");
	else if (Cat == UEdGraphSchema_K2::PC_Int64) Base = TEXT("int64");
	else if (Cat == UEdGraphSchema_K2::PC_Byte) Base = TEXT("byte");
	else if (Cat == UEdGraphSchema_K2::PC_Real) Base = TEXT("float"); // float/double both map to PC_Real (PC_Float/PC_Double are legacy-unreachable here)
	else if (Cat == UEdGraphSchema_K2::PC_String) Base = TEXT("string");
	else if (Cat == UEdGraphSchema_K2::PC_Name) Base = TEXT("name");
	else if (Cat == UEdGraphSchema_K2::PC_Text) Base = TEXT("text");
	else if (Cat == UEdGraphSchema_K2::PC_Struct)
		Base = PinType.PinSubCategoryObject.IsValid() ? PinType.PinSubCategoryObject->GetName() : TEXT("struct");
	else if (Cat == UEdGraphSchema_K2::PC_Object || Cat == UEdGraphSchema_K2::PC_Class)
		Base = PinType.PinSubCategoryObject.IsValid() ? PinType.PinSubCategoryObject->GetName() : TEXT("object");
	else
		Base = Cat.ToString();

	if (PinType.ContainerType == EPinContainerType::Array)
		return Base + TEXT("[]");
	if (PinType.ContainerType == EPinContainerType::Set)
		return Base + TEXT("{}");
	if (PinType.ContainerType == EPinContainerType::Map)
		return Base + TEXT("<map>");
	return Base;
}
