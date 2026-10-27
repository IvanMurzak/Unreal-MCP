// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRqlSession.h"
#include "UnrealMcpRqlSources.h"
#include "UnrealMcpRqlUtil.h"

#include "GameFramework/InputSettings.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/PlatformFileManager.h"
#include "ISettingsCategory.h"
#include "ISettingsContainer.h"
#include "ISettingsEditorModule.h"
#include "ISettingsModule.h"
#include "ISettingsSection.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/UnrealType.h"

#include <cctype>
#include <memory>
#include <string>
#include <vector>

/**
 * Project and editor settings (docs/rql.md "Settings"): every section of the Project Settings and Editor Preferences
 * windows, with the object that section edits. Those objects are class defaults, read-only through RQL elsewhere; here
 * they are writable, and a write is saved to its config file after the mutation the way the settings editor saves it
 * (SSettingsEditor::HandleSettingsPropertyChanged): one property into Default<Config>.ini for project (default-config)
 * classes, the whole section otherwise. Config files are not part of the editor transaction: a settings write is saved
 * at once and Ctrl+Z does not revert it. Only config properties are writable (others would be lost on restart).
 */
namespace UnrealMcpRqlSettingsLocal
{
	using namespace UnrealMcpRqlUtil;

	struct FSectionRow
	{
		std::string Container;
		std::string Category;
		std::string Section;
		std::string DisplayName;
		std::string Description;
		TWeakObjectPtr<UObject> Object;
	};

	struct FSection
	{
		FName Container;
		TSharedPtr<ISettingsSection> Section;
		TWeakObjectPtr<UObject> Object;
	};

	/** The sections of the Project Settings ("Project") and Editor Preferences ("Editor") windows that edit an object. */
	TArray<FSection> Sections()
	{
		TArray<FSection> Out;
		ISettingsModule* Module = FModuleManager::GetModulePtr<ISettingsModule>(TEXT("Settings"));
		if (!Module)
			return Out;
		// Most sections (Physics, Rendering, Collision, Audio, plugin settings) are UDeveloperSettings that the
		// SettingsEditor module registers only once a settings window has been opened: register them now, so the
		// source does not depend on whether someone opened Project Settings in this editor session.
#if !UE_VERSION_OLDER_THAN(5, 6, 0)
		if (ISettingsEditorModule* SettingsEditor = FModuleManager::LoadModulePtr<ISettingsEditorModule>(TEXT("SettingsEditor")))
			SettingsEditor->UpdateSettings(/*bForce*/ true);
#endif
		for (const FName ContainerName : { FName(TEXT("Project")), FName(TEXT("Editor")) })
		{
			TSharedPtr<ISettingsContainer> Container = Module->GetContainer(ContainerName);
			if (!Container.IsValid())
				continue;
			TArray<TSharedPtr<ISettingsCategory>> Categories;
			Container->GetCategories(Categories);
			for (const TSharedPtr<ISettingsCategory>& Category : Categories)
			{
				TArray<TSharedPtr<ISettingsSection>> CategorySections;
				Category->GetSections(CategorySections);
				for (const TSharedPtr<ISettingsSection>& Section : CategorySections)
				{
					const TWeakObjectPtr<UObject> Object = Section.IsValid() ? Section->GetSettingsObject() : nullptr;
					if (Object.IsValid())
						Out.Add({ ContainerName, Section, Object });
				}
			}
		}
		return Out;
	}

	/** Per session: the settings objects (writable templates) and their sections, gathered on first use. */
	struct FSettingsIndex
	{
		TOptional<TArray<FSection>> Cached;

		const TArray<FSection>& All()
		{
			if (!Cached.IsSet())
				Cached = Sections();
			return Cached.GetValue();
		}

		const FSection* Find(const UObject* Object)
		{
			for (const FSection& Section : All())
			{
				if (Section.Object.Get() == Object)
					return &Section;
			}
			return nullptr;
		}
	};

	/** The member a write label starts with ("ActionMappings[0].Key" -> "ActionMappings"). */
	FString RootMember(const std::string& Label)
	{
		size_t End = 0;
		while (End < Label.size() && (std::isalnum(static_cast<unsigned char>(Label[End])) || Label[End] == '_'))
			++End;
		return UnrealMcpRqlFromStd(Label.substr(0, End));
	}

	/** The property behind a member name of @p Class (authored names: Blueprint-defined settings use display names). */
	FProperty* FindMemberProperty(UClass* Class, const FString& Member)
	{
		for (TFieldIterator<FProperty> It(Class); It; ++It)
		{
			if (It->GetAuthoredName().Equals(Member, ESearchCase::IgnoreCase) || It->GetName().Equals(Member, ESearchCase::IgnoreCase))
				return *It;
		}
		return nullptr;
	}

	void MakeWritable(const FString& RelativePath)
	{
		const FString Full = FPaths::ConvertRelativePathToFull(RelativePath);
		IPlatformFile& File = FPlatformFileManager::Get().GetPlatformFile();
		if (File.FileExists(*Full) && File.IsReadOnly(*Full))
			File.SetReadOnly(*Full, false);
	}

	/** Save one changed settings object as the settings editor does (see the file comment). */
	void Persist(UObject* Object, const FSection* Section, FProperty* Member)
	{
		UClass* Class = Object->GetClass();
		if (Class->HasAnyClassFlags(CLASS_DefaultConfig))
			MakeWritable(Object->GetDefaultConfigFilename());
		else if (Class->HasAnyClassFlags(CLASS_Config))
			MakeWritable(Class->GetConfigName());

		const bool bContainer = !Member || Member->IsA<FArrayProperty>() || Member->IsA<FSetProperty>() || Member->IsA<FMapProperty>()
			|| Member->ArrayDim > 1;
		if (Class->HasAnyClassFlags(CLASS_DefaultConfig) && !bContainer)
			Object->UpdateSinglePropertyInConfigFile(Member, Object->GetDefaultConfigFilename());
		else if (Section && Section->Section.IsValid())
			Section->Section->Save();
		else if (Class->HasAnyClassFlags(CLASS_DefaultConfig))
			Object->TryUpdateDefaultConfigFile();
		else
			Object->SaveConfig();

		// Legacy input: the key maps the player input uses are rebuilt from the edited mappings.
		if (UInputSettings* Input = Cast<UInputSettings>(Object))
			Input->ForceRebuildKeymaps();
	}

	UObject* FindSettings(FSettingsIndex& Index, const FString& Wanted)
	{
		const FString Name = Wanted.TrimStartAndEnd();
		for (const FSection& Section : Index.All())
		{
			UObject* Object = Section.Object.Get();
			if (!Object || !Section.Section.IsValid())
				continue;
			const UClass* Class = Object->GetClass();
			if (Section.Section->GetName().ToString().Equals(Name, ESearchCase::IgnoreCase)
				|| Section.Section->GetDisplayName().ToString().Equals(Name, ESearchCase::IgnoreCase)
				|| Class->GetName().Equals(Name, ESearchCase::IgnoreCase)
				|| (Name.Len() > 1 && Name[0] == TEXT('U') && Class->GetName().Equals(Name.Mid(1), ESearchCase::IgnoreCase)))
				return Object;
		}
		return nullptr;
	}
}

using namespace UnrealMcpRqlSettingsLocal;

void UnrealMcpRqlAddSettings(FUnrealMcpRqlSession& Session)
{
	FUnrealMcpRqlTypes& T = Session.Types();
	FUnrealMcpRqlTypes* Types = &T;
	TSharedRef<FSettingsIndex> Index = MakeShared<FSettingsIndex>();

	rql::TypeRegistry& Registry = T.Registry();
	Registry.structure<FSectionRow>("SettingsSection")
		.member("Container", &FSectionRow::Container)
		.member("Category", &FSectionRow::Category)
		.member("Section", &FSectionRow::Section)
		.member("DisplayName", &FSectionRow::DisplayName)
		.member("Description", &FSectionRow::Description)
		.no_identity();
	rql::TypeDescriptor* RowType = Registry.descriptor<FSectionRow>();
	FUnrealMcpRqlTypes::SetMember(*RowType, Member("Object", T.ForClass(UObject::StaticClass()), [Types](void* Self)
	{
		return Types->WrapObject(static_cast<FSectionRow*>(Self)->Object.Get());
	}, {}, "the settings object (its config properties are writable and saved to its ini)", /*bOwning*/ true, /*bNullable*/ true));
	T.AddNativeType(RowType);

	Session.AddSource(TEXT("settings"), RowType, /*bList*/ true,
		TEXT("the sections of Project Settings (Container \"Project\") and Editor Preferences (\"Editor\"): Container Category Section "
			"DisplayName Description, Object = the settings object; writes to its config properties are saved to its ini at once (not undoable)"),
		[Index, RowType]()
	{
		std::vector<rql::Value> Rows;
		for (const FSection& Section : Index->All())
		{
			TSharedPtr<ISettingsCategory> Category = Section.Section->GetCategory().Pin();
			auto Row = std::make_shared<FSectionRow>();
			Row->Container = UnrealMcpRqlToStd(Section.Container.ToString());
			Row->Category = Category.IsValid() ? UnrealMcpRqlToStd(Category->GetName().ToString()) : std::string();
			Row->Section = UnrealMcpRqlToStd(Section.Section->GetName().ToString());
			Row->DisplayName = UnrealMcpRqlToStd(Section.Section->GetDisplayName().ToString());
			Row->Description = UnrealMcpRqlToStd(Section.Section->GetDescription().ToString());
			Row->Object = Section.Object;
			Rows.push_back(rql::Value::structure(RowType, std::static_pointer_cast<void>(Row)));
		}
		return rql::Value::list_of(std::move(Rows));
	});
	Session.AddFunction(TEXT("unreal.settings"),
		TEXT("the settings object of a Project Settings / Editor Preferences section by name (\"Maps & Modes\", \"GameMapsSettings\", \"Input\"); writes are saved to its ini"),
		[Types, Index](const std::vector<rql::Value>& Args) -> rql::Value
		{
			if (Args.size() != 1 || !Args[0].is_string())
				return rql::Value::error("type-mismatch", "unreal.settings(section) expects a section or class name such as \"Maps & Modes\" or \"GameMapsSettings\"");
			const FString Wanted = UnrealMcpRqlFromStd(Args[0].as_string());
			UObject* Found = FindSettings(*Index, Wanted);
			if (!Found)
				return rql::Value::error("not-found", UnrealMcpRqlToStd(FString::Printf(
					TEXT("no settings section '%s' (settings | select Container Category Section DisplayName lists them)"), *Wanted)));
			return Types->WrapObject(Found);
		});

	// Settings objects are class defaults by design: writable here, saved after the mutation.
	Session.AllowTemplateWrites([Index](const UObject* Object) { return Index->Find(Object) != nullptr; });
	Session.AddBatchValidator([Types, Index](const std::vector<rql::MutationRecord>& Records) -> FString
	{
		for (const rql::MutationRecord& Record : Records)
		{
			UObject* Owner = Types->AsObject(Record.owner);
			if (!Owner || Record.method || !Index->Find(Owner))
				continue;
			const UObject* ItemObject = Types->AsObject(Record.item);
			if (ItemObject && ItemObject != Owner)
				continue; // written through a reference: the whole section is saved (a map entry has no object item)
			const FString Root = RootMember(Record.member);
			FProperty* Property = FindMemberProperty(Owner->GetClass(), Root);
			if (Property && !Property->HasAnyPropertyFlags(CPF_Config))
				return FString::Printf(TEXT("%s.%s is not a config property: it would not be saved (only config settings are writable)"),
					*Owner->GetClass()->GetName(), *Root);
		}
		return FString();
	});
	Session.AddCommitted([Types, Index](const std::vector<rql::MutationRecord>& Records)
	{
		// One save per (object, member); calls (CallInEditor buttons of a settings object) save the whole section.
		TSet<TPair<UObject*, FProperty*>> Saved;
		for (const rql::MutationRecord& Record : Records)
		{
			UObject* Owner = Types->AsObject(Record.owner);
			if (!Owner)
				Owner = Types->AsObject(Record.item);
			const FSection* Section = Owner ? Index->Find(Owner) : nullptr;
			if (!Section)
				continue;
			const UObject* ItemObject = Types->AsObject(Record.item);
			FProperty* Property = !Record.method && (!ItemObject || ItemObject == Owner)
				? FindMemberProperty(Owner->GetClass(), RootMember(Record.member)) : nullptr;
			bool bAlready = false;
			Saved.Add(TPair<UObject*, FProperty*>(Owner, Property), &bAlready);
			if (!bAlready)
				Persist(Owner, Section, Property);
		}
	});
}
