// Copyright (c) 2026 IvanMurzak/Unreal-AI-EnhancedInput. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIEnhancedInputRqlApi.h"

#include "UnrealAIEnhancedInputGraph.h"
#include "UnrealAIEnhancedInputText.h"

#include "IUnrealMcpRqlModule.h"

#include "EdGraph/EdGraph.h"
#include "EnhancedActionKeyMapping.h"
#include "EnhancedInputDeveloperSettings.h"
#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "GameFramework/Pawn.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "PlayerMappableKeySettings.h"
#include "UObject/EnumProperty.h"
#include "UObject/UnrealType.h"

namespace UnrealAIEnhancedInputRqlLocal
{
	bool ValidKey(const FString& Key, FKey& Out)
	{
		Out = FKey(*Key.TrimStartAndEnd());
		if (Out.IsValid())
			return true;
		UnrealMcpRql::Fail(FString::Printf(TEXT("'%s' is not a key name (e.g. SpaceBar, W, LeftShift, LeftMouseButton, Gamepad_FaceButton_Bottom, Gamepad_Left2D, "
			"Gamepad_RightTriggerAxis)"), *Key));
		return false;
	}

	/** The indexes of the mappings of @p Key to @p Action in @p Context; empty after Fail (naming the context's mappings). */
	TArray<int32> FindMappings(const UInputMappingContext* Context, const UInputAction* Action, const FString& Key)
	{
		FKey Parsed;
		if (!ValidKey(Key, Parsed))
			return {};
		TArray<int32> Out;
		TArray<FString> Existing;
		const TArray<FEnhancedActionKeyMapping>& Mappings = Context->GetMappings();
		for (int32 Index = 0; Index < Mappings.Num(); ++Index)
		{
			if (Mappings[Index].Action == Action && Mappings[Index].Key == Parsed)
				Out.Add(Index);
			Existing.Add(FString::Printf(TEXT("%s: %s"), Mappings[Index].Action ? *Mappings[Index].Action->GetName() : TEXT("None"), *Mappings[Index].Key.GetFName().ToString()));
		}
		if (Out.Num() == 0)
			UnrealMcpRql::Fail(FString::Printf(TEXT("%s has no mapping of %s to %s (its mappings: %s); add it with AddMapping"), *Context->GetName(), *Parsed.GetFName().ToString(),
				*Action->GetName(), Existing.Num() > 0 ? *FString::Join(Existing, TEXT(", ")) : TEXT("none")));
		return Out;
	}

	/** "IA_MoveForward" as a key-binding menu shows it: "Move Forward". */
	FString PrettyName(const UObject* Asset)
	{
		FString Name = Asset->GetName();
		Name.RemoveFromStart(TEXT("IA_"));
		return FName::NameToDisplayString(Name, /*bIsBool*/ false);
	}

	/** @p Text trimmed, or @p Fallback when that is empty. */
	FString TrimmedOr(const FString& Text, const FString& Fallback)
	{
		const FString Trimmed = Text.TrimStartAndEnd();
		return Trimmed.IsEmpty() ? Fallback : Trimmed;
	}

	/**
	 * @p Existing updated when @p Outer owns it (its Metadata and key profiles stay), else new settings instanced into @p Outer,
	 * with the name, display name and category.
	 */
	UPlayerMappableKeySettings* KeySettings(UObject* Outer, UPlayerMappableKeySettings* Existing, const FString& Name, const FString& DisplayName, const FString& Category)
	{
		UPlayerMappableKeySettings* Settings = Existing && Existing->GetOuter() == Outer ? Existing
			: NewObject<UPlayerMappableKeySettings>(Outer, NAME_None, Outer->GetMaskedFlags(RF_PropagateToSubObjects) | RF_Transactional);
		Settings->Modify();
		Settings->Name = FName(*Name);
		Settings->DisplayName = FText::FromString(DisplayName);
		Settings->DisplayCategory = FText::FromString(Category);
		return Settings;
	}

	/** The protected FEnhancedActionKeyMapping::PlayerMappableKeySettings property. */
	const FObjectPropertyBase* MappingSettingsProperty()
	{
		return CastFieldChecked<FObjectPropertyBase>(FEnhancedActionKeyMapping::StaticStruct()->FindPropertyByName(TEXT("PlayerMappableKeySettings")));
	}

	/** The protected UInputAction::PlayerMappableKeySettings property. */
	FProperty* ActionSettingsProperty()
	{
		return CastFieldChecked<FObjectPropertyBase>(UInputAction::StaticClass()->FindPropertyByName(TEXT("PlayerMappableKeySettings")));
	}

	/** Tell the action a property changed, as the Details panel does: open Blueprints rebuild their action event nodes. */
	void ActionChanged(UInputAction* Action, FProperty* Property)
	{
		FPropertyChangedEvent Changed(Property);
		Action->PostEditChangeProperty(Changed);
		Action->MarkPackageDirty();
	}

	/** Set the protected SettingBehavior / PlayerMappableKeySettings of a mapping through reflection, as the Details panel does. */
	void SetMappingSettings(FEnhancedActionKeyMapping& Mapping, EPlayerMappableKeySettingBehaviors Behavior, UPlayerMappableKeySettings* Settings)
	{
		UScriptStruct* Struct = FEnhancedActionKeyMapping::StaticStruct();
		const FEnumProperty* BehaviorProperty = CastFieldChecked<FEnumProperty>(Struct->FindPropertyByName(TEXT("SettingBehavior")));
		BehaviorProperty->GetUnderlyingProperty()->SetIntPropertyValue(BehaviorProperty->ContainerPtrToValuePtr<void>(&Mapping), static_cast<int64>(Behavior));
		MappingSettingsProperty()->SetObjectPropertyValue_InContainer(&Mapping, Settings);
	}

	/** Change each listed mapping of @p Context with @p Change; the count (after Modify / MarkPackageDirty). */
	int32 ChangeMappings(UInputMappingContext* Context, const TArray<int32>& Indexes, TFunctionRef<void(FEnhancedActionKeyMapping&)> Change)
	{
		if (Indexes.Num() == 0)
			return 0;
		Context->Modify();
		for (const int32 Index : Indexes)
			Change(Context->GetMapping(Index));
		Context->MarkPackageDirty();
		return Indexes.Num();
	}

	/** The Blueprint and event graph @p Target names, and that it may add @p Context (once); null after Fail. */
	UEdGraph* ContextTarget(UObject* Target, UInputMappingContext* Context, UBlueprint*& OutBlueprint)
	{
		UEdGraph* Graph = UnrealAIEnhancedInputGraph::EventGraphOf(Target, OutBlueprint);
		FString Event;
		if (Graph && UnrealAIEnhancedInputGraph::FindContextCall(OutBlueprint, Context, /*bAdd*/ true, FString(), &Event))
			return UnrealMcpRql::Failed<UEdGraph*>(FString::Printf(TEXT("%s already adds %s (on %s): see unreal.asset(path).InputSetup"), *OutBlueprint->GetName(), *Context->GetName(), *Event));
		return Graph;
	}
}

using namespace UnrealAIEnhancedInputRqlLocal;

UInputAction* UUnrealAIEnhancedInputRqlApi::CreateAction(const FString& Path, EInputActionValueType ValueType, const FString& Triggers, const FString& Modifiers)
{
	TArray<UnrealAIEnhancedInputText::FEntry> TriggerEntries, ModifierEntries;
	if (!UnrealAIEnhancedInputText::Parse(Triggers, UInputTrigger::StaticClass(), TriggerEntries)
		|| !UnrealAIEnhancedInputText::Parse(Modifiers, UInputModifier::StaticClass(), ModifierEntries))
		return nullptr;
	UInputAction* Action = Cast<UInputAction>(UnrealMcpRql::CreateAsset(UInputAction::StaticClass(), Path));
	if (!Action)
		return nullptr;
	Action->ValueType = ValueType;
	Action->Triggers = UnrealAIEnhancedInputText::BuildAll<UInputTrigger>(TriggerEntries, Action);
	Action->Modifiers = UnrealAIEnhancedInputText::BuildAll<UInputModifier>(ModifierEntries, Action);
	return Action;
}

UInputMappingContext* UUnrealAIEnhancedInputRqlApi::CreateContext(const FString& Path)
{
	return Cast<UInputMappingContext>(UnrealMcpRql::CreateAsset(UInputMappingContext::StaticClass(), Path));
}

bool UUnrealAIEnhancedInputRqlApi::AddMapping(UInputMappingContext* Context, UInputAction* Action, const FString& Key, const FString& Modifiers, const FString& Triggers)
{
	FKey Parsed;
	TArray<UnrealAIEnhancedInputText::FEntry> ModifierEntries, TriggerEntries;
	if (!ValidKey(Key, Parsed) || !UnrealAIEnhancedInputText::Parse(Modifiers, UInputModifier::StaticClass(), ModifierEntries)
		|| !UnrealAIEnhancedInputText::Parse(Triggers, UInputTrigger::StaticClass(), TriggerEntries))
		return false;
	Context->Modify();
	FEnhancedActionKeyMapping& Mapping = Context->MapKey(Action, Parsed);
	// Instanced into the context, as the editor's Modifiers / Triggers arrays create them.
	Mapping.Modifiers.Append(UnrealAIEnhancedInputText::BuildAll<UInputModifier>(ModifierEntries, Context));
	Mapping.Triggers.Append(UnrealAIEnhancedInputText::BuildAll<UInputTrigger>(TriggerEntries, Context));
	Context->MarkPackageDirty();
	return true;
}

int32 UUnrealAIEnhancedInputRqlApi::RemoveMapping(UInputMappingContext* Context, UInputAction* Action, const FString& Key)
{
	FKey Parsed;
	if (!ValidKey(Key, Parsed))
		return 0;
	const int32 Removed = Context->GetMappings().FilterByPredicate([Action, &Parsed](const FEnhancedActionKeyMapping& Mapping)
	{
		return Mapping.Action == Action && Mapping.Key == Parsed;
	}).Num();
	if (Removed > 0)
	{
		// In place, keeping the order of the other mappings (UnmapKey swaps the last one in, which moves the keys of rebind slots).
		// DefaultKeyMappings is protected: reached through reflection, as the Details panel does.
		const FStructProperty* Data = FindFProperty<FStructProperty>(UInputMappingContext::StaticClass(), TEXT("DefaultKeyMappings"));
		const FArrayProperty* List = Data ? FindFProperty<FArrayProperty>(Data->Struct, TEXT("Mappings")) : nullptr;
		if (!List)
			return UnrealMcpRql::Failed<int32>(TEXT("this engine version stores mapping context keys differently (no DefaultKeyMappings.Mappings)"));
		Context->Modify();
		TArray<FEnhancedActionKeyMapping>& Mappings = *List->ContainerPtrToValuePtr<TArray<FEnhancedActionKeyMapping>>(Data->ContainerPtrToValuePtr<void>(Context));
		Mappings.RemoveAll([Action, &Parsed](const FEnhancedActionKeyMapping& Mapping) { return Mapping.Action == Action && Mapping.Key == Parsed; });
		Context->MarkPackageDirty();
	}
	return Removed;
}

int32 UUnrealAIEnhancedInputRqlApi::SetMappingTriggers(UInputMappingContext* Context, UInputAction* Action, const FString& Key, const FString& Triggers)
{
	TArray<UnrealAIEnhancedInputText::FEntry> Entries;
	if (!UnrealAIEnhancedInputText::Parse(Triggers, UInputTrigger::StaticClass(), Entries))
		return 0;
	return ChangeMappings(Context, FindMappings(Context, Action, Key), [&Entries, Context](FEnhancedActionKeyMapping& Mapping)
	{
		Mapping.Triggers = UnrealAIEnhancedInputText::BuildAll<UInputTrigger>(Entries, Context);
	});
}

int32 UUnrealAIEnhancedInputRqlApi::SetMappingModifiers(UInputMappingContext* Context, UInputAction* Action, const FString& Key, const FString& Modifiers)
{
	TArray<UnrealAIEnhancedInputText::FEntry> Entries;
	if (!UnrealAIEnhancedInputText::Parse(Modifiers, UInputModifier::StaticClass(), Entries))
		return 0;
	return ChangeMappings(Context, FindMappings(Context, Action, Key), [&Entries, Context](FEnhancedActionKeyMapping& Mapping)
	{
		Mapping.Modifiers = UnrealAIEnhancedInputText::BuildAll<UInputModifier>(Entries, Context);
	});
}

FString UUnrealAIEnhancedInputRqlApi::SetActionTriggers(UInputAction* Action, const FString& Triggers)
{
	TArray<UnrealAIEnhancedInputText::FEntry> Entries;
	if (!UnrealAIEnhancedInputText::Parse(Triggers, UInputTrigger::StaticClass(), Entries))
		return FString();
	Action->Modify();
	Action->Triggers = UnrealAIEnhancedInputText::BuildAll<UInputTrigger>(Entries, Action);
	ActionChanged(Action, UInputAction::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UInputAction, Triggers)));
	return UnrealAIEnhancedInputText::DescribeList(Action->Triggers);
}

FString UUnrealAIEnhancedInputRqlApi::SetActionModifiers(UInputAction* Action, const FString& Modifiers)
{
	TArray<UnrealAIEnhancedInputText::FEntry> Entries;
	if (!UnrealAIEnhancedInputText::Parse(Modifiers, UInputModifier::StaticClass(), Entries))
		return FString();
	Action->Modify();
	Action->Modifiers = UnrealAIEnhancedInputText::BuildAll<UInputModifier>(Entries, Action);
	ActionChanged(Action, UInputAction::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UInputAction, Modifiers)));
	return UnrealAIEnhancedInputText::DescribeList(Action->Modifiers);
}

FString UUnrealAIEnhancedInputRqlApi::SetPlayerMappable(UInputAction* Action, const FString& DisplayName, const FString& DisplayCategory, const FString& Name)
{
	const FString MappingName = TrimmedOr(Name, Action->GetName());
	const FString Display = TrimmedOr(DisplayName, PrettyName(Action));
	Action->Modify();
	// Protected in UInputAction: written through reflection, as the Details panel does.
	FProperty* Property = ActionSettingsProperty();
	CastFieldChecked<FObjectPropertyBase>(Property)->SetObjectPropertyValue_InContainer(Action,
		KeySettings(Action, Action->GetPlayerMappableKeySettings(), MappingName, Display, DisplayCategory.TrimStartAndEnd()));
	ActionChanged(Action, Property);
	return MappingName;
}

int32 UUnrealAIEnhancedInputRqlApi::SetMappingPlayerMappable(UInputMappingContext* Context, UInputAction* Action, const FString& Key, const FString& Mode,
	const FString& DisplayName, const FString& DisplayCategory, const FString& Name)
{
	const FString Wanted = Mode.TrimStartAndEnd();
	EPlayerMappableKeySettingBehaviors Behavior;
	if (Wanted.Equals(TEXT("Override"), ESearchCase::IgnoreCase) || Wanted.IsEmpty())
		Behavior = EPlayerMappableKeySettingBehaviors::OverrideSettings;
	else if (Wanted.Equals(TEXT("Inherit"), ESearchCase::IgnoreCase))
		Behavior = EPlayerMappableKeySettingBehaviors::InheritSettingsFromAction;
	else if (Wanted.Equals(TEXT("Ignore"), ESearchCase::IgnoreCase) || Wanted.Equals(TEXT("None"), ESearchCase::IgnoreCase))
		Behavior = EPlayerMappableKeySettingBehaviors::IgnoreSettings;
	else
		return UnrealMcpRql::Failed<int32>(FString::Printf(TEXT("'%s' is not a mode (Override: its own settings, Inherit: the action's, Ignore: never rebindable)"), *Mode));
	const TArray<int32> Indexes = FindMappings(Context, Action, Key);
	return ChangeMappings(Context, Indexes, [&](FEnhancedActionKeyMapping& Mapping)
	{
		UPlayerMappableKeySettings* Settings = nullptr;
		if (Behavior == EPlayerMappableKeySettingBehaviors::OverrideSettings)
		{
			const FString MappingName = TrimmedOr(Name, FString::Printf(TEXT("%s_%s"), *Action->GetName(), *Mapping.Key.GetFName().ToString()));
			UPlayerMappableKeySettings* Existing = Cast<UPlayerMappableKeySettings>(MappingSettingsProperty()->GetObjectPropertyValue_InContainer(&Mapping));
			Settings = KeySettings(Context, Existing, MappingName, TrimmedOr(DisplayName, PrettyName(Action)), DisplayCategory.TrimStartAndEnd());
		}
		SetMappingSettings(Mapping, Behavior, Settings);
	});
}

FString UUnrealAIEnhancedInputRqlApi::EnableUserSettings(bool Enable)
{
	UEnhancedInputDeveloperSettings* Settings = GetMutableDefault<UEnhancedInputDeveloperSettings>();
	FBoolProperty* Property = FindFProperty<FBoolProperty>(UEnhancedInputDeveloperSettings::StaticClass(), TEXT("bEnableUserSettings"));
	if (!Property)
		return UnrealMcpRql::Failed<FString>(TEXT("this engine version has no Enhanced Input user settings (bEnableUserSettings)"));
	const bool bWas = Property->GetPropertyValue_InContainer(Settings);
	// Checked before the change: a failed write would leave the editor's setting and the ini disagreeing.
	const FString Ini = Settings->GetDefaultConfigFilename();
	if (IFileManager::Get().FileExists(*Ini) && IFileManager::Get().IsReadOnly(*Ini))
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s is read-only (checked in to source control?): check it out first"), *Ini));
	Settings->Modify();
	Property->SetPropertyValue_InContainer(Settings, Enable);
	FPropertyChangedEvent Changed(Property);
	Settings->PostEditChangeProperty(Changed);
	if (!Settings->TryUpdateDefaultConfigFile())
		return UnrealMcpRql::Failed<FString>(TEXT("could not write Config/DefaultInput.ini (read-only or checked out by source control?)"));
	return FString::Printf(TEXT("Enable User Settings: %s -> %s (Config/DefaultInput.ini). %s"), bWas ? TEXT("true") : TEXT("false"), Enable ? TEXT("true") : TEXT("false"),
		Enable ? TEXT("Each local player now gets UEnhancedInputUserSettings (EnhancedInputLocalPlayerSubsystem.GetUserSettings): contexts added with "
			"AddMappingContextOnPossess / OnEvent register their player-mappable keys; a key-binding menu calls MapPlayerKey / UnMapPlayerKey and SaveSettings.")
		: TEXT(""));
}

UEdGraphNode* UUnrealAIEnhancedInputRqlApi::AddActionEvent(UObject* Graph, UInputAction* Action)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* EventGraph = UnrealAIEnhancedInputGraph::EventGraphOf(Graph, Blueprint);
	if (!EventGraph)
		return nullptr;
	bool bCreated = false;
	UEdGraphNode* Node = UnrealAIEnhancedInputGraph::FindOrAddActionEvent(Blueprint, EventGraph, Action, bCreated);
	if (bCreated)
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	if (Node && !bCreated)
		return UnrealMcpRql::Failed<UEdGraphNode*>(FString::Printf(TEXT("%s already has an action event for %s (one per action: connect more "
			"nodes to its pins, or BindAction(...) which appends; find it with unreal.asset(path).EventGraph | get Nodes | where Title contains \"%s\")"),
			*Blueprint->GetName(), *Action->GetName(), *Action->GetName()));
	return Node;
}

UEdGraphNode* UUnrealAIEnhancedInputRqlApi::BindAction(UObject* Blueprint, UInputAction* Action, const FString& Event, const FString& Function, const FString& Value)
{
	UBlueprint* Owner = nullptr;
	UEdGraph* EventGraph = UnrealAIEnhancedInputGraph::EventGraphOf(Blueprint, Owner);
	return EventGraph ? UnrealAIEnhancedInputGraph::BindAction(Owner, EventGraph, Action, Event, Function, Value) : nullptr;
}

UEdGraphNode* UUnrealAIEnhancedInputRqlApi::BindActionToggle(UObject* Blueprint, UInputAction* Action, const FString& OnFunction, const FString& OffFunction, const FString& Event)
{
	UBlueprint* Owner = nullptr;
	UEdGraph* EventGraph = UnrealAIEnhancedInputGraph::EventGraphOf(Blueprint, Owner);
	return EventGraph ? UnrealAIEnhancedInputGraph::BindActionToggle(Owner, EventGraph, Action, Event, OnFunction, OffFunction) : nullptr;
}

UEdGraphNode* UUnrealAIEnhancedInputRqlApi::AddMappingContextOnBeginPlay(UObject* Blueprint, UInputMappingContext* Context, int32 Priority)
{
	return AddMappingContextOnEvent(Blueprint, Context, TEXT("BeginPlay"), Priority);
}

UEdGraphNode* UUnrealAIEnhancedInputRqlApi::AddMappingContextOnPossess(UObject* Blueprint, UInputMappingContext* Context, int32 Priority)
{
	UBlueprint* Owner = nullptr;
	UEdGraph* EventGraph = ContextTarget(Blueprint, Context, Owner);
	if (!EventGraph)
		return nullptr;
	if (!Owner->ParentClass->IsChildOf(APawn::StaticClass()))
		return UnrealMcpRql::Failed<UEdGraphNode*>(FString::Printf(TEXT("%s is not a Pawn Blueprint (a %s): possession is a pawn's; a PlayerController adds its "
			"contexts with AddMappingContextOnEvent(bp, ctx, \"BeginPlay\")"), *Owner->GetName(), *Owner->ParentClass->GetName()));
	// On Controller Changed: the old controller's player loses the context, the new one's gains it (each skipped when not a player).
	UnrealAIEnhancedInputGraph::FContextStep Steps[2];
	Steps[0].bAdd = false;
	Steps[0].Context = Context;
	Steps[0].ControllerParameter = TEXT("OldController");
	Steps[1].Context = Context;
	Steps[1].Priority = Priority;
	Steps[1].ControllerParameter = TEXT("NewController");
	return UnrealAIEnhancedInputGraph::AddContextSteps(Owner, EventGraph, TEXT("ControllerChanged"), Steps);
}

UEdGraphNode* UUnrealAIEnhancedInputRqlApi::AddMappingContextOnEvent(UObject* Blueprint, UInputMappingContext* Context, const FString& Event, int32 Priority)
{
	UBlueprint* Owner = nullptr;
	UEdGraph* EventGraph = ContextTarget(Blueprint, Context, Owner);
	if (!EventGraph)
		return nullptr;
	UnrealAIEnhancedInputGraph::FContextStep Step;
	Step.Context = Context;
	Step.Priority = Priority;
	const FString Name = Event.TrimStartAndEnd();
	if (Name.Equals(TEXT("Possessed"), ESearchCase::IgnoreCase))
		Step.ControllerParameter = TEXT("NewController");
	return UnrealAIEnhancedInputGraph::AddContextSteps(Owner, EventGraph, Name, MakeArrayView(&Step, 1));
}

UEdGraphNode* UUnrealAIEnhancedInputRqlApi::RemoveMappingContextOnEvent(UObject* Blueprint, UInputMappingContext* Context, const FString& Event)
{
	UBlueprint* Owner = nullptr;
	UEdGraph* EventGraph = UnrealAIEnhancedInputGraph::EventGraphOf(Blueprint, Owner);
	if (!EventGraph)
		return nullptr;
	const FString Name = Event.TrimStartAndEnd();
	if (UnrealAIEnhancedInputGraph::FindContextCall(Owner, Context, /*bAdd*/ false, Name))
		return UnrealMcpRql::Failed<UEdGraphNode*>(FString::Printf(TEXT("%s already removes %s on %s"), *Owner->GetName(), *Context->GetName(), *Name));
	UnrealAIEnhancedInputGraph::FContextStep Step;
	Step.bAdd = false;
	Step.Context = Context;
	if (Name.Equals(TEXT("Unpossessed"), ESearchCase::IgnoreCase) || Name.Equals(TEXT("ControllerChanged"), ESearchCase::IgnoreCase))
		Step.ControllerParameter = TEXT("OldController");
	return UnrealAIEnhancedInputGraph::AddContextSteps(Owner, EventGraph, Name, MakeArrayView(&Step, 1));
}
