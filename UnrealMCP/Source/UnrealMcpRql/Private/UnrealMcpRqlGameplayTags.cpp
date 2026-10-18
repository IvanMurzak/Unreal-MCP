// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRqlSession.h"
#include "UnrealMcpRqlEditorApi.h"
#include "UnrealMcpRqlSources.h"
#include "UnrealMcpRqlUtil.h"
#include "IUnrealMcpRqlModule.h"

#include "GameplayTagContainer.h"
#include "GameplayTagsEditorModule.h"
#include "GameplayTagsManager.h"
#include "Misc/OutputDevice.h"
#include "Modules/ModuleManager.h"

#include <exception>
#include <string>
#include <vector>

/**
 * Gameplay Tags (docs/rql.md "Gameplay Tags"). FGameplayTag is a struct around one FName and FGameplayTagContainer keeps
 * a second, derived array of parent tags, so writing their reflected members would store tags the tag tree does not
 * know and containers whose parent cache is stale. Instead tags are text: a tag reads as "Ability.Fire", a container as
 * ["Ability.Fire", "State.Stunned"], and writes take the same forms, validated against the registered tags (an unknown
 * tag is an error that names similar ones). Containers are rebuilt with AddTag, so their parent tags are right.
 */
namespace UnrealMcpRqlGameplayTagsLocal
{
	using namespace UnrealMcpRqlUtil;

	struct FTagInfo
	{
		std::string Name;
		std::string Comment;
		std::string Source;
		std::string Parent;
		int32_t Children = 0;
		bool Explicit = false;
		bool Restricted = false;
	};

	/** Up to five registered tags that look like @p Wanted (share its last part or contain it), for an error message. */
	FString Similar(const FString& Wanted)
	{
		FGameplayTagContainer All;
		UGameplayTagsManager::Get().RequestAllGameplayTags(All, /*OnlyIncludeDictionaryTags*/ false);
		FString Leaf = Wanted;
		Wanted.Split(TEXT("."), nullptr, &Leaf, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		TArray<FString> Found;
		for (const FGameplayTag& Tag : All.GetGameplayTagArray())
		{
			const FString Name = Tag.ToString();
			if (Name.Contains(Wanted) || (!Leaf.IsEmpty() && Name.Contains(Leaf)) || Wanted.Contains(Name))
				Found.Add(Name);
		}
		Found.Sort([](const FString& A, const FString& B) { return A.Len() != B.Len() ? A.Len() < B.Len() : A < B; });
		if (Found.Num() > 5)
			Found.SetNum(5);
		return Found.Num() > 0 ? FString::Printf(TEXT(" (similar: %s)"), *FString::Join(Found, TEXT(", "))) : FString();
	}

	/** The registered tag @p Text names; empty text and "None" are the empty tag. Throws for an unknown tag. */
	FGameplayTag ParseTag(const FString& Text)
	{
		const FString Trimmed = Text.TrimStartAndEnd();
		if (Trimmed.IsEmpty() || Trimmed.Equals(TEXT("None"), ESearchCase::IgnoreCase))
			return FGameplayTag();
		const FGameplayTag Tag = UGameplayTagsManager::Get().RequestGameplayTag(FName(*Trimmed), /*ErrorIfNotFound*/ false);
		if (!Tag.IsValid())
			Throw(FString::Printf(TEXT("'%s' is not a registered gameplay tag%s; add it with editor | call AddGameplayTag(\"%s\")"),
				*Trimmed, *Similar(Trimmed), *Trimmed));
		return Tag;
	}

	TArray<FString> TagTexts(const rql::Value& Value)
	{
		TArray<FString> Out;
		if (Value.is_string())
		{
			// "A.B, C.D" as well as ["A.B", "C.D"]
			UnrealMcpRqlFromStd(Value.as_string()).ParseIntoArray(Out, TEXT(","), /*InCullEmpty*/ true);
			return Out;
		}
		for (const rql::Value& Item : Items(Value))
		{
			if (!Item.is_string())
				ThrowMismatch(TEXT("a tag name"), Item);
			Out.Add(UnrealMcpRqlFromStd(Item.as_string()));
		}
		return Out;
	}

	FGameplayTagContainer ParseContainer(const rql::Value& Value)
	{
		FGameplayTagContainer Out;
		for (const FString& Text : TagTexts(Value))
		{
			const FGameplayTag Tag = ParseTag(Text);
			if (Tag.IsValid())
				Out.AddTag(Tag); // keeps the parent-tag cache right
		}
		return Out;
	}

	TArray<FString> Names(const FGameplayTagContainer& Container)
	{
		TArray<FString> Out;
		for (const FGameplayTag& Tag : Container.GetGameplayTagArray())
			Out.Add(Tag.ToString());
		return Out;
	}

	/** An RQL value as a set of tag names (a container struct, a list or comma-separated text); false for other values. */
	bool AsNames(const rql::Value& Value, const rql::TypeDescriptor* ContainerType, TSet<FName>& Out)
	{
		if (Value.is_struct() && Value.type() == ContainerType && Value.ptr())
		{
			for (const FGameplayTag& Tag : static_cast<const FGameplayTagContainer*>(Value.ptr())->GetGameplayTagArray())
				Out.Add(Tag.GetTagName());
			return true;
		}
		if (!Value.is_list() && !Value.is_string())
			return false;
		try
		{
			for (const FString& Text : TagTexts(Value))
				Out.Add(FName(*Text.TrimStartAndEnd()));
		}
		catch (const std::exception&)
		{
			return false;
		}
		return true;
	}

	void TagShape(FUnrealMcpRqlTypes&, rql::TypeDescriptor& D)
	{
		// The tag's name is the whole value: no writable members (a write replaces the tag, validated).
		D.members.clear();
		FUnrealMcpRqlTypes::SetMember(D, Member("Name", rql::builtin::string_type(), [](void* Self)
		{
			return Str(static_cast<const FGameplayTag*>(Self)->ToString());
		}, {}, "the tag text (\"None\" for no tag)"));
		FUnrealMcpRqlTypes::SetMember(D, Member("Parent", rql::builtin::string_type(), [](void* Self)
		{
			const FGameplayTag Parent = static_cast<const FGameplayTag*>(Self)->RequestDirectParent();
			return Str(Parent.IsValid() ? Parent.ToString() : FString());
		}, {}, "the direct parent tag, empty at the root"));
	}

	void ContainerShape(FUnrealMcpRqlTypes& T, rql::TypeDescriptor& D)
	{
		D.members.clear();
		FUnrealMcpRqlTypes::SetMember(D, Member("Names", T.Registry().list_type(rql::builtin::string_type(), false), [](void* Self)
		{
			return StrList(Names(*static_cast<const FGameplayTagContainer*>(Self)));
		}, {}, "the tags of the container (write the container itself: set Tags = [\"A.B\", \"C\"])"));
		const rql::TypeDescriptor* Int = rql::builtin::number_type(true, true, 4);
		FUnrealMcpRqlTypes::SetMember(D, Member("Num", Int, [Int](void* Self)
		{
			return rql::Value::number(static_cast<const FGameplayTagContainer*>(Self)->Num(), Int);
		}));
	}

	void QueryShape(FUnrealMcpRqlTypes&, rql::TypeDescriptor& D)
	{
		// A query is a compiled token stream: show it, never write its raw members.
		D.members.clear();
		FUnrealMcpRqlTypes::SetMember(D, Member("Description", rql::builtin::string_type(), [](void* Self)
		{
			const FGameplayTagQuery* Query = static_cast<const FGameplayTagQuery*>(Self);
			return Str(Query->IsEmpty() ? FString() : Query->GetDescription());
		}, {}, "the query in words (edit queries in the Details panel)"));
	}

	/** Capture what a GameplayTagsEditor call logs as an error / warning (it reports failures there, not through results). */
	class FTagLogCapture final : public FOutputDevice
	{
	public:
		FTagLogCapture() { GLog->AddOutputDevice(this); }
		virtual ~FTagLogCapture() override { GLog->RemoveOutputDevice(this); }
		virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Verbosity <= ELogVerbosity::Warning && Category.ToString().Contains(TEXT("GameplayTag")))
				Messages.Add(Message);
		}
		FString Joined() const { return FString::Join(Messages, TEXT("; ")); }

	private:
		TArray<FString> Messages;
	};

	IGameplayTagsEditorModule* TagsEditor()
	{
		return FModuleManager::LoadModulePtr<IGameplayTagsEditorModule>(TEXT("GameplayTagsEditor"));
	}

	/** Whether a tag source (an ini, DataTable or native declaration) declares @p Tag, not only its children. */
	bool IsExplicitTag(FName Tag)
	{
		FString Comment;
		FName Source;
		bool bExplicit = false;
		bool bRestricted = false;
		bool bAllowChildren = false;
		return UGameplayTagsManager::Get().GetTagEditorData(Tag, Comment, Source, bExplicit, bRestricted, bAllowChildren) && bExplicit;
	}

	/** A dictionary tag (one some ini, DataTable or native declaration defines), or a failure. */
	bool RequireDictionaryTag(const FString& Name, FName& Out)
	{
		const FString Trimmed = Name.TrimStartAndEnd();
		Out = FName(*Trimmed);
		if (Trimmed.IsEmpty() || !UGameplayTagsManager::Get().IsDictionaryTag(Out))
		{
			UnrealMcpRql::Fail(FString::Printf(TEXT("'%s' is not a gameplay tag%s"), *Trimmed, *Similar(Trimmed)));
			return false;
		}
		return true;
	}
}

using namespace UnrealMcpRqlGameplayTagsLocal;

void UnrealMcpRqlAddGameplayTags(FUnrealMcpRqlSession& Session)
{
	FUnrealMcpRqlTypes& T = Session.Types();
	FUnrealMcpRqlTypes* Types = &T;
	UScriptStruct* TagStruct = FGameplayTag::StaticStruct();
	UScriptStruct* ContainerStruct = FGameplayTagContainer::StaticStruct();

	// Shapes and parsers before the first fill (AddAdapter below fills the descriptors).
	T.AddStructHook(TagStruct, &TagShape);
	T.AddStructHook(ContainerStruct, &ContainerShape);
	T.AddStructHook(FGameplayTagQuery::StaticStruct(), &QueryShape);
	T.AddStructParser(TagStruct, [](FUnrealMcpRqlTypes&, const rql::Value& Value, void* Out) -> bool
	{
		if (Value.is_null())
		{
			*static_cast<FGameplayTag*>(Out) = FGameplayTag();
			return true;
		}
		if (!Value.is_string())
			return false;
		*static_cast<FGameplayTag*>(Out) = ParseTag(UnrealMcpRqlFromStd(Value.as_string()));
		return true;
	});
	T.AddStructParser(ContainerStruct, [](FUnrealMcpRqlTypes&, const rql::Value& Value, void* Out) -> bool
	{
		if (Value.is_null())
		{
			*static_cast<FGameplayTagContainer*>(Out) = FGameplayTagContainer();
			return true;
		}
		if (!Value.is_string() && !Value.is_list())
			return false;
		*static_cast<FGameplayTagContainer*>(Out) = ParseContainer(Value);
		return true;
	});

	const rql::TypeDescriptor* TagType = T.ForStruct(TagStruct);
	const rql::TypeDescriptor* ContainerType = T.ForStruct(ContainerStruct);
	auto Coerce = [Types](UScriptStruct* Struct)
	{
		return UnrealMcpRqlStructCoerce(*Types, Struct,
			[](const rql::Value& Value) { return Value.is_string() || Value.is_list() || Value.is_null(); });
	};
	{
		rql::TypeAdapter Adapter;
		Adapter.name = "GameplayTag";
		Adapter.target = TagType;
		Adapter.description = "GameplayTag: write the tag text \"Ability.Fire\" (registered tags only; \"\" = none)";
		Adapter.encode = [](const rql::Value& Value, const rql::EncodeContext&) -> std::optional<rql::Json>
		{
			if (!Value.is_struct() || !Value.ptr())
				return std::nullopt;
			const FGameplayTag& Tag = *static_cast<const FGameplayTag*>(Value.ptr());
			return Tag.IsValid() ? rql::Json(UnrealMcpRqlToStd(Tag.ToString())) : rql::Json();
		};
		Adapter.coerce = Coerce(TagStruct);
		Adapter.equals = [TagType](const rql::Value& Value, const rql::Value& Other) -> std::optional<bool>
		{
			if (!Value.is_struct() || !Value.ptr())
				return std::nullopt;
			const FName Mine = static_cast<const FGameplayTag*>(Value.ptr())->GetTagName();
			if (Other.is_string())
				return Mine == FName(*UnrealMcpRqlFromStd(Other.as_string()).TrimStartAndEnd());
			if (Other.is_null())
				return Mine.IsNone();
			if (Other.is_struct() && Other.type() == TagType && Other.ptr())
				return Mine == static_cast<const FGameplayTag*>(Other.ptr())->GetTagName();
			return std::nullopt;
		};
		Session.AddAdapter(std::move(Adapter));
	}
	{
		rql::TypeAdapter Adapter;
		Adapter.name = "GameplayTagContainer";
		Adapter.target = ContainerType;
		Adapter.description = "GameplayTagContainer: write a list of registered tags [\"Ability.Fire\", \"State.Stunned\"] (or \"A.B, C.D\")";
		Adapter.encode = [](const rql::Value& Value, const rql::EncodeContext&) -> std::optional<rql::Json>
		{
			if (!Value.is_struct() || !Value.ptr())
				return std::nullopt;
			rql::Json List = rql::Json::array();
			for (const FString& Name : Names(*static_cast<const FGameplayTagContainer*>(Value.ptr())))
				List.push(rql::Json(UnrealMcpRqlToStd(Name)));
			return List;
		};
		Adapter.coerce = Coerce(ContainerStruct);
		Adapter.equals = [ContainerType](const rql::Value& Value, const rql::Value& Other) -> std::optional<bool>
		{
			TSet<FName> Mine;
			TSet<FName> Theirs;
			if (!AsNames(Value, ContainerType, Mine) || !AsNames(Other, ContainerType, Theirs))
				return std::nullopt;
			return Mine.Num() == Theirs.Num() && Mine.Includes(Theirs);
		};
		Session.AddAdapter(std::move(Adapter));
	}

	rql::TypeRegistry& Registry = T.Registry();
	Registry.structure<FTagInfo>("GameplayTagInfo")
		.member("Name", &FTagInfo::Name)
		.member("Comment", &FTagInfo::Comment)
		.member("Source", &FTagInfo::Source)
		.member("Parent", &FTagInfo::Parent)
		.member("Children", &FTagInfo::Children)
		.member("Explicit", &FTagInfo::Explicit)
		.member("Restricted", &FTagInfo::Restricted)
		.no_identity();
	const rql::TypeDescriptor* InfoType = Registry.descriptor<FTagInfo>();
	T.AddNativeType(InfoType);
	Session.AddSource(TEXT("gameplayTags"), InfoType, /*bList*/ true,
		TEXT("every registered gameplay tag, sorted (Name, Comment, Source ini, Parent, Children, Explicit, Restricted); "
			"change the list with editor | call AddGameplayTag / RemoveGameplayTag / RenameGameplayTag"), [InfoType]()
	{
		UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
		FGameplayTagContainer All;
		Manager.RequestAllGameplayTags(All, /*OnlyIncludeDictionaryTags*/ false);
		TArray<FGameplayTag> Tags = All.GetGameplayTagArray();
		Tags.Sort([](const FGameplayTag& A, const FGameplayTag& B) { return A.ToString() < B.ToString(); });
		std::vector<rql::Value> Rows;
		Rows.reserve(static_cast<size_t>(Tags.Num()));
		for (const FGameplayTag& Tag : Tags)
		{
			auto Info = std::make_shared<FTagInfo>();
			Info->Name = UnrealMcpRqlToStd(Tag.ToString());
			FString Comment;
			FName Source;
			bool bExplicit = false;
			bool bRestricted = false;
			bool bAllowChildren = false;
			if (Manager.GetTagEditorData(Tag.GetTagName(), Comment, Source, bExplicit, bRestricted, bAllowChildren))
			{
				Info->Comment = UnrealMcpRqlToStd(Comment);
				Info->Source = Source.IsNone() ? std::string() : UnrealMcpRqlToStd(Source.ToString());
				Info->Explicit = bExplicit;
				Info->Restricted = bRestricted;
			}
			const FGameplayTag Parent = Tag.RequestDirectParent();
			Info->Parent = Parent.IsValid() ? UnrealMcpRqlToStd(Parent.ToString()) : std::string();
			if (const TSharedPtr<FGameplayTagNode> Node = Manager.FindTagNode(Tag))
				Info->Children = Node->GetChildTagNodes().Num();
			Rows.push_back(rql::Value::structure(InfoType, std::static_pointer_cast<void>(Info)));
		}
		return rql::Value::list_of(std::move(Rows));
	});
}

// ------------------------------------------------------------------ editor operations

void UUnrealMcpRqlEditorApi::AddGameplayTag(const FString& Name, const FString& Comment, const FString& Source)
{
	UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
	const FString Tag = Name.TrimStartAndEnd();
	FText Error;
	FString Fixed;
	if (Tag.IsEmpty() || !Manager.IsValidGameplayTagString(Tag, &Error, &Fixed))
		return UnrealMcpRql::Fail(FString::Printf(TEXT("'%s' is not a valid gameplay tag: %s%s"), *Tag, *Error.ToString(),
			Fixed.IsEmpty() || Fixed == Tag ? TEXT("") : *FString::Printf(TEXT(" (try \"%s\")"), *Fixed)));
	if (Manager.IsDictionaryTag(FName(*Tag)))
		return UnrealMcpRql::Fail(FString::Printf(TEXT("the gameplay tag '%s' already exists"), *Tag));
	if (!Manager.ShouldImportTagsFromINI())
		return UnrealMcpRql::Fail(TEXT("this project does not read gameplay tags from config (Project Settings > GameplayTags > Import Tags From Config)"));
	IGameplayTagsEditorModule* Editor = TagsEditor();
	if (!Editor)
		return UnrealMcpRql::Fail(TEXT("the GameplayTagsEditor plugin is not loaded"));
	FTagLogCapture Log;
	const FString SourceName = Source.TrimStartAndEnd();
	if (!Editor->AddNewGameplayTagToINI(Tag, Comment, SourceName.IsEmpty() ? NAME_None : FName(*SourceName)))
		return UnrealMcpRql::Fail(FString::Printf(TEXT("could not add the gameplay tag '%s'%s"), *Tag,
			Log.Joined().IsEmpty() ? TEXT("") : *(TEXT(": ") + Log.Joined())));
}

void UUnrealMcpRqlEditorApi::RemoveGameplayTag(const FString& Name)
{
	FName Tag;
	if (!RequireDictionaryTag(Name, Tag))
		return;
	IGameplayTagsEditorModule* Editor = TagsEditor();
	if (!Editor)
		return UnrealMcpRql::Fail(TEXT("the GameplayTagsEditor plugin is not loaded"));
	TSharedPtr<FGameplayTagNode> Node = UGameplayTagsManager::Get().FindTagNode(Tag);
	if (!Node.IsValid())
		return UnrealMcpRql::Fail(FString::Printf(TEXT("'%s' is not a gameplay tag"), *Name));
	const bool bWasExplicit = IsExplicitTag(Tag);
	FTagLogCapture Log;
	Editor->DeleteTagsFromINI({ Node }); // the log-only variant: failures are logged, not shown as toasts
	// An explicit tag with children is removed from its ini but lives on implicitly (as the tag editor's Delete leaves it).
	if (UGameplayTagsManager::Get().IsDictionaryTag(Tag) && (!bWasExplicit || IsExplicitTag(Tag)))
	{
		// Still there: referenced by assets, declared in C++ / a DataTable, or implicit (children keep it).
		const FString Why = Log.Joined();
		return UnrealMcpRql::Fail(FString::Printf(TEXT("the gameplay tag '%s' was not removed%s"), *Name.TrimStartAndEnd(),
			Why.IsEmpty() ? TEXT(" (it is implicit: remove its children, or it is declared outside the tag ini files)") : *(TEXT(": ") + Why)));
	}
}

void UUnrealMcpRqlEditorApi::RenameGameplayTag(const FString& Name, const FString& NewName)
{
	FName Tag;
	if (!RequireDictionaryTag(Name, Tag))
		return;
	UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
	const FString Target = NewName.TrimStartAndEnd();
	FText Error;
	if (Target.IsEmpty() || !Manager.IsValidGameplayTagString(Target, &Error))
		return UnrealMcpRql::Fail(FString::Printf(TEXT("'%s' is not a valid gameplay tag: %s"), *Target, *Error.ToString()));
	if (Manager.IsDictionaryTag(FName(*Target)))
		return UnrealMcpRql::Fail(FString::Printf(TEXT("the gameplay tag '%s' already exists"), *Target));
	IGameplayTagsEditorModule* Editor = TagsEditor();
	if (!Editor)
		return UnrealMcpRql::Fail(TEXT("the GameplayTagsEditor plugin is not loaded"));
	FTagLogCapture Log;
	if (!Editor->RenameTagInINI(Tag.ToString(), Target) || !Manager.IsDictionaryTag(FName(*Target)))
		return UnrealMcpRql::Fail(FString::Printf(TEXT("could not rename '%s' to '%s'%s"), *Tag.ToString(), *Target,
			Log.Joined().IsEmpty() ? TEXT("") : *(TEXT(": ") + Log.Joined())));
}
