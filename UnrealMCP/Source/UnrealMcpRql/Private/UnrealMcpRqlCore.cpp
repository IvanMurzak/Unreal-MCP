// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRqlSession.h"
#include "UnrealMcpRqlEditorApi.h"
#include "UnrealMcpRqlSources.h"
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
#include "UObject/UObjectIterator.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace UnrealMcpRqlCoreLocal
{
	[[noreturn]] void Throw(const FString& Message)
	{
		throw std::runtime_error(UnrealMcpRqlToStd(Message));
	}

	rql::Value Str(const FString& Text)
	{
		return rql::Value::string(UnrealMcpRqlToStd(Text));
	}

	FString TextArg(const rql::Value& Value)
	{
		if (Value.is_string())
			return UnrealMcpRqlFromStd(Value.as_string());
		if (Value.is_null() || Value.is_missing())
			return FString();
		return UnrealMcpRqlFromStd(rql::to_text(Value));
	}

	bool BoolArg(const rql::Value& Value)
	{
		if (Value.is_bool())
			return Value.as_bool();
		if (Value.is_number())
			return Value.as_number() != 0.0;
		Throw(TEXT("expected true or false"));
	}

	template <typename T>
	T StructArg(FUnrealMcpRqlTypes& Types, UScriptStruct* Struct, const rql::Value& Value)
	{
		if (Value.is_struct() && Value.type() == Types.ForStruct(Struct) && Value.ptr())
			return *static_cast<const T*>(Value.ptr());
		T Parsed;
		FString Error;
		if (UnrealMcpRqlParseMathStruct(Struct, Value, &Parsed, Error))
			return Parsed;
		Throw(Error.IsEmpty() ? FString::Printf(TEXT("expected a %s"), *Struct->GetName()) : Error);
	}

	rql::MemberDescriptor Member(const char* Name, const rql::TypeDescriptor* Type, rql::Getter Get, rql::Setter Set = {},
		const char* Description = "", bool bOwning = false, bool bNullable = false)
	{
		rql::MemberDescriptor Result;
		Result.name = Name;
		Result.type = Type;
		Result.getter = MoveTemp(Get);
		Result.setter = MoveTemp(Set);
		Result.readonly = !Result.setter;
		Result.description = Description;
		Result.owning = bOwning;
		Result.nullable = bNullable;
		return Result;
	}

	rql::MethodDescriptor Method(const char* Name, std::vector<rql::ParamDescriptor> Params, const rql::TypeDescriptor* Result,
		std::function<rql::Value(void*, const std::vector<rql::Value>&)> Invoke, const char* Description)
	{
		rql::MethodDescriptor Out;
		Out.name = Name;
		Out.params = std::move(Params);
		Out.result = Result;
		Out.invoke = std::move(Invoke);
		Out.description = Description;
		return Out;
	}

	const rql::TypeDescriptor* StringType() { return rql::builtin::string_type(); }
	const rql::TypeDescriptor* BoolType() { return rql::builtin::bool_type(); }

	UWorld* EditorWorld()
	{
		return FUnrealMcpObjectRef::GetEditorWorld();
	}

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
				UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, Name,
					StructArg<FLinearColor>(*Types, TBaseStructure<FLinearColor>::Get(), Value));
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

		TArray<FName> Names() const
		{
			TArray<FName> Out;
			switch (Kind)
			{
			case EKind::Scalar: UMaterialEditingLibrary::GetScalarParameterNames(Material, Out); break;
			case EKind::Vector: UMaterialEditingLibrary::GetVectorParameterNames(Material, Out); break;
			case EKind::Texture: UMaterialEditingLibrary::GetTextureParameterNames(Material, Out); break;
			}
			return Out;
		}

		bool Resolve(const rql::Value& Key, FName& Out) const
		{
			const FString Wanted = TextArg(Key);
			for (const FName& Name : Names())
			{
				if (Name.ToString().Equals(Wanted, ESearchCase::IgnoreCase))
				{
					Out = Name;
					return true;
				}
			}
			return false;
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
		const rql::TypeDescriptor* Int = rql::builtin::number_type(true, true, 4);
		FUnrealMcpRqlTypes::SetMember(D, Member("Id", Int, [](void* Self)
		{
			return rql::Value::number(static_cast<double>(static_cast<UObject*>(Self)->GetUniqueID()), rql::builtin::number_type(true, true, 4));
		}, {}, "session id: $ref is Type#Id"));
		FUnrealMcpRqlTypes::SetMember(D, Member("Name", StringType(), [](void* Self) { return Str(static_cast<UObject*>(Self)->GetName()); }));
		FUnrealMcpRqlTypes::SetMember(D, Member("Path", StringType(), [](void* Self) { return Str(static_cast<UObject*>(Self)->GetPathName()); },
			{}, "object path (assets: \"/Game/Materials/M_Red.M_Red\")"));
		D.id_name = "Id";
		D.id_explicit = true;
		D.display_name = "Name";
		D.display_explicit = true;
	}

	void ActorMembers(FUnrealMcpRqlTypes& T, rql::TypeDescriptor& D)
	{
		FUnrealMcpRqlTypes* Types = &T;
		UScriptStruct* VectorStruct = TBaseStructure<FVector>::Get();
		UScriptStruct* RotatorStruct = TBaseStructure<FRotator>::Get();
		const rql::TypeDescriptor* Vector = T.ForStruct(VectorStruct);
		const rql::TypeDescriptor* Rotator = T.ForStruct(RotatorStruct);
		const rql::TypeDescriptor* ActorType = T.ForClass(AActor::StaticClass());
		const rql::TypeDescriptor* ComponentType = T.ForClass(UActorComponent::StaticClass());
		const rql::TypeDescriptor* LevelType = T.ForClass(ULevel::StaticClass());

		auto RequireRoot = [](AActor* Actor)
		{
			if (!Actor->GetRootComponent())
				Throw(FString::Printf(TEXT("%s has no root component (no transform)"), *Actor->GetActorLabel()));
		};

		FUnrealMcpRqlTypes::SetMember(D, Member("Label", StringType(),
			[](void* Self) { return Str(static_cast<AActor*>(Self)->GetActorLabel()); },
			[](void* Self, const rql::Value& Value) { static_cast<AActor*>(Self)->SetActorLabel(TextArg(Value)); },
			"the name shown in the Outliner"));
		FUnrealMcpRqlTypes::SetMember(D, Member("Location", Vector,
			[Types, VectorStruct](void* Self) { const FVector Value = static_cast<AActor*>(Self)->GetActorLocation(); return Types->WrapStruct(VectorStruct, &Value); },
			[Types, VectorStruct, RequireRoot](void* Self, const rql::Value& Value)
			{
				AActor* Actor = static_cast<AActor*>(Self);
				RequireRoot(Actor);
				Actor->SetActorLocation(StructArg<FVector>(*Types, VectorStruct, Value));
			}, "world location"));
		FUnrealMcpRqlTypes::SetMember(D, Member("Rotation", Rotator,
			[Types, RotatorStruct](void* Self) { const FRotator Value = static_cast<AActor*>(Self)->GetActorRotation(); return Types->WrapStruct(RotatorStruct, &Value); },
			[Types, RotatorStruct, RequireRoot](void* Self, const rql::Value& Value)
			{
				AActor* Actor = static_cast<AActor*>(Self);
				RequireRoot(Actor);
				Actor->SetActorRotation(StructArg<FRotator>(*Types, RotatorStruct, Value));
			}, "world rotation (pitch, yaw, roll)"));
		FUnrealMcpRqlTypes::SetMember(D, Member("Scale", Vector,
			[Types, VectorStruct](void* Self) { const FVector Value = static_cast<AActor*>(Self)->GetActorScale3D(); return Types->WrapStruct(VectorStruct, &Value); },
			[Types, VectorStruct, RequireRoot](void* Self, const rql::Value& Value)
			{
				AActor* Actor = static_cast<AActor*>(Self);
				RequireRoot(Actor);
				Actor->SetActorScale3D(StructArg<FVector>(*Types, VectorStruct, Value));
			}, "world scale"));
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

		rql::Value ZeroOffset = T.WrapStruct(VectorStruct, &FVector::ZeroVector);
		FUnrealMcpRqlTypes::AddMethod(D, Method("Duplicate", { FUnrealMcpRqlTypes::OptionalParam("Offset", Vector, ZeroOffset) }, ActorType,
			[Types, VectorStruct](void* Self, const std::vector<rql::Value>& Args) -> rql::Value
			{
				AActor* Source = static_cast<AActor*>(Self);
				UWorld* World = Source->GetWorld();
				if (!World)
					Throw(TEXT("the actor has no world"));
				const FVector Offset = StructArg<FVector>(*Types, VectorStruct, Args[0]);
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
				AActor* Actor = static_cast<AActor*>(Self);
				const FString ClassName = TextArg(Args[0]);
				UClass* Class = UnrealMcpRqlResolveClass(ClassName);
				if (!Class || !Class->IsChildOf(UActorComponent::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract))
					Throw(FString::Printf(TEXT("'%s' is not a component class (e.g. \"StaticMeshComponent\")"), *ClassName));
				const FString Requested = TextArg(Args[1]);
				FName Name = Requested.IsEmpty() ? MakeUniqueObjectName(Actor, Class, Class->GetFName()) : FName(*Requested);
				if (!Requested.IsEmpty() && StaticFindObjectFast(nullptr, Actor, Name))
					Throw(FString::Printf(TEXT("%s already has a component named '%s'"), *Actor->GetActorLabel(), *Requested));
				Actor->Modify();
				UActorComponent* Component = NewObject<UActorComponent>(Actor, Class, Name, RF_Transactional);
				if (!Component)
					Throw(FString::Printf(TEXT("could not create %s"), *Class->GetName()));
				Actor->AddInstanceComponent(Component);
				if (USceneComponent* Scene = Cast<USceneComponent>(Component))
				{
					if (USceneComponent* Root = Actor->GetRootComponent())
						Scene->AttachToComponent(Root, FAttachmentTransformRules::KeepRelativeTransform);
					else
						Actor->SetRootComponent(Scene);
				}
				Component->OnComponentCreated();
				Component->RegisterComponent();
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
				ULevel* Level = static_cast<ULevel*>(Self);
				if (!BoolArg(Value))
					Throw(TEXT("make another level current instead"));
				UEditorLevelUtils::MakeLevelCurrent(Level, /*bEvenIfLocked*/ false);
				if (!Level->GetWorld() || Level->GetWorld()->GetCurrentLevel() != Level)
					Throw(TEXT("the level could not be made current (locked?)"));
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
				UEditorLevelUtils::MakeLevelCurrent(Level, /*bEvenIfLocked*/ false);
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
				ULevel* Level = Cast<ULevel>(Types->AsObject(Value));
				if (!Level)
					Throw(TEXT("expected a level"));
				UEditorLevelUtils::MakeLevelCurrent(Level, /*bEvenIfLocked*/ false);
			}, "the level new actors spawn into", false, true));
		// The identity members of every object are noise on the operations object.
		D.id_name.clear();
		D.display_name.clear();
	}

	// ---------------------------------------------------------------- functions

	rql::Value Error(const char* Code, const FString& Message)
	{
		return rql::Value::error(Code, UnrealMcpRqlToStd(Message));
	}

	rql::Value FindActor(const std::vector<rql::Value>& Args, FUnrealMcpRqlTypes& Types)
	{
		if (Args.size() != 1 || !Args[0].is_string() || Args[0].as_string().empty())
			return Error("type-mismatch", TEXT("unreal.find(label) expects an actor label, name or path"));
		const FString Key = UnrealMcpRqlFromStd(Args[0].as_string());
		AActor* Actor = FUnrealMcpObjectRef::ResolveActor(Key);
		return Actor ? Types.WrapObject(Actor) : Error("not-found", FString::Printf(TEXT("no actor with the label, name or path '%s'"), *Key));
	}

	rql::Value LoadAsset(const std::vector<rql::Value>& Args, FUnrealMcpRqlTypes& Types)
	{
		if (Args.size() != 1 || !Args[0].is_string())
			return Error("type-mismatch", TEXT("unreal.asset(path) expects an asset path such as \"/Game/Materials/M_Red\""));
		return Types.WrapObject(UnrealMcpRqlLoadObject(UObject::StaticClass(), UnrealMcpRqlFromStd(Args[0].as_string())));
	}

	rql::Value FindAssets(const std::vector<rql::Value>& Args, FUnrealMcpRqlTypes& Types)
	{
		if (Args.empty() || Args.size() > 2 || !Args[0].is_string() || (Args.size() == 2 && !Args[1].is_string()))
			return Error("type-mismatch", TEXT("unreal.assets(class, path?) expects a class name such as \"Material\" and an optional content path"));
		FString ClassName = UnrealMcpRqlFromStd(Args[0].as_string()).TrimStartAndEnd();
		ClassName.RemoveFromStart(TEXT("t:"));
		UClass* Class = ClassName.IsEmpty() ? UObject::StaticClass() : UnrealMcpRqlResolveClass(ClassName);
		if (!Class)
			return Error("not-found", FString::Printf(TEXT("no class '%s'"), *ClassName));
		return UnrealMcpRqlAssetList(Types, Class, Args.size() == 2 ? UnrealMcpRqlFromStd(Args[1].as_string()) : FString());
	}

	rql::Value ListClasses(const std::vector<rql::Value>& Args, FUnrealMcpRqlTypes& Types, const rql::TypeDescriptor* ClassInfoType)
	{
		const FString BaseName = !Args.empty() && Args[0].is_string() ? UnrealMcpRqlFromStd(Args[0].as_string()) : FString(TEXT("Actor"));
		const FString Filter = Args.size() > 1 && Args[1].is_string() ? UnrealMcpRqlFromStd(Args[1].as_string()) : FString();
		UClass* Base = UnrealMcpRqlResolveClass(BaseName);
		if (!Base)
			return Error("not-found", FString::Printf(TEXT("no class '%s'"), *BaseName));
		TArray<UClass*> Found;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (!Class->IsChildOf(Base) || Class->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists | CLASS_Hidden))
				continue;
			const FString Name = Class->GetName();
			if (Name.StartsWith(TEXT("SKEL_")) || Name.StartsWith(TEXT("REINST_")) || Name.StartsWith(TEXT("TRASHCLASS_")))
				continue;
			if (!Filter.IsEmpty() && !Name.Contains(Filter))
				continue;
			Found.Add(Class);
		}
		Found.Sort([](const UClass& A, const UClass& B) { return A.GetName() < B.GetName(); });
		std::vector<rql::Value> Items;
		for (UClass* Class : Found)
		{
			if (Items.size() >= 500)
				break;
			auto Info = std::make_shared<FClassInfo>();
			Info->Name = UnrealMcpRqlToStd(Class->GetName());
			Info->Path = UnrealMcpRqlToStd(Class->GetPathName());
			Info->Parent = Class->GetSuperClass() ? UnrealMcpRqlToStd(Class->GetSuperClass()->GetName()) : std::string();
			Info->Abstract = Class->HasAnyClassFlags(CLASS_Abstract);
			Info->Blueprint = Class->ClassGeneratedBy != nullptr;
			Items.push_back(rql::Value::structure(ClassInfoType, std::static_pointer_cast<void>(Info)));
		}
		(void)Types;
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
	Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.GetObjectPathString() < B.GetObjectPathString(); });
	if (Assets.Num() > UnrealMcpRqlMaxAssets)
		Assets.SetNum(UnrealMcpRqlMaxAssets);
	const rql::TypeDescriptor* Element = Types.ForClass(Class ? Class : UObject::StaticClass());
	return rql::Value::list(std::make_shared<FAssetList>(&Types, MoveTemp(Assets), Types.Registry().list_type(Element, true)));
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

	Session.AddSource(TEXT("actors"), TEXT("list of Actor"), TEXT("every actor of the editor world's loaded levels (hidden ones included)"), [Types]()
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
	Session.AddSource(TEXT("levels"), TEXT("list of Level"), TEXT("the loaded levels of the editor world (Map, IsPersistent, IsCurrent, IsVisible, Actors)"), [Types]()
	{
		UWorld* World = EditorWorld();
		return Types->WrapObjects(World ? TArray<UObject*>(World->GetLevels()) : TArray<UObject*>());
	});
	Session.AddSource(TEXT("world"), TEXT("World"), TEXT("the editor world (Levels, PersistentLevel, CurrentLevel)"), [Types]()
	{
		return Types->WrapObject(EditorWorld());
	});
	Session.AddSource(TEXT("selection"), TEXT("list of Actor"), TEXT("the actors selected in the editor (write it: editor | set Selection = [...])"), [Types]()
	{
		return Types->WrapObjects(SelectedActors());
	});
	Session.AddSource(TEXT("editor"), TEXT("Editor"),
		TEXT("editor state (IsPlaying, IsPaused, Selection, World, CurrentLevel) and operations: call SpawnActor / OpenLevel / NewLevel / "
			"SaveLevel / SaveAll / UnloadSublevel / CopyAsset / MoveAsset / DeleteAsset / CreateFolder / RefreshAssets / "
			"CreateMaterialInstance / ImportAsset / SaveAsset / ClearConsole"), [Types]()
	{
		return Types->WrapObject(GetMutableDefault<UUnrealMcpRqlEditorApi>());
	});
	Session.AddSource(TEXT("console"), TEXT("list of LogEntry"), TEXT("editor log entries captured since startup, oldest first (Sequence, Time, Verbosity, Category, Message)"), [LogEntryType]()
	{
		const TArray<FUnrealMcpLogEntry> Entries = FUnrealMcpLogCollector::Get().Snapshot(ELogVerbosity::All, FString(), FString(), 0);
		std::vector<rql::Value> Items;
		Items.reserve(static_cast<size_t>(Entries.Num()));
		for (const FUnrealMcpLogEntry& Entry : Entries)
		{
			auto Item = std::make_shared<FLogEntry>();
			Item->Sequence = Entry.Sequence;
			Item->Time = UnrealMcpRqlToStd(Entry.Timestamp.ToIso8601());
			Item->Verbosity = UnrealMcpRqlToStd(FUnrealMcpLogCollector::VerbosityToString(Entry.Verbosity));
			Item->Category = UnrealMcpRqlToStd(Entry.Category.ToString());
			Item->Message = UnrealMcpRqlToStd(Entry.Message);
			Items.push_back(rql::Value::structure(LogEntryType, std::static_pointer_cast<void>(Item)));
		}
		return rql::Value::list_of(std::move(Items));
	});

	Session.AddFunction(TEXT("unreal.find"), TEXT("the actor with this label, name or path in the editor world"),
		[Types](const std::vector<rql::Value>& Args) { return FindActor(Args, *Types); });
	Session.AddFunction(TEXT("unreal.asset"), TEXT("the asset at a path (\"/Game/Materials/M_Red\"), loaded; null when there is none"),
		[Types](const std::vector<rql::Value>& Args) { return LoadAsset(Args, *Types); });
	Session.AddFunction(TEXT("unreal.assets"), TEXT("assets of a class (\"Material\", \"StaticMesh\", \"Blueprint\") under a content path (default /Game), at most 200, loaded as the query reads them"),
		[Types](const std::vector<rql::Value>& Args) { return FindAssets(Args, *Types); });
	Session.AddFunction(TEXT("unreal.classes"), TEXT("classes deriving from a base (default \"Actor\") whose name contains a filter: Name Path Parent Abstract Blueprint (at most 500)"),
		[Types, ClassInfoType](const std::vector<rql::Value>& Args) { return ListClasses(Args, *Types, ClassInfoType); });
}
