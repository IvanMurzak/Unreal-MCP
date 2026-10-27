// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRqlSession.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealMcpRqlGuide.h"
#include "UnrealMcpRqlSources.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "UnrealMcpRqlTools.h"

#include "Dom/JsonObject.h"
#include "GameFramework/Actor.h"
#include "Tools/UnrealMcpObjectRef.h"
#include "UObject/Package.h"

#include <stdexcept>
#include <string_view>

namespace UnrealMcpRqlSessionLocal
{
	/** Objects whose changes belong to the project (not class defaults of compiled-in classes, not transient helpers). */
	bool IsProjectObject(const UObject* Object)
	{
		if (!Object)
			return false;
		const UPackage* Package = Object->GetOutermost();
		return Package && Package != GetTransientPackage() && !Package->HasAnyPackageFlags(PKG_CompiledIn);
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
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
					Entries.emplace_back(rql::Value::string(UnrealMcpRqlToStd(Pair.Key)), FromJson(Pair.Value));
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

	FString Cell(const FString& Text)
	{
		return Text.Replace(TEXT("|"), TEXT("\\|")).Replace(TEXT("\n"), TEXT(" "));
	}

	/** The UE-typed registration surface handed to IUnrealMcpRqlModule::ConfigureRql. */
	class FModuleContext final : public FUnrealMcpRqlModuleContext
	{
	public:
		explicit FModuleContext(FUnrealMcpRqlSession& InSession) : Session(InSession) {}

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
	// rql-query / rql-mutate take no budgets argument: hints must not advise raising budgets.*. ref("Name") hints name the finder.
	RqlEngine.defaults().budgets_adjustable = false;
	RqlEngine.find_hint = "unreal.find(\"\xE2\x80\xA6\")";
	FUnrealMcpRqlTypes* TypeProvider = TypesPtr.Get();
	// Liveness from the weak pointer every object value carries: a destroyed (garbage) actor reads as null.
	RqlEngine.is_alive = [TypeProvider](const rql::Value& Object) { return TypeProvider->IsAlive(Object); };
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
	// One editor transaction per mutation (one Ctrl+Z step): owners are snapshotted before the first write. Objects the
	// calls create with RF_Transactional (SpawnActor, AddComponent, ...) are recorded into the same transaction.
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
		// all-or-nothing promise. Refuse the duplicate before anything runs.
		TSet<TPair<const void*, const rql::MethodDescriptor*>> Called;
		for (const rql::MutationRecord& Record : Records)
		{
			if (!Record.method || !Record.item.is_object() || !Record.item.ptr())
				continue;
			bool bTwice = false;
			Called.Add(TPair<const void*, const rql::MethodDescriptor*>(Record.item.ptr(), Record.method), &bTwice);
			if (bTwice)
			{
				const UObject* Object = TypeProvider->AsObject(Record.item);
				const AActor* Actor = Cast<AActor>(Object);
				const FString Name = Actor ? Actor->GetActorLabel() : Object ? Object->GetName() : FString(TEXT("one object"));
				return UnrealMcpRqlToStd(FString::Printf(TEXT("%s would run twice on %s (it is in the stream more than once: "
					"add | distinct before call)"), *UnrealMcpRqlFromStd(Record.method->name), *Name));
			}
		}
		for (const FBatchValidator& Validator : BatchValidators)
		{
			const FString Reason = Validator(Records);
			if (!Reason.IsEmpty())
				return UnrealMcpRqlToStd(Reason);
		}
		return std::string();
	};
	// UE's own text form for structs without an RQL adapter (the editor's Copy): Anchors = "(Minimum=(X=0.5,Y=0),...)".
	Policy.coerce_text = [TypeProvider](const rql::TypeDescriptor& Type, const std::string& Text, std::string& Reason)
	{
		return TypeProvider->ParseStructText(Type, Text, Reason);
	};
	Policy.before_write = [this, OwnerOf, ViewStateActor](const std::vector<rql::MutationRecord>& Records)
	{
		if (TransactionIndex == INDEX_NONE && GEditor)
			TransactionIndex = GEditor->BeginTransaction(TEXT("UnrealMcp"), FText::FromString(TEXT("RQL mutation")), nullptr);
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
		Module->ConfigureRql(Context);
		const FString Version = Module->GetExtensionVersion();
		FString Report = FString::Printf(TEXT("`%s`"), *Module->GetRqlModuleName());
		if (!Extension.IsEmpty())
			Report += FString::Printf(TEXT(" - %s %s"), *Extension, *Version);
		ModuleReports.Add(Report);
	}
}

void FUnrealMcpRqlSession::CloseTransaction()
{
	if (TransactionIndex != INDEX_NONE && GEditor)
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

FString FUnrealMcpRqlSession::Execute(const FString& PlanJson, bool bAllowMutation, bool bDryRun, bool& bOutOk)
{
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
		rql::QueryOptions Options;
		Options.allow_mutation = bAllowMutation;
		Options.dry_run = bDryRun;
		Result = EnginePtr->execute(Plan, rql::Budgets(), Options);
	}
	CloseTransaction();
	bOutOk = Result.ok;
	return UnrealMcpRqlFromStd(Result.to_json(/*include_stats*/ false).dump());
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

FString FUnrealMcpRqlSession::Schema(const FString& TypeName, bool& bOutOk)
{
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
			return UnrealMcpRqlFromStd(Root.dump());
		}
		rql::Json TypeList = rql::Json::array();
		TypeList.push(Describe(Found));
		Root.set("types", std::move(TypeList));
		return UnrealMcpRqlFromStd(Root.dump());
	}

	rql::Json SourceList = rql::Json::array();
	for (const FEntry& Entry : SourceEntries)
	{
		rql::Json Item = rql::Json::object();
		Item.set("name", UnrealMcpRqlToStd(Entry.Name));
		Item.set("type", UnrealMcpRqlToStd(TypeLabel(Entry)));
		if (!Entry.Description.IsEmpty())
			Item.set("description", UnrealMcpRqlToStd(Entry.Description));
		SourceList.push(std::move(Item));
	}
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
	Out += TEXT("- `rql-query` - read-only queries, compact table results.\n");
	Out += TEXT("- `rql-mutate` - queries ending with `set` / `call`; all-or-nothing, one undo step (Ctrl+Z), `dry_run: true` previews. "
		"Changes are not saved to disk until `editor | call SaveAll()` (or SaveLevel / SaveAsset).\n");
	Out += TEXT("- `rql-schema` - sources and the actor / component classes in the editor world; `rql-schema(type)` lists one type's members and methods.\n\n");
	Out += TEXT("```bash\n");
	Out += TEXT("unreal-mcp-cli run-tool rql-query --input '{\"query\": \"actors | where Label contains \\\"Enemy\\\" | select $ref Label\"}'\n");
	Out += TEXT("unreal-mcp-cli run-tool rql-mutate --input '{\"query\": \"actors | where Label == \\\"Player\\\" | set HiddenInEditor = true\"}'\n");
	Out += TEXT("```\n\n## Language\n\n```\n");
	Out += UnrealMcpRqlFromStd(UnrealMcpRqlGuide::Language());
	Out += TEXT("\n");
	Out += UnrealMcpRqlFromStd(UnrealMcpRqlGuide::QueryExamples());
	Out += TEXT("\n```\n\n## Changing things (rql-mutate)\n\n```\n");
	Out += UnrealMcpRqlFromStd(UnrealMcpRqlGuide::MutateStages());
	Out += UnrealMcpRqlFromStd(UnrealMcpRqlGuide::MutateExamples());
	Out += TEXT("```\n\n## Sources\n\n| Source | What |\n|---|---|\n");
	TArray<FEntry> SortedSources = SourceEntries;
	SortedSources.Sort([](const FEntry& A, const FEntry& B) { return A.Name < B.Name; });
	for (const FEntry& Entry : SortedSources)
		Out += FString::Printf(TEXT("| `%s` | %s |\n"), *Entry.Name, *Cell(Entry.Description));
	TArray<FEntry> SortedFunctions = FunctionEntries;
	SortedFunctions.Sort([](const FEntry& A, const FEntry& B) { return A.Name < B.Name; });
	for (const FEntry& Entry : SortedFunctions)
		Out += FString::Printf(TEXT("| `%s(...)` | %s |\n"), *Entry.Name, *Cell(Entry.Description));
	for (const TPair<FString, FString>& Guide : Guides)
		Out += FString::Printf(TEXT("\n## %s\n\n%s\n"), *Guide.Key, *Guide.Value.TrimStartAndEnd());
	Out += TEXT("\n## Installed RQL modules\n\n- `Core`\n");
	for (const FString& Module : ModuleReports)
		Out += FString::Printf(TEXT("- %s\n"), *Module);
	return Out;
}
