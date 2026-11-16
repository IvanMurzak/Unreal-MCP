// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRqlSession.h"

#include <algorithm>

#include "IUnrealMcpRqlModule.h"
#include "UnrealMcpAvailableExtensions.h"
#include "UnrealMcpRqlGuide.h"
#include "UnrealMcpRqlSources.h"
#include "UnrealMcpRqlWorldPartition.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "UnrealMcpRqlTools.h"
#include "UnrealMcpRqlUtil.h"

#include "Dom/JsonObject.h"
#include "GameFramework/Actor.h"
#include "Tools/UnrealMcpObjectRef.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string_view>

namespace UnrealMcpRqlSessionLocal
{
	/**
	 * A read that found nothing: no rows or items, a count of 0, or a min / max / avg over nothing (null). A sum or a min
	 * that is 0 found something; a mutation's empty receipt (a set that kept every value) is not "nothing found" either.
	 */
	bool IsEmptyRead(const rql::QueryResult& Result, const rql::QueryPlan& Plan, bool bMutation)
	{
		if (bMutation || !Result.ok)
			return false;
		if ((Result.rows && Result.rows->empty()) || (Result.items && Result.items->empty()))
			return true;
		if (!Result.has_value || Plan.stages.empty())
			return false;
		const rql::StageKind Last = Plan.stages.back().kind;
		if (Last == rql::StageKind::Count)
			return Result.value.is_number() && Result.value.as_number() == 0;
		return (Last == rql::StageKind::Min || Last == rql::StageKind::Max || Last == rql::StageKind::Avg) && Result.value.is_null();
	}

	/**
	 * The full text of a call argument for the duplicate-call guard: lists, maps and structs element by element, numbers at
	 * full precision, strings quoted, objects by identity (their text is only a type name). rql::to_text reads every vector,
	 * list or struct as its type name, so two calls differing only in a point would look identical.
	 */
	void AppendCallKey(std::string& Out, const rql::Value& Value, int32 Depth = 0)
	{
		if (Depth > 32)
		{
			Out += "...";
			return;
		}
		switch (Value.kind())
		{
		case rql::Value::Kind::Number:
			Out += Value.is_single() ? rql::format_float(static_cast<float>(Value.as_number())) : rql::format_double(Value.as_number());
			return;
		case rql::Value::Kind::String:
			Out += rql::Json(Value.as_string()).dump();
			return;
		case rql::Value::Kind::Object:
		{
			char Buffer[32];
			std::snprintf(Buffer, sizeof(Buffer), "@%p", Value.ptr());
			Out += Buffer;
			return;
		}
		case rql::Value::Kind::Struct:
			Out += Value.type()->name + "{";
			for (const rql::MemberDescriptor& Member : Value.type()->members)
			{
				if (Member.explicit_only || !Member.getter)
					continue;
				Out += Member.name + "=";
				try
				{
					AppendCallKey(Out, Member.getter(Value.ptr()), Depth + 1);
				}
				catch (const std::exception&)
				{
					Out += "?";
				}
				Out += ",";
			}
			Out += "}";
			return;
		case rql::Value::Kind::List:
			Out += "[";
			for (size_t Index = 0, Count = Value.list().size(); Index < Count; ++Index)
			{
				AppendCallKey(Out, Value.list().at(Index), Depth + 1);
				Out += ",";
			}
			Out += "]";
			return;
		case rql::Value::Kind::Map:
			Out += "{";
			Value.map().for_each([&Out, Depth](const rql::Value& Key, const rql::Value& Item)
			{
				AppendCallKey(Out, Key, Depth + 1);
				Out += ":";
				AppendCallKey(Out, Item, Depth + 1);
				Out += ",";
				return true;
			});
			Out += "}";
			return;
		case rql::Value::Kind::Entry:
			AppendCallKey(Out, Value.entry().key, Depth + 1);
			Out += ":";
			AppendCallKey(Out, Value.entry().value, Depth + 1);
			return;
		case rql::Value::Kind::Row:
			Out += "(";
			for (const rql::Value& Cell : Value.row().values)
			{
				AppendCallKey(Out, Cell, Depth + 1);
				Out += ",";
			}
			Out += ")";
			return;
		case rql::Value::Kind::Null:
			Out += "null";
			return;
		case rql::Value::Kind::Missing:
			Out += "missing";
			return;
		default: // bool, enum, error, unknown
			Out += rql::describe_value(Value) + ":" + rql::to_text(Value);
			return;
		}
	}

	/**
	 * A failed or empty query whose plan touches the domain of an extension this editor does not have (niagaraSystems,
	 * PCGGraph, gasAbilities, ...) names that extension and how to get it: on the first error's hint, or as a warning. Only a
	 * read finds "nothing": a mutation's empty receipt (a set that kept every value) is not a missing domain.
	 */
	void AddExtensionHint(rql::QueryResult& Result, const FString& PlanText, const rql::QueryPlan& Plan, bool bMutation)
	{
		rql::Diagnostic* Error = nullptr;
		for (rql::Diagnostic& Diagnostic : Result.diagnostics)
		{
			if (Diagnostic.severity == rql::Severity::Error)
			{
				Error = &Diagnostic;
				break;
			}
		}
		if (!Error && !IsEmptyRead(Result, Plan, bMutation))
			return;
		const TArray<FUnrealMcpMissingExtension> Candidates = UnrealMcpAvailableExtensions::Missing();
		const FUnrealMcpMissingExtension* Match = UnrealMcpAvailableExtensions::MatchDomain(PlanText, Candidates);
		if (!Match)
			return;
		const std::string Hint = UnrealMcpRqlToStd(UnrealMcpAvailableExtensions::Hint(*Match));
		if (!Error)
		{
			Result.diagnostics.push_back(rql::Diagnostic::warning("extension-missing", "nothing found; it may come with an Unreal-MCP extension this editor does not have", Hint));
			return;
		}
		const std::string Id = UnrealMcpRqlToStd(Match->Extension.ExtensionId);
		if (Error->message.find(Id) != std::string::npos || Error->hint.find(Id) != std::string::npos)
			return; // the error already names it (the Enhanced Input node hint of Blueprint graphs)
		Error->hint = Error->hint.empty() ? Hint : Error->hint + "; " + Hint;
	}

	/**
	 * A read of actors that found nothing names the level it searched: actors covers only the level open in the editor,
	 * so an agent asking about another map gets nothing (seen in an eval, which then fell back to Python). The hint says
	 * how to open the other level.
	 */
	void AddOpenLevelHint(rql::QueryResult& Result, const rql::QueryPlan& Plan, bool bMutation)
	{
		const rql::ExprPtr& Source = Plan.source;
		const bool bActors = Source && Source->kind == rql::ExprKind::Path && !Source->on && !Source->segments.empty()
			&& Source->segments[0].kind == rql::SegmentKind::Member && Source->segments[0].name == "actors";
		if (!bActors || !IsEmptyRead(Result, Plan, bMutation))
			return;
		const UWorld* World = UnrealMcpRqlUtil::EditorWorld();
		// A World Partition level lists the actors of its loaded cells only: there, "not found" may also mean "not loaded".
		const TCHAR* Partitioned = World && World->GetWorldPartition() ? TEXT(" (a World Partition level: only its loaded cells)") : TEXT("");
		const FString Message = FString::Printf(TEXT("nothing found in the level open in the editor, \"%s\": actors covers only that level%s"),
			*UnrealMcpRqlUtil::OpenLevelPath(), Partitioned);
		Result.diagnostics.push_back(rql::Diagnostic::warning("open-level-only", UnrealMcpRqlToStd(Message), UnrealMcpRqlGuide::OpenLevelHint()));
	}

	/**
	 * A query over the editor world's actors (actors, levels, world) in a World Partition level sees only the loaded ones:
	 * agents never found a lake or orphaned proxies outside the loaded regions and spent dozens of Python calls loading
	 * them. While some are not loaded, the answer says how many, names a few and how to list and load them.
	 */
	void AddUnloadedActorsNote(rql::QueryResult& Result, const rql::QueryPlan& Plan)
	{
		const rql::ExprPtr& Source = Plan.source;
		if (!Result.ok || !Source || Source->kind != rql::ExprKind::Path || Source->on || Source->segments.empty()
			|| Source->segments[0].kind != rql::SegmentKind::Member)
			return;
		const std::string& Name = Source->segments[0].name;
		if (Name != "actors" && Name != "levels" && Name != "world")
			return;
		FString Message;
		FString Hint;
		if (UnrealMcpRqlWorldPartition::UnloadedNote(UnrealMcpRqlUtil::EditorWorld(), Message, Hint))
			Result.diagnostics.push_back(rql::Diagnostic::warning("world-partition-unloaded", UnrealMcpRqlToStd(Message), UnrealMcpRqlToStd(Hint)));
	}

	/**
	 * A read-only call over more input than one call takes failed with RQL's mutation-limit ("... nothing was changed, pass
	 * max_targets"), which reads as a refused change. It is a read: say so, and how to split it. Reads keep the 1000 ceiling
	 * on purpose: every item is one result row, and 1000 rows already fill the 64 KB answer.
	 */
	void RewordReadLimit(rql::QueryResult& Result, const std::string& Method, int32 Limit)
	{
		for (rql::Diagnostic& Diagnostic : Result.diagnostics)
		{
			if (Diagnostic.code != "mutation-limit")
				continue;
			const std::string& Old = Diagnostic.message;
			const size_t Digits = Old.find_first_not_of("0123456789");
			const std::string Count = Digits > 0 && Digits != std::string::npos ? Old.substr(0, Digits) : "more than " + std::to_string(Limit);
			const std::string N = std::to_string(Limit);
			Diagnostic.code = "read-limit";
			Diagnostic.message = Count + " items reach the read-only '" + Method + "' call: at most " + N + " per call (one result row each)";
			Diagnostic.hint = "split the input: ... | first " + N + " | call " + Method + "(...), then ... | skip " + N + " | first " + N
				+ " | call " + Method + "(...); or sample fewer points";
		}
	}

	/**
	 * A read's answer cut by the 64 KB output limit gets RQL's receipt hint ("the receipt is shortened, not the change: all N
	 * calls were made; do not repeat it"), the opposite of what a read needs: the rest is read in batches of what fitted.
	 */
	void RewordReadTruncation(rql::QueryResult& Result, const std::string& Method)
	{
		for (rql::Diagnostic& Diagnostic : Result.diagnostics)
		{
			if (Diagnostic.code != "output-truncated")
				continue;
			// "output limited to 65536 bytes; <shown> of <total> rows returned"
			const size_t Of = Diagnostic.message.find(" of ");
			const size_t Start = Diagnostic.message.rfind("; ", Of);
			if (Of == std::string::npos || Start == std::string::npos)
				continue;
			const std::string Shown = Diagnostic.message.substr(Start + 2, Of - Start - 2);
			Diagnostic.hint = "a read, not a change: the answer holds only the first " + Shown + " rows; read the rest in batches of " + Shown
				+ ": ... | skip " + Shown + " | first " + Shown + " | call " + Method + "(...), and so on (fewer outputs per call fit more rows)";
		}
	}

	/**
	 * Items a list-valued call result shows. RQL encodes at most maxListItems (20) items of a list and appends "…+N", which
	 * suits browsing a member but silently dropped call results (50 sampled heights, 20 seen), and call is terminal, so
	 * nothing can page them. A plan ending in call encodes up to this many instead; plain queries keep RQL's default.
	 */
	constexpr int32 MaxCallResultItems = 1000;

	bool EndsWithCall(const rql::QueryPlan& Plan)
	{
		return !Plan.stages.empty() && Plan.stages.back().kind == rql::StageKind::Call;
	}

	/**
	 * The lists and maps of one encoded call result that RQL cut at MaxCallResultItems, at any depth: a list ends in
	 * "…+<left out>", a map has the key "…" with the value "+<left out>" (U+2026 in UTF-8).
	 */
	void CountCutLists(const rql::Json& Value, int32& OutLists, int64& OutLeftOut)
	{
		static const std::string Ellipsis = "\xE2\x80\xA6";
		const size_t Cut = static_cast<size_t>(MaxCallResultItems) + 1;
		if (Value.is_array() && Value.size() == Cut && Value.at(MaxCallResultItems).is_string()
			&& Value.at(MaxCallResultItems).as_string().rfind(Ellipsis + "+", 0) == 0)
		{
			++OutLists;
			OutLeftOut += FCString::Atoi64(UTF8_TO_TCHAR(Value.at(MaxCallResultItems).as_string().c_str() + Ellipsis.size() + 1));
		}
		else if (Value.is_object() && Value.size() == Cut)
		{
			const rql::Json* LeftOut = Value.find(Ellipsis);
			if (LeftOut && LeftOut->is_string() && LeftOut->as_string().rfind("+", 0) == 0)
			{
				++OutLists;
				OutLeftOut += FCString::Atoi64(UTF8_TO_TCHAR(LeftOut->as_string().c_str() + 1));
			}
		}
		for (const rql::Json& Item : Value.items())
			CountCutLists(Item, OutLists, OutLeftOut);
	}

	/** Call results with a list or map cut at MaxCallResultItems get a warning: how many items were left out, and what to do. */
	void AddCallResultListNote(rql::QueryResult& Result)
	{
		if (!Result.ok || !Result.rows || !Result.columns)
			return;
		const std::vector<std::string>& Columns = *Result.columns;
		const auto Column = std::find(Columns.begin(), Columns.end(), "result");
		if (Column == Columns.end())
			return;
		const size_t Index = static_cast<size_t>(Column - Columns.begin());
		int64 LeftOut = 0;
		int32 Lists = 0;
		bool bWholeResult = false; // the one cut list is the result itself (not a member or an output of it)
		for (const std::vector<rql::Json>& Row : *Result.rows)
		{
			if (Index >= Row.size())
				continue;
			const int32 Before = Lists;
			CountCutLists(Row[Index], Lists, LeftOut);
			if (Before == 0 && Lists == 1)
				bWholeResult = Row[Index].is_array() && Row[Index].size() == static_cast<size_t>(MaxCallResultItems) + 1;
		}
		if (Lists == 0)
			return;
		const FString Message = Lists == 1 && bWholeResult
			? FString::Printf(TEXT("the call result lists %lld items: the first %d are shown, %lld left out"),
				LeftOut + MaxCallResultItems, MaxCallResultItems, LeftOut)
			: FString::Printf(TEXT("%d list(s) in the call results hold more than %d items: the first %d of each are shown, %lld item(s) left out"),
				Lists, MaxCallResultItems, MaxCallResultItems, LeftOut);
		Result.diagnostics.push_back(rql::Diagnostic::warning("call-result-truncated", UnrealMcpRqlToStd(Message),
			UnrealMcpRqlToStd(FString::Printf(TEXT("the call ran: only its result is shortened. To see every item, call again with fewer inputs "
				"(at most %d items per list); do not repeat a call that creates or changes things just to read its result"), MaxCallResultItems))));
	}

	/** Objects whose changes belong to the project (not class defaults of compiled-in classes, not transient helpers). */
	bool IsProjectObject(const UObject* Object)
	{
		if (!Object)
			return false;
		const UPackage* Package = Object->GetOutermost();
		return Package && Package != GetTransientPackage() && !Package->HasAnyPackageFlags(PKG_CompiledIn);
	}

	/** A few concrete subclasses of an abstract instanced class, for the error that asks for a $type. */
	FString ConcreteSubclassNames(const UClass* Base)
	{
		TArray<UClass*> Derived;
		GetDerivedClasses(Base, Derived, /*bRecursive*/ true);
		TArray<FString> Names;
		for (const UClass* Class : Derived)
		{
			const FString Name = Class->GetName();
			if (Class->HasAnyClassFlags(CLASS_EditInlineNew) && !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
				&& !Name.StartsWith(TEXT("SKEL_")) && !Name.StartsWith(TEXT("REINST_")))
				Names.Add(Name);
		}
		Names.Sort();
		static constexpr int32 MaxNames = 8;
		const int32 More = Names.Num() - MaxNames;
		if (More > 0)
			Names.SetNum(MaxNames);
		if (Names.IsEmpty())
			return TEXT("none is loaded");
		const FString Rest = More > 0 ? FString::Printf(TEXT(" and %d more"), More) : FString();
		return FString::Printf(TEXT("e.g. %s%s"), *FString::Join(Names, TEXT(", ")), *Rest);
	}

	/** JSON from an extension (AddJsonMember) as an owned RQL value. */
	rql::Value FromJson(const TSharedPtr<FJsonValue>& Json)
	{
		if (!Json.IsValid())
			return rql::Value::null();
		switch (Json->Type)
		{
		case EJson::Boolean: return rql::Value::boolean(Json->AsBool());
		case EJson::Number: return rql::Value::number(Json->AsNumber());
		case EJson::String: return rql::Value::string(UnrealMcpRqlToStd(Json->AsString()));
		case EJson::Array:
		{
			std::vector<rql::Value> Items;
			for (const TSharedPtr<FJsonValue>& Item : Json->AsArray())
				Items.push_back(FromJson(Item));
			return rql::Value::list_of(std::move(Items));
		}
		case EJson::Object:
		{
			std::vector<std::pair<rql::Value, rql::Value>> Entries;
			const TSharedPtr<FJsonObject>& Object = Json->AsObject();
			if (Object.IsValid())
			{
				for (const auto& Pair : Object->Values)
					Entries.emplace_back(rql::Value::string(UnrealMcpRqlToStd(FString(*Pair.Key))), FromJson(Pair.Value));
			}
			return rql::Value::map(std::make_shared<rql::OwnedMap>(nullptr, std::move(Entries)));
		}
		default:
			return rql::Value::null();
		}
	}

	FString TypeLabel(const FUnrealMcpRqlSession::FEntry& Entry)
	{
		const FString Name = Entry.Type ? UnrealMcpRqlFromStd(Entry.Type->name) : FString(TEXT("any"));
		return Entry.bList ? FString(TEXT("list of ")) + Name : Name;
	}

	/** A source as rql-schema lists it: name, type, description. */
	rql::Json SourceJson(const FUnrealMcpRqlSession::FEntry& Entry)
	{
		rql::Json Item = rql::Json::object();
		Item.set("name", UnrealMcpRqlToStd(Entry.Name));
		Item.set("type", UnrealMcpRqlToStd(TypeLabel(Entry)));
		if (!Entry.Description.IsEmpty())
			Item.set("description", UnrealMcpRqlToStd(Entry.Description));
		return Item;
	}

	/** A Markdown table cell. Not `Cell`: macOS MacTypes.h (pulled in by UE 5.7 on Mac) declares a global `Cell`. */
	FString TableCell(const FString& Text)
	{
		return Text.Replace(TEXT("|"), TEXT("\\|")).Replace(TEXT("\n"), TEXT(" "));
	}

	/** The UE-typed registration surface handed to IUnrealMcpRqlModule::ConfigureRql. */
	class FModuleContext final : public FUnrealMcpRqlModuleContext
	{
	public:
		explicit FModuleContext(FUnrealMcpRqlSession& InSession) : Session(InSession) {}

		/** The operations classes the module being configured adds (its guide covers them); set per module. */
		TArray<UClass*>* ApiClasses = nullptr;

		virtual void AddObjectSource(const FString& Name, const FString& Description, UClass* Class, TFunction<UObject*()> Getter) override
		{
			FUnrealMcpRqlTypes* Types = &Session.Types();
			Session.AddSource(Name, Types->ForClass(Class ? Class : UObject::StaticClass()), /*bList*/ false, Description,
				[Types, Getter]() { return Types->WrapObject(Getter ? Getter() : nullptr); });
		}

		virtual void AddObjectListSource(const FString& Name, const FString& Description, UClass* ElementClass,
			TFunction<TArray<UObject*>()> Getter) override
		{
			FUnrealMcpRqlTypes* Types = &Session.Types();
			Session.AddSource(Name, Types->ForClass(ElementClass ? ElementClass : UObject::StaticClass()), /*bList*/ true, Description,
				[Types, Getter]() { return Types->WrapObjects(Getter ? Getter() : TArray<UObject*>()); });
		}

		virtual void AddAssetSource(const FString& Name, const FString& Description, UClass* AssetClass) override
		{
			FUnrealMcpRqlTypes* Types = &Session.Types();
			Session.AddSource(Name, Types->ForClass(AssetClass ? AssetClass : UObject::StaticClass()), /*bList*/ true, Description,
				[Types, AssetClass]() { return UnrealMcpRqlAssetList(*Types, AssetClass, FString()); });
		}

		virtual void AddApiClass(UClass* Class, const FString& TypeName) override
		{
			Session.Types().AddApiClass(Class, TypeName);
			if (ApiClasses && Class)
				ApiClasses->Add(Class);
		}

		virtual void ExposeProperties(UStruct* Owner, const TArray<FName>& PropertyNames, bool bWritable) override
		{
			Session.Types().ExposeProperties(Owner, PropertyNames, bWritable);
		}

		virtual void AddJsonMember(UClass* Class, const FString& Name, const FString& Description,
			TFunction<TSharedPtr<FJsonValue>(UObject*)> Getter) override
		{
			const std::string MemberName = UnrealMcpRqlToStd(Name);
			const std::string MemberDescription = UnrealMcpRqlToStd(Description);
			Session.Types().AddClassHook(Class, [MemberName, MemberDescription, Getter](FUnrealMcpRqlTypes&, rql::TypeDescriptor& Descriptor)
			{
				rql::MemberDescriptor Member;
				Member.name = MemberName;
				Member.description = MemberDescription;
				Member.type = rql::builtin::any_type();
				Member.nullable = true;
				Member.owning = false;
				Member.readonly = true;
				Member.getter = [Getter](void* Self) -> rql::Value
				{
					return Getter ? FromJson(Getter(static_cast<UObject*>(Self))) : rql::Value::null();
				};
				FUnrealMcpRqlTypes::SetMember(Descriptor, std::move(Member));
			});
		}

		virtual void AddClassDefaultsSource(const FString& Name, const FString& Description, UClass* BaseClass) override
		{
			FUnrealMcpRqlTypes* Types = &Session.Types();
			Session.AddSource(Name, Types->ForClass(BaseClass ? BaseClass : UObject::StaticClass()), /*bList*/ true, Description,
				[Types, BaseClass]() { return UnrealMcpRqlClassDefaultsList(*Types, BaseClass); });
		}

		virtual void AddObjectMember(UClass* Class, const FString& Name, const FString& Description, UClass* ObjectClass,
			TFunction<UObject*(UObject*)> Getter) override
		{
			const std::string MemberName = UnrealMcpRqlToStd(Name);
			const std::string MemberDescription = UnrealMcpRqlToStd(Description);
			Session.Types().AddClassHook(Class, [MemberName, MemberDescription, ObjectClass, Getter](FUnrealMcpRqlTypes& Types, rql::TypeDescriptor& Descriptor)
			{
				rql::MemberDescriptor Member;
				Member.name = MemberName;
				Member.description = MemberDescription;
				Member.type = Types.ForClass(ObjectClass ? ObjectClass : UObject::StaticClass());
				Member.nullable = true;
				Member.owning = false;
				Member.readonly = true;
				FUnrealMcpRqlTypes* TypesPtr = &Types;
				Member.getter = [TypesPtr, Getter](void* Self) -> rql::Value
				{
					return TypesPtr->WrapObject(Getter ? Getter(static_cast<UObject*>(Self)) : nullptr);
				};
				FUnrealMcpRqlTypes::SetMember(Descriptor, std::move(Member));
			});
		}

		virtual void AddObjectListMember(UClass* Class, const FString& Name, const FString& Description, UClass* ElementClass,
			TFunction<TArray<UObject*>(UObject*)> Getter) override
		{
			const std::string MemberName = UnrealMcpRqlToStd(Name);
			const std::string MemberDescription = UnrealMcpRqlToStd(Description);
			Session.Types().AddClassHook(Class, [MemberName, MemberDescription, ElementClass, Getter](FUnrealMcpRqlTypes& Types, rql::TypeDescriptor& Descriptor)
			{
				rql::MemberDescriptor Member;
				Member.name = MemberName;
				Member.description = MemberDescription;
				Member.type = Types.Registry().list_type(Types.ForClass(ElementClass ? ElementClass : UObject::StaticClass()), true);
				Member.owning = true;
				Member.readonly = true;
				FUnrealMcpRqlTypes* TypesPtr = &Types;
				Member.getter = [TypesPtr, Getter](void* Self) -> rql::Value
				{
					return TypesPtr->WrapObjects(Getter ? Getter(static_cast<UObject*>(Self)) : TArray<UObject*>());
				};
				FUnrealMcpRqlTypes::SetMember(Descriptor, std::move(Member));
			});
		}

		virtual void AddGuide(const FString& Topic, const FString& Markdown) override
		{
			Session.AddGuide(Topic, Markdown);
		}

	private:
		FUnrealMcpRqlSession& Session;
	};
}

using namespace UnrealMcpRqlSessionLocal;

void FUnrealMcpRqlModuleContext::AddApiSource(const FString& Name, const FString& Description, UClass* ApiClass, const FString& TypeName)
{
	AddApiClass(ApiClass, TypeName);
	AddObjectSource(Name, Description, ApiClass, [ApiClass]() -> UObject* { return ApiClass ? ApiClass->GetDefaultObject() : nullptr; });
}

FUnrealMcpRqlSession::FUnrealMcpRqlSession()
	: TypesPtr(MakeUnique<FUnrealMcpRqlTypes>())
	, EnginePtr(std::make_unique<rql::Engine>(TypesPtr->Registry()))
{
	rql::Engine& RqlEngine = *EnginePtr;
	RqlEngine.runtime_name = "unreal";
	// rql-query / rql-mutate take no budgets argument: hints must not advise raising budgets.* (rql-mutate's target limit
	// has its own argument, MutationPolicy::limit_argument). ref("Name") hints name the finder, type-list hints the schema tool.
	RqlEngine.defaults().budgets_adjustable = false;
	RqlEngine.find_hint = "unreal.find(\"\xE2\x80\xA6\")";
	RqlEngine.schema_hint = "rql-schema";
	RqlEngine.mutate_hint = "rql-mutate"; // a set / call sent to rql-query: the permission-denied hint names the mutation tool
	FUnrealMcpRqlTypes* TypeProvider = TypesPtr.Get();
	// Liveness from the weak pointer every object value carries: a destroyed (garbage) actor reads as null.
	RqlEngine.is_alive = [TypeProvider](const rql::Value& Object) { return TypeProvider->IsAlive(Object); };
	// Member names of every reflected class and struct: streams are polymorphic (actors | get Components), so a set whose
	// items all lack a member some component class has is a no-match, not a typo (RQL spec 8: a typo that empties a
	// mutation is an error).
	RqlEngine.is_known_member = [TypeProvider](const std::string& Name) { return TypeProvider->IsKnownMemberName(UnrealMcpRqlFromStd(Name)); };
	// "PointLight#1234": the id is FUnrealMcpRqlTypes::IdOf (object slot + serial), resolved without searching the sources.
	RqlEngine.resolve_stable_ref = [TypeProvider](const std::string& Type, const std::string& Id) -> rql::Value
	{
		int64 Parsed = 0;
		if (!LexTryParseString(Parsed, *UnrealMcpRqlFromStd(Id)))
			return rql::Value::missing();
		UObject* Object = FUnrealMcpRqlTypes::FindById(Parsed);
		if (!Object)
			return rql::Value::missing();
		rql::Value Wrapped = TypeProvider->WrapObject(Object);
		return Wrapped.type() && Wrapped.type()->is_a(Type) ? Wrapped : rql::Value::missing();
	};
	ConfigureMutations();
	UnrealMcpRqlConfigureCore(*this);
	ConfigureModules();
}

FUnrealMcpRqlSession::~FUnrealMcpRqlSession()
{
	CloseTransaction();
	EnginePtr.reset(); // before the registry it references
}

void FUnrealMcpRqlSession::ConfigureMutations()
{
	rql::MutationPolicy& Policy = EnginePtr->mutations();
	Policy.enabled = true;
	// A mutation is one undo step whatever its size, so the default target limit is an editor's (RQL spec 8 target
	// limit); rql-mutate's max_targets raises it per call up to the ceiling, and the mutation-limit hint names it.
	Policy.max_targets = DefaultMaxTargets;
	Policy.hard_max_targets = HardMaxTargets;
	Policy.limit_argument = "max_targets";
	FUnrealMcpRqlTypes* TypeProvider = TypesPtr.Get();
	auto WrittenObject = [TypeProvider](const rql::MutationRecord& Record) -> UObject*
	{
		UObject* Owner = TypeProvider->AsObject(Record.owner);
		return Owner ? Owner : TypeProvider->AsObject(Record.item);
	};
	// The actor whose editor view state (its selection, its temporary viewport hiding) a record sets: not saved with the
	// level, so it must not mark the level unsaved. The core members only: a class property of that name is saved data.
	auto ViewStateActor = [WrittenObject](const rql::MutationRecord& Record) -> AActor*
	{
		if (Record.method)
			return nullptr;
		FString Member = UnrealMcpRqlFromStd(Record.member);
		Member.Split(TEXT("."), nullptr, &Member, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (!Member.Equals(TEXT("Selected"), ESearchCase::IgnoreCase) && !Member.Equals(TEXT("HiddenInEditor"), ESearchCase::IgnoreCase))
			return nullptr;
		AActor* Actor = Cast<AActor>(WrittenObject(Record));
		return Actor && !FindFProperty<FProperty>(Actor->GetClass(), *Member) ? Actor : nullptr;
	};
	// The object whose saved state a record changes (null for view state).
	auto OwnerOf = [WrittenObject, ViewStateActor](const rql::MutationRecord& Record) -> UObject*
	{
		return ViewStateActor(Record) ? nullptr : WrittenObject(Record);
	};
	auto IsWritableTemplate = [this](const UObject* Object)
	{
		for (const TFunction<bool(const UObject*)>& Predicate : WritableTemplates)
		{
			if (Predicate(Object))
				return true;
		}
		return false;
	};
	// The checks that need the planned changes (dry runs report them too): the core one, then the modules' validators.
	Policy.validate_batch = [this, TypeProvider, IsWritableTemplate](const std::vector<rql::MutationRecord>& Records) -> std::string
	{
		// rql-query runs a call only when every planned call is of a read-only operation (Execute's name check let it
		// through); anything else is refused like any call in rql-query, before it runs (first: the
		// refusal is rql-query's permission-denied, whatever else the batch would fail on).
		bReadOnlyCalls = AllReadOnlyCalls(Records);
		if (bReadOnlyCallRequest && !bReadOnlyCalls)
		{
			bRefusedReadOnlyCall = true;
			return std::string("not a read-only operation");
		}
		// rql-mutate raised the limit for a read-only operation's name: a same-named call that changes something keeps the
		// editor's limit.
		if (bReadOnlyLimit && Records.size() > static_cast<size_t>(DefaultMaxTargets) && !AllReadOnlyCalls(Records))
			return UnrealMcpRqlToStd(FString::Printf(TEXT("%d calls, more than the limit of %d for a change (pass max_targets = %d)"),
				static_cast<int32>(Records.size()), DefaultMaxTargets, static_cast<int32>(Records.size())));
		// Class defaults and archetypes (the class-default sources' rows, a Blueprint's defaults) are read-only: an
		// in-memory change to a CDO is not a saved edit and leaks into every new instance. API objects (`editor`, the
		// extensions' operations) are class defaults by design and stay callable.
		for (const rql::MutationRecord& Record : Records)
		{
			for (UObject* Object : { TypeProvider->AsObject(Record.item), TypeProvider->AsObject(Record.owner) })
			{
				if (Object && Object->IsTemplate() && !TypeProvider->IsApiClass(Object->GetClass()) && !IsWritableTemplate(Object))
					return UnrealMcpRqlToStd(FString::Printf(
						TEXT("%s is a class default object, read-only through RQL (edit the class or Blueprint instead)"), *Object->GetName()));
			}
		}
		// A call cannot be undone generically: one object reached twice (actors | get Parent | call Destroy() over two
		// siblings) would run the call twice, and a second call failing after the first changed the editor breaks the
		// all-or-nothing promise. Refuse the duplicate before anything runs. A read-only operation changes nothing, so
		// running it twice is harmless: it is never a duplicate.
		TMap<TPair<const void*, FString>, int32> Called;
		for (int32 Index = 0; Index < static_cast<int32>(Records.size()); ++Index)
		{
			const rql::MutationRecord& Record = Records[Index];
			if (!Record.method || !Record.item.is_object() || !Record.item.ptr() || IsReadOnlyCall(Record))
				continue;
			// An operations object (`editor`, an extension's) is called once per stream item by design (range(0, 300) | call
			// editor.SpawnActor(...) creates 300 actors): the same operation twice with the same arguments is the duplicate
			// (DeleteAsset("/Game/X") twice), except a spawn, where identical arguments still mean another actor.
			FString Key = UnrealMcpRqlFromStd(Record.method->name).ToLower();
			const UObject* Receiver = TypeProvider->AsObject(Record.item);
			const bool bOperation = Receiver && TypeProvider->IsApiClass(Receiver->GetClass());
			if (bOperation)
			{
				if (Key.StartsWith(TEXT("spawn")))
					continue;
				std::string Arguments;
				for (const rql::Value& Argument : Record.args)
				{
					Arguments += "|";
					AppendCallKey(Arguments, Argument);
				}
				// Hashed: an argument can be a whole heightmap or vertex list, and every key stays alive for the batch.
				Key += FSHA1::HashBuffer(Arguments.data(), Arguments.size()).ToString();
			}
			const TPair<const void*, FString> CallKey(Record.item.ptr(), Key);
			if (const int32* First = Called.Find(CallKey))
			{
				const FString Method = UnrealMcpRqlFromStd(Record.method->name);
				// An operation: two stream items gave it the same arguments.
				if (bOperation)
					return UnrealMcpRqlToStd(FString::Printf(TEXT("%s would run twice with the same arguments (calls %d and %d of this "
						"stage, counting from 1): give each item its own arguments ($, $[0]), or drop repeated items with | distinct before call"), *Method, *First + 1, Index + 1));
				const AActor* Actor = Cast<AActor>(Receiver);
				const FString Name = Actor ? Actor->GetActorLabel() : Receiver ? Receiver->GetName() : FString(TEXT("one object"));
				return UnrealMcpRqlToStd(FString::Printf(TEXT("%s would run twice on %s (it is in the stream more than once: "
					"add | distinct before call)"), *Method, *Name));
			}
			Called.Add(CallKey, Index);
		}
		for (const FBatchValidator& Validator : BatchValidators)
		{
			const FString Reason = Validator(Records);
			if (!Reason.IsEmpty())
				return UnrealMcpRqlToStd(Reason);
		}
		return std::string();
	};
	// Literal kinds (RQL spec 4.5): actors, components, assets and every other UObject are engine objects, never built from
	// a literal ({} for an actor parameter is refused and nothing is constructed); EditInlineNew classes (input triggers and
	// modifiers, ...) are instanced: a typed literal builds one for the object that owns the written member or is the call's
	// target. Liveness is the engine-wide is_alive hook here, not a per-type adapter, so RQL's own default does not apply.
	Policy.literal_kind_of = [TypeProvider](const rql::TypeDescriptor& Type) -> std::optional<rql::LiteralKind>
	{
		const UClass* Class = TypeProvider->ClassOf(&Type);
		if (!Class)
			return std::nullopt;
		if (!Class->HasAnyClassFlags(CLASS_EditInlineNew))
			return rql::LiteralKind::Engine;
		TypeProvider->Fill(&Type); // the literal's members are looked up next, also by probes that build nothing
		return rql::LiteralKind::Instanced;
	};
	// A literal's $type names a UClass the provider builds descriptors for on demand (RQL only knows the registered ones):
	// by name (InputTriggerHold, UInputTriggerHold, a Blueprint's BP_Hold or BP_Hold_C) or by the path of a loaded class,
	// if it is or derives from the target. The hook also runs for overload probes and dry runs, so a path never loads a
	// package. An abstract class is returned too, so the factory's error names its concrete classes.
	Policy.resolve_subtype = [TypeProvider, OfferedBases = MakeShared<TSet<UClass*>>()](const rql::TypeDescriptor& Target, const std::string& Name) -> const rql::TypeDescriptor*
	{
		UClass* Base = TypeProvider->ClassOf(&Target);
		if (!Base)
			return nullptr;
		const FString Text = UnrealMcpRqlFromStd(Name).TrimStartAndEnd();
		UClass* Class = nullptr;
		if (Text.Contains(TEXT("/")))
		{
			Class = FindObject<UClass>(nullptr, *Text);
			if (!Class && !Text.Contains(TEXT(".")))
				Class = FindObject<UClass>(nullptr, *FString::Printf(TEXT("%s.%s_C"), *Text, *FPaths::GetBaseFilename(Text)));
		}
		else
		{
			Class = FUnrealMcpObjectRef::ResolveClass(Text);
			if (!Class)
				Class = FUnrealMcpObjectRef::ResolveClass(Text + TEXT("_C"));
		}
		if (!Class || !Class->IsChildOf(Base))
		{
			// No such class: register the loaded concrete subclasses of the target, so RQL's "did you mean" (it suggests
			// registered descriptors only, the hook cannot list names) offers InputTriggerHold for "InputTrigerHold". Capped:
			// a broad target (Object, Actor) has thousands of subclasses and gets no suggestion rather than a slow error.
			// Once per target and session (probes and dry runs call the hook again). A name two loaded classes share is left
			// out: registered, it would make later $type lookups of that name ambiguous instead of resolved here.
			constexpr int32 MaxSuggestedSubclasses = 200;
			bool bAlreadyOffered = false;
			OfferedBases->Add(Base, &bAlreadyOffered);
			if (bAlreadyOffered)
				return nullptr;
			TArray<UClass*> Derived;
			GetDerivedClasses(Base, Derived, /*bRecursive*/ true);
			const bool bInstanced = Base->HasAnyClassFlags(CLASS_EditInlineNew);
			TMap<FName, int32> NameCounts;
			Derived.RemoveAll([bInstanced, &NameCounts](const UClass* Sub)
			{
				const FString SubName = Sub->GetName();
				const bool bDrop = Sub->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists | CLASS_Hidden)
					|| (bInstanced && !Sub->HasAnyClassFlags(CLASS_EditInlineNew))
					|| SubName.StartsWith(TEXT("SKEL_")) || SubName.StartsWith(TEXT("REINST_"));
				if (!bDrop)
					++NameCounts.FindOrAdd(Sub->GetFName());
				return bDrop;
			});
			if (Derived.Num() <= MaxSuggestedSubclasses)
			{
				for (UClass* Sub : Derived)
				{
					if (NameCounts.FindRef(Sub->GetFName()) != 1)
						continue;
					const rql::TypeDescriptor* Descriptor = TypeProvider->ForClass(Sub);
					TypeProvider->Fill(Descriptor);
					TypeProvider->Registry().register_descriptor(const_cast<rql::TypeDescriptor*>(Descriptor)); // idempotent
				}
			}
			return nullptr;
		}
		const rql::TypeDescriptor* Descriptor = TypeProvider->ForClass(Class);
		TypeProvider->Fill(Descriptor);
		return Descriptor;
	};
	// Builds an instanced subobject as the editor's instanced arrays do: NewObject in the owner, transactional. Runs while the
	// mutation is planned (dry runs too): nothing references the object until the write (an unused one is collected), so
	// the session holds it until the query ends (a call in the same query may collect garbage).
	Policy.create_for_literal = [this, TypeProvider](const rql::TypeDescriptor& Type, const rql::Value& Outer) -> std::optional<rql::Value>
	{
		UClass* Class = TypeProvider->ClassOf(&Type);
		if (!Class)
			return std::nullopt;
		UObject* Owner = TypeProvider->AsObject(Outer);
		if (!Owner)
			throw std::runtime_error("it needs an owning object (write it to a member of one, or pass it to a call on one)");
		if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
			throw std::runtime_error(UnrealMcpRqlToStd(FString::Printf(TEXT("%s is %s: name a concrete class with $type (%s)"),
				*Class->GetName(), Class->HasAnyClassFlags(CLASS_Abstract) ? TEXT("abstract") : TEXT("deprecated or outdated"),
				*UnrealMcpRqlSessionLocal::ConcreteSubclassNames(Class))));
		// NewObject asserts (a fatal error) on an outer its class does not allow (UCLASS(Within=...)), and an operations
		// object or a class default would make the subobject a read-only template.
		if (Class->ClassWithin && !Owner->IsA(Class->ClassWithin))
			throw std::runtime_error(UnrealMcpRqlToStd(FString::Printf(TEXT("%s can only be owned by a %s, not by %s"),
				*Class->GetName(), *Class->ClassWithin->GetName(), *Owner->GetName())));
		if (Owner->IsTemplate())
			throw std::runtime_error(UnrealMcpRqlToStd(FString::Printf(TEXT("%s is a class default or operations object and cannot own it"), *Owner->GetName())));
		UObject* Made = NewObject<UObject>(Owner, Class, NAME_None, Owner->GetMaskedFlags(RF_PropagateToSubObjects) | RF_Transactional);
		LiteralObjects.Emplace(Made);
		return TypeProvider->WrapObject(Made);
	};
	Policy.literal_refusal_hint = [](const rql::TypeDescriptor&)
	{
		return std::string("reference an existing one: unreal.find(\"Label\"), ref(\"Type#id\") or an asset path such as "
			"\"/Game/Meshes/SM_Rock\" (create actors, components and assets with editor | call SpawnActor / AddComponent / CreateAsset)");
	};
	// UE's own text form for structs without an RQL adapter (the editor's Copy): Anchors = "(Minimum=(X=0.5,Y=0),...)".
	Policy.coerce_text = [TypeProvider](const rql::TypeDescriptor& Type, const std::string& Text, std::string& Reason)
	{
		return TypeProvider->ParseStructText(Type, Text, Reason);
	};
	// One editor transaction per mutation (one Ctrl+Z step): owners are snapshotted before the first write. Objects the
	// calls create with RF_Transactional (SpawnActor, AddComponent, ...) are recorded into the same transaction.
	Policy.before_write = [this, OwnerOf, ViewStateActor](const std::vector<rql::MutationRecord>& Records)
	{
		// Calls of read-only operations change nothing: no undo step, nothing to snapshot.
		if (AllReadOnlyCalls(Records))
			return;
		// Also when a call ended it (StartPlay / StopPlay close it before PIE starts or ends): later writes of the same
		// query still get an undo step.
		if (GEditor && (TransactionIndex == INDEX_NONE || !GEditor->IsTransactionActive()))
			TransactionIndex = GEditor->BeginTransaction(TEXT("UnrealMcp"), FText::FromString(UndoTitle), nullptr);
		for (const rql::MutationRecord& Record : Records)
		{
			UObject* Owner = OwnerOf(Record);
			if (IsProjectObject(Owner))
				Owner->Modify();
			else if (AActor* Actor = ViewStateActor(Record))
				Actor->Modify(/*bAlwaysMarkDirty*/ false); // undoable, without marking the level unsaved
		}
	};
	// Property writes already ran the editor's PreEditChange / PostEditChangeProperty; mark what changed as unsaved.
	Policy.committed = [this, OwnerOf](const std::vector<rql::MutationRecord>& Records)
	{
		// Calls of read-only operations changed nothing: nothing to mark unsaved, no change for the modules' hooks.
		if (AllReadOnlyCalls(Records))
			return;
		for (const rql::MutationRecord& Record : Records)
		{
			UObject* Owner = OwnerOf(Record);
			if (IsProjectObject(Owner))
				Owner->MarkPackageDirty();
		}
		for (const FMutationHook& Hook : CommittedHooks)
			Hook(Records);
	};
}

bool FUnrealMcpRqlSession::IsReadOnlyCall(const rql::MutationRecord& Record) const
{
	if (!Record.method)
		return false;
	// The chosen method must be the flagged UFUNCTION's own descriptor (marked by AddFunctionMethod), not a hand-written
	// method or overload of the same name on that class.
	static const std::string Mark = UnrealMcpRqlToStd(FUnrealMcpRqlTypes::ReadOnlyMark);
	const UObject* Receiver = TypesPtr->AsObject(Record.item);
	return Receiver && Record.method->description.rfind(Mark, 0) == 0
		&& TypesPtr->IsReadOnlyOperation(Receiver->GetClass(), UnrealMcpRqlFromStd(Record.method->name));
}

bool FUnrealMcpRqlSession::AllReadOnlyCalls(const std::vector<rql::MutationRecord>& Records) const
{
	return !Records.empty() && std::all_of(Records.begin(), Records.end(), [this](const rql::MutationRecord& Record) { return IsReadOnlyCall(Record); });
}

void FUnrealMcpRqlSession::ConfigureModules()
{
	FModuleContext Context(*this);
	TSet<FString> Configured;
	for (IUnrealMcpRqlModule* Module : UnrealMcpRqlTools::Modules())
	{
		if (!UnrealMcpRqlTools::IsModuleEnabled(*Module))
			continue; // its extension is disabled in the Extensions window
		const FString Extension = Module->GetExtensionId();
		// One module per extension id, the first in Modules() order (the extension manager records the later ones as
		// duplicates with an error): a second module would register the same sources and types again.
		if (!Extension.IsEmpty())
		{
			bool bAlreadyConfigured = false;
			Configured.Add(Extension, &bAlreadyConfigured);
			if (bAlreadyConfigured)
				continue;
		}
		FModuleInfo Info;
		Info.Name = Module->GetRqlModuleName();
		Info.ExtensionId = Extension;
		Info.SourceBegin = SourceEntries.Num();
		Info.GuideBegin = Guides.Num();
		Context.ApiClasses = &Info.ApiClasses;
		Module->ConfigureRql(Context);
		Context.ApiClasses = nullptr;
		const FString Version = Module->GetExtensionVersion();
		Info.Version = Version;
		Info.SourceEnd = SourceEntries.Num();
		Info.GuideEnd = Guides.Num();
		if (Info.SourceEnd > Info.SourceBegin || Info.GuideEnd > Info.GuideBegin)
			ModuleInfos.Add(MoveTemp(Info));
		FString Report = FString::Printf(TEXT("`%s`"), *Module->GetRqlModuleName());
		if (!Extension.IsEmpty())
			Report += FString::Printf(TEXT(" - %s %s"), *Extension, *Version);
		ModuleReports.Add(Report);
	}
}

void FUnrealMcpRqlSession::SetUndoTitle(const FString& QueryText)
{
	// One line (whitespace runs as one space), at most 120 characters.
	static constexpr int32 MaxChars = 120;
	TArray<FString> Words;
	QueryText.ParseIntoArrayWS(Words);
	const FString Query = FString::Join(Words, TEXT(" "));
	if (!Query.IsEmpty())
		UndoTitle = TEXT("RQL: ") + (Query.Len() > MaxChars ? Query.Left(MaxChars - 3) + TEXT("...") : Query);
}

void FUnrealMcpRqlSession::CloseTransaction()
{
	if (TransactionIndex != INDEX_NONE && GEditor && GEditor->IsTransactionActive())
		GEditor->EndTransaction();
	TransactionIndex = INDEX_NONE;
}

void FUnrealMcpRqlSession::AddSource(const FString& Name, const rql::TypeDescriptor* Type, bool bList, const FString& Description,
	std::function<rql::Value()> Getter)
{
	EnginePtr->add_source(UnrealMcpRqlToStd(Name), std::move(Getter), UnrealMcpRqlToStd(Description));
	SourceEntries.Add({ Name, Type, bList, Description });
}

void FUnrealMcpRqlSession::AddFunction(const FString& Name, const FString& Description, rql::FunctionHandler Handler)
{
	EnginePtr->add_function(UnrealMcpRqlToStd(Name), std::move(Handler));
	FunctionEntries.Add({ Name, nullptr, false, Description });
}

void FUnrealMcpRqlSession::AddGuide(const FString& Topic, const FString& Markdown)
{
	Guides.Add(TPair<FString, FString>(Topic, Markdown));
}

void FUnrealMcpRqlSession::AddBatchValidator(FBatchValidator Validator)
{
	BatchValidators.Add(MoveTemp(Validator));
}

void FUnrealMcpRqlSession::AddCommitted(FMutationHook Hook)
{
	CommittedHooks.Add(MoveTemp(Hook));
}

void FUnrealMcpRqlSession::AllowTemplateWrites(TFunction<bool(const UObject*)> Predicate)
{
	WritableTemplates.Add(MoveTemp(Predicate));
}

void FUnrealMcpRqlSession::AddAdapter(rql::TypeAdapter Adapter)
{
	TypesPtr->Fill(Adapter.target); // adapted views are cached per type: the descriptor must be complete first
	EnginePtr->add_adapter(std::move(Adapter));
}

FString FUnrealMcpRqlSession::Execute(const FString& PlanJson, bool bAllowMutation, bool bDryRun, bool& bOutOk, TOptional<int32> MaxTargets)
{
	Types().SetMutationRequest(bAllowMutation);
	bReadOnlyCallRequest = false;
	bRefusedReadOnlyCall = false;
	bReadOnlyLimit = false;
	bReadOnlyCalls = false;
	const std::string Text = UnrealMcpRqlToStd(PlanJson);
	rql::QueryPlan Plan;
	rql::Diagnostic Error;
	rql::QueryResult Result;
	if (!rql::try_decode_plan(std::string_view(Text), Plan, Error))
	{
		Result = rql::QueryResult::fail({ Error });
	}
	else if (!FunctionSource(Plan, Result))
	{
		const bool bCall = EndsWithCall(Plan);
		// rql-query may call read-only operations (meta = (RqlReadOnly)): the engine allows the call stage, and the batch
		// check refuses every planned call that is not one (a same-named operation of another type) before any runs. Any
		// other call keeps the engine's own permission-denied, before its target is evaluated.
		const bool bReadOnlyName = bCall && Types().HasReadOnlyOperation(UnrealMcpRqlFromStd(Plan.stages.back().method));
		bReadOnlyCallRequest = !bAllowMutation && bReadOnlyName;
		rql::QueryOptions Options;
		Options.allow_mutation = bAllowMutation || bReadOnlyCallRequest;
		Options.dry_run = bDryRun;
		rql::Budgets Budgets;
		// A read-only call changes nothing, so the editor's mutation limit does not apply: it takes the ceiling in both tools
		// (rql-query has no max_targets). In rql-mutate a same-named operation that is not read-only is held to the
		// default limit by the batch check (bReadOnlyLimit).
		bReadOnlyLimit = bReadOnlyName && !MaxTargets.IsSet();
		if (MaxTargets.IsSet())
			Budgets.max_mutation_targets = MaxTargets.GetValue(); // outside 1 .. hard_max_targets the engine refuses the request
		else if (bReadOnlyLimit)
			Budgets.max_mutation_targets = HardMaxTargets;
		if (bCall)
			Budgets.max_list_items = MaxCallResultItems;
		Result = EnginePtr->execute(Plan, Budgets, Options);
		if (bRefusedReadOnlyCall)
			Result = rql::QueryResult::fail({ rql::Diagnostic::error("permission-denied", "'call' requires a mutation request: run it with " + EnginePtr->mutate_hint) });
		else if (bCall)
			AddCallResultListNote(Result);
		if (bReadOnlyLimit)
			UnrealMcpRqlSessionLocal::RewordReadLimit(Result, Plan.stages.back().method, HardMaxTargets);
	}
	CloseTransaction();
	LiteralObjects.Reset(); // written by now (the undo buffer or their owner holds them), or garbage
	UnrealMcpRqlSessionLocal::AddExtensionHint(Result, PlanJson, Plan, bAllowMutation);
	UnrealMcpRqlSessionLocal::AddOpenLevelHint(Result, Plan, bAllowMutation);
	UnrealMcpRqlSessionLocal::AddUnloadedActorsNote(Result, Plan);
	for (rql::Diagnostic& Warning : Types().TakeWarnings())
		Result.diagnostics.push_back(std::move(Warning));
	if (bReadOnlyCalls && !Plan.stages.empty())
		UnrealMcpRqlSessionLocal::RewordReadTruncation(Result, Plan.stages.back().method);
	bOutOk = Result.ok;
	rql::Json Answer = Result.to_json(/*include_stats*/ false);
	// The sidecar renders an rql-mutate answer as a receipt (its first 50 rows and a count): a read's rows are its answer
	// (a grid of sampled heights), so "readOnly" asks for every row, as rql-query shows them (within the 64 KB answer).
	if (Result.ok && bReadOnlyCalls)
		Answer.set("readOnly", true);
	return UnrealMcpRqlFromStd(Answer.dump());
}

bool FUnrealMcpRqlSession::FunctionSource(rql::QueryPlan& Plan, rql::QueryResult& OutFailure)
{
	// RQL text lets a host function start a query, as in unreal.assets("Material") | select Name, optionally followed by
	// a member path (unreal.asset("/Game/BP").EventGraph). The C++ runtime (0.2.0) runs a bare function call itself but
	// not the member-path form, so the session handles both the same way: evaluate the call once (its arguments are
	// literals) and run the query on a source named after the function holding the value, navigating the member path.
	rql::ExprPtr Source = Plan.source;
	std::vector<rql::Segment> Members;
	if (Source && Source->kind == rql::ExprKind::Path && Source->on && Source->on->kind == rql::ExprKind::Call)
	{
		Members = Source->segments;
		Source = Source->on;
	}
	if (!Source || Source->kind != rql::ExprKind::Call || !Source->on || Source->on->kind != rql::ExprKind::Path || Source->on->on)
		return false;
	std::string Name;
	for (const rql::Segment& Segment : Source->on->segments)
	{
		if (Segment.kind != rql::SegmentKind::Member)
			return false;
		Name += Segment.name + ".";
	}
	Name += Source->name;
	const auto Function = EnginePtr->functions().find(Name);
	if (Function == EnginePtr->functions().end())
		return false;

	std::function<bool(const rql::Expr&, rql::Value&)> Literal = [&Literal](const rql::Expr& Expr, rql::Value& Out) -> bool
	{
		switch (Expr.kind)
		{
		case rql::ExprKind::Lit:
			switch (Expr.lit.type)
			{
			case rql::Literal::Type::Null: Out = rql::Value::null(); return true;
			case rql::Literal::Type::Bool: Out = rql::Value::boolean(Expr.lit.b); return true;
			case rql::Literal::Type::Number: Out = rql::Value::number(Expr.lit.n); return true;
			case rql::Literal::Type::String: Out = rql::Value::string(Expr.lit.s); return true;
			}
			return false;
		case rql::ExprKind::List:
		{
			std::vector<rql::Value> Items;
			for (const rql::ExprPtr& Item : Expr.args)
			{
				rql::Value Value;
				if (!Item || !Literal(*Item, Value))
					return false;
				Items.push_back(Value);
			}
			Out = rql::Value::list_of(std::move(Items));
			return true;
		}
		default:
			return false;
		}
	};
	std::vector<rql::Value> Args;
	for (const rql::ExprPtr& Arg : Source->args)
	{
		rql::Value Value;
		if (!Arg || !Literal(*Arg, Value))
		{
			OutFailure = rql::QueryResult::fail({ rql::Diagnostic::error("invalid-plan",
				Name + "(...) starts a query: its arguments must be literals (\"text\", numbers, lists)") });
			return true;
		}
		Args.push_back(Value);
	}
	rql::Value Value;
	try
	{
		Value = Function->second(Args);
	}
	catch (const std::exception& Error)
	{
		OutFailure = rql::QueryResult::fail({ rql::Diagnostic::error("function-threw", Name + "() threw: " + Error.what()) });
		return true;
	}
	catch (...)
	{
		OutFailure = rql::QueryResult::fail({ rql::Diagnostic::error("function-threw", Name + "() threw: non-standard exception") });
		return true;
	}
	if (Value.is_error())
	{
		OutFailure = rql::QueryResult::fail({ rql::Diagnostic::error(Value.error_code(), Value.error_detail()) });
		return true;
	}
	EnginePtr->add_source(Name, [Value]() { return Value; });
	if (Members.empty())
	{
		Plan.source = rql::plan::path(Name);
		return false;
	}
	std::vector<rql::Segment> Path;
	rql::Segment Head;
	Head.kind = rql::SegmentKind::Member;
	Head.name = Name;
	Path.push_back(Head);
	Path.insert(Path.end(), Members.begin(), Members.end());
	Plan.source = rql::plan::path(std::move(Path));
	return false;
}

TArray<FString> FUnrealMcpRqlSession::ModuleNames() const
{
	TArray<FString> Names;
	for (const FModuleInfo& Info : ModuleInfos)
		Names.Add(Info.Name);
	return Names;
}

rql::Json FUnrealMcpRqlSession::ModuleJson(const FModuleInfo& Info)
{
	rql::Json Item = rql::Json::object();
	Item.set("name", UnrealMcpRqlToStd(Info.Name));
	if (!Info.ExtensionId.IsEmpty())
		Item.set("extension", UnrealMcpRqlToStd(Info.ExtensionId));
	if (!Info.Version.IsEmpty())
		Item.set("version", UnrealMcpRqlToStd(Info.Version));
	return Item;
}

FString FUnrealMcpRqlSession::GuideKey(const FString& Name)
{
	FString Key;
	Key.Reserve(Name.Len());
	for (const TCHAR Char : Name)
	{
		if (Char != TEXT('-') && Char != TEXT('_') && !FChar::IsWhitespace(Char))
			Key.AppendChar(FChar::ToLower(Char));
	}
	return Key;
}

const FUnrealMcpRqlSession::FModuleInfo* FUnrealMcpRqlSession::FindModuleInfo(const FString& Name) const
{
	// Case, hyphens, underscores and spaces do not matter: "mesh-terrain", "Mesh Terrain" and "meshTerrain" name one module.
	const FString Key = GuideKey(Name);
	if (Key.IsEmpty())
		return nullptr;
	for (const FModuleInfo& Info : ModuleInfos)
	{
		// The module name, its extension id (com.ivanmurzak.unreal-ai-mesh-terrain), the id's last segment
		// (unreal-ai-mesh-terrain) and that segment without the first-party "unreal-ai-" prefix (mesh-terrain).
		FString LastSegment;
		if (!Info.ExtensionId.Split(TEXT("."), nullptr, &LastSegment, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
			LastSegment = Info.ExtensionId;
		FString Short = GuideKey(LastSegment);
		Short.RemoveFromStart(TEXT("unrealai"));
		for (const FString& Candidate : { GuideKey(Info.Name), GuideKey(Info.ExtensionId), GuideKey(LastSegment), Short })
		{
			if (!Candidate.IsEmpty() && Candidate == Key)
				return &Info;
		}
	}
	for (const FModuleInfo& Info : ModuleInfos)
	{
		for (int32 Index = Info.SourceBegin; Index < Info.SourceEnd; ++Index)
		{
			// A source by name, or an operations source by its type (MeshTerrainOperations); a source typed by a shared
			// engine class (Light, StaticMesh) does not make that class this module's.
			const FEntry& Source = SourceEntries[Index];
			if (GuideKey(Source.Name) == Key || (IsApiType(Info, Source.Type) && GuideKey(UnrealMcpRqlFromStd(Source.Type->name)) == Key))
				return &Info;
		}
	}
	return nullptr;
}

const FUnrealMcpRqlSession::FModuleInfo* FUnrealMcpRqlSession::ModuleOfType(const rql::TypeDescriptor* Type) const
{
	for (const FModuleInfo& Info : ModuleInfos)
	{
		if (Info.GuideEnd > Info.GuideBegin && IsApiType(Info, Type))
			return &Info;
	}
	return nullptr;
}

bool FUnrealMcpRqlSession::IsApiType(const FModuleInfo& Info, const rql::TypeDescriptor* Type) const
{
	return Type && Info.ApiClasses.Contains(TypesPtr->ClassOf(Type));
}

FString FUnrealMcpRqlSession::GuideSchema(const FString& Key, bool& bOutOk) const
{
	rql::Json Root = rql::Json::object();
	const FModuleInfo* Info = FindModuleInfo(Key);
	if (!Info)
	{
		bOutOk = false;
		Root.set("error", "unknown-guide");
		Root.set("name", UnrealMcpRqlToStd(Key));
		rql::Json Candidates = rql::Json::array();
		for (const FModuleInfo& Each : ModuleInfos)
			Candidates.push(rql::Json(UnrealMcpRqlToStd(Each.Name)));
		Root.set("candidates", std::move(Candidates));
		return UnrealMcpRqlFromStd(Root.dump());
	}
	bOutOk = true;
	rql::Json Guide = ModuleJson(*Info);
	rql::Json SourceList = rql::Json::array();
	for (int32 Index = Info->SourceBegin; Index < Info->SourceEnd; ++Index)
		SourceList.push(SourceJson(SourceEntries[Index]));
	Guide.set("sources", std::move(SourceList));
	rql::Json Sections = rql::Json::array();
	for (int32 Index = Info->GuideBegin; Index < Info->GuideEnd; ++Index)
	{
		rql::Json Item = rql::Json::object();
		Item.set("topic", UnrealMcpRqlToStd(Guides[Index].Key));
		Item.set("markdown", UnrealMcpRqlToStd(Guides[Index].Value.TrimStartAndEnd()));
		Sections.push(std::move(Item));
	}
	Guide.set("sections", std::move(Sections));
	Root.set("guide", std::move(Guide));
	return UnrealMcpRqlFromStd(Root.dump());
}

FString FUnrealMcpRqlSession::Schema(const FString& TypeName, const FString& Guide, bool& bOutOk)
{
	const FString WantedGuide = Guide.TrimStartAndEnd();
	if (!WantedGuide.IsEmpty())
		return GuideSchema(WantedGuide, bOutOk);

	rql::Json Root = rql::Json::object();
	bOutOk = true;

	// The classes present in the editor world (with counts), like Unity's component types of the loaded scenes: only for
	// the overview and the unknown-type candidates. Most frequent first, then by name (the chain fixture records it).
	TMap<FString, int32> ActorClasses;
	TMap<FString, int32> ComponentClasses;
	auto CountPresentClasses = [&ActorClasses, &ComponentClasses]()
	{
		if (UWorld* World = FUnrealMcpObjectRef::GetEditorWorld())
		{
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (!IsValid(Actor))
					continue;
				ActorClasses.FindOrAdd(Actor->GetClass()->GetName())++;
				Actor->ForEachComponent(/*bIncludeFromChildActors*/ false, [&ComponentClasses](UActorComponent* Component)
				{
					ComponentClasses.FindOrAdd(Component->GetClass()->GetName())++;
				});
			}
		}
		for (TMap<FString, int32>* Counts : { &ActorClasses, &ComponentClasses })
		{
			Counts->KeySort([](const FString& A, const FString& B) { return A < B; });
			Counts->ValueStableSort([](int32 A, int32 B) { return A > B; });
		}
	};

	const FString Wanted = TypeName.TrimStartAndEnd();
	if (!Wanted.IsEmpty())
	{
		const rql::TypeDescriptor* Found = TypesPtr->FindByName(Wanted);
		if (!Found)
		{
			// A source name ("console") stands for the type of its items.
			if (const FEntry* Source = SourceEntries.FindByPredicate([&Wanted](const FEntry& Entry) { return Entry.Name.Equals(Wanted, ESearchCase::IgnoreCase); }))
				Found = Source->Type;
		}
		if (!Found || Found->kind == rql::TypeKind::Scalar)
		{
			CountPresentClasses();
			bOutOk = false;
			Root.set("error", "unknown-type");
			Root.set("name", UnrealMcpRqlToStd(Wanted));
			rql::Json Candidates = rql::Json::array();
			for (const TMap<FString, int32>* Counts : { &ActorClasses, &ComponentClasses })
			{
				for (const TPair<FString, int32>& Pair : *Counts)
					Candidates.push(rql::Json(UnrealMcpRqlToStd(Pair.Key)));
			}
			Root.set("candidates", std::move(Candidates));
			const TArray<FUnrealMcpMissingExtension> Missing = UnrealMcpAvailableExtensions::Missing();
			if (const FUnrealMcpMissingExtension* Match = UnrealMcpAvailableExtensions::MatchDomain(Wanted, Missing))
				Root.set("hint", UnrealMcpRqlToStd(UnrealMcpAvailableExtensions::Hint(*Match))); // a type of an extension's domain
			return UnrealMcpRqlFromStd(Root.dump());
		}
		rql::Json TypeList = rql::Json::array();
		TypeList.push(Describe(Found));
		Root.set("types", std::move(TypeList));
		// An extension's source type: its guide says how the operations work together.
		if (const FModuleInfo* Owner = ModuleOfType(Found))
			Root.set("guideCall", UnrealMcpRqlToStd(UnrealMcpRqlGuide::GuideCall(Owner->Name)));
		return UnrealMcpRqlFromStd(Root.dump());
	}

	rql::Json SourceList = rql::Json::array();
	for (const FEntry& Entry : SourceEntries)
		SourceList.push(SourceJson(Entry));
	Root.set("sources", std::move(SourceList));

	rql::Json FunctionList = rql::Json::array();
	for (const FEntry& Entry : FunctionEntries)
	{
		rql::Json Item = rql::Json::object();
		Item.set("name", UnrealMcpRqlToStd(Entry.Name));
		Item.set("description", UnrealMcpRqlToStd(Entry.Description));
		FunctionList.push(std::move(Item));
	}
	Root.set("functions", std::move(FunctionList));

	// The installed extension modules: their sources and the call that returns their guide (AddGuide).
	if (!ModuleInfos.IsEmpty())
	{
		rql::Json ModuleList = rql::Json::array();
		for (const FModuleInfo& Info : ModuleInfos)
		{
			rql::Json Item = ModuleJson(Info);
			rql::Json Names = rql::Json::array();
			for (int32 Index = Info.SourceBegin; Index < Info.SourceEnd; ++Index)
				Names.push(rql::Json(UnrealMcpRqlToStd(SourceEntries[Index].Name)));
			Item.set("sources", std::move(Names));
			if (Info.GuideEnd > Info.GuideBegin)
				Item.set("guideCall", UnrealMcpRqlToStd(UnrealMcpRqlGuide::GuideCall(Info.Name)));
			ModuleList.push(std::move(Item));
		}
		Root.set("modules", std::move(ModuleList));
	}

	// The extensions agents could add (each brings sources and operations), with how to get them.
	const TArray<FUnrealMcpMissingExtension> Missing = UnrealMcpAvailableExtensions::Missing();
	if (!Missing.IsEmpty())
	{
		rql::Json ExtensionList = rql::Json::array();
		for (const FUnrealMcpMissingExtension& Extension : Missing)
		{
			rql::Json Item = rql::Json::object();
			Item.set("id", UnrealMcpRqlToStd(Extension.Extension.ExtensionId));
			Item.set("line", UnrealMcpRqlToStd(UnrealMcpAvailableExtensions::Line(Extension)));
			ExtensionList.push(std::move(Item));
		}
		Root.set("extensions", std::move(ExtensionList));
		Root.set("extensionsNote", UnrealMcpRqlToStd(UnrealMcpAvailableExtensions::InstallNote()));
	}

	CountPresentClasses();
	auto Present = [](const TMap<FString, int32>& Counts)
	{
		rql::Json List = rql::Json::array();
		int32 Shown = 0;
		for (const TPair<FString, int32>& Pair : Counts)
		{
			if (++Shown > 60)
				break;
			rql::Json Item = rql::Json::object();
			Item.set("type", UnrealMcpRqlToStd(Pair.Key));
			Item.set("count", rql::Json(static_cast<int>(Pair.Value)));
			List.push(std::move(Item));
		}
		return List;
	};
	Root.set("actorClasses", Present(ActorClasses));
	Root.set("componentClasses", Present(ComponentClasses));

	// Actor and SceneComponent (as Unity-MCP shows GameObject and Transform): the member names agents meet first.
	rql::Json TypeList = rql::Json::array();
	TypeList.push(Describe(TypesPtr->ForClass(AActor::StaticClass())));
	TypeList.push(Describe(TypesPtr->ForClass(USceneComponent::StaticClass())));
	Root.set("types", std::move(TypeList));
	return UnrealMcpRqlFromStd(Root.dump());
}

rql::Json FUnrealMcpRqlSession::Describe(const rql::TypeDescriptor* Type)
{
	rql::Json Entry = rql::Json::object();
	if (!Type)
		return Entry;
	TypesPtr->Fill(Type);
	Entry.set("type", EnginePtr->describe_type(*Type));
	// The base: the sidecar lists the type's own members and summarises the inherited ones ("+ inherited from <base>").
	if (Type->base)
	{
		TypesPtr->Fill(Type->base);
		Entry.set("shared", EnginePtr->describe_type(*Type->base));
	}
	return Entry;
}

FString FUnrealMcpRqlSession::SkillMarkdown() const
{
	FString Out;
	Out += TEXT("# RQL - query and change the Unreal Editor\n\n");
	Out += TEXT("RQL is one pipeline language for the whole editor: find actors and assets, read members, filter, aggregate, and change "
		"them in one call. Prefer one precise query over many exploratory calls.\n\n");
	Out += TEXT("- `rql-query` - read-only queries, compact table results; `call` of read-only operations (rql-schema marks them [read-only]).\n");
	Out += TEXT("- `rql-mutate` - queries ending with `set` / `call`; all-or-nothing, one undo step (Ctrl+Z), `dry_run: true` previews. "
		"Several queries separated by `;` run in order, each its own change and undo step; the first that fails stops the rest. "
		"Changes are not saved to disk until `editor | call SaveAll()` (or SaveLevel / SaveAsset).\n");
	Out += TEXT("- `rql-schema` - sources and the actor / component classes in the editor world; `rql-schema(type)` lists one type's members and methods.\n\n");
	Out += TEXT("```bash\n");
	Out += TEXT("unreal-mcp-cli run-tool rql-query --input '{\"query\": \"actors | where Label contains \\\"Enemy\\\" | select $ref Label\"}'\n");
	Out += TEXT("unreal-mcp-cli run-tool rql-mutate --input '{\"query\": \"actors | where Label == \\\"Player\\\" | set HiddenInEditor = true\"}'\n");
	Out += TEXT("```\n\n## Language\n\n```\n");
	Out += UnrealMcpRqlFromStd(UnrealMcpRqlGuide::Language());
	Out += TEXT("\n");
	Out += UnrealMcpRqlFromStd(UnrealMcpRqlGuide::LanguageRules());
	Out += UnrealMcpRqlFromStd(UnrealMcpRqlGuide::QueryExamples());
	Out += TEXT("\n```\n\n## Changing things (rql-mutate)\n\n```\n");
	Out += UnrealMcpRqlFromStd(UnrealMcpRqlGuide::MutateStages());
	Out += UnrealMcpRqlFromStd(UnrealMcpRqlGuide::MutateExamples());
	Out += TEXT("```\n\n## Sources\n\n| Source | What |\n|---|---|\n");
	TArray<FEntry> SortedSources = SourceEntries;
	SortedSources.Sort([](const FEntry& A, const FEntry& B) { return A.Name < B.Name; });
	for (const FEntry& Entry : SortedSources)
		Out += FString::Printf(TEXT("| `%s` | %s |\n"), *Entry.Name, *TableCell(Entry.Description));
	TArray<FEntry> SortedFunctions = FunctionEntries;
	SortedFunctions.Sort([](const FEntry& A, const FEntry& B) { return A.Name < B.Name; });
	for (const FEntry& Entry : SortedFunctions)
		Out += FString::Printf(TEXT("| `%s(...)` | %s |\n"), *Entry.Name, *TableCell(Entry.Description));
	for (const TPair<FString, FString>& Guide : Guides)
		Out += FString::Printf(TEXT("\n## %s\n\n%s\n"), *Guide.Key, *Guide.Value.TrimStartAndEnd());
	Out += TEXT("\n## Installed RQL modules\n\n- `Core`\n");
	for (const FString& Module : ModuleReports)
		Out += FString::Printf(TEXT("- %s\n"), *Module);
	const TArray<FUnrealMcpMissingExtension> Missing = UnrealMcpAvailableExtensions::Missing();
	if (!Missing.IsEmpty())
	{
		Out += TEXT("\n## Extensions you can add\n\n") + UnrealMcpAvailableExtensions::InstallNote() + TEXT("\n\n");
		for (const FUnrealMcpMissingExtension& Extension : Missing)
			Out += FString::Printf(TEXT("- %s\n"), *UnrealMcpAvailableExtensions::Line(Extension));
	}
	return Out;
}
