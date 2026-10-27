// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRqlSession.h"
#include "UnrealMcpRqlSources.h"
#include "UnrealMcpRqlUtil.h"
#include "Tools/UnrealMcpObjectRef.h"

#include "Channels/MovieSceneBoolChannel.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneBoolTrack.h"
#include "Tracks/MovieSceneColorTrack.h"
#include "Tracks/MovieSceneDoubleTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "Tracks/MovieSceneVectorTrack.h"
#include "Tracks/MovieSceneVisibilityTrack.h"

#include <memory>
#include <string>
#include <vector>

/**
 * Level Sequences (docs/rql.md "Level Sequences"). A sequence's bindings, tracks, sections and keys live in its
 * UMovieScene, mostly outside reflection (bindings are structs of GUIDs, keys are channel data). The core module
 * exposes them in the units the Sequencer shows: frames at the sequence's display rate. Every change is undoable; keys
 * are added to the double / float / bool channels sections have (transforms, floats, visibility ...).
 */
namespace UnrealMcpRqlSequencesLocal
{
	using namespace UnrealMcpRqlUtil;

	/** A binding of a sequence: valid while the sequence lives and still has a binding with that id. */
	struct FBindingHandle
	{
		TWeakObjectPtr<ULevelSequence> Sequence;
		FGuid Guid;

		const FMovieSceneBinding* Resolve() const
		{
			const ULevelSequence* Owner = Sequence.Get();
			const UMovieScene* Scene = Owner ? Owner->GetMovieScene() : nullptr;
			return Scene ? Scene->FindBinding(Guid) : nullptr;
		}
		const FMovieSceneBinding& Require() const
		{
			const FMovieSceneBinding* Binding = Resolve();
			if (!Binding)
				Throw(TEXT("the binding is gone: read the sequence's Bindings again"));
			return *Binding;
		}
	};

	char BindingTypeKey = 0;

	UMovieScene* SceneOf(const ULevelSequence* Sequence)
	{
		UMovieScene* Scene = Sequence ? Sequence->GetMovieScene() : nullptr;
		if (!Scene)
			Throw(TEXT("the Level Sequence has no movie scene"));
		return Scene;
	}

	UMovieScene* SceneOf(const UObject* Object)
	{
		UMovieScene* Scene = Object ? const_cast<UMovieScene*>(Object->GetTypedOuter<UMovieScene>()) : nullptr;
		if (!Scene)
			Throw(FString::Printf(TEXT("%s is not part of a Level Sequence"), Object ? *Object->GetName() : TEXT("the object")));
		return Scene;
	}

	/** Display frames (what the Sequencer shows) <-> tick frames (what the movie scene stores). */
	int32 ToDisplay(const UMovieScene* Scene, FFrameNumber Tick)
	{
		return FFrameRate::TransformTime(FFrameTime(Tick), Scene->GetTickResolution(), Scene->GetDisplayRate()).RoundToFrame().Value;
	}

	FFrameNumber ToTick(const UMovieScene* Scene, double Display)
	{
		return FFrameRate::TransformTime(FFrameTime::FromDecimal(Display), Scene->GetDisplayRate(), Scene->GetTickResolution()).RoundToFrame();
	}

	FString RateText(const FFrameRate& Rate)
	{
		return Rate.ToPrettyText().ToString();
	}

	const rql::TypeDescriptor* Int()
	{
		return rql::builtin::number_type(true, true, 4);
	}

	double NumberArg(const rql::Value& Value, const TCHAR* What)
	{
		if (!Value.is_number())
			ThrowMismatch(What, Value);
		return Value.as_number();
	}

	/** The track class a name means ("Transform", "Visibility", "CameraCut", "MovieSceneFloatTrack"); nullptr when none. */
	UClass* FindTrackClass(const FString& Name)
	{
		const FString Trimmed = Name.TrimStartAndEnd();
		if (Trimmed.Equals(TEXT("Transform"), ESearchCase::IgnoreCase))
			return UMovieScene3DTransformTrack::StaticClass();
		if (Trimmed.Equals(TEXT("Visibility"), ESearchCase::IgnoreCase))
			return UMovieSceneVisibilityTrack::StaticClass();
		UClass* Class = FUnrealMcpObjectRef::ResolveClass(Trimmed);
		if (!Class || !Class->IsChildOf(UMovieSceneTrack::StaticClass()))
			Class = FUnrealMcpObjectRef::ResolveClass(FString(TEXT("MovieScene")) + Trimmed + TEXT("Track"));
		if (!Class || !Class->IsChildOf(UMovieSceneTrack::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract))
			return nullptr;
		return Class;
	}

	UClass* TrackClass(const FString& Name)
	{
		UClass* Class = FindTrackClass(Name);
		if (!Class)
			Throw(FString::Printf(TEXT("'%s' is not a track class (\"Transform\", \"Visibility\", \"CameraCut\", ...)"), *Name.TrimStartAndEnd()));
		return Class;
	}

	/** A section spanning the playback range on @p Track (what the Sequencer's "+ Track" does). */
	UMovieSceneSection* AddSection(UMovieScene* Scene, UMovieSceneTrack* Track)
	{
		Track->Modify();
		UMovieSceneSection* Section = Track->CreateNewSection();
		if (!Section)
			Throw(FString::Printf(TEXT("%s cannot have sections"), *Track->GetClass()->GetName()));
		Section->SetFlags(RF_Transactional);
		Section->SetRange(Scene->GetPlaybackRange());
		Track->AddSection(*Section);
		return Section;
	}

	/** A property track animates nothing until it names its property (Transform names its own; Visibility is set by AddBindingTrack). */
	bool NeedsProperty(const UClass* Class)
	{
		return Class->IsChildOf(UMovieScenePropertyTrack::StaticClass()) && !Class->IsChildOf(UMovieScene3DTransformTrack::StaticClass());
	}

	UMovieSceneTrack* AddTrackTo(UMovieScene* Scene, const FGuid* Binding, const FString& ClassName)
	{
		UClass* Class = TrackClass(ClassName);
		if (!Binding && NeedsProperty(Class))
			Throw(FString::Printf(TEXT("%s animates a property of a bound object: bind the actor (AddActor) and call AddTrack(\"<property>\") on its binding, e.g. AddTrack(\"Intensity\")"),
				*Class->GetName()));
		Scene->Modify();
		UMovieSceneTrack* Track = Binding ? Scene->AddTrack(Class, *Binding) : Scene->AddTrack(Class);
		if (!Track)
			Throw(FString::Printf(TEXT("could not add a %s"), *Class->GetName()));
		Track->SetFlags(RF_Transactional);
		AddSection(Scene, Track);
		return Track;
	}

	// ---------------------------------------------------------------- property tracks

	/** The property track class the Sequencer uses for a property (nullptr: no track animates it). */
	UClass* PropertyTrackClass(const FProperty* Property)
	{
		if (Property->IsA<FFloatProperty>())
			return UMovieSceneFloatTrack::StaticClass();
		if (Property->IsA<FDoubleProperty>())
			return UMovieSceneDoubleTrack::StaticClass();
		if (Property->IsA<FBoolProperty>())
			return UMovieSceneBoolTrack::StaticClass();
		if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
		{
			if (Struct->Struct == TBaseStructure<FLinearColor>::Get() || Struct->Struct == TBaseStructure<FColor>::Get())
				return UMovieSceneColorTrack::StaticClass();
			if (Struct->Struct == TBaseStructure<FVector>::Get())
				return UMovieSceneDoubleVectorTrack::StaticClass();
		}
		return nullptr;
	}

	/** Keyable in the Sequencer: marked Interp (the "+ Track" property menu) and of a type a property track animates. */
	bool IsAnimatable(const FProperty* Property)
	{
		return Property->HasAnyPropertyFlags(CPF_Interp) && PropertyTrackClass(Property) != nullptr;
	}

	/** The object a binding possesses in the editor world: an actor, or a component of a bound actor (nullptr: not found). */
	UObject* BoundObject(ULevelSequence* Sequence, const FGuid& Guid)
	{
		UWorld* World = EditorWorld();
		UMovieScene* Scene = SceneOf(Sequence);
		const FMovieScenePossessable* Possessable = Scene->FindPossessable(Guid);
		if (!World || !Possessable)
			return nullptr;
		if (Possessable->GetParent().IsValid())
		{
			AActor* Owner = Cast<AActor>(BoundObject(Sequence, Possessable->GetParent()));
			if (!Owner)
				return nullptr;
			for (UActorComponent* Component : TInlineComponentArray<UActorComponent*>(Owner))
				if (Component->GetName() == Possessable->GetName() && Sequence->FindBindingFromObject(Component, Owner) == Guid)
					return Component;
			return nullptr;
		}
		// Asking the sequence is costly (a transient playback state per object): try the actors still labelled as the
		// possessable (named after the label when bound) first, then the rest of its class.
		const UClass* Class = Possessable->GetPossessedObjectClass();
		for (const bool bLabelled : { true, false })
		{
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if ((Class && !It->IsA(Class)) || (It->GetActorLabel() == Possessable->GetName()) != bLabelled)
					continue;
				if (Sequence->FindBindingFromObject(*It, World) == Guid)
					return *It;
			}
		}
		return nullptr;
	}

	/** An animatable property of @p Bound or (for an actor) of one of its components: "Intensity", "LightComponent0.Intensity". */
	TPair<UObject*, FProperty*> FindAnimatable(UObject* Bound, const FString& Path, bool bThrowIfMissing = true)
	{
		FString Owner, Name = Path.TrimStartAndEnd();
		if (Name.Contains(TEXT(".")))
			Name.Split(TEXT("."), &Owner, &Name);
		TArray<UObject*> Objects;
		if (Owner.IsEmpty())
			Objects.Add(Bound);
		TArray<FString> Available;
		if (AActor* Actor = Cast<AActor>(Bound))
		{
			for (UActorComponent* Component : TInlineComponentArray<UActorComponent*>(Actor))
			{
				const bool bNamed = Owner.IsEmpty() || Component->GetName().Equals(Owner, ESearchCase::IgnoreCase)
					|| Component->GetClass()->GetName().Equals(Owner, ESearchCase::IgnoreCase)
					|| Component->GetClass()->GetName().Equals(Owner + TEXT("Component"), ESearchCase::IgnoreCase);
				if (bNamed)
					Objects.Add(Component);
			}
		}
		TArray<TPair<UObject*, FProperty*>> Hits;
		for (UObject* Object : Objects)
		{
			for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
			{
				if (!IsAnimatable(*It))
					continue;
				const FString Shown = Object == Bound ? It->GetName() : Object->GetName() + TEXT(".") + It->GetName();
				Available.AddUnique(Shown);
				if (It->GetName().Equals(Name, ESearchCase::IgnoreCase))
					Hits.Add({ Object, *It });
			}
		}
		if (Hits.Num() == 1)
			return Hits[0];
		const FString Label = Cast<AActor>(Bound) ? Cast<AActor>(Bound)->GetActorLabel() : Bound->GetName();
		if (Hits.Num() > 1)
		{
			TArray<FString> Names;
			for (const TPair<UObject*, FProperty*>& Hit : Hits)
				Names.Add(Hit.Key->GetName() + TEXT(".") + Hit.Value->GetName());
			Throw(FString::Printf(TEXT("'%s' is a property of several components of %s: name one (%s)"), *Path, *Label, *FString::Join(Names, TEXT(", "))));
		}
		if (!bThrowIfMissing)
			return { nullptr, nullptr };
		Available.Sort();
		if (Available.Num() > 25)
		{
			Available.SetNum(25);
			Available.Add(TEXT("..."));
		}
		Throw(FString::Printf(TEXT("'%s' is not a track class nor an animatable property of %s (track classes: \"Transform\", \"Visibility\"; animatable properties: %s)"),
			*Path, *Label, Available.Num() ? *FString::Join(Available, TEXT(", ")) : TEXT("none")));
	}

	/** The binding of a component of a bound actor, made as the Sequencer makes it (a child possessable) when missing. */
	FGuid ComponentBinding(ULevelSequence* Sequence, const FGuid& ActorGuid, AActor* Actor, UActorComponent* Component)
	{
		UMovieScene* Scene = SceneOf(Sequence);
		for (int32 Index = 0; Index < Scene->GetPossessableCount(); ++Index)
		{
			const FMovieScenePossessable& Child = Scene->GetPossessable(Index);
			if (Child.GetParent() == ActorGuid && Child.GetName() == Component->GetName())
				return Child.GetGuid();
		}
		Sequence->Modify();
		Scene->Modify();
		const FGuid Guid = Scene->AddPossessable(Component->GetName(), Component->GetClass());
		if (FMovieScenePossessable* Possessable = Scene->FindPossessable(Guid))
			Possessable->SetParent(ActorGuid, Scene);
		Sequence->BindPossessableObject(Guid, *Component, Actor);
		return Guid;
	}

	/**
	 * AddTrack on a binding: a track class ("Transform", "Visibility", "MovieSceneFloatTrack" refused without a property),
	 * or a property to animate ("Intensity", "LightComponent0.Intensity"): the property track of its type, bound to the
	 * property, on the actor's binding or on its component's (created as the Sequencer does).
	 */
	UMovieSceneTrack* AddBindingTrack(ULevelSequence* Sequence, const FGuid& Guid, const FString& What)
	{
		UMovieScene* Scene = SceneOf(Sequence);
		UClass* Class = FindTrackClass(What);
		if (Class && Class->IsChildOf(UMovieSceneVisibilityTrack::StaticClass()))
		{
			// The Sequencer's Visibility track keys the actor's "Actor Hidden In Game" (a component's bHiddenInGame).
			const FMovieScenePossessable* Possessable = Scene->FindPossessable(Guid);
			const UClass* BoundClass = Possessable ? Possessable->GetPossessedObjectClass() : nullptr;
			UMovieScenePropertyTrack* Track = Cast<UMovieScenePropertyTrack>(AddTrackTo(Scene, &Guid, What));
			const FName Hidden = BoundClass && BoundClass->IsChildOf(USceneComponent::StaticClass()) ? FName(TEXT("bHiddenInGame")) : FName(TEXT("bHidden"));
			Track->SetPropertyNameAndPath(Hidden, Hidden.ToString());
			return Track;
		}
		if (Class && !NeedsProperty(Class))
			return AddTrackTo(Scene, &Guid, What);
		// A property, or a name that only resolves to a property track class ("Color" -> MovieSceneColorTrack): a property
		// of that name wins, else the class alone is refused (it would animate nothing).
		UObject* Bound = BoundObject(Sequence, Guid);
		const TPair<UObject*, FProperty*> Target = Bound ? FindAnimatable(Bound, What, /*bThrowIfMissing*/ Class == nullptr) : TPair<UObject*, FProperty*>(nullptr, nullptr);
		if (!Target.Key)
		{
			if (Class)
				Throw(FString::Printf(TEXT("%s needs the property it animates: call AddTrack(\"<property>\") instead (e.g. AddTrack(\"Intensity\")); the track class follows from the property's type"),
					*Class->GetName()));
			Throw(FString::Printf(TEXT("'%s' is not a track class, and the bound object is not in the editor world, so its properties cannot be looked up"), *What));
		}
		FGuid TrackGuid = Guid;
		if (Target.Key != Bound)
			TrackGuid = ComponentBinding(Sequence, Guid, CastChecked<AActor>(Bound), CastChecked<UActorComponent>(Target.Key));
		const FMovieSceneBinding* Binding = Scene->FindBinding(TrackGuid);
		if (!Binding)
			Throw(FString::Printf(TEXT("the binding of %s is missing from the movie scene (a damaged sequence): rebind it"), *Target.Key->GetName()));
		const FName Property = Target.Value->GetFName();
		for (UMovieSceneTrack* Existing : Binding->GetTracks())
		{
			const UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Existing);
			if (PropertyTrack && PropertyTrack->GetPropertyName() == Property)
				Throw(FString::Printf(TEXT("%s already has a %s track: key it (get Tracks | get Sections | call AddKey(\"%s\", frame, value))"),
					*Target.Key->GetName(), *Property.ToString(), *Property.ToString()));
		}
		UClass* PropertyClass = PropertyTrackClass(Target.Value);
		Scene->Modify();
		UMovieScenePropertyTrack* Track = Cast<UMovieScenePropertyTrack>(Scene->AddTrack(PropertyClass, TrackGuid));
		if (!Track)
			Throw(FString::Printf(TEXT("could not add a %s"), *PropertyClass->GetName()));
		Track->SetFlags(RF_Transactional);
		Track->SetPropertyNameAndPath(Property, Property.ToString());
		if (UMovieSceneDoubleVectorTrack* Vector = Cast<UMovieSceneDoubleVectorTrack>(Track))
			Vector->SetNumChannelsUsed(3);
		AddSection(Scene, Track);
		return Track;
	}

	// ---------------------------------------------------------------- channels

	/**
	 * Visit every keyable channel of a section with its display name ("Location.X", "Rotation.Yaw", "Bool", "Weight") and
	 * its internal name ("Rotation.Z", the name Python and the channel metadata use; empty when there is none).
	 */
	void ForEachChannel(UMovieSceneSection* Section, TFunctionRef<void(const FString&, const FString&, FMovieSceneChannel*, FName)> Visit)
	{
		FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();
		// A property track's single channel carries no name (float, bool ... sections): it is named after the property.
		const UMovieScenePropertyTrack* PropertyTrack = Section->GetTypedOuter<UMovieScenePropertyTrack>();
		// A Visibility section's bool is inverted (true = visible) although the track binds bHidden: it is named Visible.
		const FString PropertyName = Cast<UMovieSceneVisibilityTrack>(PropertyTrack) ? FString(TEXT("Visible"))
			: PropertyTrack && !PropertyTrack->GetPropertyName().IsNone() ? PropertyTrack->GetPropertyName().ToString() : FString();
		for (const FMovieSceneChannelEntry& Entry : Proxy.GetAllEntries())
		{
			const FName Type = Entry.GetChannelTypeName();
			TArrayView<FMovieSceneChannel* const> Channels = Entry.GetChannels();
#if WITH_EDITOR
			TArrayView<const FMovieSceneChannelMetaData> Meta = Entry.GetMetaData();
#endif
			for (int32 Index = 0; Index < Channels.Num(); ++Index)
			{
				FString Name = FString::Printf(TEXT("%s%d"), *Type.ToString(), Index);
				FString Internal;
#if WITH_EDITOR
				if (Meta.IsValidIndex(Index))
				{
					const FString Display = Meta[Index].DisplayText.ToString();
					const FString Group = Meta[Index].Group.ToString();
					Internal = Meta[Index].Name.IsNone() ? FString() : Meta[Index].Name.ToString();
					Name = Display.IsEmpty() ? Internal : (Group.IsEmpty() ? Display : Group + TEXT(".") + Display);
				}
#endif
				if (Name.IsEmpty() || (Internal.IsEmpty() && Name.StartsWith(Type.ToString())))
					Name = PropertyName.IsEmpty() ? FString::Printf(TEXT("%s%d"), *Type.ToString(), Index)
						: Channels.Num() == 1 ? PropertyName : FString::Printf(TEXT("%s%d"), *PropertyName, Index);
				Visit(Name, Internal, Channels[Index], Type);
			}
		}
	}

	bool IsDouble(FName Type) { return Type == FMovieSceneDoubleChannel::StaticStruct()->GetFName(); }
	bool IsFloat(FName Type) { return Type == FMovieSceneFloatChannel::StaticStruct()->GetFName(); }
	bool IsBool(FName Type) { return Type == FMovieSceneBoolChannel::StaticStruct()->GetFName(); }

	rql::Value Keys(const UMovieScene* Scene, FMovieSceneChannel* Channel, FName Type)
	{
		std::vector<rql::Value> Out;
		auto Push = [&Out, Scene](FFrameNumber Time, rql::Value Value)
		{
			std::vector<std::pair<rql::Value, rql::Value>> Key;
			Key.emplace_back(Str(TEXT("Frame")), rql::Value::number(ToDisplay(Scene, Time), Int()));
			Key.emplace_back(Str(TEXT("Value")), std::move(Value));
			Out.push_back(rql::Value::map(std::make_shared<rql::OwnedMap>(nullptr, std::move(Key))));
		};
		if (IsDouble(Type))
		{
			TMovieSceneChannelData<FMovieSceneDoubleValue> Data = static_cast<FMovieSceneDoubleChannel*>(Channel)->GetData();
			for (int32 Index = 0; Index < Data.GetTimes().Num(); ++Index)
				Push(Data.GetTimes()[Index], rql::Value::number(Data.GetValues()[Index].Value));
		}
		else if (IsFloat(Type))
		{
			TMovieSceneChannelData<FMovieSceneFloatValue> Data = static_cast<FMovieSceneFloatChannel*>(Channel)->GetData();
			for (int32 Index = 0; Index < Data.GetTimes().Num(); ++Index)
				Push(Data.GetTimes()[Index], rql::Value::number(Data.GetValues()[Index].Value, rql::builtin::single_type()));
		}
		else if (IsBool(Type))
		{
			TMovieSceneChannelData<bool> Data = static_cast<FMovieSceneBoolChannel*>(Channel)->GetData();
			for (int32 Index = 0; Index < Data.GetTimes().Num(); ++Index)
				Push(Data.GetTimes()[Index], rql::Value::boolean(Data.GetValues()[Index]));
		}
		return rql::Value::list_of(std::move(Out));
	}

	void AddKey(UMovieSceneSection* Section, const FString& Wanted, double Frame, const rql::Value& Value)
	{
		const UMovieScene* Scene = SceneOf(Section);
		const FFrameNumber Time = ToTick(Scene, Frame);
		TArray<FString> Names;
		const FString Key = Wanted.TrimStartAndEnd();
		// The channel shown under that name, else the one with that internal name ("Rotation.Z" is the yaw).
		struct FHit { FString Name; FMovieSceneChannel* Channel = nullptr; FName Type; };
		FHit ByDisplay, ByInternal;
		ForEachChannel(Section, [&](const FString& Name, const FString& Internal, FMovieSceneChannel* Channel, FName Type)
		{
			Names.Add(Name);
			if (!ByDisplay.Channel && Name.Equals(Key, ESearchCase::IgnoreCase))
				ByDisplay = { Name, Channel, Type };
			else if (!ByInternal.Channel && !Internal.IsEmpty() && Internal.Equals(Key, ESearchCase::IgnoreCase))
				ByInternal = { Name, Channel, Type };
		});
		const FHit& Hit = ByDisplay.Channel ? ByDisplay : ByInternal;
		if (!Hit.Channel)
			Throw(FString::Printf(TEXT("%s has no channel '%s' (channels: %s)"), *Section->GetName(), *Wanted, *FString::Join(Names, TEXT(", "))));
		FMovieSceneChannel* Channel = Hit.Channel;
		const FName Type = Hit.Type;
		// AddCubicKey inserts next to a key at the same frame: drop that key first, so the new one replaces it.
		if (IsDouble(Type))
		{
			const double Number = NumberArg(Value, TEXT("a number"));
			Section->Modify();
			FMovieSceneDoubleChannel* DoubleChannel = static_cast<FMovieSceneDoubleChannel*>(Channel);
			TMovieSceneChannelData<FMovieSceneDoubleValue> Data = DoubleChannel->GetData();
			const int32 Existing = Data.FindKey(Time);
			if (Existing != INDEX_NONE)
				Data.RemoveKey(Existing);
			DoubleChannel->AddCubicKey(Time, Number);
		}
		else if (IsFloat(Type))
		{
			const double Number = NumberArg(Value, TEXT("a number"));
			Section->Modify();
			FMovieSceneFloatChannel* FloatChannel = static_cast<FMovieSceneFloatChannel*>(Channel);
			TMovieSceneChannelData<FMovieSceneFloatValue> Data = FloatChannel->GetData();
			const int32 Existing = Data.FindKey(Time);
			if (Existing != INDEX_NONE)
				Data.RemoveKey(Existing);
			FloatChannel->AddCubicKey(Time, static_cast<float>(Number));
		}
		else if (IsBool(Type))
		{
			const bool bValue = BoolArg(Value);
			Section->Modify();
			TMovieSceneChannelData<bool> Data = static_cast<FMovieSceneBoolChannel*>(Channel)->GetData();
			const int32 Existing = Data.FindKey(Time);
			if (Existing != INDEX_NONE)
				Data.GetValues()[Existing] = bValue;
			else
				Data.AddKey(Time, bValue);
		}
		else
		{
			Throw(FString::Printf(TEXT("channel '%s' (%s) is not a number or bool channel"), *Hit.Name, *Type.ToString()));
		}
		// A key outside the section grows it, as keying in the Sequencer does.
		if (Section->HasStartFrame() && Section->HasEndFrame() && !Section->GetRange().Contains(Time))
			Section->ExpandToFrame(Time);
	}

	// ---------------------------------------------------------------- hooks

	rql::Value WrapBinding(const rql::TypeDescriptor* Type, ULevelSequence* Sequence, const FGuid& Guid)
	{
		auto Handle = std::make_shared<FBindingHandle>();
		Handle->Sequence = Sequence;
		Handle->Guid = Guid;
		FBindingHandle* Raw = Handle.get();
		return rql::Value::object(Type, Raw, std::static_pointer_cast<void>(Handle));
	}

	void SequenceMembers(FUnrealMcpRqlTypes& T, rql::TypeDescriptor& D, const rql::TypeDescriptor* BindingType)
	{
		FUnrealMcpRqlTypes* Types = &T;
		const rql::TypeDescriptor* TrackType = T.ForClass(UMovieSceneTrack::StaticClass());
		auto Sequence = [](void* Self) { return static_cast<ULevelSequence*>(Self); };
		FUnrealMcpRqlTypes::SetMember(D, Member("DisplayRate", rql::builtin::string_type(),
			[Sequence](void* Self) { return Str(RateText(SceneOf(Sequence(Self))->GetDisplayRate())); }, {}, "frames per second of the frame numbers below"));
		FUnrealMcpRqlTypes::SetMember(D, Member("Start", Int(),
			[Sequence](void* Self)
			{
				const UMovieScene* Scene = SceneOf(Sequence(Self));
				return rql::Value::number(ToDisplay(Scene, UE::MovieScene::DiscreteInclusiveLower(Scene->GetPlaybackRange())), Int());
			},
			[Sequence](void* Self, const rql::Value& Value)
			{
				UMovieScene* Scene = SceneOf(Sequence(Self));
				const FFrameNumber Start = ToTick(Scene, NumberArg(Value, TEXT("a frame number")));
				const FFrameNumber End = UE::MovieScene::DiscreteExclusiveUpper(Scene->GetPlaybackRange());
				if (Start > End)
					Throw(TEXT("Start must be before End"));
				Scene->Modify();
				Scene->SetPlaybackRange(TRange<FFrameNumber>(Start, End));
			}, "first frame of the playback range"));
		FUnrealMcpRqlTypes::SetMember(D, Member("End", Int(),
			[Sequence](void* Self)
			{
				const UMovieScene* Scene = SceneOf(Sequence(Self));
				return rql::Value::number(ToDisplay(Scene, UE::MovieScene::DiscreteExclusiveUpper(Scene->GetPlaybackRange())), Int());
			},
			[Sequence](void* Self, const rql::Value& Value)
			{
				UMovieScene* Scene = SceneOf(Sequence(Self));
				const FFrameNumber Start = UE::MovieScene::DiscreteInclusiveLower(Scene->GetPlaybackRange());
				const FFrameNumber End = ToTick(Scene, NumberArg(Value, TEXT("a frame number")));
				if (End < Start)
					Throw(TEXT("End must be after Start"));
				Scene->Modify();
				Scene->SetPlaybackRange(TRange<FFrameNumber>(Start, End));
			}, "end of the playback range (exclusive)"));
		FUnrealMcpRqlTypes::SetMember(D, Member("Bindings", T.Registry().list_type(BindingType, false),
			[Sequence, BindingType](void* Self)
			{
				ULevelSequence* Owner = Sequence(Self);
				std::vector<rql::Value> Out;
				for (const FMovieSceneBinding& Binding : SceneOf(Owner)->GetBindings())
					Out.push_back(WrapBinding(BindingType, Owner, Binding.GetObjectGuid()));
				return rql::Value::list_of(std::move(Out));
			}, {}, "the objects the sequence animates (Name Kind Class Tracks)", /*bOwning*/ true));
		FUnrealMcpRqlTypes::SetMember(D, Member("Tracks", T.Registry().list_type(TrackType, true),
			[Types, Sequence](void* Self)
			{
				TArray<UObject*> Out;
				for (UMovieSceneTrack* Track : SceneOf(Sequence(Self))->GetTracks())
					Out.Add(Track);
				return Types->WrapObjects(Out);
			}, {}, "tracks not bound to an object (camera cuts, audio, events ...)", /*bOwning*/ true));

		FUnrealMcpRqlTypes::AddMethod(D, Method("AddActor", { FUnrealMcpRqlTypes::Param("Actor", T.ForClass(AActor::StaticClass())) }, BindingType,
			[Types, Sequence, BindingType](void* Self, const std::vector<rql::Value>& Args) -> rql::Value
			{
				ULevelSequence* Owner = Sequence(Self);
				AActor* Actor = Cast<AActor>(Types->AsObject(Args[0]));
				if (!Actor)
					ThrowMismatch(TEXT("an actor of the editor world"), Args[0]);
				UWorld* World = Actor->GetWorld();
				const FGuid Existing = Owner->FindBindingFromObject(Actor, World);
				if (Existing.IsValid())
					Throw(FString::Printf(TEXT("%s is already bound in %s"), *Actor->GetActorLabel(), *Owner->GetName()));
				UMovieScene* Scene = SceneOf(Owner);
				Owner->Modify();
				Scene->Modify();
				const FGuid Guid = Scene->AddPossessable(Actor->GetActorLabel(), Actor->GetClass());
				Owner->BindPossessableObject(Guid, *Actor, World);
				return WrapBinding(BindingType, Owner, Guid);
			}, "bind an actor of the level (a possessable, as dragging it into the Sequencer); returns the binding"));
		FUnrealMcpRqlTypes::AddMethod(D, Method("AddTrack", { FUnrealMcpRqlTypes::Param("Class", rql::builtin::string_type()) }, TrackType,
			[Types, Sequence](void* Self, const std::vector<rql::Value>& Args) -> rql::Value
			{
				return Types->WrapObject(AddTrackTo(SceneOf(Sequence(Self)), nullptr, TextArg(Args[0])));
			}, "add a track not bound to an object (with a section over the playback range); returns it"));
	}

	void BindingMembers(FUnrealMcpRqlTypes& T, rql::TypeDescriptor& D)
	{
		FUnrealMcpRqlTypes* Types = &T;
		const rql::TypeDescriptor* String = rql::builtin::string_type();
		auto Binding = [](void* Self) -> const FMovieSceneBinding& { return static_cast<FBindingHandle*>(Self)->Require(); };
		auto Handle = [](void* Self) { return static_cast<FBindingHandle*>(Self); };
		FUnrealMcpRqlTypes::SetMember(D, Member("Name", String, [Handle](void* Self)
		{
			UMovieScene* Scene = SceneOf(Handle(Self)->Sequence.Get());
			if (const FMovieScenePossessable* Possessable = Scene->FindPossessable(Handle(Self)->Guid))
				return Str(Possessable->GetName());
			if (const FMovieSceneSpawnable* Spawnable = Scene->FindSpawnable(Handle(Self)->Guid))
				return Str(Spawnable->GetName());
			return Str(FString());
		}, {}, "the name shown in the Sequencer"));
		FUnrealMcpRqlTypes::SetMember(D, Member("Guid", String, [Handle](void* Self) { return Str(Handle(Self)->Guid.ToString()); }));
		FUnrealMcpRqlTypes::SetMember(D, Member("Kind", String, [Handle](void* Self)
		{
			UMovieScene* Scene = SceneOf(Handle(Self)->Sequence.Get());
			const FGuid& Guid = Handle(Self)->Guid;
			return Str(Scene->FindPossessable(Guid) ? TEXT("Possessable") : Scene->FindSpawnable(Guid) ? TEXT("Spawnable") : TEXT("Other"));
		}, {}, "Possessable (an object of the level) or Spawnable (spawned by the sequence)"));
		FUnrealMcpRqlTypes::SetMember(D, Member("Class", String, [Handle](void* Self)
		{
			UMovieScene* Scene = SceneOf(Handle(Self)->Sequence.Get());
			if (const FMovieScenePossessable* Possessable = Scene->FindPossessable(Handle(Self)->Guid))
				return Str(Possessable->GetPossessedObjectClass() ? Possessable->GetPossessedObjectClass()->GetName() : FString());
			return Str(FString());
		}));
		FUnrealMcpRqlTypes::SetMember(D, Member("Tracks", T.Registry().list_type(T.ForClass(UMovieSceneTrack::StaticClass()), true),
			[Types, Binding](void* Self)
			{
				TArray<UObject*> Out;
				for (UMovieSceneTrack* Track : Binding(Self).GetTracks())
					Out.Add(Track);
				return Types->WrapObjects(Out);
			}, {}, "the tracks animating this object", /*bOwning*/ true));
		D.display_name = "Name";
		D.display_explicit = true;
		FUnrealMcpRqlTypes::AddMethod(D, Method("AddTrack", { FUnrealMcpRqlTypes::Param("Class", String) }, T.ForClass(UMovieSceneTrack::StaticClass()),
			[Types, Handle](void* Self, const std::vector<rql::Value>& Args) -> rql::Value
			{
				FBindingHandle* Bound = Handle(Self);
				Bound->Require();
				return Types->WrapObject(AddBindingTrack(Bound->Sequence.Get(), Bound->Guid, TextArg(Args[0])));
			}, "add a track with a section over the playback range: \"Transform\", \"Visibility\", or a property to animate (\"Intensity\", \"LightColor\", \"LightComponent0.Intensity\": the actor's or a component's, keyed in the section's channel of that name); returns it"));
	}

	void TrackMembers(FUnrealMcpRqlTypes& T, rql::TypeDescriptor& D)
	{
		FUnrealMcpRqlTypes* Types = &T;
		const rql::TypeDescriptor* SectionType = T.ForClass(UMovieSceneSection::StaticClass());
		FUnrealMcpRqlTypes::SetMember(D, Member("Sections", T.Registry().list_type(SectionType, true), [Types](void* Self)
		{
			TArray<UObject*> Out;
			for (UMovieSceneSection* Section : static_cast<UMovieSceneTrack*>(Self)->GetAllSections())
				Out.Add(Section);
			return Types->WrapObjects(Out);
		}, {}, "the sections of the track", /*bOwning*/ true));
		FUnrealMcpRqlTypes::SetMember(D, Member("DisplayName", rql::builtin::string_type(), [](void* Self)
		{
			return Str(static_cast<UMovieSceneTrack*>(Self)->GetDisplayName().ToString());
		}));
		FUnrealMcpRqlTypes::AddMethod(D, Method("AddSection", {}, SectionType, [Types](void* Self, const std::vector<rql::Value>&) -> rql::Value
		{
			UMovieSceneTrack* Track = static_cast<UMovieSceneTrack*>(Self);
			return Types->WrapObject(AddSection(SceneOf(Track), Track));
		}, "add a section over the playback range; returns it"));
	}

	void SectionMembers(FUnrealMcpRqlTypes& T, rql::TypeDescriptor& D)
	{
		auto Section = [](void* Self) { return static_cast<UMovieSceneSection*>(Self); };
		FUnrealMcpRqlTypes::SetMember(D, Member("Start", Int(),
			[Section](void* Self)
			{
				UMovieSceneSection* Target = Section(Self);
				return Target->HasStartFrame() ? rql::Value::number(ToDisplay(SceneOf(Target), Target->GetInclusiveStartFrame()), Int()) : rql::Value::null();
			},
			[Section](void* Self, const rql::Value& Value)
			{
				UMovieSceneSection* Target = Section(Self);
				const FFrameNumber Start = ToTick(SceneOf(Target), NumberArg(Value, TEXT("a frame number")));
				if (Target->HasEndFrame() && Start > Target->GetExclusiveEndFrame())
					Throw(TEXT("Start must be before End"));
				Target->Modify();
				Target->SetStartFrame(TRangeBound<FFrameNumber>::Inclusive(Start));
			}, "first frame (null: unbounded)", false, true));
		FUnrealMcpRqlTypes::SetMember(D, Member("End", Int(),
			[Section](void* Self)
			{
				UMovieSceneSection* Target = Section(Self);
				return Target->HasEndFrame() ? rql::Value::number(ToDisplay(SceneOf(Target), Target->GetExclusiveEndFrame()), Int()) : rql::Value::null();
			},
			[Section](void* Self, const rql::Value& Value)
			{
				UMovieSceneSection* Target = Section(Self);
				const FFrameNumber End = ToTick(SceneOf(Target), NumberArg(Value, TEXT("a frame number")));
				if (Target->HasStartFrame() && End < Target->GetInclusiveStartFrame())
					Throw(TEXT("End must be after Start"));
				Target->Modify();
				Target->SetEndFrame(TRangeBound<FFrameNumber>::Exclusive(End));
			}, "end frame, exclusive (null: unbounded)", false, true));
		FUnrealMcpRqlTypes::SetMember(D, Member("Channels", rql::builtin::any_type(), [Section](void* Self)
		{
			UMovieSceneSection* Target = Section(Self);
			const UMovieScene* Scene = SceneOf(Target);
			std::vector<rql::Value> Out;
			ForEachChannel(Target, [&Out, Scene](const FString& Name, const FString&, FMovieSceneChannel* Channel, FName Type)
			{
				std::vector<std::pair<rql::Value, rql::Value>> Entry;
				Entry.emplace_back(Str(TEXT("Name")), Str(Name));
				Entry.emplace_back(Str(TEXT("Type")), Str(IsDouble(Type) || IsFloat(Type) ? TEXT("Number") : IsBool(Type) ? TEXT("Bool") : Type.ToString()));
				Entry.emplace_back(Str(TEXT("Keys")), Keys(Scene, Channel, Type));
				Out.push_back(rql::Value::map(std::make_shared<rql::OwnedMap>(nullptr, std::move(Entry))));
			});
			return rql::Value::list_of(std::move(Out));
		}, {}, "the keyable channels: [{Name, Type, Keys: [{Frame, Value}]}] (frames at the display rate)"));
		FUnrealMcpRqlTypes::AddMethod(D, Method("AddKey",
			{ FUnrealMcpRqlTypes::Param("Channel", rql::builtin::string_type()), FUnrealMcpRqlTypes::Param("Frame", rql::builtin::double_type()),
			  FUnrealMcpRqlTypes::Param("Value", rql::builtin::any_type()) },
			nullptr, [Section](void* Self, const std::vector<rql::Value>& Args) -> rql::Value
			{
				AddKey(Section(Self), TextArg(Args[0]), NumberArg(Args[1], TEXT("a frame number")), Args[2]);
				return rql::Value::null();
			}, "key a channel (\"Location.X\", \"Intensity\", \"Visible\") at a frame: a number, or true / false for bool channels; replaces a key at that frame"));
	}
}

void UnrealMcpRqlAddSequences(FUnrealMcpRqlSession& Session)
{
	using namespace UnrealMcpRqlSequencesLocal;
	FUnrealMcpRqlTypes& T = Session.Types();
	FUnrealMcpRqlTypes* Types = &T;
	rql::TypeDescriptor& BindingType = T.NativeObjectType(&BindingTypeKey, TEXT("SequenceBinding"),
		TEXT("an object a Level Sequence animates: Name Guid Kind Class Tracks; call AddTrack"));
	BindingMembers(T, BindingType);
	const rql::TypeDescriptor* Binding = &BindingType;
	T.AddClassHook(ULevelSequence::StaticClass(), [Binding](FUnrealMcpRqlTypes& Types, rql::TypeDescriptor& D) { SequenceMembers(Types, D, Binding); });
	T.AddClassHook(UMovieSceneTrack::StaticClass(), &TrackMembers);
	T.AddClassHook(UMovieSceneSection::StaticClass(), &SectionMembers);
	Session.AddSource(TEXT("levelSequences"), T.ForClass(ULevelSequence::StaticClass()), /*bList*/ true,
		TEXT("the project's Level Sequences (DisplayRate Start End Bindings Tracks; call AddActor / AddTrack); sections have Start End Channels, call AddKey"),
		[Types]() { return UnrealMcpRqlAssetList(*Types, ULevelSequence::StaticClass(), FString()); });
}
