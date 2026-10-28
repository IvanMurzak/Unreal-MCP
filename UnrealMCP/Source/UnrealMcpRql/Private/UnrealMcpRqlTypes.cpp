// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRqlTypes.h"

#include "UnrealMcpRqlSources.h"
#include "UnrealMcpRqlUtil.h"
#include "Tools/UnrealMcpObjectRef.h"

#include "Containers/StringConv.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/World.h"
#include "Engine/LevelScriptActor.h"
#include "GameFramework/Actor.h"
#include "HAL/UnrealMemory.h"
#include "Misc/OutputDeviceNull.h"
#include "UObject/Class.h"
#include "UObject/EnumProperty.h"
#include "UObject/Object.h"
#include "UObject/PropertyPortFlags.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"
#if __has_include("UObject/StrProperty.h")
#include "UObject/StrProperty.h" // FStrProperty moved out of UnrealType.h in UE 5.6+
#endif
#include "UObject/TextProperty.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#if __has_include("StructUtils/UserDefinedStruct.h")
#include "StructUtils/UserDefinedStruct.h" // moved from Engine/ to CoreUObject in UE 5.8
#else
#include "Engine/UserDefinedStruct.h"
#endif
#include "UObject/StructOnScope.h"
#include "UObject/UObjectArray.h"
#include "UObject/UnrealType.h"

#include <cmath>
#include <stdexcept>

// ------------------------------------------------------------------ text

std::string UnrealMcpRqlToStd(const FString& Text)
{
	FTCHARToUTF8 Converted(*Text);
	return std::string(reinterpret_cast<const char*>(Converted.Get()), static_cast<size_t>(Converted.Length()));
}

FString UnrealMcpRqlFromStd(const std::string& Text)
{
	FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Text.data()), static_cast<int32>(Text.size()));
	return FString(Converted.Length(), Converted.Get());
}

namespace UnrealMcpRqlTypesLocal
{
	using UnrealMcpRqlUtil::Throw;
	using UnrealMcpRqlUtil::ThrowMismatch;

	void* IdentityCast(void* Pointer)
	{
		return Pointer;
	}

	/** Struct memory owned by a shared_ptr (struct values are copies, RQL writes them back through the owner). */
	std::shared_ptr<void> MakeStructData(UScriptStruct* Struct, const void* Source)
	{
		const int32 Size = FMath::Max(Struct->GetStructureSize(), 1);
		void* Memory = FMemory::Malloc(Size, FMath::Max(Struct->GetMinAlignment(), 1));
		FMemory::Memzero(Memory, Size);
		Struct->InitializeStruct(Memory);
		if (Source)
			Struct->CopyScriptStruct(Memory, Source);
		return std::shared_ptr<void>(Memory, [Struct](void* Pointer)
		{
			Struct->DestroyStruct(Pointer);
			FMemory::Free(Pointer);
		});
	}

	/** One initialized property value on the heap (map keys, defaults), destroyed with the scope. */
	struct FTempValue
	{
		const FProperty* Property;
		void* Ptr;

		explicit FTempValue(const FProperty* InProperty) : Property(InProperty), Ptr(InProperty->AllocateAndInitializeValue()) {}
		~FTempValue() { Property->DestroyAndFreeValue(Ptr); }
		FTempValue(const FTempValue&) = delete;
		FTempValue& operator=(const FTempValue&) = delete;
	};

	bool IsUnsigned(const FProperty* Property)
	{
		return Property->IsA<FByteProperty>() || Property->IsA<FUInt16Property>() || Property->IsA<FUInt32Property>()
			|| Property->IsA<FUInt64Property>();
	}

	bool IsOwning(const FProperty* Property)
	{
		if (!Property->IsA<FObjectPropertyBase>() && !Property->IsA<FArrayProperty>() && !Property->IsA<FSetProperty>()
			&& !Property->IsA<FMapProperty>())
			return true; // values (numbers, strings, structs) are part of their owner
		return Property->HasAnyPropertyFlags(CPF_InstancedReference | CPF_PersistentInstance | CPF_ContainsInstancedReference);
	}

	bool IsExposedOnClass(const FProperty* Property)
	{
		return !Property->HasAnyPropertyFlags(CPF_Deprecated) && Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible);
	}

	bool IsBlueprintWritable(const FProperty* Property)
	{
		return Property->HasAnyPropertyFlags(CPF_BlueprintVisible) && !Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly);
	}

	bool IsWritableOnClass(const FProperty* Property)
	{
		if (Property->HasAnyPropertyFlags(CPF_Edit) && !Property->HasAnyPropertyFlags(CPF_EditConst))
			return true;
		return IsBlueprintWritable(Property);
	}

	/**
	 * A property of a Blueprint actor class that AActor::ResetPropertiesForConstruction copies back from the class default
	 * before every construction script run: Blueprint-writable but not editable per instance (a new Blueprint variable
	 * without "Instance Editable", a native EditDefaultsOnly + BlueprintReadWrite one). Only Blueprint classes rerun their
	 * construction script; level script actors and delegates are not reset.
	 */
	bool IsResetByConstruction(const UClass* ActorClass, const FProperty* Property)
	{
		if (!ActorClass || !Cast<UBlueprintGeneratedClass>(ActorClass) || !ActorClass->IsChildOf(AActor::StaticClass())
			|| ActorClass->IsChildOf(ALevelScriptActor::StaticClass()))
			return false;
		if (Property->IsA<FDelegateProperty>() || Property->IsA<FMulticastDelegateProperty>())
			return false;
		// Instanced sub-objects (component variables) are kept, unless they sit in a container the Blueprint added.
		const bool bContainer = Property->IsA<FArrayProperty>() || Property->IsA<FSetProperty>() || Property->IsA<FMapProperty>();
		if (Property->ContainsInstancedObjectProperty() && !bContainer)
			return false;
		const bool bInstanceEditable = Property->HasAnyPropertyFlags(CPF_Edit) && !Property->HasAnyPropertyFlags(CPF_DisableEditOnInstance);
		return !bInstanceEditable && IsBlueprintWritable(Property);
	}

	double ToNumber(const rql::Value& Value)
	{
		if (Value.is_number())
			return Value.as_number();
		if (Value.is_bool())
			return Value.as_bool() ? 1.0 : 0.0;
		if (Value.is_enum())
			return static_cast<double>(Value.enum_value());
		double Parsed = 0.0;
		if (Value.is_string() && rql::parse_double(Value.as_string(), Parsed))
			return Parsed;
		ThrowMismatch(TEXT("a number"), Value);
	}

	FString ToText(const rql::Value& Value)
	{
		if (Value.is_string() || Value.is_null() || Value.is_number() || Value.is_bool() || Value.is_enum())
			return UnrealMcpRqlUtil::TextArg(Value);
		ThrowMismatch(TEXT("text"), Value);
	}

	int64 ToEnumValue(const UEnum* Enum, const rql::Value& Value)
	{
		if (Value.is_enum())
			return Value.enum_value();
		if (Value.is_number())
			return static_cast<int64>(Value.as_number());
		if (Value.is_string())
		{
			// Short ("ECC_Pawn") or full ("ECollisionChannel::ECC_Pawn") names, case-insensitively (FName rules).
			const int64 Found = Enum->GetValueByNameString(UnrealMcpRqlFromStd(Value.as_string()));
			if (Found != INDEX_NONE)
				return Found;
		}
		Throw(FString::Printf(TEXT("'%s' is not a value of %s"), *UnrealMcpRqlFromStd(rql::describe_value(Value)), *Enum->GetName()));
	}

	/** The items of a list value, copied first (the source may be a view of the very container being written). */
	std::vector<rql::Value> ListItems(const rql::Value& Value)
	{
		if (!Value.is_list())
			ThrowMismatch(TEXT("a list"), Value);
		std::vector<rql::Value> Items;
		for (size_t Index = 0, Count = Value.list().size(); Index < Count; ++Index)
			Items.push_back(rql::snapshot(Value.list().at(Index)));
		return Items;
	}

	/** A value read from a temporary buffer must not keep views into it. */
	rql::Value Detached(const rql::Value& Value)
	{
		return rql::snapshot(Value);
	}
}

using namespace UnrealMcpRqlTypesLocal;

// ------------------------------------------------------------------ container adapters

namespace UnrealMcpRqlTypesLocal
{
	/** A live TArray property value. Element writes go straight into the array (the owner is notified on commit). */
	class FArrayAdapter final : public rql::IListAdapter
	{
	public:
		FArrayAdapter(FUnrealMcpRqlTypes* InTypes, const FArrayProperty* InProperty, void* InData, const rql::TypeDescriptor* InType)
			: Types(InTypes), Property(InProperty), Data(InData), Type(InType)
		{
		}

		size_t size() const override
		{
			FScriptArrayHelper Helper(Property, Data);
			return static_cast<size_t>(Helper.Num());
		}
		rql::Value at(size_t Index) const override
		{
			FScriptArrayHelper Helper(Property, Data);
			if (static_cast<int32>(Index) >= Helper.Num())
				return rql::Value::missing();
			return Types->Read(Property->Inner, Helper.GetRawPtr(static_cast<int32>(Index)), Type ? Type->element : nullptr);
		}
		const rql::TypeDescriptor* type() const override { return Type; }
		const void* identity() const override { return Data; }
		bool writable() const override { return true; }
		void set(size_t Index, const rql::Value& Value) override
		{
			FScriptArrayHelper Helper(Property, Data);
			if (static_cast<int32>(Index) >= Helper.Num())
				Throw(FString::Printf(TEXT("index %d is out of range (%d elements)"), static_cast<int32>(Index), Helper.Num()));
			Types->Write(Property->Inner, Helper.GetRawPtr(static_cast<int32>(Index)), Value);
		}

	private:
		FUnrealMcpRqlTypes* Types;
		const FArrayProperty* Property;
		void* Data;
		const rql::TypeDescriptor* Type;
	};

	/** A live TMap property value (iteration in the map's own order, which is deterministic). */
	class FMapAdapter final : public rql::IMapAdapter
	{
	public:
		FMapAdapter(FUnrealMcpRqlTypes* InTypes, const FMapProperty* InProperty, void* InData, const rql::TypeDescriptor* InType)
			: Types(InTypes), Property(InProperty), Data(InData), Type(InType)
		{
		}

		size_t size() const override
		{
			FScriptMapHelper Helper(Property, Data);
			return static_cast<size_t>(Helper.Num());
		}
		void for_each(const std::function<bool(const rql::Value&, const rql::Value&)>& Fn) const override
		{
			FScriptMapHelper Helper(Property, Data);
			for (int32 Index = 0, Max = Helper.GetMaxIndex(); Index < Max; ++Index)
			{
				if (!Helper.IsValidIndex(Index))
					continue;
				if (!Fn(Types->Read(Property->KeyProp, Helper.GetKeyPtr(Index)), Types->Read(Property->ValueProp, Helper.GetValuePtr(Index))))
					return;
			}
		}
		rql::MapLookup find(const rql::Value& Key, rql::Value& Out) const override
		{
			FTempValue KeyValue(Property->KeyProp);
			try
			{
				Types->Write(Property->KeyProp, KeyValue.Ptr, Key);
			}
			catch (const std::exception&)
			{
				return rql::MapLookup::BadKey;
			}
			FScriptMapHelper Helper(Property, Data);
			uint8* Found = Helper.FindValueFromHash(KeyValue.Ptr);
			if (!Found)
				return rql::MapLookup::NotFound;
			Out = Types->Read(Property->ValueProp, Found);
			return rql::MapLookup::Found;
		}
		const rql::TypeDescriptor* type() const override { return Type; }
		const void* identity() const override { return Data; }
		bool writable() const override { return true; }
		bool accepts_key(const rql::Value& Key) const override
		{
			FTempValue KeyValue(Property->KeyProp);
			try
			{
				Types->Write(Property->KeyProp, KeyValue.Ptr, Key);
				return true;
			}
			catch (const std::exception&)
			{
				return false;
			}
		}
		void set(const rql::Value& Key, const rql::Value& Value) override
		{
			FTempValue KeyValue(Property->KeyProp);
			Types->Write(Property->KeyProp, KeyValue.Ptr, Key);
			FScriptMapHelper Helper(Property, Data);
			if (uint8* Found = Helper.FindValueFromHash(KeyValue.Ptr))
			{
				Types->Write(Property->ValueProp, Found, Value);
				return;
			}
			// Converted before the entry exists: a value that does not fit adds nothing.
			FTempValue Item(Property->ValueProp);
			Types->Write(Property->ValueProp, Item.Ptr, Value);
			Helper.AddPair(KeyValue.Ptr, Item.Ptr);
		}
		void erase(const rql::Value& Key) override
		{
			FTempValue KeyValue(Property->KeyProp);
			Types->Write(Property->KeyProp, KeyValue.Ptr, Key);
			FScriptMapHelper Helper(Property, Data);
			Helper.RemovePair(KeyValue.Ptr);
		}

	private:
		FUnrealMcpRqlTypes* Types;
		const FMapProperty* Property;
		void* Data;
		const rql::TypeDescriptor* Type;
	};
}

// ------------------------------------------------------------------ registry

FUnrealMcpRqlTypes::FUnrealMcpRqlTypes()
	: RegistryPtr(std::make_unique<rql::TypeRegistry>())
{
	ObjectsReplacedHandle = FCoreUObjectDelegates::OnObjectsReplaced.AddRaw(this, &FUnrealMcpRqlTypes::OnObjectsReplaced);
}

FUnrealMcpRqlTypes::~FUnrealMcpRqlTypes()
{
	FCoreUObjectDelegates::OnObjectsReplaced.Remove(ObjectsReplacedHandle);
}

void FUnrealMcpRqlTypes::OnObjectsReplaced(const TMap<UObject*, UObject*>& OldToNew)
{
	for (const TPair<UObject*, UObject*>& Pair : OldToNew)
	{
		if (!Pair.Key || !Pair.Value)
			continue;
		// Earlier copies lead to the newest one: a weak pointer to a discarded copy no longer resolves.
		const TWeakObjectPtr<UObject> Discarded(Pair.Key);
		for (TPair<const UObject*, TWeakObjectPtr<UObject>>& Entry : Replaced)
		{
			if (Entry.Value.HasSameIndexAndSerialNumber(Discarded)) // == would match any two invalid pointers
				Entry.Value = Pair.Value;
		}
		Replaced.Add(Pair.Key, Pair.Value);
	}
}

rql::TypeDescriptor& FUnrealMcpRqlTypes::Shell(const void* Key, const FString& Name, rql::TypeKind Kind)
{
	rql::TypeDescriptor* Descriptor = RegistryPtr->descriptor_for(Key);
	Descriptor->name = UnrealMcpRqlToStd(Name);
	Descriptor->friendly = Descriptor->name;
	Descriptor->kind = Kind;
	Descriptor->registered = true;
	return *Descriptor;
}

const rql::TypeDescriptor* FUnrealMcpRqlTypes::ForClass(UClass* Class)
{
	if (!Class)
		return nullptr;
	if (rql::TypeDescriptor** Found = Classes.Find(Class))
		return *Found;
	const FString* Renamed = ApiClasses.Find(Class);
	rql::TypeDescriptor& Descriptor = Shell(Class, Renamed && !Renamed->IsEmpty() ? *Renamed : Class->GetName(), rql::TypeKind::Object);
	// A Blueprint's class is BP_Door_C; `is BP_Door` (the asset name agents see) matches it and its subclasses too, unless
	// a native class has that name (a Blueprint "PointLight" must not make `is PointLight` match it).
	if (Cast<UBlueprintGeneratedClass>(Class) && Class->GetName().EndsWith(TEXT("_C"), ESearchCase::CaseSensitive))
	{
		const FString Alias = Class->GetName().LeftChop(2);
		const UClass* Named = FindFirstObject<UClass>(*Alias, EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging);
		if (!Named || !Named->HasAnyClassFlags(CLASS_Native))
			Descriptor.also_is.push_back(UnrealMcpRqlToStd(Alias));
	}
	Classes.Add(Class, &Descriptor);
	ClassByDescriptor.Add(&Descriptor, Class);
	if (UClass* Super = Class->GetSuperClass())
	{
		Descriptor.base = ForClass(Super);
		Descriptor.to_base = &IdentityCast; // UObject classes are single-inheritance: one pointer for every base
		Descriptor.from_base = &IdentityCast;
	}
	return &Descriptor;
}

const rql::TypeDescriptor* FUnrealMcpRqlTypes::ForStruct(UScriptStruct* Struct)
{
	if (!Struct)
		return nullptr;
	if (rql::TypeDescriptor** Found = Structs.Find(Struct))
		return *Found;
	rql::TypeDescriptor& Descriptor = Shell(Struct, Struct->GetName(), rql::TypeKind::Struct);
	Descriptor.copy = [Struct](const void* Source) { return MakeStructData(Struct, Source); };
	// An object literal ({Damage: 5}) constructs the struct before reading its members: fill them first, the struct may
	// not have been read yet in this session.
	const rql::TypeDescriptor* Constructed = &Descriptor;
	Descriptor.create = [this, Struct, Constructed]() { Fill(Constructed); return MakeStructData(Struct, nullptr); };
	Structs.Add(Struct, &Descriptor);
	StructByDescriptor.Add(&Descriptor, Struct);
	return &Descriptor;
}

const rql::TypeDescriptor* FUnrealMcpRqlTypes::ForEnum(UEnum* Enum)
{
	if (!Enum)
		return nullptr;
	if (rql::TypeDescriptor** Found = Enums.Find(Enum))
		return *Found;
	rql::TypeDescriptor& Descriptor = Shell(Enum, Enum->GetName(), rql::TypeKind::Enum);
	const int32 Count = Enum->ContainsExistingMax() ? Enum->NumEnums() - 1 : Enum->NumEnums();
	TArray<int32> Visible;
	TSet<FString> EntryNames; // FString hashes and compares case-insensitively
	for (int32 Index = 0; Index < Count; ++Index)
	{
		if (Enum->HasMetaData(TEXT("Hidden"), Index))
			continue;
		const FString Name = Enum->GetNameStringByIndex(Index);
		Visible.Add(Index);
		EntryNames.Add(Name);
		Descriptor.enum_values.emplace_back(UnrealMcpRqlToStd(Name), Enum->GetValueByIndex(Index));
	}
#if WITH_METADATA
	// The editor's display names (UMETA(DisplayName = "No Collision")) resolve like entry names (RQL enum aliases). One
	// that spells another entry's name is left out: it would make that name ambiguous.
	for (const int32 Index : Visible)
	{
		const FString Display = Enum->GetMetaData(TEXT("DisplayName"), Index);
		if (!Display.IsEmpty() && !EntryNames.Contains(Display))
			Descriptor.enum_aliases.emplace_back(UnrealMcpRqlToStd(Display), Enum->GetValueByIndex(Index));
	}
#endif
	Enums.Add(Enum, &Descriptor);
	return &Descriptor;
}

const rql::TypeDescriptor* FUnrealMcpRqlTypes::ForProperty(const FProperty* Property, bool& bOutNullable)
{
	bOutNullable = false;
	if (!Property)
		return nullptr;
	if (Property->IsA<FBoolProperty>())
		return rql::builtin::bool_type();
	if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		return ForEnum(EnumProperty->GetEnum());
	if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
	{
		if (ByteProperty->Enum)
			return ForEnum(ByteProperty->Enum);
	}
	if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
	{
		if (Numeric->IsFloatingPoint())
			return Property->GetElementSize() == 4 ? rql::builtin::single_type() : rql::builtin::double_type();
		return rql::builtin::number_type(true, !IsUnsigned(Property), static_cast<size_t>(Property->GetElementSize()));
	}
	if (Property->IsA<FStrProperty>() || Property->IsA<FNameProperty>() || Property->IsA<FTextProperty>())
		return rql::builtin::string_type();
	if (Property->IsA<FSoftObjectProperty>()) // before FObjectPropertyBase: a soft reference reads as its path
	{
		bOutNullable = true;
		return rql::builtin::string_type();
	}
	if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
	{
		bOutNullable = true;
		return ForClass(ObjectProperty->PropertyClass);
	}
	if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		return ForStruct(StructProperty->Struct);
	if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
	{
		bool bElementNullable = false;
		const rql::TypeDescriptor* Element = ForProperty(ArrayProperty->Inner, bElementNullable);
		return Element ? RegistryPtr->list_type(Element, bElementNullable) : nullptr;
	}
	if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
	{
		bool bElementNullable = false;
		const rql::TypeDescriptor* Element = ForProperty(SetProperty->ElementProp, bElementNullable);
		return Element ? RegistryPtr->list_type(Element, bElementNullable) : nullptr;
	}
	if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
	{
		bool bKeyNullable = false;
		bool bValueNullable = false;
		const rql::TypeDescriptor* Key = ForProperty(MapProperty->KeyProp, bKeyNullable);
		const rql::TypeDescriptor* Value = ForProperty(MapProperty->ValueProp, bValueNullable);
		return Key && Value ? RegistryPtr->map_type(Key, Value, bValueNullable) : nullptr;
	}
	return nullptr; // delegates, interfaces, field paths, optionals: not exposed
}

UClass* FUnrealMcpRqlTypes::ClassOf(const rql::TypeDescriptor* Descriptor) const
{
	UClass* const* Found = Descriptor ? ClassByDescriptor.Find(Descriptor) : nullptr;
	return Found ? *Found : nullptr;
}

void FUnrealMcpRqlTypes::AddNativeType(const rql::TypeDescriptor* Descriptor)
{
	if (Descriptor)
		NativeTypes.AddUnique(Descriptor);
}

const rql::TypeDescriptor* FUnrealMcpRqlTypes::FindByName(const FString& InName)
{
	const FString Name = InName.TrimStartAndEnd();
	if (Name.IsEmpty())
		return nullptr;
	const std::string Wanted = UnrealMcpRqlToStd(Name);
	for (const rql::TypeDescriptor* Native : NativeTypes)
	{
		if (rql::iequals(Native->name, Wanted))
			return Native;
	}
	for (const TPair<UClass*, FString>& Api : ApiClasses)
	{
		if (!Api.Value.IsEmpty() && Api.Value.Equals(Name, ESearchCase::IgnoreCase))
			return ForClass(Api.Key);
	}
	// Classes resolve like everywhere else (FUnrealMcpObjectRef::ResolveClass: paths, Blueprint assets, A/U prefixes).
	if (UClass* Class = FUnrealMcpObjectRef::ResolveClass(Name))
		return ForClass(Class);
	auto TryFind = [this](const FString& Candidate) -> const rql::TypeDescriptor*
	{
		if (Candidate.Contains(TEXT("/")) || Candidate.Contains(TEXT(".")))
		{
			if (UScriptStruct* StructByPath = FindObject<UScriptStruct>(nullptr, *Candidate))
				return ForStruct(StructByPath);
			if (UEnum* EnumByPath = FindObject<UEnum>(nullptr, *Candidate))
				return ForEnum(EnumByPath);
			return nullptr;
		}
		if (UScriptStruct* Struct = FindFirstObject<UScriptStruct>(*Candidate, EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging))
			return ForStruct(Struct);
		if (UEnum* Enum = FindFirstObject<UEnum>(*Candidate, EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging))
			return ForEnum(Enum);
		return nullptr;
	};
	if (const rql::TypeDescriptor* Found = TryFind(Name))
		return Found;
	// C++ spellings of structs and enums: FVector, ECollisionChannel.
	if (Name.Len() > 1 && (Name[0] == TEXT('F') || Name[0] == TEXT('E')) && FChar::IsUpper(Name[1]))
		return TryFind(Name.Mid(1));
	return nullptr;
}

// ------------------------------------------------------------------ members and methods

void FUnrealMcpRqlTypes::SetMember(rql::TypeDescriptor& Descriptor, rql::MemberDescriptor Member)
{
	for (rql::MemberDescriptor& Existing : Descriptor.members)
	{
		if (Existing.name == Member.name)
		{
			Existing = std::move(Member);
			return;
		}
	}
	Descriptor.members.push_back(std::move(Member));
}

void FUnrealMcpRqlTypes::AddMethod(rql::TypeDescriptor& Descriptor, rql::MethodDescriptor Method)
{
	for (rql::MethodDescriptor& Existing : Descriptor.methods)
	{
		if (Existing.name == Method.name && Existing.params.size() == Method.params.size())
		{
			Existing = std::move(Method);
			return;
		}
	}
	Descriptor.methods.push_back(std::move(Method));
}

rql::ParamDescriptor FUnrealMcpRqlTypes::Param(const char* Name, const rql::TypeDescriptor* Type, bool bNullable)
{
	rql::ParamDescriptor Result;
	Result.name = Name;
	Result.type = Type;
	Result.nullable = bNullable;
	return Result;
}

rql::ParamDescriptor FUnrealMcpRqlTypes::OptionalParam(const char* Name, const rql::TypeDescriptor* Type, rql::Value Default, bool bNullable)
{
	rql::ParamDescriptor Result = Param(Name, Type, bNullable);
	Result.has_default = true;
	Result.default_value = std::move(Default);
	return Result;
}

void FUnrealMcpRqlTypes::AddClassHook(UClass* Class, FClassHook Hook)
{
	if (Class && Hook)
		Hooks.Add(Class, MoveTemp(Hook));
}

void FUnrealMcpRqlTypes::AddStructHook(UScriptStruct* Struct, FClassHook Hook)
{
	if (Struct && Hook)
		StructHooks.Add(Struct, MoveTemp(Hook));
}

void FUnrealMcpRqlTypes::AddStructParser(UScriptStruct* Struct, FStructParser Parser)
{
	if (Struct && Parser)
		StructParsers.Add(Struct, MoveTemp(Parser));
}

rql::TypeDescriptor& FUnrealMcpRqlTypes::NativeObjectType(const void* Key, const FString& Name, const FString& Description)
{
	rql::TypeDescriptor& Descriptor = Shell(Key, Name, rql::TypeKind::Object);
	Descriptor.description = UnrealMcpRqlToStd(Description);
	Descriptor.id_name.clear();
	Descriptor.id_explicit = true;
	AddNativeType(&Descriptor);
	return Descriptor;
}

void FUnrealMcpRqlTypes::AddApiClass(UClass* Class, const FString& TypeName)
{
	if (Class)
		ApiClasses.Add(Class, TypeName);
}

void FUnrealMcpRqlTypes::ExposeProperties(UStruct* Owner, const TArray<FName>& Names, bool bWritable)
{
	if (!Owner)
		return;
	TMap<FName, bool>& ForOwner = Exposed.FindOrAdd(Owner);
	for (const FName& Name : Names)
		ForOwner.Add(Name, bWritable);
}

bool FUnrealMcpRqlTypes::IsKnownMemberName(const FString& Name)
{
	if (Name.IsEmpty())
		return false;
	if (const bool* Cached = KnownMemberNames.Find(Name))
		return *Cached;
	auto Lookup = [this, &Name]()
	{
		// The descriptors this session filled (hook members: Label, Components, Scalars, ...) and the native types.
		const std::string Member = UnrealMcpRqlToStd(Name);
		for (const rql::TypeDescriptor* Descriptor : Filled)
		{
			if (Descriptor->find(Member))
				return true;
		}
		for (const rql::TypeDescriptor* Descriptor : NativeTypes)
		{
			if (Descriptor && Descriptor->find(Member))
				return true;
		}
		// The properties FillClass / FillStruct would make members, on every loaded class and struct. FName lookup ignores
		// case, and a name no FName was ever made of is no property of a native type (a typo costs one name-table lookup).
		const FName Wanted(*Name, FNAME_Find);
		if (!Wanted.IsNone())
		{
			for (TObjectIterator<UClass> It; It; ++It)
			{
				const TMap<FName, bool>* Forced = Exposed.Find(*It);
				for (TFieldIterator<FProperty> Property(*It, EFieldIteratorFlags::ExcludeSuper); Property; ++Property)
				{
					if (Property->GetFName() == Wanted && (IsExposedOnClass(*Property) || (Forced && Forced->Contains(Wanted))))
						return true;
				}
			}
		}
		for (TObjectIterator<UScriptStruct> It; It; ++It)
		{
			// User-defined struct fields are members under their authored name (Health), not their FName (Health_12_3F9A...).
			const bool bUserDefined = It->IsA<UUserDefinedStruct>();
			if (Wanted.IsNone() && !bUserDefined)
				continue;
			for (TFieldIterator<FProperty> Property(*It, EFieldIteratorFlags::ExcludeSuper); Property; ++Property)
			{
				const bool bNamed = bUserDefined ? Property->GetAuthoredName().Equals(Name, ESearchCase::IgnoreCase) : Property->GetFName() == Wanted;
				if (bNamed && !Property->HasAnyPropertyFlags(CPF_Deprecated))
					return true;
			}
		}
		// Members only class / struct hooks add (Scalars on materials, Blueprint graphs, extension members), on types this
		// session has not filled yet. The engine asks while it reports diagnostics, after the streams ran, so filling is safe.
		TArray<UClass*> HookedClasses;
		Hooks.GetKeys(HookedClasses);
		for (UClass* Class : HookedClasses)
		{
			const rql::TypeDescriptor* Descriptor = ForClass(Class);
			Fill(Descriptor);
			if (Descriptor && Descriptor->find(Member))
				return true;
		}
		TArray<UScriptStruct*> HookedStructs;
		StructHooks.GetKeys(HookedStructs);
		for (UScriptStruct* Struct : HookedStructs)
		{
			const rql::TypeDescriptor* Descriptor = ForStruct(Struct);
			Fill(Descriptor);
			if (Descriptor && Descriptor->find(Member))
				return true;
		}
		return false;
	};
	const bool bKnown = Lookup();
	KnownMemberNames.Add(Name, bKnown);
	return bKnown;
}

void FUnrealMcpRqlTypes::Fill(const rql::TypeDescriptor* Constant)
{
	if (!Constant || Filled.Contains(Constant))
		return;
	rql::TypeDescriptor* Descriptor = const_cast<rql::TypeDescriptor*>(Constant); // ours: created by Shell()
	if (UClass* Class = ClassOf(Constant))
	{
		Filled.Add(Constant);
		FillClass(*Descriptor, Class);
	}
	else if (UScriptStruct* const* Struct = StructByDescriptor.Find(Constant))
	{
		Filled.Add(Constant);
		FillStruct(*Descriptor, *Struct);
	}
}

void FUnrealMcpRqlTypes::FillClass(rql::TypeDescriptor& Descriptor, UClass* Class)
{
	if (Descriptor.base)
	{
		Fill(Descriptor.base);
		// Every UObject class has one pointer for all its bases (IdentityCast), so inherited members and methods are
		// used as they are.
		Descriptor.members = Descriptor.base->members;
		Descriptor.methods = Descriptor.base->methods;
		Descriptor.id_name = Descriptor.base->id_name;
		Descriptor.id_explicit = Descriptor.base->id_explicit;
		Descriptor.display_name = Descriptor.base->display_name;
		Descriptor.display_explicit = Descriptor.base->display_explicit;
	}
	const TMap<FName, bool>* Forced = Exposed.Find(Class);
	for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		FProperty* Property = *It;
		const bool* ForcedWritable = Forced ? Forced->Find(Property->GetFName()) : nullptr;
		if (!ForcedWritable && !IsExposedOnClass(Property))
			continue;
		AddPropertyMember(Descriptor, Property, /*bOnObject*/ true, ForcedWritable && *ForcedWritable);
	}
	for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if (IsCallable(Class, *It))
			AddFunctionMethod(Descriptor, *It);
	}
	TArray<FClassHook*> ClassHooks;
	Hooks.MultiFindPointer(Class, ClassHooks, /*bMaintainOrder*/ true);
	for (FClassHook* Hook : ClassHooks)
		(*Hook)(*this, Descriptor);
}

void FUnrealMcpRqlTypes::FillStruct(rql::TypeDescriptor& Descriptor, UScriptStruct* Struct)
{
	for (TFieldIterator<FProperty> It(Struct); It; ++It) // structs are data: every property, base structs included
	{
		FProperty* Property = *It;
		if (Property->HasAnyPropertyFlags(CPF_Deprecated))
			continue;
		AddPropertyMember(Descriptor, Property, /*bOnObject*/ false, /*bForceWritable*/ true);
	}
	Descriptor.id_name.clear();
	Descriptor.id_explicit = true;
	Descriptor.display_name.clear();
	Descriptor.display_explicit = true;
	TArray<FClassHook*> Reshape;
	StructHooks.MultiFindPointer(Struct, Reshape, /*bMaintainOrder*/ true);
	for (FClassHook* Hook : Reshape)
		(*Hook)(*this, Descriptor);
}

void FUnrealMcpRqlTypes::AddPropertyMember(rql::TypeDescriptor& Descriptor, FProperty* Property, bool bOnObject, bool bForceWritable)
{
	if (Property->GetSize() != Property->GetElementSize())
		return; // fixed-size C arrays (int32 X[4]) are not exposed
	bool bNullable = false;
	const rql::TypeDescriptor* Type = ForProperty(Property, bNullable);
	if (!Type)
		return;
	// Struct members (and struct elements / values of containers) get their members now: a query can name them before a
	// value of the struct was ever read (where Row.Damage > 3, set Row = {Damage: 5}). Filled is set before filling, so
	// self-referencing structs (TArray<FSelf>) terminate.
	for (const rql::TypeDescriptor* Value = Type; Value; Value = Value->element)
	{
		if (Value->kind == rql::TypeKind::Struct)
			Fill(Value);
		if (Value->kind != rql::TypeKind::List && Value->kind != rql::TypeKind::Map)
			break;
	}

	rql::MemberDescriptor Member;
	Member.name = UnrealMcpRqlToStd(Property->GetAuthoredName());
	Member.type = Type;
	Member.nullable = bNullable;
	Member.owning = IsOwning(Property);
	Member.readonly = bOnObject ? !(bForceWritable || IsWritableOnClass(Property)) : false;
	if (bOnObject && !bForceWritable && !Member.readonly && IsResetByConstruction(Property->GetOwnerClass(), Property))
	{
		// Writable on actors in Play In Editor; WriteObjectProperty refuses it on placed (editor) actors.
		Member.description = "not Instance Editable: placed actors always use the Blueprint default; change it with "
			"blueprint-set-default, or make the variable Instance Editable (blueprint-modify-variable instanceEditable=true)";
	}
	// Containers of an object are live views: an element write (set Tags[0] = ...) goes into a copy assigned back through
	// the setter, so it gets the editor notifications and honours the member's read-only flag like a whole-container write.
	Member.copy_on_read = bOnObject
		&& (Property->IsA<FArrayProperty>() || Property->IsA<FSetProperty>() || Property->IsA<FMapProperty>());
	FUnrealMcpRqlTypes* Self = this;
	// On an object, read the live one: the receipt reads a value back after a write that may have rebuilt the component.
	Member.getter = [Self, Property, Type, bOnObject](void* Container) -> rql::Value
	{
		void* Owner = bOnObject ? Self->LiveObject(static_cast<UObject*>(Container)) : Container;
		return Self->Read(Property, Property->ContainerPtrToValuePtr<void>(Owner), Type);
	};
	if (bOnObject)
	{
		Member.setter = [Self, Property](void* Container, const rql::Value& Value)
		{
			Self->WriteObjectProperty(static_cast<UObject*>(Container), Property, Value);
		};
	}
	else
	{
		Member.setter = [Self, Property](void* Container, const rql::Value& Value)
		{
			Self->Write(Property, Property->ContainerPtrToValuePtr<void>(Container), Value);
		};
	}
	SetMember(Descriptor, std::move(Member));
}

bool FUnrealMcpRqlTypes::IsCallable(UClass* Class, UFunction* Function) const
{
	if (!Function || Function->HasAnyFunctionFlags(FUNC_Delegate))
		return false;
	if (ApiClasses.Contains(Class))
		return true;
	if (Function->HasAnyFunctionFlags(FUNC_Static | FUNC_Private | FUNC_Protected))
		return false;
	// The Details panel's buttons: CallInEditor functions without parameters (other CallInEditor functions are internal
	// callbacks that merely carry the flag).
	return Function->HasMetaData(TEXT("CallInEditor")) && Function->NumParms == 0;
}

void FUnrealMcpRqlTypes::AddFunctionMethod(rql::TypeDescriptor& Descriptor, UFunction* Function)
{
	rql::MethodDescriptor Method;
	FString Name = Function->GetName();
	if (Name.StartsWith(TEXT("K2_")))
		Name.RightChopInline(3);
	Method.name = UnrealMcpRqlToStd(Name);
	FString ToolTip = Function->GetMetaData(TEXT("ToolTip"));
	ToolTip.ReplaceInline(TEXT("\r"), TEXT(" "));
	ToolTip.ReplaceInline(TEXT("\n"), TEXT(" "));
	Method.description = UnrealMcpRqlToStd(ToolTip.TrimStartAndEnd());

	TArray<FProperty*> Inputs;
	TArray<FProperty*> Outputs;
	FProperty* Return = nullptr;
	for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		FProperty* Property = *It;
		bool bNullable = false;
		const rql::TypeDescriptor* Type = ForProperty(Property, bNullable);
		if (!Type)
			return; // a parameter RQL cannot express: the function is not offered
		if (Property->HasAnyPropertyFlags(CPF_ReturnParm))
		{
			Return = Property;
			Method.result = Type;
			continue;
		}
		if (Property->HasAnyPropertyFlags(CPF_OutParm) && !Property->HasAnyPropertyFlags(CPF_ReferenceParm | CPF_ConstParm))
		{
			Outputs.Add(Property);
			continue;
		}
		rql::ParamDescriptor Parameter;
		Parameter.name = UnrealMcpRqlToStd(Property->GetAuthoredName());
		Parameter.type = Type;
		const FString DefaultKey = FString(TEXT("CPP_Default_")) + Property->GetName();
		// An object / class parameter is required unless C++ gives it a default (only nullptr is possible): the engine
		// refuses null for it, so functions need no "no actor" guards. Soft references (paths) stay nullable.
		Parameter.nullable = bNullable && (!Property->IsA<FObjectPropertyBase>() || Property->IsA<FSoftObjectProperty>()
			|| Function->HasMetaData(*DefaultKey));
		if (Function->HasMetaData(*DefaultKey))
		{
			// UHT records C++ default arguments as text; an empty text (FVector::ZeroVector) is the type's zero value.
			// Math structs come as "1.000000,1.000000,1.000000" (FVector(1.0)), which their text import does not read.
			FTempValue Default(Property);
			FOutputDeviceNull Ignore;
			const FString DefaultText = Function->GetMetaData(*DefaultKey);
			if (!DefaultText.IsEmpty() && !Property->ImportText_Direct(*DefaultText, Default.Ptr, nullptr, PPF_None, &Ignore))
			{
				if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
				{
					FString Ignored;
					UnrealMcpRqlParseMathStruct(StructProperty->Struct, rql::Value::string(UnrealMcpRqlToStd(DefaultText)), Default.Ptr, Ignored);
				}
			}
			Parameter.has_default = true;
			Parameter.default_value = Detached(Read(Property, Default.Ptr));
		}
		Method.params.push_back(std::move(Parameter));
		Inputs.Add(Property);
	}
	if (!Return && Outputs.Num() == 1)
	{
		bool bNullable = false;
		Method.result = ForProperty(Outputs[0], bNullable);
	}
	else if (!Return && Outputs.Num() > 1)
	{
		Method.result = rql::builtin::any_type();
	}

	FUnrealMcpRqlTypes* Self = this;
	Method.invoke = [Self, Function, Inputs, Outputs, Return](void* Target, const std::vector<rql::Value>& Args) -> rql::Value
	{
		if (Args.size() != static_cast<size_t>(Inputs.Num()))
			Throw(FString::Printf(TEXT("%s expects %d argument(s), got %d"), *Function->GetName(), Inputs.Num(), static_cast<int32>(Args.size())));
		UObject* Object = static_cast<UObject*>(Target);
		if (Function->HasAnyFunctionFlags(FUNC_Static))
			Object = Function->GetOuterUClass()->GetDefaultObject();
		if (!Object)
			Throw(TEXT("no object to call on"));

		FStructOnScope Frame(Function); // the parameter block, initialized and destroyed with the scope
		uint8* Params = Frame.GetStructMemory();
		for (int32 Index = 0; Index < Inputs.Num(); ++Index)
			Self->Write(Inputs[Index], Inputs[Index]->ContainerPtrToValuePtr<void>(Params), Args[static_cast<size_t>(Index)]);

		UnrealMcpRqlBeginCall();
		Object->ProcessEvent(Function, Params);
		FString Failure;
		if (UnrealMcpRqlEndCall(Failure))
			Throw(Failure);

		if (Return)
			return Detached(Self->Read(Return, Return->ContainerPtrToValuePtr<void>(Params)));
		if (Outputs.Num() == 1)
			return Detached(Self->Read(Outputs[0], Outputs[0]->ContainerPtrToValuePtr<void>(Params)));
		if (Outputs.Num() > 1)
		{
			std::vector<std::pair<rql::Value, rql::Value>> Entries;
			for (FProperty* Output : Outputs)
				Entries.emplace_back(rql::Value::string(UnrealMcpRqlToStd(Output->GetAuthoredName())),
					Detached(Self->Read(Output, Output->ContainerPtrToValuePtr<void>(Params))));
			return rql::Value::map(std::make_shared<rql::OwnedMap>(nullptr, std::move(Entries)));
		}
		return rql::Value::null();
	};
	AddMethod(Descriptor, std::move(Method));
}

// ------------------------------------------------------------------ values

rql::Value FUnrealMcpRqlTypes::WrapObject(UObject* Object)
{
	if (!Object || !IsValid(Object))
		return rql::Value::null();
	const rql::TypeDescriptor* Descriptor = ForClass(Object->GetClass());
	Fill(Descriptor);
	return rql::Value::object(Descriptor, Object, std::make_shared<FWeakObjectPtr>(Object));
}

rql::Value FUnrealMcpRqlTypes::WrapObjects(const TArray<UObject*>& Objects)
{
	std::vector<rql::Value> Items;
	Items.reserve(static_cast<size_t>(Objects.Num()));
	for (UObject* Object : Objects)
	{
		if (Object && IsValid(Object))
			Items.push_back(WrapObject(Object));
	}
	return rql::Value::list_of(std::move(Items));
}

rql::Value FUnrealMcpRqlTypes::WrapStruct(UScriptStruct* Struct, const void* Data)
{
	const rql::TypeDescriptor* Descriptor = ForStruct(Struct);
	if (!Descriptor)
		return rql::Value::null();
	Fill(Descriptor);
	return rql::Value::structure(Descriptor, MakeStructData(Struct, Data));
}

UObject* FUnrealMcpRqlTypes::AsObject(const rql::Value& Value) const
{
	if (!Value.is_object() || !IsObjectType(Value.type()) || !IsAlive(Value))
		return nullptr;
	return static_cast<UObject*>(Value.ptr());
}

bool FUnrealMcpRqlTypes::IsAlive(const rql::Value& Value) const
{
	if (!Value.is_object() || !IsObjectType(Value.type()))
		return true;
	const FWeakObjectPtr* Weak = static_cast<const FWeakObjectPtr*>(Value.holder().get());
	return !Weak || Weak->IsValid();
}

namespace UnrealMcpRqlTypesLocal
{
	/** Ids are Serial * IdIndexSpan + Index: the object array holds fewer than 2^25 objects (gc.MaxObjectsInEditor). */
	constexpr int64 IdIndexSpan = int64(1) << 25;
}

int64 FUnrealMcpRqlTypes::IdOf(const UObject* Object)
{
	if (!Object)
		return 0;
	const int32 Index = GUObjectArray.ObjectToIndex(Object);
	return static_cast<int64>(GUObjectArray.AllocateSerialNumber(Index)) * IdIndexSpan + Index;
}

UObject* FUnrealMcpRqlTypes::FindById(int64 Id)
{
	if (Id <= 0)
		return nullptr;
	const int32 Index = static_cast<int32>(Id % IdIndexSpan);
	const int64 Serial = Id / IdIndexSpan;
	// Serial 0 marks a slot with no allocated serial (IdOf never issues it): "Type#5" must not match index 5.
	if (Serial <= 0 || Serial > MAX_int32)
		return nullptr;
	FUObjectItem* Item = GUObjectArray.IndexToValidObject(Index, /*bEvenIfGarbage*/ false);
	if (!Item || Item->GetSerialNumber() != Serial)
		return nullptr; // gone, or the slot now holds another object
	// FIndexToObject, not FUObjectItem::GetObject(): the latter only exists from UE 5.6.
	UObject* Object = static_cast<UObject*>(FIndexToObject::IndexToObject(Index, /*bEvenIfGarbage*/ false));
	return IsValid(Object) ? Object : nullptr;
}

rql::Value FUnrealMcpRqlTypes::Read(const FProperty* Property, const void* ValuePtr, const rql::TypeDescriptor* Type)
{
	auto TypeOf = [this, Property, Type]()
	{
		bool bNullable = false;
		return Type ? Type : ForProperty(Property, bNullable);
	};
	if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		return rql::Value::boolean(BoolProperty->GetPropertyValue(ValuePtr));
	if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		return rql::Value::enumeration(ForEnum(EnumProperty->GetEnum()), EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr));
	if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
	{
		if (ByteProperty->Enum)
			return rql::Value::enumeration(ForEnum(ByteProperty->Enum), static_cast<int64>(ByteProperty->GetPropertyValue(ValuePtr)));
	}
	if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
	{
		if (Numeric->IsFloatingPoint())
			return rql::Value::number(Numeric->GetFloatingPointPropertyValue(ValuePtr), TypeOf());
		if (IsUnsigned(Property))
			return rql::Value::number(static_cast<double>(Numeric->GetUnsignedIntPropertyValue(ValuePtr)), TypeOf());
		return rql::Value::number(static_cast<double>(Numeric->GetSignedIntPropertyValue(ValuePtr)), TypeOf());
	}
	if (const FStrProperty* StrProperty = CastField<FStrProperty>(Property))
		return rql::Value::string(UnrealMcpRqlToStd(StrProperty->GetPropertyValue(ValuePtr)));
	if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		return rql::Value::string(UnrealMcpRqlToStd(NameProperty->GetPropertyValue(ValuePtr).ToString()));
	if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
		return rql::Value::string(UnrealMcpRqlToStd(TextProperty->GetPropertyValue(ValuePtr).ToString()));
	if (Property->IsA<FSoftObjectProperty>())
	{
		const FSoftObjectPtr& Soft = *static_cast<const FSoftObjectPtr*>(ValuePtr);
		const FString Path = Soft.ToSoftObjectPath().ToString();
		return Path.IsEmpty() ? rql::Value::null() : rql::Value::string(UnrealMcpRqlToStd(Path));
	}
	if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		return WrapObject(ObjectProperty->GetObjectPropertyValue(ValuePtr));
	if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		return WrapStruct(StructProperty->Struct, ValuePtr);
	if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		return rql::Value::list(std::make_shared<FArrayAdapter>(this, ArrayProperty, const_cast<void*>(ValuePtr), TypeOf()));
	if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
	{
		FScriptSetHelper Helper(SetProperty, ValuePtr);
		std::vector<rql::Value> Items;
		for (int32 Index = 0, Max = Helper.GetMaxIndex(); Index < Max; ++Index)
		{
			if (Helper.IsValidIndex(Index))
				Items.push_back(Read(SetProperty->ElementProp, Helper.GetElementPtr(Index)));
		}
		return rql::Value::list_of(std::move(Items));
	}
	if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		return rql::Value::map(std::make_shared<FMapAdapter>(this, MapProperty, const_cast<void*>(ValuePtr), TypeOf()));
	return rql::Value::missing();
}

void FUnrealMcpRqlTypes::Write(const FProperty* Property, void* ValuePtr, const rql::Value& Value)
{
	if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
	{
		BoolProperty->SetPropertyValue(ValuePtr, UnrealMcpRqlUtil::BoolArg(Value));
		return;
	}
	if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
	{
		EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, ToEnumValue(EnumProperty->GetEnum(), Value));
		return;
	}
	if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
	{
		if (ByteProperty->Enum)
		{
			ByteProperty->SetPropertyValue(ValuePtr, static_cast<uint8>(ToEnumValue(ByteProperty->Enum, Value)));
			return;
		}
	}
	if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
	{
		const double Number = ToNumber(Value);
		if (Numeric->IsFloatingPoint())
		{
			Numeric->SetFloatingPointPropertyValue(ValuePtr, Number);
			return;
		}
		// Integers: out of range fails instead of wrapping (300 into a uint8 would store 44). The bounds are powers of
		// two, exact as doubles; the upper one is exclusive.
		const bool bUnsigned = IsUnsigned(Property);
		const int32 Bits = Property->GetElementSize() * 8;
		const double Rounded = FMath::RoundToDouble(Number);
		const double Lower = bUnsigned ? 0.0 : -std::ldexp(1.0, Bits - 1);
		const double Upper = bUnsigned ? std::ldexp(1.0, Bits) : std::ldexp(1.0, Bits - 1);
		if (!FMath::IsFinite(Rounded) || Rounded < Lower || Rounded >= Upper)
			Throw(FString::Printf(TEXT("%s is out of range for %s (%s integer of %d bits)"), *UnrealMcpRqlFromStd(rql::describe_value(Value)),
				*Property->GetAuthoredName(), bUnsigned ? TEXT("an unsigned") : TEXT("a signed"), Bits));
		if (bUnsigned)
			Numeric->SetIntPropertyValue(ValuePtr, static_cast<uint64>(Rounded));
		else
			Numeric->SetIntPropertyValue(ValuePtr, static_cast<int64>(Rounded));
		return;
	}
	if (const FStrProperty* StrProperty = CastField<FStrProperty>(Property))
	{
		StrProperty->SetPropertyValue(ValuePtr, ToText(Value));
		return;
	}
	if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
	{
		NameProperty->SetPropertyValue(ValuePtr, FName(*ToText(Value)));
		return;
	}
	if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
	{
		TextProperty->SetPropertyValue(ValuePtr, FText::FromString(ToText(Value)));
		return;
	}
	if (Property->IsA<FSoftObjectProperty>())
	{
		FSoftObjectPtr& Soft = *static_cast<FSoftObjectPtr*>(ValuePtr);
		if (Value.is_null())
			Soft = FSoftObjectPtr();
		else if (UObject* Object = AsObject(Value))
			Soft = FSoftObjectPtr(Object);
		else
			Soft = FSoftObjectPtr(FSoftObjectPath(ToText(Value)));
		return;
	}
	if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
	{
		UClass* Wanted = ObjectProperty->PropertyClass;
		UObject* Object = nullptr;
		if (!Value.is_null())
		{
			Object = AsObject(Value);
			if (!Object && Value.is_string())
			{
				// References as an edit field accepts them: set StaticMesh = "/Game/Meshes/Cube", ActorClass = "PointLight".
				const FString Text = UnrealMcpRqlFromStd(Value.as_string());
				Object = FUnrealMcpObjectRef::ResolveReference(Wanted, Text);
				if (!Object)
					Throw(FString::Printf(TEXT("no %s '%s'%s"), *Wanted->GetName(), *Text, *UnrealMcpRqlSimilarAssets(Wanted, Text)));
			}
			if (!Object)
				ThrowMismatch(*FString::Printf(TEXT("a %s"), *Wanted->GetName()), Value);
			if (!Object->IsA(Wanted))
				Throw(FString::Printf(TEXT("%s is not a %s"), *Object->GetName(), *Wanted->GetName()));
			if (const FClassProperty* ClassProperty = CastField<FClassProperty>(Property))
			{
				const UClass* AsClass = Cast<UClass>(Object);
				if (ClassProperty->MetaClass && (!AsClass || !AsClass->IsChildOf(ClassProperty->MetaClass)))
					Throw(FString::Printf(TEXT("%s is not a subclass of %s"), *Object->GetName(), *ClassProperty->MetaClass->GetName()));
			}
		}
		ObjectProperty->SetObjectPropertyValue(ValuePtr, Object);
		return;
	}
	if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
	{
		CoerceStruct(StructProperty->Struct, Value, ValuePtr);
		return;
	}
	if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
	{
		const std::vector<rql::Value> Items = ListItems(Value);
		FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
		Helper.Resize(static_cast<int32>(Items.size()));
		for (size_t Index = 0; Index < Items.size(); ++Index)
			Write(ArrayProperty->Inner, Helper.GetRawPtr(static_cast<int32>(Index)), Items[Index]);
		return;
	}
	if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
	{
		const std::vector<rql::Value> Items = ListItems(Value);
		FScriptSetHelper Helper(SetProperty, ValuePtr);
		Helper.EmptyElements();
		for (const rql::Value& Item : Items)
		{
			// Converted first, then added by hash: equal elements (["a", "a"], or names differing in case) collapse to one.
			FTempValue Element(SetProperty->ElementProp);
			Write(SetProperty->ElementProp, Element.Ptr, Item);
			Helper.AddElement(Element.Ptr);
		}
		return;
	}
	if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
	{
		if (!Value.is_map())
			ThrowMismatch(TEXT("a map"), Value);
		std::vector<std::pair<rql::Value, rql::Value>> Entries;
		Value.map().for_each([&Entries](const rql::Value& Key, const rql::Value& Item)
		{
			Entries.emplace_back(rql::snapshot(Key), rql::snapshot(Item));
			return true;
		});
		FScriptMapHelper Helper(MapProperty, ValuePtr);
		Helper.EmptyValues();
		for (const std::pair<rql::Value, rql::Value>& Entry : Entries)
		{
			// Converted first, then added by hash: keys that convert to the same key (FName case) keep the last value.
			FTempValue Key(MapProperty->KeyProp);
			FTempValue Item(MapProperty->ValueProp);
			Write(MapProperty->KeyProp, Key.Ptr, Entry.first);
			Write(MapProperty->ValueProp, Item.Ptr, Entry.second);
			Helper.AddPair(Key.Ptr, Item.Ptr);
		}
		return;
	}
	Throw(FString::Printf(TEXT("%s cannot be written"), *Property->GetAuthoredName()));
}

UObject* FUnrealMcpRqlTypes::LiveObject(UObject* Object) const
{
	if (Object && !IsValid(Object))
	{
		// Same class only: a Blueprint compile also reports replacements (reinstanced objects of a new class layout), which
		// the descriptor's FProperty offsets do not fit.
		const TWeakObjectPtr<UObject>* Newest = Replaced.Find(Object);
		if (Newest && Newest->IsValid() && Newest->Get()->GetClass() == Object->GetClass())
			return Newest->Get();
	}
	return Object;
}

void FUnrealMcpRqlTypes::WriteObjectProperty(UObject* Object, FProperty* Property, const rql::Value& Value)
{
	// A previous write of this mutation may have rebuilt the component (set A = 1, B = 2 on a Blueprint actor's component):
	// write into the one the actor has now, not into the discarded copy.
	Object = LiveObject(Object);
	if (!Object || !IsValid(Object))
		Throw(TEXT("the object is gone"));
	// A placed Blueprint actor resets such a property to the class default whenever its construction script reruns (every
	// edit does): the write would report success and then vanish. Play In Editor actors do not rerun it.
	if (const AActor* Actor = Cast<AActor>(Object); Actor && !Object->IsTemplate() && !(Actor->GetWorld() && Actor->GetWorld()->IsGameWorld())
		&& IsResetByConstruction(Actor->GetClass(), Property))
		Throw(FString::Printf(TEXT("'%s' is not Instance Editable, so placed %s actors always use the Blueprint default: change the "
			"default with blueprint-set-default, or make the variable Instance Editable first (blueprint-modify-variable instanceEditable=true)"),
			*Property->GetAuthoredName(), *Actor->GetClass()->GetName()));
	// The editor's own property-change path, so components refresh (transforms, render state) and Blueprint actors rerun
	// their construction script — exactly as an edit in the Details panel.
	// PreEditChange and PostEditChangeProperty always pair up: an actor unregisters its components in PreEditChange and
	// re-registers them in PostEditChangeProperty, so a failing write must still send the post notification.
	// An archetype (a Blueprint's component template) passes the change on to the objects made from it that still hold
	// its old value, as the Details panel does: placed actors follow a new default unless they override it.
	// Instanced sub-objects are not copied over (each owner needs its own): the construction rerun rebuilds those.
	TArray<UObject*> Instances;
	if (Object->HasAnyFlags(RF_ArchetypeObject) && !Object->HasAnyFlags(RF_ClassDefaultObject) && !Property->ContainsInstancedObjectProperty())
		Object->GetArchetypeInstances(Instances);
	TOptional<FTempValue> Old;
	if (Instances.Num() > 0)
	{
		Old.Emplace(Property);
		Property->CopyCompleteValue(Old->Ptr, Property->ContainerPtrToValuePtr<void>(Object));
	}
	Object->PreEditChange(Property);
	FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
	try
	{
		Write(Property, Property->ContainerPtrToValuePtr<void>(Object), Value);
	}
	catch (...)
	{
		Object->PostEditChangeProperty(Event);
		throw;
	}
	Object->PostEditChangeProperty(Event);
	const void* New = Property->ContainerPtrToValuePtr<void>(Object);
	// A raw write of a relative transform (the Details panel's transform rows call the setters instead) only moves a
	// component that re-registers in PostEditChangeProperty; some never do (Niagara). Recompute its world transform, as
	// SetRelativeLocation does. PostEditChangeProperty may have rebuilt it (a Blueprint actor's construction script).
	const FName Name = Property->GetFName();
	if (Name == USceneComponent::GetRelativeLocationPropertyName() || Name == USceneComponent::GetRelativeRotationPropertyName()
		|| Name == USceneComponent::GetRelativeScale3DPropertyName())
	{
		if (USceneComponent* Scene = Cast<USceneComponent>(LiveObject(Object)); IsValid(Scene) && Scene->IsRegistered())
			Scene->UpdateComponentToWorld();
	}
	for (UObject* Instance : Instances)
	{
		// Direct instances only (a child Blueprint's template holds its own value), never the running Play In Editor copies.
		if (!IsValid(Instance) || Instance->HasAnyFlags(RF_ClassDefaultObject) || Instance->GetArchetype() != Object
			|| Instance->GetClass() != Object->GetClass() || (Instance->GetWorld() && Instance->GetWorld()->IsGameWorld()))
			continue;
		void* Current = Property->ContainerPtrToValuePtr<void>(Instance);
		if (!Property->Identical(Current, Old->Ptr))
			continue; // the instance overrides this value: keep it
		// As FComponentEditorUtils::ApplyDefaultValueChange: a component the construction script made is rebuilt on undo
		// through its owner, so the owner is recorded instead of the component.
		UActorComponent* Component = Cast<UActorComponent>(Instance);
		AActor* Owner = Component ? Component->GetOwner() : nullptr;
		if (Owner)
			Owner->Modify();
		if (!Component || !Component->IsCreatedByConstructionScript())
			Instance->Modify();
		Instance->PreEditChange(Property);
		Property->CopyCompleteValue(Current, New);
		FPropertyChangedEvent InstanceEvent(Property, EPropertyChangeType::ValueSet);
		Instance->PostEditChangeProperty(InstanceEvent);
		Instance->MarkPackageDirty();
	}
}

/**
 * The first top-level "Name=" of a "(...)" text form that names no member of @p Struct (C++ or authored name), or "":
 * ImportText skips a member it does not know without a word, so ParseStructText names it, as the {Member: value} form does.
 */
static FString UnrealMcpRqlFirstUnknownTextMember(UScriptStruct* Struct, const FString& Source)
{
	int32 Depth = 0;
	bool bQuoted = false;
	for (int32 Index = 0; Index < Source.Len(); ++Index)
	{
		const TCHAR Char = Source[Index];
		if (bQuoted)
		{
			if (Char == TEXT('\\'))
				++Index;
			else if (Char == TEXT('"'))
				bQuoted = false;
			continue;
		}
		if (Char == TEXT('"'))
			bQuoted = true;
		else if (Char == TEXT(')'))
			--Depth;
		else if ((Char == TEXT('(') && ++Depth == 1) || (Char == TEXT(',') && Depth == 1))
		{
			int32 End = Index + 1;
			while (End < Source.Len() && Source[End] != TEXT('=') && Source[End] != TEXT(',') && Source[End] != TEXT('(') && Source[End] != TEXT(')'))
				++End;
			if (End >= Source.Len() || Source[End] != TEXT('='))
				continue; // not "Name=" (a static-array element "Member(1)=" is left to ImportText)
			const FString Name = Source.Mid(Index + 1, End - Index - 1).TrimStartAndEnd();
			bool bKnown = Name.IsEmpty();
			for (TFieldIterator<FProperty> It(Struct); It && !bKnown; ++It)
				bKnown = It->GetName().Equals(Name, ESearchCase::IgnoreCase) || It->GetAuthoredName().Equals(Name, ESearchCase::IgnoreCase);
			if (!bKnown)
				return Name;
		}
	}
	return FString();
}

/**
 * UE's own text form of @p Struct ("(Minimum=(X=0.5,Y=0),Maximum=(X=0.5,Y=0))", what the editor's Copy and ExportText
 * write) imported over @p Out; false with @p OutWhy when it names an unknown member, does not parse or has text after it.
 */
static bool UnrealMcpRqlImportStructText(UScriptStruct* Struct, const FString& Source, void* Out, FString& OutWhy)
{
	const FString Unknown = UnrealMcpRqlFirstUnknownTextMember(Struct, Source);
	if (!Unknown.IsEmpty())
	{
		OutWhy = FString::Printf(TEXT("no member '%s'"), *Unknown);
		return false;
	}
	// What ImportText reports (a bad value); UE 5.5-5.7 differ in where FStringOutputDevice lives.
	struct FImportErrors final : FOutputDevice
	{
		FString Text;
		virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type, const FName&) override
		{
			Text += Text.IsEmpty() ? FString(Message) : FString(TEXT(" ")) + Message;
		}
	} Errors;
	const TCHAR* End = Struct->ImportText(*Source, Out, nullptr, PPF_None, &Errors, Struct->GetName());
	const FString Rest = End ? FString(End).TrimStartAndEnd() : FString();
	if (End && Rest.IsEmpty() && Errors.Text.IsEmpty())
		return true;
	OutWhy = !Errors.Text.IsEmpty() ? Errors.Text.TrimStartAndEnd()
		: !End ? FString(TEXT("it does not parse")) : FString::Printf(TEXT("unexpected text after the closing parenthesis: %s"), *Rest);
	return false;
}

/** Whether @p Text is a "(Name=value, ...)" text form (not a math literal such as "(1, 2, 3)"). */
static bool UnrealMcpRqlIsNamedTextForm(const FString& Text)
{
	return Text.StartsWith(TEXT("(")) && Text.Contains(TEXT("="));
}

void FUnrealMcpRqlTypes::CoerceStruct(UScriptStruct* Struct, const rql::Value& Value, void* Out)
{
	if (Value.is_struct() && Value.type() == ForStruct(Struct) && Value.ptr())
	{
		Struct->CopyScriptStruct(Out, Value.ptr());
		return;
	}
	if (const FStructParser* Parser = StructParsers.Find(Struct))
	{
		if ((*Parser)(*this, Value, Out))
			return;
	}
	if (Value.is_string())
	{
		// UE's named text form for any struct, the math structs included (an FColor copies as "(B=0,G=0,R=255,A=255)").
		const FString Text = UnrealMcpRqlFromStd(Value.as_string()).TrimStartAndEnd();
		if (UnrealMcpRqlIsNamedTextForm(Text))
		{
			FString Why;
			if (!UnrealMcpRqlImportStructText(Struct, Text, Out, Why))
				Throw(FString::Printf(TEXT("'%s' is not Unreal's text form of %s (%s)"), *Text, *Struct->GetName(), *Why));
			return;
		}
	}
	if (Value.is_string() || Value.is_list())
	{
		// "(1, 2, 3)", [1, 2, 3], "#ff8800" for the math structs.
		FString Error;
		if (UnrealMcpRqlParseMathStruct(Struct, Value, Out, Error))
			return;
		if (!Error.IsEmpty())
			Throw(Error);
	}
	if (Value.is_map())
	{
		// An object literal {X: 1, Y: 2}: named members over the current value.
		Value.map().for_each([this, Struct, Out](const rql::Value& Key, const rql::Value& Item)
		{
			const FString MemberName = ToText(Key);
			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				if (It->GetAuthoredName().Equals(MemberName, ESearchCase::IgnoreCase))
				{
					Write(*It, It->ContainerPtrToValuePtr<void>(Out), Item);
					return true;
				}
			}
			Throw(FString::Printf(TEXT("%s has no member '%s'"), *Struct->GetName(), *MemberName));
		});
		return;
	}
	ThrowMismatch(*FString::Printf(TEXT("a %s"), *Struct->GetName()), Value);
}

std::optional<rql::Value> FUnrealMcpRqlTypes::ParseStructText(const rql::TypeDescriptor& Type, const std::string& Text, std::string& OutReason)
{
	UScriptStruct* const* Found = StructByDescriptor.Find(&Type);
	const FString Source = UnrealMcpRqlFromStd(Text).TrimStartAndEnd();
	if (!Found || !Source.StartsWith(TEXT("(")))
		return std::nullopt;
	std::shared_ptr<void> Data = MakeStructData(*Found, nullptr);
	FString Why;
	if (!UnrealMcpRqlImportStructText(*Found, Source, Data.get(), Why))
	{
		OutReason = UnrealMcpRqlToStd(FString::Printf(TEXT("'%s' is not Unreal's text form of %s (%s)"), *Source, *(*Found)->GetName(), *Why));
		return std::nullopt;
	}
	Fill(&Type);
	return rql::Value::structure(&Type, std::move(Data));
}
