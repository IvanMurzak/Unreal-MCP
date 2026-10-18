// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRqlSession.h"
#include "UnrealMcpRqlEditorApi.h"
#include "UnrealMcpRqlSources.h"
#include "UnrealMcpRqlUtil.h"
#include "IUnrealMcpRqlModule.h"
#include "Tools/UnrealMcpLogCollector.h"
#include "Tools/UnrealMcpObjectRef.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Editor.h"
#include "EditorLevelUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/Level.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "PlayInEditorDataTypes.h"
#include "Selection.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"

#include <algorithm>
#include <string>
#include <vector>

UWorld* UnrealMcpRqlUtil::EditorWorld()
{
	return FUnrealMcpObjectRef::GetEditorWorld();
}

void UnrealMcpRqlUtil::MakeLevelCurrent(ULevel* Level)
{
	if (!Level)
		Throw(TEXT("expected a level"));
	UEditorLevelUtils::MakeLevelCurrent(Level, /*bEvenIfLocked*/ false);
	if (!Level->GetWorld() || Level->GetWorld()->GetCurrentLevel() != Level)
		Throw(TEXT("the level could not be made current (locked?)"));
}

namespace UnrealMcpRqlCoreLocal
{
	using namespace UnrealMcpRqlUtil;

	const rql::TypeDescriptor* StringType() { return rql::builtin::string_type(); }
	const rql::TypeDescriptor* BoolType() { return rql::builtin::bool_type(); }

	FString ShortPackageName(const UObject* Object)
	{
		const UPackage* Package = Object ? Object->GetOutermost() : nullptr;
		return Package ? FPackageName::GetShortName(Package->GetName()) : FString();
	}

	TArray<UObject*> SelectedActors()
	{
		TArray<UObject*> Out;
		if (GEditor)
		{
			if (USelection* Selection = GEditor->GetSelectedActors())
			{
				TArray<AActor*> Actors;
				Selection->GetSelectedObjects<AActor>(Actors);
				for (AActor* Actor : Actors)
					Out.Add(Actor);
			}
		}
		return Out;
	}

	// ---------------------------------------------------------------- native host structs

	struct FLogEntry
	{
		int64_t Sequence = 0;
		std::string Time;
		std::string Verbosity;
		std::string Category;
		std::string Message;
	};

	struct FClassInfo
	{
		std::string Name;
		std::string Path;
		std::string Parent;
		bool Abstract = false;
		bool Blueprint = false;
	};

	// ---------------------------------------------------------------- assets

	/** Asset registry hits, loaded when the query reads them (`unreal.assets(...) | first 3` loads three). */
	class FAssetList final : public rql::IListAdapter
	{
	public:
		FAssetList(FUnrealMcpRqlTypes* InTypes, TArray<FAssetData> InAssets, const rql::TypeDescriptor* InType)
			: Types(InTypes), Assets(MoveTemp(InAssets)), Type(InType)
		{
		}
		size_t size() const override { return static_cast<size_t>(Assets.Num()); }
		rql::Value at(size_t Index) const override
		{
			if (static_cast<int32>(Index) >= Assets.Num())
				return rql::Value::missing();
			UObject* Asset = Assets[static_cast<int32>(Index)].GetAsset();
			return Asset ? Types->WrapObject(Asset) : rql::Value::null();
		}
		const rql::TypeDescriptor* type() const override { return Type; }

	private:
		FUnrealMcpRqlTypes* Types;
		TArray<FAssetData> Assets;
		const rql::TypeDescriptor* Type;
	};

	/** The captured log (a snapshot), converted to LogEntry values only as the query reads them. */
	class FConsoleList final : public rql::IListAdapter
	{
	public:
		FConsoleList(TArray<FUnrealMcpLogEntry> InEntries, const rql::TypeDescriptor* InEntryType, const rql::TypeDescriptor* InType)
			: Entries(MoveTemp(InEntries)), EntryType(InEntryType), Type(InType)
		{
		}
		size_t size() const override { return static_cast<size_t>(Entries.Num()); }
		rql::Value at(size_t Index) const override
		{
			if (static_cast<int32>(Index) >= Entries.Num())
				return rql::Value::missing();
			const FUnrealMcpLogEntry& Entry = Entries[static_cast<int32>(Index)];
			auto Item = std::make_shared<FLogEntry>();
			Item->Sequence = Entry.Sequence;
			Item->Time = UnrealMcpRqlToStd(Entry.Timestamp.ToIso8601());
			Item->Verbosity = UnrealMcpRqlToStd(FUnrealMcpLogCollector::VerbosityToString(Entry.Verbosity));
			Item->Category = UnrealMcpRqlToStd(Entry.Category.ToString());
			Item->Message = UnrealMcpRqlToStd(Entry.Message);
			return rql::Value::structure(EntryType, std::static_pointer_cast<void>(Item));
		}
		const rql::TypeDescriptor* type() const override { return Type; }

	private:
		TArray<FUnrealMcpLogEntry> Entries;
		const rql::TypeDescriptor* EntryType;
		const rql::TypeDescriptor* Type;
	};

	// ---------------------------------------------------------------- material parameters

	/** Scalars / Vectors / Textures of a material by parameter name; writable on material instances (constant). */
	class FMaterialParameters final : public rql::IMapAdapter
	{
	public:
		enum class EKind : uint8 { Scalar, Vector, Texture };

		FMaterialParameters(FUnrealMcpRqlTypes* InTypes, UMaterialInterface* InMaterial, EKind InKind, const rql::TypeDescriptor* InType)
			: Types(InTypes), Material(InMaterial), Kind(InKind), Type(InType)
		{
		}

		size_t size() const override { return static_cast<size_t>(Names().Num()); }
		void for_each(const std::function<bool(const rql::Value&, const rql::Value&)>& Fn) const override
		{
			for (const FName& Name : Names())
			{
				if (!Fn(Str(Name.ToString()), Get(Name)))
					return;
			}
		}
		rql::MapLookup find(const rql::Value& Key, rql::Value& Out) const override
		{
			if (!Key.is_string())
				return rql::MapLookup::BadKey;
			FName Name;
			if (!Resolve(Key, Name))
				return rql::MapLookup::NotFound;
			Out = Get(Name);
			return rql::MapLookup::Found;
		}
		const rql::TypeDescriptor* type() const override { return Type; }
		const void* identity() const override { return this; }
		bool writable() const override { return Cast<UMaterialInstanceConstant>(Material) != nullptr; }
		bool fixed_keys() const override { return true; }
		bool accepts_key(const rql::Value& Key) const override { return Key.is_string(); }
		void set(const rql::Value& Key, const rql::Value& Value) override
		{
			UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Material);
			FName Name;
			if (!Instance)
				Throw(TEXT("only material instances can be changed (editor | call CreateMaterialInstance(path, parent))"));
			if (!Resolve(Key, Name))
				Throw(FString::Printf(TEXT("%s has no parameter '%s'"), *Material->GetName(), *TextArg(Key)));
			switch (Kind)
			{
			case EKind::Scalar:
				if (!Value.is_number())
					Throw(TEXT("expected a number"));
				UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, Name, static_cast<float>(Value.as_number()));
				break;
			case EKind::Vector:
				UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, Name, StructArg<FLinearColor>(*Types, Value));
				break;
			case EKind::Texture:
			{
				UTexture* Texture = Value.is_null() ? nullptr : Cast<UTexture>(Types->AsObject(Value));
				if (!Value.is_null() && !Texture)
					Throw(TEXT("expected a Texture"));
				UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, Name, Texture);
				break;
			}
			}
			UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
		}

	private:
		FUnrealMcpRqlTypes* Types;
		UMaterialInterface* Material;
		EKind Kind;
		const rql::TypeDescriptor* Type;

		mutable TOptional<TArray<FName>> CachedNames;

		/** The parameter names, read once per value (a material's parameters do not change during a query). */
		const TArray<FName>& Names() const
		{
			if (!CachedNames.IsSet())
			{
				TArray<FName> Out;
				switch (Kind)
				{
				case EKind::Scalar: UMaterialEditingLibrary::GetScalarParameterNames(Material, Out); break;
				case EKind::Vector: UMaterialEditingLibrary::GetVectorParameterNames(Material, Out); break;
				case EKind::Texture: UMaterialEditingLibrary::GetTextureParameterNames(Material, Out); break;
				}
				CachedNames = MoveTemp(Out);
			}
			return CachedNames.GetValue();
		}

		/** The parameter named by @p Key (FName equality: case-insensitive, no new name entries for unknown keys). */
		bool Resolve(const rql::Value& Key, FName& Out) const
		{
			const FName Wanted(*TextArg(Key), FNAME_Find);
			const FName* Found = Wanted.IsNone() ? nullptr : Names().FindByKey(Wanted);
			if (!Found)
				return false;
			Out = *Found;
			return true;
		}

		rql::Value Get(const FName& Name) const
		{
			const FHashedMaterialParameterInfo Info(Name);
			switch (Kind)
			{
			case EKind::Scalar:
			{
				float Value = 0.0f;
				return Material->GetScalarParameterValue(Info, Value)
					? rql::Value::number(Value, rql::builtin::single_type()) : rql::Value::null();
			}
			case EKind::Vector:
			{
				FLinearColor Value = FLinearColor::Black;
				return Material->GetVectorParameterValue(Info, Value)
					? Types->WrapStruct(TBaseStructure<FLinearColor>::Get(), &Value) : rql::Value::null();
			}
			case EKind::Texture:
			{
				UTexture* Value = nullptr;
				return Material->GetTextureParameterValue(Info, Value) ? Types->WrapObject(Value) : rql::Value::null();
			}
			}
			return rql::Value::null();
		}
	};

	// ---------------------------------------------------------------- class hooks

	void ObjectMembers(FUnrealMcpRqlTypes&, rql::TypeDescriptor& D)
	{
		const rql::TypeDescriptor* Int = rql::builtin::number_type(true, true, 8);
		FUnrealMcpRqlTypes::SetMember(D, Member("Id", Int, [Int](void* Self)
		{
			return rql::Value::number(static_cast<double>(FUnrealMcpRqlTypes::IdOf(static_cast<UObject*>(Self))), Int);
		}, {}, "object id for this editor session: $ref is Type#Id"));
		FUnrealMcpRqlTypes::SetMember(D, Member("Name", StringType(), [](void* Self) { return Str(static_cast<UObject*>(Self)->GetName()); }));
		FUnrealMcpRqlTypes::SetMember(D, Member("Path", StringType(), [](void* Self) { return Str(static_cast<UObject*>(Self)->GetPathName()); },
			{}, "object path (assets: \"/Game/Materials/M_Red.M_Red\")"));
		D.id_name = "Id";
		D.id_explicit = true;
		D.display_name = "Name";
		D.display_explicit = true;
	}

	/** UnrealMcpRql::AddComponent with the failure as text (the actor method throws it, the exported helper Fail()s it). */
	UActorComponent* AddActorComponent(AActor* Actor, UClass* Class, const FString& Requested, const TFunction<void(UActorComponent*)>& Configure,
		FString& OutError)
	{
		if (!Actor)
		{
			OutError = TEXT("no actor");
			return nullptr;
		}
		if (!Class || !Class->IsChildOf(UActorComponent::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract))
		{
			OutError = FString::Printf(TEXT("'%s' is not a component class (e.g. \"StaticMeshComponent\")"), Class ? *Class->GetName() : TEXT("None"));
			return nullptr;
		}
		if (!Requested.IsEmpty() && !FName::IsValidXName(Requested, INVALID_OBJECTNAME_CHARACTERS))
		{
			OutError = FString::Printf(TEXT("'%s' is not a valid component name (spaces and characters such as . : / , are not allowed)"), *Requested);
			return nullptr;
		}
		const FName Name = Requested.IsEmpty() ? MakeUniqueObjectName(Actor, Class, Class->GetFName()) : FName(*Requested);
		if (!Requested.IsEmpty() && StaticFindObjectFast(nullptr, Actor, Name))
		{
			OutError = FString::Printf(TEXT("%s already has a component named '%s'"), *Actor->GetActorLabel(), *Requested);
			return nullptr;
		}
		Actor->Modify();
		UActorComponent* Component = NewObject<UActorComponent>(Actor, Class, Name, RF_Transactional);
		if (!Component)
		{
			OutError = FString::Printf(TEXT("could not create %s"), *Class->GetName());
			return nullptr;
		}
		Actor->AddInstanceComponent(Component);
		if (USceneComponent* Scene = Cast<USceneComponent>(Component))
		{
			if (USceneComponent* Root = Actor->GetRootComponent())
				Scene->AttachToComponent(Root, FAttachmentTransformRules::KeepRelativeTransform);
			else
				Actor->SetRootComponent(Scene);
		}
		if (Configure)
			Configure(Component);
		Component->OnComponentCreated();
		Component->RegisterComponent();
		return Component;
	}

	/** A world-transform member of actors (Location / Rotation / Scale): the struct value, written through its root component. */
	template <typename TValue>
	rql::MemberDescriptor TransformMember(FUnrealMcpRqlTypes& T, const char* Name, const char* Description,
		TValue (*Get)(const AActor*), void (*Set)(AActor*, const TValue&))
	{
		FUnrealMcpRqlTypes* Types = &T;
		UScriptStruct* Struct = TBaseStructure<TValue>::Get();
		return Member(Name, T.ForStruct(Struct),
			[Types, Struct, Get](void* Self) { const TValue Value = Get(static_cast<AActor*>(Self)); return Types->WrapStruct(Struct, &Value); },
			[Types, Set](void* Self, const rql::Value& Value)
			{
				AActor* Actor = static_cast<AActor*>(Self);
				if (!Actor->GetRootComponent())
					Throw(FString::Printf(TEXT("%s has no root component (no transform)"), *Actor->GetActorLabel()));
				Set(Actor, StructArg<TValue>(*Types, Value));
			}, Description);
	}

	void ActorMembers(FUnrealMcpRqlTypes& T, rql::TypeDescriptor& D)
	{
		FUnrealMcpRqlTypes* Types = &T;
		const rql::TypeDescriptor* Vector = T.ForStruct(TBaseStructure<FVector>::Get());
		const rql::TypeDescriptor* ActorType = T.ForClass(AActor::StaticClass());
		const rql::TypeDescriptor* ComponentType = T.ForClass(UActorComponent::StaticClass());
		const rql::TypeDescriptor* LevelType = T.ForClass(ULevel::StaticClass());

		FUnrealMcpRqlTypes::SetMember(D, Member("Label", StringType(),
			[](void* Self) { return Str(static_cast<AActor*>(Self)->GetActorLabel()); },
			[](void* Self, const rql::Value& Value) { static_cast<AActor*>(Self)->SetActorLabel(TextArg(Value)); },
			"the name shown in the Outliner"));
		FUnrealMcpRqlTypes::SetMember(D, TransformMember<FVector>(T, "Location", "world location",
			[](const AActor* Actor) { return Actor->GetActorLocation(); },
			[](AActor* Actor, const FVector& Value) { Actor->SetActorLocation(Value); }));
		FUnrealMcpRqlTypes::SetMember(D, TransformMember<FRotator>(T, "Rotation", "world rotation (pitch, yaw, roll)",
			[](const AActor* Actor) { return Actor->GetActorRotation(); },
			[](AActor* Actor, const FRotator& Value) { Actor->SetActorRotation(Value); }));
		FUnrealMcpRqlTypes::SetMember(D, TransformMember<FVector>(T, "Scale", "world scale",
			[](const AActor* Actor) { return Actor->GetActorScale3D(); },
			[](AActor* Actor, const FVector& Value) { Actor->SetActorScale3D(Value); }));
		FUnrealMcpRqlTypes::SetMember(D, Member("Components", T.Registry().list_type(ComponentType, true),
			[Types](void* Self)
			{
				TArray<UActorComponent*> Components;
				static_cast<AActor*>(Self)->GetComponents(Components);
				return Types->WrapObjects(TArray<UObject*>(Components));
			}, {}, "", /*bOwning*/ true));
		FUnrealMcpRqlTypes::SetMember(D, Member("Children", T.Registry().list_type(ActorType, true),
			[Types](void* Self)
			{
				TArray<AActor*> Children;
				static_cast<AActor*>(Self)->GetAttachedActors(Children);
				return Types->WrapObjects(TArray<UObject*>(Children));
			}, {}, "actors attached to this one", /*bOwning*/ true));
		FUnrealMcpRqlTypes::SetMember(D, Member("Parent", ActorType,
			[Types](void* Self) { return Types->WrapObject(static_cast<AActor*>(Self)->GetAttachParentActor()); },
			[Types](void* Self, const rql::Value& Value)
			{
				AActor* Actor = static_cast<AActor*>(Self);
				if (Value.is_null())
				{
					Actor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
					return;
				}
				AActor* Parent = Cast<AActor>(Types->AsObject(Value));
				if (!Parent)
					Throw(TEXT("expected an Actor or null"));
				if (Parent == Actor || Parent->IsAttachedTo(Actor))
					Throw(FString::Printf(TEXT("attaching %s to %s would create a cycle"), *Actor->GetActorLabel(), *Parent->GetActorLabel()));
				Actor->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform);
				if (Actor->GetAttachParentActor() != Parent)
					Throw(FString::Printf(TEXT("could not attach %s (no root component)"), *Actor->GetActorLabel()));
			}, "the actor this one is attached to (keeps the world transform; null detaches)", false, true));
		FUnrealMcpRqlTypes::SetMember(D, Member("Folder", StringType(),
			[](void* Self) { return Str(static_cast<AActor*>(Self)->GetFolderPath().ToString()); },
			[](void* Self, const rql::Value& Value) { static_cast<AActor*>(Self)->SetFolderPath(FName(*TextArg(Value))); },
			"Outliner folder"));
		FUnrealMcpRqlTypes::SetMember(D, Member("Level", LevelType,
			[Types](void* Self) { return Types->WrapObject(static_cast<AActor*>(Self)->GetLevel()); }, {}, "", false, true));
		FUnrealMcpRqlTypes::SetMember(D, Member("HiddenInEditor", BoolType(),
			[](void* Self) { return rql::Value::boolean(static_cast<AActor*>(Self)->IsTemporarilyHiddenInEditor()); },
			[](void* Self, const rql::Value& Value) { static_cast<AActor*>(Self)->SetIsTemporarilyHiddenInEditor(BoolArg(Value)); },
			"hidden in the editor viewport (the Outliner eye)"));
		D.display_name = "Label";
		D.display_explicit = true;

		FUnrealMcpRqlTypes::AddMethod(D, Method("Destroy", {}, nullptr, [](void* Self, const std::vector<rql::Value>&) -> rql::Value
		{
			AActor* Actor = static_cast<AActor*>(Self);
			UWorld* World = Actor->GetWorld();
			if (!World || !World->EditorDestroyActor(Actor, /*bShouldModifyLevel*/ true))
				Throw(FString::Printf(TEXT("could not destroy %s"), *Actor->GetActorLabel()));
			return rql::Value::null();
		}, "remove the actor from its level (undoable)"));

		rql::Value ZeroOffset = T.WrapStruct(TBaseStructure<FVector>::Get(), &FVector::ZeroVector);
		FUnrealMcpRqlTypes::AddMethod(D, Method("Duplicate", { FUnrealMcpRqlTypes::OptionalParam("Offset", Vector, ZeroOffset) }, ActorType,
			[Types](void* Self, const std::vector<rql::Value>& Args) -> rql::Value
			{
				AActor* Source = static_cast<AActor*>(Self);
				UWorld* World = Source->GetWorld();
				if (!World)
					Throw(TEXT("the actor has no world"));
				const FVector Offset = StructArg<FVector>(*Types, Args[0]);
				FActorSpawnParameters Params;
				Params.Template = Source;
				Params.ObjectFlags |= RF_Transactional;
				const FTransform Transform(Source->GetActorRotation(), Source->GetActorLocation() + Offset, Source->GetActorScale3D());
				AActor* Copy = World->SpawnActor<AActor>(Source->GetClass(), Transform, Params);
				if (!Copy)
					Throw(FString::Printf(TEXT("could not duplicate %s"), *Source->GetActorLabel()));
				FActorLabelUtilities::SetActorLabelUnique(Copy, Source->GetActorLabel());
				return Types->WrapObject(Copy);
			}, "copy the actor, moved by Offset; returns the copy"));

		FUnrealMcpRqlTypes::AddMethod(D, Method("AddComponent",
			{ FUnrealMcpRqlTypes::Param("ClassName", StringType()), FUnrealMcpRqlTypes::OptionalParam("Name", StringType(), rql::Value::string("")) },
			ComponentType,
			[Types](void* Self, const std::vector<rql::Value>& Args) -> rql::Value
			{
				const FString ClassName = TextArg(Args[0]);
				UClass* Class = FUnrealMcpObjectRef::ResolveClass(ClassName);
				if (!Class || !Class->IsChildOf(UActorComponent::StaticClass()))
					Throw(FString::Printf(TEXT("'%s' is not a component class (e.g. \"StaticMeshComponent\")"), *ClassName));
				FString Error;
				UActorComponent* Component = AddActorComponent(static_cast<AActor*>(Self), Class, TextArg(Args[1]), nullptr, Error);
				if (!Component)
					Throw(Error);
				return Types->WrapObject(Component);
			}, "add a component by class name (scene components attach to the root); returns it"));
	}

	void ComponentMembers(FUnrealMcpRqlTypes& T, rql::TypeDescriptor& D)
	{
		FUnrealMcpRqlTypes* Types = &T;
		FUnrealMcpRqlTypes::SetMember(D, Member("Owner", T.ForClass(AActor::StaticClass()),
			[Types](void* Self) { return Types->WrapObject(static_cast<UActorComponent*>(Self)->GetOwner()); }, {}, "the owning actor", false, true));
		FUnrealMcpRqlTypes::AddMethod(D, Method("Destroy", {}, nullptr, [](void* Self, const std::vector<rql::Value>&) -> rql::Value
		{
			UActorComponent* Component = static_cast<UActorComponent*>(Self);
			AActor* Owner = Component->GetOwner();
			if (!Owner || !Owner->GetInstanceComponents().Contains(Component))
				Throw(TEXT("only components added to the actor instance can be destroyed, not the ones of its class or Blueprint"));
			Owner->Modify();
			Owner->RemoveInstanceComponent(Component);
			Component->DestroyComponent();
			return rql::Value::null();
		}, "remove a component added to this actor instance"));
	}

	void LevelMembers(FUnrealMcpRqlTypes& T, rql::TypeDescriptor& D)
	{
		FUnrealMcpRqlTypes* Types = &T;
		FUnrealMcpRqlTypes::SetMember(D, Member("Map", StringType(), [](void* Self) { return Str(ShortPackageName(static_cast<ULevel*>(Self))); },
			{}, "map asset name"));
		FUnrealMcpRqlTypes::SetMember(D, Member("Package", StringType(),
			[](void* Self) { const UPackage* Package = static_cast<ULevel*>(Self)->GetOutermost(); return Str(Package ? Package->GetName() : FString()); }));
		FUnrealMcpRqlTypes::SetMember(D, Member("IsPersistent", BoolType(),
			[](void* Self) { return rql::Value::boolean(static_cast<ULevel*>(Self)->IsPersistentLevel()); }));
		FUnrealMcpRqlTypes::SetMember(D, Member("IsCurrent", BoolType(),
			[](void* Self)
			{
				ULevel* Level = static_cast<ULevel*>(Self);
				return rql::Value::boolean(Level->GetWorld() && Level->GetWorld()->GetCurrentLevel() == Level);
			},
			[](void* Self, const rql::Value& Value)
			{
				if (!BoolArg(Value))
					Throw(TEXT("make another level current instead"));
				MakeLevelCurrent(static_cast<ULevel*>(Self));
			}, "new actors spawn into the current level"));
		FUnrealMcpRqlTypes::SetMember(D, Member("IsVisible", BoolType(), [](void* Self) { return rql::Value::boolean(static_cast<ULevel*>(Self)->bIsVisible); }));
		FUnrealMcpRqlTypes::SetMember(D, Member("Actors", T.Registry().list_type(T.ForClass(AActor::StaticClass()), true),
			[Types](void* Self)
			{
				TArray<UObject*> Actors;
				for (AActor* Actor : static_cast<ULevel*>(Self)->Actors)
				{
					if (IsValid(Actor))
						Actors.Add(Actor);
				}
				return Types->WrapObjects(Actors);
			}, {}, "", /*bOwning*/ true));
		D.display_name = "Map";
		D.display_explicit = true;
	}

	void WorldMembers(FUnrealMcpRqlTypes& T, rql::TypeDescriptor& D)
	{
		FUnrealMcpRqlTypes* Types = &T;
		const rql::TypeDescriptor* LevelType = T.ForClass(ULevel::StaticClass());
		FUnrealMcpRqlTypes::SetMember(D, Member("Map", StringType(), [](void* Self) { return Str(ShortPackageName(static_cast<UWorld*>(Self))); }));
		FUnrealMcpRqlTypes::SetMember(D, Member("Levels", T.Registry().list_type(LevelType, true),
			[Types](void* Self) { return Types->WrapObjects(TArray<UObject*>(static_cast<UWorld*>(Self)->GetLevels())); }, {}, "", /*bOwning*/ true));
		FUnrealMcpRqlTypes::SetMember(D, Member("PersistentLevel", LevelType,
			[Types](void* Self) { return Types->WrapObject(static_cast<UWorld*>(Self)->PersistentLevel); }, {}, "", false, true));
		FUnrealMcpRqlTypes::SetMember(D, Member("CurrentLevel", LevelType,
			[Types](void* Self) { return Types->WrapObject(static_cast<UWorld*>(Self)->GetCurrentLevel()); },
			[Types](void* Self, const rql::Value& Value)
			{
				ULevel* Level = Cast<ULevel>(Types->AsObject(Value));
				if (!Level || Level->GetWorld() != static_cast<UWorld*>(Self))
					Throw(TEXT("expected a level of this world"));
				MakeLevelCurrent(Level);
			}, "", false, true));
		D.display_name = "Map";
		D.display_explicit = true;
	}

	void MaterialMembers(FUnrealMcpRqlTypes& T, rql::TypeDescriptor& D)
	{
		FUnrealMcpRqlTypes* Types = &T;
		using EKind = FMaterialParameters::EKind;
		struct FParameterMap
		{
			const char* Name;
			EKind Kind;
			const rql::TypeDescriptor* Value;
			bool bNullable;
			const char* Description;
		};
		const FParameterMap Maps[] = {
			{ "Scalars", EKind::Scalar, rql::builtin::single_type(), false, "scalar parameters by name (writable on material instances)" },
			{ "Vectors", EKind::Vector, T.ForStruct(TBaseStructure<FLinearColor>::Get()), false, "vector parameters by name (writable on material instances)" },
			{ "Textures", EKind::Texture, T.ForClass(UTexture::StaticClass()), true, "texture parameters by name (writable on material instances)" },
		};
		for (const FParameterMap& Map : Maps)
		{
			const rql::TypeDescriptor* MapType = T.Registry().map_type(StringType(), Map.Value, Map.bNullable);
			const EKind Kind = Map.Kind;
			FUnrealMcpRqlTypes::SetMember(D, Member(Map.Name, MapType, [Types, Kind, MapType](void* Self)
			{
				return rql::Value::map(std::make_shared<FMaterialParameters>(Types, static_cast<UMaterialInterface*>(Self), Kind, MapType));
			}, {}, Map.Description));
		}
	}

	void EditorMembers(FUnrealMcpRqlTypes& T, rql::TypeDescriptor& D)
	{
		FUnrealMcpRqlTypes* Types = &T;
		const rql::TypeDescriptor* ActorType = T.ForClass(AActor::StaticClass());
		FUnrealMcpRqlTypes::SetMember(D, Member("IsPlaying", BoolType(),
			[](void*) { return rql::Value::boolean(GEditor && GEditor->IsPlaySessionInProgress()); },
			[](void*, const rql::Value& Value)
			{
				if (!GEditor)
					Throw(TEXT("no editor"));
				const bool bPlay = BoolArg(Value);
				if (bPlay && !GEditor->IsPlaySessionInProgress())
					GEditor->RequestPlaySession(FRequestPlaySessionParams());
				else if (!bPlay && GEditor->IsPlaySessionInProgress())
					GEditor->RequestEndPlayMap();
			}, "Play In Editor (starts / stops on the next editor tick)"));
		FUnrealMcpRqlTypes::SetMember(D, Member("IsPaused", BoolType(),
			[](void*) { return rql::Value::boolean(GEditor && GEditor->PlayWorld && GEditor->PlayWorld->bDebugPauseExecution); },
			[](void*, const rql::Value& Value)
			{
				if (!GEditor || !GEditor->PlayWorld)
					Throw(TEXT("not playing"));
				GEditor->SetPIEWorldsPaused(BoolArg(Value));
			}, "Play In Editor paused"));
		FUnrealMcpRqlTypes::SetMember(D, Member("Selection", T.Registry().list_type(ActorType, true),
			[Types](void*) { return Types->WrapObjects(SelectedActors()); },
			[Types](void*, const rql::Value& Value)
			{
				if (!GEditor)
					Throw(TEXT("no editor"));
				if (!Value.is_list())
					Throw(TEXT("expected a list of actors"));
				TArray<AActor*> Actors;
				for (size_t Index = 0, Count = Value.list().size(); Index < Count; ++Index)
				{
					AActor* Actor = Cast<AActor>(Types->AsObject(Value.list().at(Index)));
					if (!Actor)
						Throw(TEXT("expected a list of actors"));
					Actors.Add(Actor);
				}
				GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
				for (AActor* Actor : Actors)
					GEditor->SelectActor(Actor, /*bInSelected*/ true, /*bNotify*/ false);
				GEditor->NoteSelectionChange();
			}, "the selected actors"));
		FUnrealMcpRqlTypes::SetMember(D, Member("World", T.ForClass(UWorld::StaticClass()),
			[Types](void*) { return Types->WrapObject(EditorWorld()); }, {}, "the editor world", false, true));
		FUnrealMcpRqlTypes::SetMember(D, Member("CurrentLevel", T.ForClass(ULevel::StaticClass()),
			[Types](void*) { UWorld* World = EditorWorld(); return Types->WrapObject(World ? World->GetCurrentLevel() : nullptr); },
			[Types](void*, const rql::Value& Value)
			{
				MakeLevelCurrent(Cast<ULevel>(Types->AsObject(Value)));
			}, "the level new actors spawn into", false, true));
		// The identity members of every object are noise on the operations object.
		D.id_name.clear();
		D.display_name.clear();
	}

	// ---------------------------------------------------------------- functions

	/** The first @p Limit items of @p Items in @p Less order (a partial sort: only the kept items are ordered). */
	template <typename T, typename TLess>
	void SortAndCap(TArray<T>& Items, int32 Limit, TLess Less)
	{
		if (Items.Num() > Limit)
		{
			std::nth_element(Items.GetData(), Items.GetData() + Limit, Items.GetData() + Items.Num(), Less);
			Items.SetNum(Limit);
		}
		std::sort(Items.GetData(), Items.GetData() + Items.Num(), Less);
	}

	/** A class listings show: not deprecated / replaced / hidden, not a Blueprint compiler's skeleton or leftover. */
	bool IsListedClass(const UClass* Class)
	{
		if (!Class || Class->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists | CLASS_Hidden))
			return false;
		const FString Name = Class->GetName();
		return !Name.StartsWith(TEXT("SKEL_")) && !Name.StartsWith(TEXT("REINST_")) && !Name.StartsWith(TEXT("TRASHCLASS_"));
	}

	/** Class default objects of a base: loaded classes, plus Blueprint classes by path that load as the query reads them. */
	class FClassDefaultsList final : public rql::IListAdapter
	{
	public:
		struct FEntry
		{
			FName Name;               // the class name (sort key)
			UClass* Class = nullptr;  // loaded
			FString Path;             // else the generated class path, loaded on read
		};

		FClassDefaultsList(FUnrealMcpRqlTypes* InTypes, TArray<FEntry> InEntries, const rql::TypeDescriptor* InType)
			: Types(InTypes), Entries(MoveTemp(InEntries)), Type(InType)
		{
		}
		size_t size() const override { return static_cast<size_t>(Entries.Num()); }
		rql::Value at(size_t Index) const override
		{
			if (static_cast<int32>(Index) >= Entries.Num())
				return rql::Value::missing();
			const FEntry& Entry = Entries[static_cast<int32>(Index)];
			UClass* Class = Entry.Class ? Entry.Class : Cast<UClass>(FUnrealMcpObjectRef::LoadObjectQuiet(UClass::StaticClass(), Entry.Path));
			return Class ? Types->WrapObject(Class->GetDefaultObject()) : rql::Value::null();
		}
		const rql::TypeDescriptor* type() const override { return Type; }

	private:
		FUnrealMcpRqlTypes* Types;
		TArray<FEntry> Entries;
		const rql::TypeDescriptor* Type;
	};

	rql::Value ErrorValue(const char* Code, const FString& Message)
	{
		return rql::Value::error(Code, UnrealMcpRqlToStd(Message));
	}

	rql::Value FindActor(const std::vector<rql::Value>& Args, FUnrealMcpRqlTypes& Types)
	{
		if (Args.size() != 1 || !Args[0].is_string() || Args[0].as_string().empty())
			return ErrorValue("type-mismatch", TEXT("unreal.find(label) expects an actor label, name or path"));
		const FString Key = UnrealMcpRqlFromStd(Args[0].as_string());
		AActor* Actor = FUnrealMcpObjectRef::ResolveActor(Key);
		return Actor ? Types.WrapObject(Actor) : ErrorValue("not-found", FString::Printf(TEXT("no actor with the label, name or path '%s'"), *Key));
	}

	rql::Value LoadAsset(const std::vector<rql::Value>& Args, FUnrealMcpRqlTypes& Types)
	{
		if (Args.size() != 1 || !Args[0].is_string())
			return ErrorValue("type-mismatch", TEXT("unreal.asset(path) expects an asset path such as \"/Game/Materials/M_Red\""));
		return Types.WrapObject(FUnrealMcpObjectRef::LoadObjectQuiet(UObject::StaticClass(), UnrealMcpRqlFromStd(Args[0].as_string())));
	}

	rql::Value FindAssets(const std::vector<rql::Value>& Args, FUnrealMcpRqlTypes& Types)
	{
		if (Args.empty() || Args.size() > 2 || !Args[0].is_string() || (Args.size() == 2 && !Args[1].is_string()))
			return ErrorValue("type-mismatch", TEXT("unreal.assets(class, path?) expects a class name such as \"Material\" and an optional content path"));
		FString ClassName = UnrealMcpRqlFromStd(Args[0].as_string()).TrimStartAndEnd();
		ClassName.RemoveFromStart(TEXT("t:"));
		UClass* Class = ClassName.IsEmpty() ? UObject::StaticClass() : FUnrealMcpObjectRef::ResolveClass(ClassName);
		if (!Class)
			return ErrorValue("not-found", FString::Printf(TEXT("no class '%s'"), *ClassName));
		return UnrealMcpRqlAssetList(Types, Class, Args.size() == 2 ? UnrealMcpRqlFromStd(Args[1].as_string()) : FString());
	}

	rql::Value ListClasses(const std::vector<rql::Value>& Args, const rql::TypeDescriptor* ClassInfoType)
	{
		const FString BaseName = !Args.empty() && Args[0].is_string() ? UnrealMcpRqlFromStd(Args[0].as_string()) : FString(TEXT("Actor"));
		const FString Filter = Args.size() > 1 && Args[1].is_string() ? UnrealMcpRqlFromStd(Args[1].as_string()) : FString();
		UClass* Base = FUnrealMcpObjectRef::ResolveClass(BaseName);
		if (!Base)
			return ErrorValue("not-found", FString::Printf(TEXT("no class '%s'"), *BaseName));
		TArray<UClass*> Candidates;
		GetDerivedClasses(Base, Candidates, /*bRecursive*/ true);
		Candidates.Add(Base);
		TArray<UClass*> Found;
		for (UClass* Class : Candidates)
		{
			if (IsListedClass(Class) && (Filter.IsEmpty() || Class->GetName().Contains(Filter)))
				Found.Add(Class);
		}
		SortAndCap(Found, 500, [](const UClass* A, const UClass* B) { return A->GetFName().LexicalLess(B->GetFName()); });
		std::vector<rql::Value> Items;
		Items.reserve(static_cast<size_t>(Found.Num()));
		for (UClass* Class : Found)
		{
			auto Info = std::make_shared<FClassInfo>();
			Info->Name = UnrealMcpRqlToStd(Class->GetName());
			Info->Path = UnrealMcpRqlToStd(Class->GetPathName());
			Info->Parent = Class->GetSuperClass() ? UnrealMcpRqlToStd(Class->GetSuperClass()->GetName()) : std::string();
			Info->Abstract = Class->HasAnyClassFlags(CLASS_Abstract);
			Info->Blueprint = Class->ClassGeneratedBy != nullptr;
			Items.push_back(rql::Value::structure(ClassInfoType, std::static_pointer_cast<void>(Info)));
		}
		return rql::Value::list_of(std::move(Items));
	}
}

using namespace UnrealMcpRqlCoreLocal;

rql::Value UnrealMcpRqlAssetList(FUnrealMcpRqlTypes& Types, UClass* Class, const FString& Path)
{
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	if (Class && Class != UObject::StaticClass())
	{
		Filter.ClassPaths.Add(Class->GetClassPathName());
		Filter.bRecursiveClasses = true;
	}
	Filter.PackagePaths.Add(FName(*(Path.IsEmpty() ? FString(TEXT("/Game")) : Path)));
	Filter.bRecursivePaths = true;
	TArray<FAssetData> Assets;
	Registry.GetAssets(Filter, Assets);
	// By package, then asset name (FName order: no string building per comparison); only the kept assets are sorted.
	SortAndCap(Assets, UnrealMcpRqlMaxAssets, [](const FAssetData& A, const FAssetData& B)
	{
		return A.PackageName != B.PackageName ? A.PackageName.LexicalLess(B.PackageName) : A.AssetName.LexicalLess(B.AssetName);
	});
	const rql::TypeDescriptor* Element = Types.ForClass(Class ? Class : UObject::StaticClass());
	return rql::Value::list(std::make_shared<FAssetList>(&Types, MoveTemp(Assets), Types.Registry().list_type(Element, true)));
}

rql::Value UnrealMcpRqlClassDefaultsList(FUnrealMcpRqlTypes& Types, UClass* Base)
{
	TArray<FClassDefaultsList::FEntry> Entries;
	// Keyed by the full class path: Blueprints of the same name in different folders are different classes.
	TSet<FTopLevelAssetPath> Seen;
	if (Base)
	{
		TArray<UClass*> Loaded;
		GetDerivedClasses(Base, Loaded, /*bRecursive*/ true);
		for (UClass* Class : Loaded)
		{
			Seen.Add(Class->GetClassPathName());
			if (IsListedClass(Class) && !Class->HasAnyClassFlags(CLASS_Abstract))
				Entries.Add({ Class->GetFName(), Class, FString() });
		}
		// The project's Blueprint classes that are not loaded yet (the asset registry knows them without loading).
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TSet<FTopLevelAssetPath> Derived;
		Registry.GetDerivedClassNames({ Base->GetClassPathName() }, TSet<FTopLevelAssetPath>(), Derived);
		for (const FTopLevelAssetPath& Path : Derived)
		{
			if (Seen.Contains(Path) || !Path.GetPackageName().ToString().StartsWith(TEXT("/Game")))
				continue;
			if (UClass* Class = FindObject<UClass>(Path)) // loaded after all (e.g. under another name)
			{
				if (IsListedClass(Class) && !Class->HasAnyClassFlags(CLASS_Abstract))
					Entries.Add({ Class->GetFName(), Class, FString() });
				continue;
			}
			Entries.Add({ Path.GetAssetName(), nullptr, Path.ToString() });
		}
	}
	SortAndCap(Entries, UnrealMcpRqlMaxAssets, [](const FClassDefaultsList::FEntry& A, const FClassDefaultsList::FEntry& B)
	{
		return A.Name.LexicalLess(B.Name);
	});
	const rql::TypeDescriptor* Element = Types.ForClass(Base ? Base : UObject::StaticClass());
	return rql::Value::list(std::make_shared<FClassDefaultsList>(&Types, MoveTemp(Entries), Types.Registry().list_type(Element, true)));
}

void UnrealMcpRqlConfigureCore(FUnrealMcpRqlSession& Session)
{
	FUnrealMcpRqlTypes& T = Session.Types();
	FUnrealMcpRqlTypes* Types = &T;

	T.AddApiClass(UUnrealMcpRqlEditorApi::StaticClass(), TEXT("Editor"));
	T.AddClassHook(UObject::StaticClass(), &ObjectMembers);
	T.AddClassHook(AActor::StaticClass(), &ActorMembers);
	T.AddClassHook(UActorComponent::StaticClass(), &ComponentMembers);
	T.AddClassHook(ULevel::StaticClass(), &LevelMembers);
	T.AddClassHook(UWorld::StaticClass(), &WorldMembers);
	T.AddClassHook(UMaterialInterface::StaticClass(), &MaterialMembers);
	T.AddClassHook(UUnrealMcpRqlEditorApi::StaticClass(), &EditorMembers);

	rql::TypeRegistry& Registry = T.Registry();
	Registry.structure<FLogEntry>("LogEntry")
		.member("Sequence", &FLogEntry::Sequence)
		.member("Time", &FLogEntry::Time)
		.member("Verbosity", &FLogEntry::Verbosity)
		.member("Category", &FLogEntry::Category)
		.member("Message", &FLogEntry::Message)
		.no_identity();
	Registry.structure<FClassInfo>("ClassInfo")
		.member("Name", &FClassInfo::Name)
		.member("Path", &FClassInfo::Path)
		.member("Parent", &FClassInfo::Parent)
		.member("Abstract", &FClassInfo::Abstract)
		.member("Blueprint", &FClassInfo::Blueprint)
		.no_identity();
	const rql::TypeDescriptor* LogEntryType = Registry.descriptor<FLogEntry>();
	const rql::TypeDescriptor* ClassInfoType = Registry.descriptor<FClassInfo>();
	T.AddNativeType(LogEntryType);
	T.AddNativeType(ClassInfoType);

	UnrealMcpRqlAddStructAdapters(Session);
	UnrealMcpRqlAddGameplayTags(Session);
	UnrealMcpRqlAddDataTables(Session);
	UnrealMcpRqlAddSettings(Session);
	UnrealMcpRqlAddBlueprints(Session);

	// Object references from text, as an edit field accepts them: an asset path ("/Game/Meshes/SM_Cube"), a class name
	// for class references ("PointLight"), an actor label / name / path for actors. Only coerce: the adapter shapes no
	// members, so UObject descriptors stay unadapted (their members fill lazily).
	{
		rql::TypeAdapter References;
		References.name = "UObject references";
		References.target = T.ForClass(UObject::StaticClass());
		References.coerce = [Types](const rql::Value& Value, const rql::TypeDescriptor& Target, rql::Value& Result, std::string& Reason) -> rql::AdapterOutcome
		{
			UClass* Wanted = Types->ClassOf(&Target);
			if (!Value.is_string() || !Wanted)
				return rql::AdapterOutcome::NotHandled;
			const FString Text = UnrealMcpRqlFromStd(Value.as_string()).TrimStartAndEnd();
			UObject* Found = FUnrealMcpObjectRef::ResolveReference(Wanted, Text);
			if (!Found)
			{
				Reason = UnrealMcpRqlToStd(FString::Printf(TEXT("no %s '%s'"), *Wanted->GetName(), *Text));
				return rql::AdapterOutcome::Failed;
			}
			Result = Types->WrapObject(Found);
			return rql::AdapterOutcome::Ok;
		};
		Session.AddAdapter(std::move(References));
	}

	const rql::TypeDescriptor* ActorType = T.ForClass(AActor::StaticClass());
	Session.AddSource(TEXT("actors"), ActorType, /*bList*/ true, TEXT("every actor of the editor world's loaded levels (hidden ones included)"), [Types]()
	{
		TArray<UObject*> Actors;
		if (UWorld* World = EditorWorld())
		{
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (IsValid(*It))
					Actors.Add(*It);
			}
		}
		return Types->WrapObjects(Actors);
	});
	Session.AddSource(TEXT("levels"), T.ForClass(ULevel::StaticClass()), /*bList*/ true, TEXT("the loaded levels of the editor world (Map, IsPersistent, IsCurrent, IsVisible, Actors)"), [Types]()
	{
		UWorld* World = EditorWorld();
		return Types->WrapObjects(World ? TArray<UObject*>(World->GetLevels()) : TArray<UObject*>());
	});
	Session.AddSource(TEXT("world"), T.ForClass(UWorld::StaticClass()), /*bList*/ false, TEXT("the editor world (Levels, PersistentLevel, CurrentLevel)"), [Types]()
	{
		return Types->WrapObject(EditorWorld());
	});
	Session.AddSource(TEXT("selection"), ActorType, /*bList*/ true, TEXT("the actors selected in the editor (write it: editor | set Selection = [...])"), [Types]()
	{
		return Types->WrapObjects(SelectedActors());
	});
	Session.AddSource(TEXT("editor"), T.ForClass(UUnrealMcpRqlEditorApi::StaticClass()), /*bList*/ false,
		TEXT("editor state (IsPlaying, IsPaused, Selection, World, CurrentLevel) and operations: call SpawnActor / OpenLevel / NewLevel / "
			"SaveLevel / SaveAll / UnloadSublevel / CreateAsset / CopyAsset / MoveAsset / DeleteAsset / CreateFolder / RefreshAssets / "
			"CreateMaterialInstance / ImportAsset / SaveAsset / ClearConsole / CreateDataTable / AddGameplayTag / RemoveGameplayTag / "
			"RenameGameplayTag"), [Types]()
	{
		return Types->WrapObject(GetMutableDefault<UUnrealMcpRqlEditorApi>());
	});
	const rql::TypeDescriptor* ConsoleType = Registry.list_type(LogEntryType, false);
	Session.AddSource(TEXT("console"), LogEntryType, /*bList*/ true, TEXT("editor log entries captured since startup, oldest first (Sequence, Time, Verbosity, Category, Message)"), [LogEntryType, ConsoleType]()
	{
		return rql::Value::list(std::make_shared<FConsoleList>(FUnrealMcpLogCollector::Get().Snapshot(), LogEntryType, ConsoleType));
	});

	Session.AddFunction(TEXT("unreal.find"), TEXT("the actor with this label, name or path in the editor world"),
		[Types](const std::vector<rql::Value>& Args) { return FindActor(Args, *Types); });
	Session.AddFunction(TEXT("unreal.asset"), TEXT("the asset at a path (\"/Game/Materials/M_Red\"), loaded; null when there is none"),
		[Types](const std::vector<rql::Value>& Args) { return LoadAsset(Args, *Types); });
	Session.AddFunction(TEXT("unreal.assets"), TEXT("assets of a class (\"Material\", \"StaticMesh\", \"Blueprint\") under a content path (default /Game), at most 200, loaded as the query reads them"),
		[Types](const std::vector<rql::Value>& Args) { return FindAssets(Args, *Types); });
	Session.AddFunction(TEXT("unreal.classes"), TEXT("classes deriving from a base (default \"Actor\") whose name contains a filter: Name Path Parent Abstract Blueprint (at most 500)"),
		[ClassInfoType](const std::vector<rql::Value>& Args) { return ListClasses(Args, ClassInfoType); });
}

UActorComponent* UnrealMcpRql::AddComponent(AActor* Actor, UClass* Class, const FString& Name, TFunction<void(UActorComponent*)> Configure)
{
	FString Error;
	UActorComponent* Component = UnrealMcpRqlCoreLocal::AddActorComponent(Actor, Class, Name, Configure, Error);
	if (!Component)
		Fail(Error);
	return Component;
}
