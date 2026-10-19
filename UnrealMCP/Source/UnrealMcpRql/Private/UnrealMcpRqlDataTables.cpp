// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRqlSession.h"
#include "UnrealMcpRqlSources.h"
#include "UnrealMcpRqlUtil.h"

#include "DataTableEditorUtils.h"
#include "Engine/DataTable.h"
#include "UObject/Class.h"

#include <string>
#include <vector>

/**
 * DataTable rows (docs/rql.md "DataTables"). A table's rows live in UDataTable::RowMap, which is not a UPROPERTY, so
 * reflection does not see them: the `Rows` member is a map from row name to the row struct, read and written in place.
 * Row writes run inside the mutation's transaction (the table is Modify()d by the session before the first write, like
 * the DataTable editor does) and notify the table and its editors; structural changes go through FDataTableEditorUtils,
 * the functions the DataTable editor's Add / Remove / Rename / Duplicate buttons call.
 */
namespace UnrealMcpRqlDataTablesLocal
{
	using namespace UnrealMcpRqlUtil;

	/** The row named by @p Key; NAME_None when the table has no such row (FName equality, case-insensitive). */
	FName ExistingRow(const UDataTable* Table, const FString& Key)
	{
		const FName Wanted(*Key, FNAME_Find);
		return !Wanted.IsNone() && Table->GetRowMap().Contains(Wanted) ? Wanted : NAME_None;
	}

	/** A row name agents may create: not empty, not taken, no characters FName rejects. */
	FName NewRowName(const UDataTable* Table, const FString& Name)
	{
		const FString Trimmed = Name.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
			Throw(TEXT("a row name cannot be empty"));
		FText Reason;
		if (!FName::IsValidXName(Trimmed, INVALID_NAME_CHARACTERS, &Reason))
			Throw(FString::Printf(TEXT("'%s' is not a valid row name: %s"), *Trimmed, *Reason.ToString()));
		if (!ExistingRow(Table, Trimmed).IsNone())
			Throw(FString::Printf(TEXT("%s already has a row '%s'"), *Table->GetName(), *Trimmed));
		return FName(*Trimmed);
	}

	UScriptStruct* RequireRowStruct(const UDataTable* Table)
	{
		UScriptStruct* RowStruct = const_cast<UScriptStruct*>(Table->GetRowStruct());
		if (!RowStruct)
			Throw(FString::Printf(TEXT("%s has no row struct"), *Table->GetName()));
		return RowStruct;
	}

	/** The row data after a change: what the DataTable editor does after an edit in its row details panel. */
	void NotifyRowChanged(UDataTable* Table, FName RowName)
	{
		Table->HandleDataTableChanged(RowName);
		FDataTableEditorUtils::BroadcastPostChange(Table, FDataTableEditorUtils::EDataTableChangeInfo::RowData);
		Table->MarkPackageDirty();
	}

	/** Write @p Value (a row struct value, or a {Member: value} / text literal over the current row) into row @p RowName. */
	void WriteRow(FUnrealMcpRqlTypes& Types, UDataTable* Table, FName RowName, const rql::Value& Value)
	{
		UScriptStruct* RowStruct = RequireRowStruct(Table);
		uint8* Row = Table->FindRowUnchecked(RowName);
		if (!Row)
			Throw(FString::Printf(TEXT("%s has no row '%s'"), *Table->GetName(), *RowName.ToString()));
		// Converted into a copy first: a value that does not fit leaves the row as it was.
		rql::Value Scratch = Types.WrapStruct(RowStruct, Row);
		Types.CoerceStruct(RowStruct, Value, Scratch.ptr());
		RowStruct->CopyScriptStruct(Row, Scratch.ptr());
		NotifyRowChanged(Table, RowName);
	}

	/** `Rows`: row name -> row struct, in the table's row order; `set Rows["Sword"].Damage = 12` writes the row in place. */
	class FRowMap final : public rql::IMapAdapter
	{
	public:
		FRowMap(FUnrealMcpRqlTypes* InTypes, UDataTable* InTable, const rql::TypeDescriptor* InType)
			: Types(InTypes), Table(InTable), Type(InType)
		{
		}

		size_t size() const override { return static_cast<size_t>(Table->GetRowMap().Num()); }
		void for_each(const std::function<bool(const rql::Value&, const rql::Value&)>& Fn) const override
		{
			UScriptStruct* RowStruct = const_cast<UScriptStruct*>(Table->GetRowStruct());
			if (!RowStruct)
				return;
			// Names first: the callback may read other rows (or the engine may write this one) while iterating.
			TArray<FName> Names = Table->GetRowNames();
			for (const FName& Name : Names)
			{
				const uint8* Row = Table->FindRowUnchecked(Name);
				if (Row && !Fn(Str(Name.ToString()), Types->WrapStruct(RowStruct, Row)))
					return;
			}
		}
		rql::MapLookup find(const rql::Value& Key, rql::Value& Out) const override
		{
			if (!Key.is_string())
				return rql::MapLookup::BadKey;
			UScriptStruct* RowStruct = const_cast<UScriptStruct*>(Table->GetRowStruct());
			const FName Name = ExistingRow(Table, TextArg(Key));
			const uint8* Row = Name.IsNone() || !RowStruct ? nullptr : Table->FindRowUnchecked(Name);
			if (!Row)
				return rql::MapLookup::NotFound;
			Out = Types->WrapStruct(RowStruct, Row);
			return rql::MapLookup::Found;
		}
		const rql::TypeDescriptor* type() const override { return Type; }
		const void* identity() const override { return Table; }
		bool writable() const override { return Table->GetRowStruct() != nullptr; }
		bool accepts_key(const rql::Value& Key) const override { return Key.is_string() && !TextArg(Key).TrimStartAndEnd().IsEmpty(); }
		void set(const rql::Value& Key, const rql::Value& Value) override
		{
			FName Name = ExistingRow(Table, TextArg(Key));
			if (Name.IsNone())
			{
				// A new key adds a row (its rollback erases it again).
				Name = NewRowName(Table, TextArg(Key));
				if (!FDataTableEditorUtils::AddRow(Table, Name))
					Throw(FString::Printf(TEXT("could not add row '%s' to %s"), *Name.ToString(), *Table->GetName()));
			}
			WriteRow(*Types, Table, Name, Value);
		}
		void erase(const rql::Value& Key) override
		{
			const FName Name = ExistingRow(Table, TextArg(Key));
			if (Name.IsNone())
				Throw(FString::Printf(TEXT("%s has no row '%s'"), *Table->GetName(), *TextArg(Key)));
			FDataTableEditorUtils::RemoveRow(Table, Name);
		}

	private:
		FUnrealMcpRqlTypes* Types;
		UDataTable* Table;
		const rql::TypeDescriptor* Type;
	};

	UDataTable* Self(void* Object)
	{
		return static_cast<UDataTable*>(Object);
	}

	void DataTableMembers(FUnrealMcpRqlTypes& T, rql::TypeDescriptor& D)
	{
		FUnrealMcpRqlTypes* Types = &T;
		const rql::TypeDescriptor* String = rql::builtin::string_type();
		const rql::TypeDescriptor* Int = rql::builtin::number_type(true, true, 4);

		// The map's value type is the table's row struct, which differs per table: the declared type is "any" and each
		// value carries its row struct type.
		const rql::TypeDescriptor* RowsType = T.Registry().map_type(String, rql::builtin::any_type(), false);
		FUnrealMcpRqlTypes::SetMember(D, Member("Rows", RowsType, [Types, RowsType](void* Object)
		{
			return rql::Value::map(std::make_shared<FRowMap>(Types, Self(Object), RowsType));
		}, {}, "the rows by name (each a row struct): set Rows[\"Sword\"].Damage = 12; a new key adds a row", /*bOwning*/ true));
		FUnrealMcpRqlTypes::SetMember(D, Member("RowNames", T.Registry().list_type(String, false), [](void* Object)
		{
			TArray<FString> Names;
			for (const FName& Name : Self(Object)->GetRowNames())
				Names.Add(Name.ToString());
			return StrList(Names);
		}, {}, "the row names, in table order"));
		FUnrealMcpRqlTypes::SetMember(D, Member("RowCount", Int, [Int](void* Object)
		{
			return rql::Value::number(Self(Object)->GetRowMap().Num(), Int);
		}));
		FUnrealMcpRqlTypes::SetMember(D, Member("RowStructName", String, [](void* Object)
		{
			const UScriptStruct* RowStruct = Self(Object)->GetRowStruct();
			return Str(RowStruct ? RowStruct->GetName() : FString());
		}, {}, "the row struct (rql-schema(RowStructName) lists the row members)"));

		FUnrealMcpRqlTypes::AddMethod(D, Method("AddRow",
			{ FUnrealMcpRqlTypes::Param("Name", String),
			  FUnrealMcpRqlTypes::OptionalParam("Values", rql::builtin::any_type(), rql::Value::null(), /*bNullable*/ true) },
			nullptr,
			[Types](void* Object, const std::vector<rql::Value>& Args) -> rql::Value
			{
				UDataTable* Table = Self(Object);
				RequireRowStruct(Table);
				const FName Name = NewRowName(Table, TextArg(Args[0]));
				if (!FDataTableEditorUtils::AddRow(Table, Name))
					Throw(FString::Printf(TEXT("could not add row '%s' to %s"), *Name.ToString(), *Table->GetName()));
				if (!Args[1].is_null() && !Args[1].is_missing())
				{
					try
					{
						WriteRow(*Types, Table, Name, Args[1]);
					}
					catch (...)
					{
						FDataTableEditorUtils::RemoveRow(Table, Name); // the values did not fit: no half-made row
						throw;
					}
				}
				return rql::Value::null();
			}, "add a row (default values, then Values: a {Member: value} literal or a row struct)"));
		FUnrealMcpRqlTypes::AddMethod(D, Method("RemoveRow", { FUnrealMcpRqlTypes::Param("Name", String) }, nullptr,
			[](void* Object, const std::vector<rql::Value>& Args) -> rql::Value
			{
				UDataTable* Table = Self(Object);
				const FName Name = ExistingRow(Table, TextArg(Args[0]));
				if (Name.IsNone())
					Throw(FString::Printf(TEXT("%s has no row '%s'"), *Table->GetName(), *TextArg(Args[0])));
				FDataTableEditorUtils::RemoveRow(Table, Name);
				return rql::Value::null();
			}, "remove a row"));
		FUnrealMcpRqlTypes::AddMethod(D, Method("RenameRow",
			{ FUnrealMcpRqlTypes::Param("Name", String), FUnrealMcpRqlTypes::Param("NewName", String) }, nullptr,
			[](void* Object, const std::vector<rql::Value>& Args) -> rql::Value
			{
				UDataTable* Table = Self(Object);
				const FName Name = ExistingRow(Table, TextArg(Args[0]));
				if (Name.IsNone())
					Throw(FString::Printf(TEXT("%s has no row '%s'"), *Table->GetName(), *TextArg(Args[0])));
				const FName NewName = NewRowName(Table, TextArg(Args[1]));
				if (!FDataTableEditorUtils::RenameRow(Table, Name, NewName))
					Throw(FString::Printf(TEXT("could not rename row '%s' to '%s'"), *Name.ToString(), *NewName.ToString()));
				return rql::Value::null();
			}, "rename a row (references by row handle are not updated)"));
		FUnrealMcpRqlTypes::AddMethod(D, Method("DuplicateRow",
			{ FUnrealMcpRqlTypes::Param("Name", String), FUnrealMcpRqlTypes::Param("NewName", String) }, nullptr,
			[](void* Object, const std::vector<rql::Value>& Args) -> rql::Value
			{
				UDataTable* Table = Self(Object);
				const FName Name = ExistingRow(Table, TextArg(Args[0]));
				if (Name.IsNone())
					Throw(FString::Printf(TEXT("%s has no row '%s'"), *Table->GetName(), *TextArg(Args[0])));
				const FName NewName = NewRowName(Table, TextArg(Args[1]));
				if (!FDataTableEditorUtils::DuplicateRow(Table, Name, NewName))
					Throw(FString::Printf(TEXT("could not duplicate row '%s'"), *Name.ToString()));
				return rql::Value::null();
			}, "copy a row under a new name"));
	}
}

void UnrealMcpRqlAddDataTables(FUnrealMcpRqlSession& Session)
{
	using namespace UnrealMcpRqlDataTablesLocal;
	FUnrealMcpRqlTypes& T = Session.Types();
	T.AddClassHook(UDataTable::StaticClass(), &DataTableMembers);
	FUnrealMcpRqlTypes* Types = &T;
	Session.AddSource(TEXT("dataTables"), T.ForClass(UDataTable::StaticClass()), /*bList*/ true,
		TEXT("the project's DataTables (Rows by name, RowNames, RowCount, RowStructName; call AddRow / RemoveRow / RenameRow / DuplicateRow)"),
		[Types]() { return UnrealMcpRqlAssetList(*Types, UDataTable::StaticClass(), FString()); });
}
