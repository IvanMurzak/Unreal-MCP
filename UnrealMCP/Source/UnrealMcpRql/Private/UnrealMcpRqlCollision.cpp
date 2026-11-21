// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRqlEditorApi.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealMcpRqlSession.h"
#include "UnrealMcpRqlSources.h"
#include "UnrealMcpRqlUtil.h"

#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/EngineTypes.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "UObject/UnrealType.h"

#include <memory>

/**
 * Collision as the Details panel's Collision section and Project Settings > Collision edit it (docs/rql.md "Collision"):
 * a primitive component's CollisionProfile, CollisionEnabled, ObjectType and Responses (channel name -> Ignore / Overlap /
 * Block), by the channel names the editor shows ("WorldStatic", "Pawn", a project channel such as "Debris"), and the
 * editor operation AddCollisionChannel. Writing ObjectType or a response switches the component to the Custom profile, as
 * the panel does.
 */
namespace UnrealMcpRqlCollisionLocal
{
	using namespace UnrealMcpRqlUtil;

	struct FChannel
	{
		ECollisionChannel Channel;
		FName Name;
		bool bTrace;
	};

	/** The project's custom channels (Project Settings > Collision): UCollisionProfile keeps them private, reflection reads them. */
	TArray<FCustomChannelSetup>& CustomChannelSetups()
	{
		static FArrayProperty* const Property = []() -> FArrayProperty*
		{
			FArrayProperty* Found = FindFProperty<FArrayProperty>(UCollisionProfile::StaticClass(), TEXT("DefaultChannelResponses"));
			return Found && Found->Inner->GetCPPType() == TEXT("FCustomChannelSetup") ? Found : nullptr;
		}();
		if (!Property)
			Throw(TEXT("this engine version keeps its collision channels where Unreal-MCP cannot read them"));
		return *Property->ContainerPtrToValuePtr<TArray<FCustomChannelSetup>>(UCollisionProfile::Get());
	}

	/** The engine's channels and the project's custom ones, by the names the editor shows. */
	TArray<FChannel> Channels()
	{
		const UCollisionProfile* Profile = UCollisionProfile::Get();
		TArray<FChannel> Out;
		const ECollisionChannel Engine[] = { ECC_WorldStatic, ECC_WorldDynamic, ECC_Pawn, ECC_Visibility, ECC_Camera, ECC_PhysicsBody,
			ECC_Vehicle, ECC_Destructible };
		for (const ECollisionChannel Channel : Engine)
			Out.Add({ Channel, Profile->ReturnChannelNameFromContainerIndex(Channel), Channel == ECC_Visibility || Channel == ECC_Camera });
		for (const FCustomChannelSetup& Setup : CustomChannelSetups())
			Out.Add({ Setup.Channel.GetValue(), Setup.Name, Setup.bTraceType });
		return Out;
	}

	FString ChannelNames(const TArray<FChannel>& All, bool bObjectOnly)
	{
		TArray<FString> Names;
		for (const FChannel& Channel : All)
		{
			if (!bObjectOnly || !Channel.bTrace)
				Names.Add(Channel.Name.ToString());
		}
		return FString::Join(Names, TEXT(", "));
	}

	/** The channel of @p All named "Debris", "WorldStatic", "ECC_WorldStatic" or "ECC_GameTraceChannel1" (a custom channel's enum name). */
	const FChannel* FindChannel(const TArray<FChannel>& All, const FString& Text)
	{
		FString Wanted = Text.TrimStartAndEnd();
		Wanted.RemoveFromStart(TEXT("ECC_"));
		const UEnum* Enum = StaticEnum<ECollisionChannel>();
		return All.FindByPredicate([&Wanted, Enum](const FChannel& Channel)
		{
			FString EnumName = Enum ? Enum->GetNameStringByValue(Channel.Channel) : FString();
			EnumName.RemoveFromStart(TEXT("ECC_"));
			return Channel.Name.ToString().Equals(Wanted, ESearchCase::IgnoreCase) || EnumName.Equals(Wanted, ESearchCase::IgnoreCase);
		});
	}

	FChannel ResolveChannel(const FString& Text, bool bObjectOnly)
	{
		const TArray<FChannel> All = Channels();
		const FChannel* Found = FindChannel(All, Text);
		if (!Found)
			Throw(FString::Printf(TEXT("no collision channel '%s' (channels: %s; add one with editor | call AddCollisionChannel(\"%s\"))"),
				*Text, *ChannelNames(All, bObjectOnly), *Text.TrimStartAndEnd()));
		if (bObjectOnly && Found->bTrace)
			Throw(FString::Printf(TEXT("'%s' is a trace channel, not an object type (object types: %s)"), *Text, *ChannelNames(All, true)));
		return *Found;
	}

	FString ChannelName(ECollisionChannel Channel)
	{
		return UCollisionProfile::Get()->ReturnChannelNameFromContainerIndex(Channel).ToString();
	}

	const TCHAR* ResponseName(ECollisionResponse Response)
	{
		switch (Response)
		{
		case ECR_Ignore: return TEXT("Ignore");
		case ECR_Overlap: return TEXT("Overlap");
		default: return TEXT("Block");
		}
	}

	const TCHAR* const ResponseNames = TEXT("Ignore, Overlap, Block");

	bool TryParseResponse(const FString& Text, ECollisionResponse& Out)
	{
		FString Name = Text.TrimStartAndEnd();
		Name.RemoveFromStart(TEXT("ECR_"));
		for (const ECollisionResponse Response : { ECR_Ignore, ECR_Overlap, ECR_Block })
		{
			if (Name.Equals(ResponseName(Response), ESearchCase::IgnoreCase))
			{
				Out = Response;
				return true;
			}
		}
		return false;
	}

	ECollisionResponse ResolveResponse(const rql::Value& Value)
	{
		ECollisionResponse Response = ECR_Block;
		if (!TryParseResponse(TextArg(Value), Response))
			Throw(FString::Printf(TEXT("'%s' is not a collision response (%s)"), *TextArg(Value), ResponseNames));
		return Response;
	}

	/**
	 * Before a collision edit: undoable (a construction-script component through its actor, as the editor records it), and a
	 * static mesh component stops using its mesh's default collision, as the Details panel's Collision section does on any
	 * edit (otherwise the game applies the mesh's collision again on load).
	 */
	void BeginCollisionEdit(UPrimitiveComponent& Component)
	{
		AActor* Owner = Component.GetOwner();
		if (Owner && Component.IsCreatedByConstructionScript())
			Owner->Modify();
		Component.Modify();
		if (UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(&Component))
			Mesh->bUseDefaultCollision = false;
	}

	/** The component a member reads or writes: the one that replaced it during this call when a construction rerun did. */
	UPrimitiveComponent& LivePrimitive(FUnrealMcpRqlTypes& T, void* Self)
	{
		UPrimitiveComponent* Component = Cast<UPrimitiveComponent>(T.LiveObject(static_cast<UObject*>(Self)));
		if (!IsValid(Component))
			Throw(TEXT("the component is gone"));
		return *Component;
	}

	/**
	 * One collision member write through the engine setters (@p Apply), with the BodyInstance write path's rules: a value that
	 * is already there changes nothing; a value the body had before this call's first collision write (the rollback of a
	 * failed mutation) restores that whole state, profile and default collision included (FUnrealMcpRqlTypes::FirstCollision);
	 * a Blueprint's component template passes the change on to the placed components that still hold its old value.
	 * @p Read gives the member's value of a body, as the member reads it.
	 */
	void WriteCollision(FUnrealMcpRqlTypes& T, void* Self, const FString& Wanted, TFunctionRef<FString(const FBodyInstance&)> Read,
		TFunctionRef<void(UPrimitiveComponent&)> Apply)
	{
		UPrimitiveComponent& Component = LivePrimitive(T, Self);
		const FString Old = Read(Component.BodyInstance);
		if (Old == Wanted)
			return;
		TArray<UPrimitiveComponent*> Targets{ &Component };
		if (Component.HasAnyFlags(RF_ArchetypeObject) && !Component.HasAnyFlags(RF_ClassDefaultObject))
		{
			TArray<UObject*> Instances;
			Component.GetArchetypeInstances(Instances);
			for (UObject* Instance : Instances)
			{
				UPrimitiveComponent* Placed = Cast<UPrimitiveComponent>(Instance);
				if (IsValid(Placed) && Placed->GetArchetype() == &Component && !(Placed->GetWorld() && Placed->GetWorld()->IsGameWorld())
					&& Read(Placed->BodyInstance) == Old)
					Targets.Add(Placed);
			}
		}
		const bool bRestore = Read(T.FirstCollision(&Component)) == Wanted;
		for (UPrimitiveComponent* Target : Targets)
		{
			if (bRestore)
			{
				T.RestoreFirstCollision(Target);
				if (Read(Target->BodyInstance) == Wanted)
					continue;
			}
			T.FirstCollision(Target);
			BeginCollisionEdit(*Target);
			Apply(*Target);
		}
	}

	/** Responses: channel name -> Ignore / Overlap / Block, writable per channel (the panel's Collision Responses grid). */
	class FResponses final : public rql::IMapAdapter
	{
	public:
		FResponses(FUnrealMcpRqlTypes* InTypes, void* InSelf, const rql::TypeDescriptor* InType) : Types(InTypes), Self(InSelf), Type(InType) {}

		size_t size() const override { return static_cast<size_t>(Channels().Num()); }
		void for_each(const std::function<bool(const rql::Value&, const rql::Value&)>& Fn) const override
		{
			const FBodyInstance& Body = LivePrimitive(*Types, Self).BodyInstance;
			for (const FChannel& Channel : Channels())
			{
				if (!Fn(Str(Channel.Name.ToString()), Str(ResponseName(Body.GetResponseToChannel(Channel.Channel)))))
					return;
			}
		}
		rql::MapLookup find(const rql::Value& Key, rql::Value& Out) const override
		{
			if (!Key.is_string())
				return rql::MapLookup::BadKey;
			const FChannel* Channel = FindChannel(Channels(), TextArg(Key));
			if (!Channel)
				return rql::MapLookup::NotFound;
			Out = Str(ResponseName(LivePrimitive(*Types, Self).BodyInstance.GetResponseToChannel(Channel->Channel)));
			return rql::MapLookup::Found;
		}
		const rql::TypeDescriptor* type() const override { return Type; }
		const void* identity() const override { return this; }
		bool writable() const override { return true; }
		bool fixed_keys() const override { return true; }
		bool accepts_key(const rql::Value& Key) const override { return Key.is_string(); }
		void set(const rql::Value& Key, const rql::Value& Value) override
		{
			const ECollisionChannel Channel = ResolveChannel(TextArg(Key), /*bObjectOnly*/ false).Channel;
			const ECollisionResponse Response = ResolveResponse(Value);
			WriteCollision(*Types, Self, ResponseName(Response),
				[Channel](const FBodyInstance& Body) { return FString(ResponseName(Body.GetResponseToChannel(Channel))); },
				[Channel, Response](UPrimitiveComponent& Component) { Component.SetCollisionResponseToChannel(Channel, Response); });
		}

	private:
		FUnrealMcpRqlTypes* Types;
		void* Self;
		const rql::TypeDescriptor* Type;
	};

	const TCHAR* EnabledName(ECollisionEnabled::Type Enabled)
	{
		switch (Enabled)
		{
		case ECollisionEnabled::NoCollision: return TEXT("NoCollision");
		case ECollisionEnabled::QueryOnly: return TEXT("QueryOnly");
		case ECollisionEnabled::PhysicsOnly: return TEXT("PhysicsOnly");
		case ECollisionEnabled::QueryAndPhysics: return TEXT("QueryAndPhysics");
		case ECollisionEnabled::ProbeOnly: return TEXT("ProbeOnly");
		case ECollisionEnabled::QueryAndProbe: return TEXT("QueryAndProbe");
		default: return TEXT("Unknown");
		}
	}

	/** The component's own collision mode, not the one its actor's collision switch overrides (what the panel shows). */
	FString OwnEnabled(const FBodyInstance& Body)
	{
		return EnabledName(Body.GetCollisionEnabled(/*bCheckOwner*/ false));
	}

	FString ProfileOf(const FBodyInstance& Body)
	{
		return Body.GetCollisionProfileName().ToString();
	}

	FString ObjectTypeOf(const FBodyInstance& Body)
	{
		return ChannelName(Body.GetObjectType());
	}

	void PrimitiveMembers(FUnrealMcpRqlTypes& T, rql::TypeDescriptor& D)
	{
		FUnrealMcpRqlTypes* Types = &T;
		const rql::TypeDescriptor* String = rql::builtin::string_type();
		FUnrealMcpRqlTypes::SetMember(D, Member("CollisionProfile", String,
			[Types](void* Self) { return Str(ProfileOf(LivePrimitive(*Types, Self).BodyInstance)); },
			[Types](void* Self, const rql::Value& Value)
			{
				const FName Name(*TextArg(Value).TrimStartAndEnd());
				FCollisionResponseTemplate Template;
				if (Name != UCollisionProfile::CustomCollisionProfileName && !UCollisionProfile::Get()->GetProfileTemplate(Name, Template))
				{
					TArray<TSharedPtr<FName>> Names;
					UCollisionProfile::GetProfileNames(Names);
					TArray<FString> Shown;
					for (const TSharedPtr<FName>& Known : Names)
						Shown.Add(Known->ToString());
					Throw(FString::Printf(TEXT("no collision profile '%s' (profiles: %s)"), *Name.ToString(), *FString::Join(Shown, TEXT(", "))));
				}
				WriteCollision(*Types, Self, Name.ToString(), &ProfileOf,
					[Name](UPrimitiveComponent& Component) { Component.SetCollisionProfileName(Name); });
			}, "the collision preset (BlockAll, OverlapAllDynamic, Pawn, NoCollision, a project profile; Custom after an ObjectType / Responses edit)"));
		FUnrealMcpRqlTypes::SetMember(D, Member("ObjectType", String,
			[Types](void* Self) { return Str(ObjectTypeOf(LivePrimitive(*Types, Self).BodyInstance)); },
			[Types](void* Self, const rql::Value& Value)
			{
				const ECollisionChannel Wanted = ResolveChannel(TextArg(Value), /*bObjectOnly*/ true).Channel;
				WriteCollision(*Types, Self, ChannelName(Wanted), &ObjectTypeOf,
					[Wanted](UPrimitiveComponent& Component) { Component.SetCollisionObjectType(Wanted); });
			}, "the collision object type by channel name (WorldStatic, WorldDynamic, Pawn, PhysicsBody, a project channel such as \"Debris\"); writing it makes the profile Custom"));
		FUnrealMcpRqlTypes::SetMember(D, Member("CollisionEnabled", String,
			[Types](void* Self) { return Str(OwnEnabled(LivePrimitive(*Types, Self).BodyInstance)); },
			[Types](void* Self, const rql::Value& Value)
			{
				const FString Text = TextArg(Value).TrimStartAndEnd();
				for (const ECollisionEnabled::Type Enabled : { ECollisionEnabled::NoCollision, ECollisionEnabled::QueryOnly, ECollisionEnabled::PhysicsOnly,
					ECollisionEnabled::QueryAndPhysics, ECollisionEnabled::ProbeOnly, ECollisionEnabled::QueryAndProbe })
				{
					if (Text.Equals(EnabledName(Enabled), ESearchCase::IgnoreCase))
					{
						WriteCollision(*Types, Self, EnabledName(Enabled), &OwnEnabled,
							[Enabled](UPrimitiveComponent& Component) { Component.SetCollisionEnabled(Enabled); });
						return;
					}
				}
				Throw(FString::Printf(TEXT("'%s' is not a collision mode (NoCollision, QueryOnly, PhysicsOnly, QueryAndPhysics, ProbeOnly, QueryAndProbe)"), *Text));
			}, "NoCollision, QueryOnly, PhysicsOnly, QueryAndPhysics, ProbeOnly or QueryAndProbe (the component's own; its actor's collision switch can override it)"));
		const rql::TypeDescriptor* MapType = T.Registry().map_type(String, String, false);
		FUnrealMcpRqlTypes::SetMember(D, Member("Responses", MapType, [Types, MapType](void* Self)
		{
			return rql::Value::map(std::make_shared<FResponses>(Types, Self, MapType));
		}, {}, "response to each collision channel by name: Responses[\"Debris\"] = \"Ignore\" (Ignore, Overlap, Block); writing one makes the profile Custom"));
	}
}

FString UUnrealMcpRqlEditorApi::AddCollisionChannel(const FString& Name, const FString& DefaultResponse, bool bTraceChannel)
{
	using namespace UnrealMcpRqlCollisionLocal;
	using UnrealMcpRql::Failed;
	const FString Trimmed = Name.TrimStartAndEnd();
	if (Trimmed.IsEmpty() || FName(*Trimmed).IsNone() || !FName::IsValidXName(Trimmed, INVALID_OBJECTNAME_CHARACTERS))
		return Failed<FString>(FString::Printf(TEXT("'%s' is not a valid channel name"), *Name));
	ECollisionResponse Response = ECR_Block;
	if (!TryParseResponse(DefaultResponse, Response))
		return Failed<FString>(FString::Printf(TEXT("'%s' is not a collision response (%s)"), *DefaultResponse, ResponseNames));
	TArray<FChannel> All;
	try
	{
		All = Channels();
	}
	catch (const std::exception& Error)
	{
		return Failed<FString>(UnrealMcpRqlFromStd(Error.what()));
	}
	if (const FChannel* Taken = FindChannel(All, Trimmed))
		return Failed<FString>(FString::Printf(TEXT("the project already has a collision channel '%s'"), *Taken->Name.ToString()));
	TArray<FCustomChannelSetup>& Setups = CustomChannelSetups();
	ECollisionChannel Free = ECC_MAX;
	for (int32 Index = ECC_GameTraceChannel1; Index <= ECC_GameTraceChannel18 && Free == ECC_MAX; ++Index)
	{
		if (!Setups.ContainsByPredicate([Index](const FCustomChannelSetup& Setup) { return Setup.Channel == Index; }))
			Free = static_cast<ECollisionChannel>(Index);
	}
	if (Free == ECC_MAX)
		return Failed<FString>(TEXT("all 18 custom collision channels are in use"));

	UCollisionProfile* Profile = UCollisionProfile::Get();
	FCustomChannelSetup Setup;
	Setup.Channel = Free;
	Setup.DefaultResponse = Response;
	Setup.bTraceType = bTraceChannel;
	Setup.bStaticObject = false;
	Setup.Name = FName(*Trimmed);
	Setups.Add(Setup);
	// As Project Settings > Collision saves a new channel: DefaultEngine.ini, then the profiles reload so the name resolves.
	const FString Ini = FPaths::ConvertRelativePathToFull(Profile->GetDefaultConfigFilename());
	IPlatformFile& File = FPlatformFileManager::Get().GetPlatformFile();
	if (File.FileExists(*Ini) && File.IsReadOnly(*Ini))
		File.SetReadOnly(*Ini, false);
	if (!Profile->TryUpdateDefaultConfigFile())
	{
		Setups.Pop();
		return Failed<FString>(FString::Printf(TEXT("could not write %s; the channel was not added"), *Ini));
	}
	Profile->LoadProfileConfig(/*bForceInit*/ true);
	const UEnum* Enum = StaticEnum<ECollisionChannel>();
	return Enum ? Enum->GetNameStringByValue(Free) : FString::FromInt(Free);
}

void UnrealMcpRqlAddCollision(FUnrealMcpRqlSession& Session)
{
	Session.Types().AddClassHook(UPrimitiveComponent::StaticClass(), &UnrealMcpRqlCollisionLocal::PrimitiveMembers);
}
