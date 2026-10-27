// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRqlSession.h"
#include "UnrealMcpRqlSources.h"
#include "UnrealMcpRqlUtil.h"
#include "UnrealMcpPinTypes.h"

#include "Engine/UserDefinedStruct.h"
#include "Kismet2/StructureEditorUtils.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"

#include <string>
#include <vector>

/**
 * User-defined structs (docs/rql.md "User-defined structs"): the struct assets a DataTable row or a Blueprint variable
 * uses when the project has no C++ struct. Their fields are not reflected as editable data (the Struct editor keeps them
 * in its editor data), so the core module adds `Fields` and the Struct editor's operations, through
 * FStructureEditorUtils: the struct recompiles and DataTables / Blueprints using it update, as after an edit in the
 * Struct editor. Field names are the names the editor shows (the reflected members use them too).
 */
namespace UnrealMcpRqlUserStructsLocal
{
	using namespace UnrealMcpRqlUtil;

	UUserDefinedStruct* Self(void* Object)
	{
		return static_cast<UUserDefinedStruct*>(Object);
	}

	/** The field shown as @p Name (case-insensitive); null when there is none. */
	FStructVariableDescription* FindField(UUserDefinedStruct* Struct, const FString& Name)
	{
		for (FStructVariableDescription& Field : FStructureEditorUtils::GetVarDesc(Struct))
		{
			if (Field.FriendlyName.Equals(Name.TrimStartAndEnd(), ESearchCase::IgnoreCase))
				return &Field;
		}
		return nullptr;
	}

	FStructVariableDescription& RequireField(UUserDefinedStruct* Struct, const FString& Name)
	{
		if (FStructVariableDescription* Field = FindField(Struct, Name))
			return *Field;
		TArray<FString> Names;
		for (const FStructVariableDescription& Field : FStructureEditorUtils::GetVarDesc(Struct))
			Names.Add(Field.FriendlyName);
		Throw(FString::Printf(TEXT("%s has no field '%s' (fields: %s)"), *Struct->GetName(), *Name, *FString::Join(Names, TEXT(", "))));
	}

	/** A name a new or renamed field may take: valid for the Struct editor and not used by another field. */
	FString NewFieldName(UUserDefinedStruct* Struct, const FString& Name)
	{
		const FString Trimmed = Name.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
			Throw(TEXT("a field name cannot be empty"));
		if (FindField(Struct, Trimmed))
			Throw(FString::Printf(TEXT("%s already has a field '%s'"), *Struct->GetName(), *Trimmed));
		return Trimmed;
	}

	void SetDefault(UUserDefinedStruct* Struct, const FStructVariableDescription& Field, const rql::Value& Value)
	{
		// The Struct editor's default box takes the property's text form ("5", "true", "(X=1,Y=2,Z=3)").
		const FString Text = Value.is_bool() ? (Value.as_bool() ? TEXT("true") : TEXT("false")) : TextArg(Value);
		if (!FStructureEditorUtils::ChangeVariableDefaultValue(Struct, Field.VarGuid, Text))
		{
			// ChangeVariableDefaultValue refuses a text the field's type cannot import, and an unchanged value.
			if (Field.DefaultValue != Text)
				Throw(FString::Printf(TEXT("'%s' is not a valid default for %s field '%s'"), *Text,
					*UnrealMcpPinTypes::ToString(Field.ToPinType()), *Field.FriendlyName));
		}
	}

	void UserStructMembers(FUnrealMcpRqlTypes& T, rql::TypeDescriptor& D)
	{
		const rql::TypeDescriptor* String = rql::builtin::string_type();
		FUnrealMcpRqlTypes::SetMember(D, Member("Fields", T.Registry().list_type(rql::builtin::any_type(), false), [](void* Object)
		{
			std::vector<rql::Value> Out;
			for (const FStructVariableDescription& Field : FStructureEditorUtils::GetVarDesc(Self(Object)))
			{
				std::vector<std::pair<rql::Value, rql::Value>> Entry;
				Entry.emplace_back(Str(TEXT("Name")), Str(Field.FriendlyName));
				Entry.emplace_back(Str(TEXT("Type")), Str(UnrealMcpPinTypes::ToString(Field.ToPinType())));
				Entry.emplace_back(Str(TEXT("Default")), Str(Field.DefaultValue));
				Out.push_back(rql::Value::map(std::make_shared<rql::OwnedMap>(nullptr, std::move(Entry))));
			}
			return rql::Value::list_of(std::move(Out));
		}, {}, "the fields, in order: [{Name, Type, Default}] (change them with AddField / RemoveField / RenameField / SetFieldDefault)"));

		FUnrealMcpRqlTypes::AddMethod(D, Method("AddField",
			{ FUnrealMcpRqlTypes::Param("Name", String), FUnrealMcpRqlTypes::Param("Type", String),
			  FUnrealMcpRqlTypes::OptionalParam("Default", rql::builtin::any_type(), rql::Value::null(), /*bNullable*/ true) },
			nullptr, [](void* Object, const std::vector<rql::Value>& Args) -> rql::Value
			{
				UUserDefinedStruct* Struct = Self(Object);
				const FString Name = NewFieldName(Struct, TextArg(Args[0]));
				FEdGraphPinType PinType;
				FString Error;
				if (!UnrealMcpPinTypes::Make(TextArg(Args[1]), /*bIsArray*/ false, PinType, Error))
					Throw(Error);
				if (!FStructureEditorUtils::AddVariable(Struct, PinType))
					Throw(FString::Printf(TEXT("could not add a %s field to %s"), *TextArg(Args[1]), *Struct->GetName()));
				// AddVariable appends a field with a generated name: give it the requested one.
				FStructVariableDescription& Added = FStructureEditorUtils::GetVarDesc(Struct).Last();
				const FGuid Guid = Added.VarGuid;
				if (!Added.FriendlyName.Equals(Name, ESearchCase::CaseSensitive) && !FStructureEditorUtils::RenameVariable(Struct, Guid, Name))
				{
					FStructureEditorUtils::RemoveVariable(Struct, Guid);
					Throw(FString::Printf(TEXT("'%s' is not a valid field name"), *Name));
				}
				if (!Args[2].is_null() && !Args[2].is_missing())
				{
					try
					{
						SetDefault(Struct, *FStructureEditorUtils::GetVarDescByGuid(Struct, Guid), Args[2]);
					}
					catch (...)
					{
						FStructureEditorUtils::RemoveVariable(Struct, Guid); // a default that does not fit: no half-made field
						throw;
					}
				}
				return rql::Value::null();
			}, "add a field: Name, Type (bool, int, float, string, name, text, vector, rotator, color, a struct such as "
			"\"GameplayTag\", a class or asset type such as \"StaticMesh\"), optional Default (text form: 5, true, \"(X=1,Y=2,Z=3)\")"));
		FUnrealMcpRqlTypes::AddMethod(D, Method("RemoveField", { FUnrealMcpRqlTypes::Param("Name", String) }, nullptr,
			[](void* Object, const std::vector<rql::Value>& Args) -> rql::Value
			{
				UUserDefinedStruct* Struct = Self(Object);
				const FStructVariableDescription& Field = RequireField(Struct, TextArg(Args[0]));
				if (FStructureEditorUtils::GetVarDesc(Struct).Num() <= 1)
					Throw(TEXT("a struct keeps at least one field: add the new field first"));
				if (!FStructureEditorUtils::RemoveVariable(Struct, Field.VarGuid))
					Throw(FString::Printf(TEXT("could not remove field '%s'"), *TextArg(Args[0])));
				return rql::Value::null();
			}, "remove a field (a struct keeps at least one)"));
		FUnrealMcpRqlTypes::AddMethod(D, Method("RenameField",
			{ FUnrealMcpRqlTypes::Param("Name", String), FUnrealMcpRqlTypes::Param("NewName", String) }, nullptr,
			[](void* Object, const std::vector<rql::Value>& Args) -> rql::Value
			{
				UUserDefinedStruct* Struct = Self(Object);
				const FGuid Guid = RequireField(Struct, TextArg(Args[0])).VarGuid;
				const FString NewName = NewFieldName(Struct, TextArg(Args[1]));
				if (!FStructureEditorUtils::RenameVariable(Struct, Guid, NewName))
					Throw(FString::Printf(TEXT("'%s' is not a valid field name"), *NewName));
				return rql::Value::null();
			}, "rename a field (existing values are kept)"));
		FUnrealMcpRqlTypes::AddMethod(D, Method("SetFieldDefault",
			{ FUnrealMcpRqlTypes::Param("Name", String), FUnrealMcpRqlTypes::Param("Default", rql::builtin::any_type()) }, nullptr,
			[](void* Object, const std::vector<rql::Value>& Args) -> rql::Value
			{
				UUserDefinedStruct* Struct = Self(Object);
				SetDefault(Struct, RequireField(Struct, TextArg(Args[0])), Args[1]);
				return rql::Value::null();
			}, "set a field's default value (text form: 5, true, \"(X=1,Y=2,Z=3)\")"));
	}
}

void UnrealMcpRqlAddUserStructs(FUnrealMcpRqlSession& Session)
{
	using namespace UnrealMcpRqlUserStructsLocal;
	Session.Types().AddClassHook(UUserDefinedStruct::StaticClass(), &UserStructMembers);
}
