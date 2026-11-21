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
		FArrayProperty* Property = FindFProperty<FArrayProperty>(UCollisionProfile::StaticClass(), TEXT("DefaultChannelResponses"));
		if (!Property || Property->Inner->GetCPPType() != TEXT("FCustomChannelSetup"))
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

	FString ChannelNames(bool bObjectOnly)
	{
		TArray<FString> Names;
		for (const FChannel& Channel : Channels())
		{
			if (!bObjectOnly || !Channel.bTrace)
				Names.Add(Channel.Name.ToString());
		}
		return FString::Join(Names, TEXT(", "));
	}

	/** "Debris", "WorldStatic", "ECC_WorldStatic" or "ECC_GameTraceChannel1" (the enum name of a custom channel). */
	FChannel ResolveChannel(const FString& Text, bool bObjectOnly)
	{
		FString Wanted = Text.TrimStartAndEnd();
		Wanted.RemoveFromStart(TEXT("ECC_"));
		const UEnum* Enum = StaticEnum<ECollisionChannel>();
		for (const FChannel& Channel : Channels())
		{
			FString EnumName = Enum ? Enum->GetNameStringByValue(Channel.Channel) : FString();
			EnumName.RemoveFromStart(TEXT("ECC_"));
			if (Channel.Name.ToString().Equals(Wanted, ESearchCase::IgnoreCase) || EnumName.Equals(Wanted, ESearchCase::IgnoreCase))
			{
				if (bObjectOnly && Channel.bTrace)
					Throw(FString::Printf(TEXT("'%s' is a trace channel, not an object type (object types: %s)"), *Text, *ChannelNames(true)));
				return Channel;
			}
		}
		Throw(FString::Printf(TEXT("no collision channel '%s' (channels: %s; add one with editor | call AddCollisionChannel(\"%s\"))"),
			*Text, *ChannelNames(bObjectOnly), *Wanted));
	}

	FString ChannelName(ECollisionChannel Channel)
	{
		return UCollisionProfile::Get()->ReturnChannelNameFromContainerIndex(Channel).ToString();
	}

	ECollisionResponse ResolveResponse(const rql::Value& Value)
	{
		FString Text = TextArg(Value).TrimStartAndEnd();
		Text.RemoveFromStart(TEXT("ECR_"));
		if (Text.Equals(TEXT("Ignore"), ESearchCase::IgnoreCase))
			return ECR_Ignore;
		if (Text.Equals(TEXT("Overlap"), ESearchCase::IgnoreCase))
			return ECR_Overlap;
		if (Text.Equals(TEXT("Block"), ESearchCase::IgnoreCase))
			return ECR_Block;
		Throw(FString::Printf(TEXT("'%s' is not a collision response (Ignore, Overlap, Block)"), *TextArg(Value)));
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

	/**
	 * Before a collision edit: undoable, and a static mesh component stops using its mesh's default collision, as the Details
	 * panel's Collision section does on any edit (otherwise the game applies the mesh's collision again on load).
	 */
	void BeginCollisionEdit(UPrimitiveComponent* Component)
	{
		Component->Modify();
		if (UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Component))
			Mesh->bUseDefaultCollision = false;
	}

	/** Responses: channel name -> Ignore / Overlap / Block, writable per channel (the panel's Collision Responses grid). */
	class FResponses final : public rql::IMapAdapter
	{
	public:
		FResponses(UPrimitiveComponent* InComponent, const rql::TypeDescriptor* InType) : Component(InComponent), Type(InType) {}

		size_t size() const override { return static_cast<size_t>(Channels().Num()); }
		void for_each(const std::function<bool(const rql::Value&, const rql::Value&)>& Fn) const override
		{
			for (const FChannel& Channel : Channels())
			{
				if (!Fn(Str(Channel.Name.ToString()), Str(ResponseName(Component->GetCollisionResponseToChannel(Channel.Channel)))))
					return;
			}
		}
		rql::MapLookup find(const rql::Value& Key, rql::Value& Out) const override
		{
			if (!Key.is_string())
				return rql::MapLookup::BadKey;
			for (const FChannel& Channel : Channels())
			{
				if (Channel.Name.ToString().Equals(TextArg(Key), ESearchCase::IgnoreCase))
				{
					Out = Str(ResponseName(Component->GetCollisionResponseToChannel(Channel.Channel)));
					return rql::MapLookup::Found;
				}
			}
			return rql::MapLookup::NotFound;
		}
		const rql::TypeDescriptor* type() const override { return Type; }
		const void* identity() const override { return this; }
		bool writable() const override { return true; }
		bool fixed_keys() const override { return true; }
		bool accepts_key(const rql::Value& Key) const override { return Key.is_string(); }
		void set(const rql::Value& Key, const rql::Value& Value) override
		{
			const FChannel Channel = ResolveChannel(TextArg(Key), /*bObjectOnly*/ false);
			const ECollisionResponse Response = ResolveResponse(Value);
			if (Component->GetCollisionResponseToChannel(Channel.Channel) == Response)
				return;
			BeginCollisionEdit(Component);
			Component->SetCollisionResponseToChannel(Channel.Channel, Response);
		}

	private:
		UPrimitiveComponent* Component;
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

	void PrimitiveMembers(FUnrealMcpRqlTypes& T, rql::TypeDescriptor& D)
	{
		const rql::TypeDescriptor* String = rql::builtin::string_type();
		auto Primitive = [](void* Self) { return static_cast<UPrimitiveComponent*>(Self); };
		FUnrealMcpRqlTypes::SetMember(D, Member("CollisionProfile", String,
			[Primitive](void* Self) { return Str(Primitive(Self)->GetCollisionProfileName().ToString()); },
			[Primitive](void* Self, const rql::Value& Value)
			{
				UPrimitiveComponent* Component = Primitive(Self);
				const FName Name(*TextArg(Value).TrimStartAndEnd());
				if (Component->GetCollisionProfileName() == Name)
					return;
				FCollisionResponseTemplate Template;
				if (Name != UCollisionProfile::CustomCollisionProfileName && !UCollisionProfile::Get()->GetProfileTemplate(Name, Template))
				{
					TArray<TSharedPtr<FName>> Names;
					UCollisionProfile::GetProfileNames(Names);
					TArray<FString> Shown;
					for (const TSharedPtr<FName>& Profile : Names)
						Shown.Add(Profile->ToString());
					Throw(FString::Printf(TEXT("no collision profile '%s' (profiles: %s)"), *Name.ToString(), *FString::Join(Shown, TEXT(", "))));
				}
				BeginCollisionEdit(Component);
				Component->SetCollisionProfileName(Name);
			}, "the collision preset (BlockAll, OverlapAllDynamic, Pawn, NoCollision, a project profile; Custom after an ObjectType / Responses edit)"));
		FUnrealMcpRqlTypes::SetMember(D, Member("ObjectType", String,
			[Primitive](void* Self) { return Str(ChannelName(Primitive(Self)->GetCollisionObjectType())); },
			[Primitive](void* Self, const rql::Value& Value)
			{
				UPrimitiveComponent* Component = Primitive(Self);
				const FChannel Channel = ResolveChannel(TextArg(Value), /*bObjectOnly*/ true);
				if (Component->GetCollisionObjectType() == Channel.Channel)
					return;
				BeginCollisionEdit(Component);
				Component->SetCollisionObjectType(Channel.Channel);
			}, "the collision object type by channel name (WorldStatic, WorldDynamic, Pawn, PhysicsBody, a project channel such as \"Debris\"); writing it makes the profile Custom"));
		FUnrealMcpRqlTypes::SetMember(D, Member("CollisionEnabled", String,
			[Primitive](void* Self) { return Str(EnabledName(Primitive(Self)->GetCollisionEnabled())); },
			[Primitive](void* Self, const rql::Value& Value)
			{
				UPrimitiveComponent* Component = Primitive(Self);
				const FString Text = TextArg(Value).TrimStartAndEnd();
				for (const ECollisionEnabled::Type Enabled : { ECollisionEnabled::NoCollision, ECollisionEnabled::QueryOnly, ECollisionEnabled::PhysicsOnly,
					ECollisionEnabled::QueryAndPhysics, ECollisionEnabled::ProbeOnly, ECollisionEnabled::QueryAndProbe })
				{
					if (Text.Equals(EnabledName(Enabled), ESearchCase::IgnoreCase))
					{
						if (Component->GetCollisionEnabled() != Enabled)
						{
							BeginCollisionEdit(Component);
							Component->SetCollisionEnabled(Enabled);
						}
						return;
					}
				}
				Throw(FString::Printf(TEXT("'%s' is not a collision mode (NoCollision, QueryOnly, PhysicsOnly, QueryAndPhysics)"), *Text));
			}, "NoCollision, QueryOnly, PhysicsOnly or QueryAndPhysics"));
		const rql::TypeDescriptor* MapType = T.Registry().map_type(String, String, false);
		FUnrealMcpRqlTypes::SetMember(D, Member("Responses", MapType, [Primitive, MapType](void* Self)
		{
			return rql::Value::map(std::make_shared<FResponses>(Primitive(Self), MapType));
		}, {}, "response to each collision channel by name: Responses[\"Debris\"] = \"Ignore\" (Ignore, Overlap, Block); writing one makes the profile Custom"));
	}
}

FString UUnrealMcpRqlEditorApi::AddCollisionChannel(const FString& Name, const FString& DefaultResponse, bool bTraceChannel)
{
	using namespace UnrealMcpRqlCollisionLocal;
	using UnrealMcpRql::Failed;
	const FString Trimmed = Name.TrimStartAndEnd();
	if (Trimmed.IsEmpty() || !FName::IsValidXName(Trimmed, INVALID_OBJECTNAME_CHARACTERS) || Trimmed.Contains(TEXT(" ")))
		return Failed<FString>(FString::Printf(TEXT("'%s' is not a valid channel name"), *Name));
	ECollisionResponse Response = ECR_Block;
	FString ResponseError;
	try
	{
		Response = ResolveResponse(rql::Value::string(UnrealMcpRqlToStd(DefaultResponse)));
		for (const FChannel& Channel : Channels())
		{
			if (Channel.Name.ToString().Equals(Trimmed, ESearchCase::IgnoreCase))
				return Failed<FString>(FString::Printf(TEXT("the project already has a collision channel '%s'"), *Channel.Name.ToString()));
		}
	}
	catch (const std::exception& Error)
	{
		return Failed<FString>(UnrealMcpRqlFromStd(Error.what()));
	}
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
	Profile->TryUpdateDefaultConfigFile();
	Profile->LoadProfileConfig(/*bForceInit*/ true);
	const UEnum* Enum = StaticEnum<ECollisionChannel>();
	return Enum ? Enum->GetNameStringByValue(Free) : FString::FromInt(Free);
}

void UnrealMcpRqlAddCollision(FUnrealMcpRqlSession& Session)
{
	Session.Types().AddClassHook(UPrimitiveComponent::StaticClass(), &UnrealMcpRqlCollisionLocal::PrimitiveMembers);
}
