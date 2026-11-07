// Copyright (c) 2026 IvanMurzak/Unreal-AI-GAS. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIGASRqlApi.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealAIAbilitySystemComponent.h"
#include "UnrealAIAttributeSet.h"
#include "UnrealAIAttributeTextComponent.h"
#include "UnrealAIGameplayAbility.h"
#include "UnrealAIGASAttributes.h"
#include "UnrealAIGASProjectile.h"

#include "Abilities/GameplayAbility.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "AbilitySystemInterface.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameplayAbilityBlueprint.h"
#include "GameplayAbilitySpec.h"
#include "GameplayEffect.h"
#include "GameplayEffectComponents/AssetTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/BlockAbilityTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/TargetTagRequirementsGameplayEffectComponent.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#include "GameplayTagsEditorModule.h"
#include "GameplayTagsManager.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "Interfaces/IProjectManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/StringOutputDevice.h"
#include "ProjectDescriptor.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UnrealAIGASRqlApiLocal
{
	using UnrealMcpRql::Failed;

	// ------------------------------------------------------------ text

	/** "A, B" / ["A", "B"] → trimmed, unquoted, non-empty items. */
	TArray<FString> SplitList(const FString& Text)
	{
		FString Clean = Text.Replace(TEXT("["), TEXT("")).Replace(TEXT("]"), TEXT("")).Replace(TEXT("\""), TEXT("")).Replace(TEXT("'"), TEXT(""));
		TArray<FString> Items;
		Clean.ParseIntoArray(Items, TEXT(","), true);
		for (FString& Item : Items)
			Item.TrimStartAndEndInline();
		Items.RemoveAll([](const FString& Item) { return Item.IsEmpty(); });
		return Items;
	}

	bool ParseNumber(const FString& Text, float& Out)
	{
		const FString Trimmed = Text.TrimStartAndEnd();
		return !Trimmed.IsEmpty() && LexTryParseString(Out, *Trimmed);
	}

	/** "Health=100, MaxHealth=100" → pairs; false (after Fail) on a malformed item. */
	bool ParsePairs(const FString& Text, const TCHAR* What, TArray<TPair<FString, float>>& Out)
	{
		for (const FString& Item : SplitList(Text))
		{
			FString Name, Value;
			float Number = 0.f;
			if (!Item.Split(TEXT("="), &Name, &Value) || Name.TrimStartAndEnd().IsEmpty() || !ParseNumber(Value, Number))
			{
				UnrealMcpRql::Fail(FString::Printf(TEXT("'%s' is not %s=<number> (e.g. \"%s\")"), *Item, What,
					FCString::Strcmp(What, TEXT("Tag")) == 0 ? TEXT("Damage=25") : TEXT("Health=100, MaxHealth=100")));
				return false;
			}
			Out.Emplace(Name.TrimStartAndEnd(), Number);
		}
		return true;
	}

	/** A gameplay tag by name; one that does not exist yet is added to the project's tag ini (DefaultGameplayTags.ini). */
	bool EnsureTag(const FString& Name, FGameplayTag& Out)
	{
		UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
		FText Error;
		FString Fixed;
		if (!Manager.IsValidGameplayTagString(Name, &Error, &Fixed))
		{
			UnrealMcpRql::Fail(FString::Printf(TEXT("'%s' is not a valid gameplay tag: %s%s"), *Name, *Error.ToString(),
				Fixed.IsEmpty() || Fixed == Name ? TEXT("") : *FString::Printf(TEXT(" (try \"%s\")"), *Fixed)));
			return false;
		}
		Out = Manager.RequestGameplayTag(FName(*Name), false);
		if (Out.IsValid())
			return true;
		if (!Manager.ShouldImportTagsFromINI())
		{
			UnrealMcpRql::Fail(FString::Printf(TEXT("the gameplay tag '%s' does not exist and this project does not read tags from config (add it in Project Settings > GameplayTags)"), *Name));
			return false;
		}
		IGameplayTagsEditorModule* TagsEditor = FModuleManager::LoadModulePtr<IGameplayTagsEditorModule>(TEXT("GameplayTagsEditor"));
		if (!TagsEditor || !TagsEditor->AddNewGameplayTagToINI(Name, TEXT("Added by Unreal AI GAS")))
		{
			UnrealMcpRql::Fail(FString::Printf(TEXT("could not add the gameplay tag '%s' to the project's tags"), *Name));
			return false;
		}
		Out = Manager.RequestGameplayTag(FName(*Name), false);
		if (!Out.IsValid())
		{
			UnrealMcpRql::Fail(FString::Printf(TEXT("the gameplay tag '%s' was added but is not registered yet"), *Name));
			return false;
		}
		return true;
	}

	bool ParseTags(const FString& Text, FGameplayTagContainer& Out)
	{
		for (const FString& Name : SplitList(Text))
		{
			FGameplayTag Tag;
			if (!EnsureTag(Name, Tag))
				return false;
			Out.AddTag(Tag);
		}
		return true;
	}

	FString TagsText(const FGameplayTagContainer& Tags)
	{
		return Tags.IsEmpty() ? FString(TEXT("(none)")) : Tags.ToStringSimple();
	}

	/** "Instant" / "Infinite" / seconds. */
	bool ParseDuration(const FString& Text, EGameplayEffectDurationType& Policy, float& Seconds)
	{
		const FString Trimmed = Text.TrimStartAndEnd();
		Seconds = 0.f;
		if (Trimmed.Equals(TEXT("Instant"), ESearchCase::IgnoreCase) || Trimmed.IsEmpty())
			Policy = EGameplayEffectDurationType::Instant;
		else if (Trimmed.Equals(TEXT("Infinite"), ESearchCase::IgnoreCase))
			Policy = EGameplayEffectDurationType::Infinite;
		else if (ParseNumber(Trimmed, Seconds) && Seconds > 0.f)
			Policy = EGameplayEffectDurationType::HasDuration;
		else
		{
			UnrealMcpRql::Fail(FString::Printf(TEXT("Duration '%s' is not \"Instant\", \"Infinite\" or a number of seconds above 0"), *Text));
			return false;
		}
		return true;
	}

	FString DurationText(const UGameplayEffect& Effect)
	{
		FString Text;
		switch (Effect.DurationPolicy)
		{
		case EGameplayEffectDurationType::Instant: Text = TEXT("Instant"); break;
		case EGameplayEffectDurationType::Infinite: Text = TEXT("Infinite"); break;
		default:
		{
			float Seconds = 0.f;
			Effect.DurationMagnitude.GetStaticMagnitudeIfPossible(1.f, Seconds);
			Text = FString::Printf(TEXT("%gs"), Seconds);
		}
		}
		const float Period = Effect.Period.GetValueAtLevel(1.f);
		if (Effect.DurationPolicy != EGameplayEffectDurationType::Instant && Period > 0.f)
			Text += FString::Printf(TEXT(", every %gs"), Period);
		return Text;
	}

	// ------------------------------------------------------------ Blueprint classes

	/** A Blueprint asset at Path (not taken yet) whose class derives from Parent; unsaved. */
	UBlueprint* CreateBlueprintAsset(const FString& Path, UClass* Parent, TSubclassOf<UBlueprint> BlueprintType)
	{
		const FString Trimmed = Path.TrimStartAndEnd();
		if (!FPackageName::IsValidLongPackageName(Trimmed) || FPackageName::GetLongPackageAssetName(Trimmed).IsEmpty())
			return Failed<UBlueprint*>(FString::Printf(TEXT("'%s' is not an asset path such as /Game/GAS/GE_Damage"), *Path));
		if (FindPackage(nullptr, *Trimmed) || FPackageName::DoesPackageExist(Trimmed))
			return Failed<UBlueprint*>(FString::Printf(TEXT("an asset already exists at '%s' (pick another path, or editor | call DeleteAsset(\"%s\"))"), *Trimmed, *Trimmed));
		UPackage* Package = CreatePackage(*Trimmed);
		UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(Parent, Package, FName(*FPackageName::GetLongPackageAssetName(Trimmed)),
			BPTYPE_Normal, BlueprintType, UBlueprintGeneratedClass::StaticClass(), TEXT("UnrealAIGAS"));
		if (!Blueprint || !Blueprint->GeneratedClass)
			return Failed<UBlueprint*>(FString::Printf(TEXT("could not create a %s Blueprint at '%s'"), *Parent->GetName(), *Trimmed));
		FAssetRegistryModule::AssetCreated(Blueprint);
		Package->MarkPackageDirty();
		return Blueprint;
	}

	/** The class default object of a project Blueprint class (writable, Modify()d); nullptr after Fail for native classes. */
	template <typename T>
	T* EditableDefaults(UClass* Class, const TCHAR* Create)
	{
		if (!Class)
			return Failed<T*>(TEXT("no class given"));
		if (!Class->IsChildOf(T::StaticClass()))
			return Failed<T*>(FString::Printf(TEXT("%s is not a %s"), *Class->GetName(), *T::StaticClass()->GetName()));
		if (!UBlueprint::GetBlueprintFromClass(Class))
			return Failed<T*>(FString::Printf(TEXT("%s is a native class, whose defaults are code: %s"), *Class->GetName(), Create));
		T* Defaults = Class->GetDefaultObject<T>();
		Defaults->Modify();
		return Defaults;
	}

	/** Mark a class default object's Blueprint changed (saved with it; no recompile needed). */
	void DefaultsChanged(UObject& Defaults)
	{
		if (UGameplayEffect* Effect = Cast<UGameplayEffect>(&Defaults))
			Effect->OnGameplayEffectChanged();
		Defaults.MarkPackageDirty();
		if (UBlueprint* Blueprint = UBlueprint::GetBlueprintFromClass(Defaults.GetClass()))
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}

	FString AssetPath(const UClass& Class)
	{
		return Class.GetOutermost()->GetName();
	}

	/** The Gameplay Effect Blueprint at Path, or a new one (Instant) when nothing is there; nullptr after Fail. */
	UGameplayEffect* FindOrCreateEffect(const FString& Path)
	{
		if (UBlueprint* Existing = Cast<UBlueprint>(UnrealMcpRql::LoadObject(UBlueprint::StaticClass(), Path)))
		{
			if (!Existing->GeneratedClass || !Existing->GeneratedClass->IsChildOf(UGameplayEffect::StaticClass()))
				return Failed<UGameplayEffect*>(FString::Printf(TEXT("%s is not a Gameplay Effect Blueprint"), *Path));
			UGameplayEffect* Effect = Existing->GeneratedClass->GetDefaultObject<UGameplayEffect>();
			Effect->Modify();
			return Effect;
		}
		UBlueprint* Created = CreateBlueprintAsset(Path, UGameplayEffect::StaticClass(), UBlueprint::StaticClass());
		return Created ? Created->GeneratedClass->GetDefaultObject<UGameplayEffect>() : nullptr;
	}

	void SetDuration(UGameplayEffect& Effect, EGameplayEffectDurationType Policy, float Seconds, float Period, bool bExecuteOnApplication)
	{
		Effect.DurationPolicy = Policy;
		Effect.DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(Policy == EGameplayEffectDurationType::HasDuration ? Seconds : 0.f));
		Effect.Period = FScalableFloat(Policy == EGameplayEffectDurationType::Instant ? 0.f : Period);
		Effect.bExecutePeriodicEffectOnApplication = bExecuteOnApplication;
	}

	/** A reflected property value of an object (protected UGameplayAbility members are edited this way, as the Details panel does). */
	template <typename T>
	T* PropertyValue(UObject& Object, const TCHAR* Name)
	{
		FProperty* Property = FindFProperty<FProperty>(Object.GetClass(), Name);
		return Property ? Property->ContainerPtrToValuePtr<T>(&Object) : nullptr;
	}

	bool SetClassProperty(UObject& Object, const TCHAR* Name, UClass* Value)
	{
		FClassProperty* Property = FindFProperty<FClassProperty>(Object.GetClass(), Name);
		if (!Property)
			return false;
		Property->SetObjectPropertyValue_InContainer(&Object, Value);
		return true;
	}

	FString ClassName(const UClass* Class)
	{
		return Class ? Class->GetName() : FString(TEXT("None"));
	}

	// ------------------------------------------------------------ Blueprint components

	USimpleConstructionScript* Construction(UBlueprint& Blueprint)
	{
		if (!Blueprint.GeneratedClass || !Blueprint.GeneratedClass->IsChildOf(AActor::StaticClass()) || !Blueprint.SimpleConstructionScript)
			return Failed<USimpleConstructionScript*>(FString::Printf(TEXT("%s is not an actor Blueprint (create one with blueprint-create, parent Character or Actor)"), *Blueprint.GetName()));
		return Blueprint.SimpleConstructionScript;
	}

	USCS_Node* FindNode(UBlueprint& Blueprint, UClass* Class)
	{
		if (!Blueprint.SimpleConstructionScript)
			return nullptr;
		for (USCS_Node* Node : Blueprint.SimpleConstructionScript->GetAllNodes())
		{
			if (Node && Node->ComponentTemplate && Node->ComponentTemplate->IsA(Class))
				return Node;
		}
		return nullptr;
	}

	/** Recompile so placed actors and the next Play In Editor rebuild their components from the changed templates. */
	void Recompile(UBlueprint& Blueprint)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
		FKismetEditorUtilities::CompileBlueprint(&Blueprint);
	}

	/** The UnrealAIAbilitySystemComponent template of Blueprint; nullptr after Fail when it has none. */
	UUnrealAIAbilitySystemComponent* AbilitySystemTemplate(UBlueprint& Blueprint)
	{
		if (!Construction(Blueprint))
			return nullptr;
		if (USCS_Node* Node = FindNode(Blueprint, UUnrealAIAbilitySystemComponent::StaticClass()))
		{
			Node->ComponentTemplate->Modify();
			return Cast<UUnrealAIAbilitySystemComponent>(Node->ComponentTemplate);
		}
		return Failed<UUnrealAIAbilitySystemComponent*>(FString::Printf(TEXT("%s has no UnrealAIAbilitySystemComponent: gas | call AddAbilitySystem(\"%s\") first"),
			*Blueprint.GetName(), *Blueprint.GetOutermost()->GetName()));
	}

	/** A new component node of Class named Name: scene components go under the Blueprint's own scene root when it has one. */
	USCS_Node* AddNode(UBlueprint& Blueprint, UClass* Class, const TCHAR* Name)
	{
		USimpleConstructionScript* Script = Blueprint.SimpleConstructionScript;
		USCS_Node* Node = Script->CreateNode(Class, FName(Name));
		USCS_Node* Parent = nullptr;
		const AActor* Defaults = Blueprint.GeneratedClass->GetDefaultObject<AActor>();
		if (Class->IsChildOf(USceneComponent::StaticClass()) && !(Defaults && Defaults->GetRootComponent()))
		{
			for (USCS_Node* Root : Script->GetRootNodes())
			{
				if (Root && Root->ComponentTemplate && Root->ComponentTemplate->IsA(USceneComponent::StaticClass()))
				{
					Parent = Root;
					break;
				}
			}
		}
		if (Parent)
			Parent->AddChildNode(Node);
		else
			Script->AddNode(Node);
		return Node;
	}

	// ------------------------------------------------------------ the running game

	/** An actor of the running game by label or name ("" = the player's pawn); nullptr after Fail when the game is not running. */
	AActor* FindGameActor(const FString& Name)
	{
		UWorld* World = GEditor ? GEditor->PlayWorld.Get() : nullptr;
		if (!World)
			return Failed<AActor*>(TEXT("the game is not running: start Play In Editor (editor | set IsPlaying = true), then call again"));
		const FString Trimmed = Name.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			const APlayerController* Player = World->GetFirstPlayerController();
			if (Player && Player->GetPawn())
				return Player->GetPawn();
			return Failed<AActor*>(TEXT("the running game has no player pawn (yet); name an actor"));
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->GetActorNameOrLabel().Equals(Trimmed, ESearchCase::IgnoreCase) || It->GetName().Equals(Trimmed, ESearchCase::IgnoreCase) || It->GetPathName() == Trimmed)
				return *It;
		}
		return Failed<AActor*>(FString::Printf(TEXT("no actor '%s' in the running game (gasComponents | select Owner.Label)"), *Trimmed));
	}

	UAbilitySystemComponent* FindAbilitySystem(AActor& Actor)
	{
		UAbilitySystemComponent* Asc = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(&Actor);
		if (!Asc)
			return Failed<UAbilitySystemComponent*>(FString::Printf(TEXT("%s has no AbilitySystemComponent (gas | call AddAbilitySystem(<its Blueprint>))"), *Actor.GetActorNameOrLabel()));
		return Asc;
	}

	TMap<FString, float> AttributeValues(const UAbilitySystemComponent& Asc)
	{
		TMap<FString, float> Values;
		for (const UAttributeSet* Set : Asc.GetSpawnedAttributes())
		{
			if (Set)
			{
				for (const FGameplayAttribute& Attribute : UnrealAIGAS::AttributesOf(Set->GetClass()))
					Values.Add(Attribute.GetName(), Asc.GetNumericAttribute(Attribute));
			}
		}
		return Values;
	}
}

using namespace UnrealAIGASRqlApiLocal;

// ---------------------------------------------------------------- setup

FString UUnrealAIGASRqlApi::EnableGameplayAbilities()
{
	if (const FProjectDescriptor* Project = IProjectManager::Get().GetCurrentProject())
	{
		for (const FPluginReferenceDescriptor& Plugin : Project->Plugins)
		{
			if (Plugin.Name == TEXT("GameplayAbilities") && Plugin.bEnabled)
				return TEXT("GameplayAbilities is already enabled in the project file and loaded");
		}
	}
	FText Reason;
	if (!IProjectManager::Get().SetPluginEnabled(TEXT("GameplayAbilities"), true, Reason) || !IProjectManager::Get().SaveCurrentProjectToDisk(Reason))
		return Failed<FString>(FString::Printf(TEXT("could not enable GameplayAbilities: %s"), *Reason.ToString()));
	return TEXT("GameplayAbilities is enabled in the project file; it is already loaded (this extension uses it), so no restart is needed");
}

// ---------------------------------------------------------------- effects

UBlueprint* UUnrealAIGASRqlApi::CreateEffect(const FString& Path, const FString& Duration, float Period)
{
	EGameplayEffectDurationType Policy;
	float Seconds = 0.f;
	if (!ParseDuration(Duration, Policy, Seconds))
		return nullptr;
	if (Period < 0.f || (Period > 0.f && Policy == EGameplayEffectDurationType::Instant))
		return Failed<UBlueprint*>(TEXT("an instant effect has no period: use Duration \"Infinite\" (regeneration) or a number of seconds"));
	UBlueprint* Blueprint = CreateBlueprintAsset(Path, UGameplayEffect::StaticClass(), UBlueprint::StaticClass());
	if (!Blueprint)
		return nullptr;
	UGameplayEffect* Effect = Blueprint->GeneratedClass->GetDefaultObject<UGameplayEffect>();
	SetDuration(*Effect, Policy, Seconds, Period, true);
	DefaultsChanged(*Effect);
	return Blueprint;
}

FString UUnrealAIGASRqlApi::SetEffectDuration(TSubclassOf<UGameplayEffect> Effect, const FString& Duration, float Period, bool bExecuteOnApplication)
{
	EGameplayEffectDurationType Policy;
	float Seconds = 0.f;
	if (!ParseDuration(Duration, Policy, Seconds))
		return FString();
	if (Period < 0.f || (Period > 0.f && Policy == EGameplayEffectDurationType::Instant))
		return Failed<FString>(TEXT("an instant effect has no period: use Duration \"Infinite\" or a number of seconds"));
	UGameplayEffect* Defaults = EditableDefaults<UGameplayEffect>(Effect, TEXT("create a Blueprint with gas | call CreateEffect(path)"));
	if (!Defaults)
		return FString();
	SetDuration(*Defaults, Policy, Seconds, Period, bExecuteOnApplication);
	DefaultsChanged(*Defaults);
	return DurationText(*Defaults);
}

int32 UUnrealAIGASRqlApi::AddModifier(TSubclassOf<UGameplayEffect> Effect, const FString& Attribute, const FString& Operation, const FString& Magnitude)
{
	FString Error;
	const FGameplayAttribute Target = UnrealAIGAS::FindAttribute(Attribute, UnrealAIGAS::LoadedAttributeSetClasses(), &Error);
	if (!Target.IsValid())
		return Failed<int32>(Error);

	const FString Op = Operation.TrimStartAndEnd();
	EGameplayModOp::Type ModOp;
	if (Op.IsEmpty() || Op.Equals(TEXT("Add"), ESearchCase::IgnoreCase) || Op.Equals(TEXT("Additive"), ESearchCase::IgnoreCase) || Op == TEXT("+"))
		ModOp = EGameplayModOp::Additive;
	else if (Op.Equals(TEXT("Multiply"), ESearchCase::IgnoreCase) || Op == TEXT("*"))
		ModOp = EGameplayModOp::Multiplicitive;
	else if (Op.Equals(TEXT("Divide"), ESearchCase::IgnoreCase) || Op == TEXT("/"))
		ModOp = EGameplayModOp::Division;
	else if (Op.Equals(TEXT("Override"), ESearchCase::IgnoreCase) || Op == TEXT("="))
		ModOp = EGameplayModOp::Override;
	else
		return Failed<int32>(FString::Printf(TEXT("Operation '%s' is not Add, Multiply, Divide or Override"), *Operation));

	FGameplayEffectModifierMagnitude Value;
	const FString Text = Magnitude.TrimStartAndEnd();
	float Number = 0.f;
	if (ParseNumber(Text, Number))
	{
		Value = FGameplayEffectModifierMagnitude(FScalableFloat(Number));
	}
	else if (Text.StartsWith(TEXT("SetByCaller"), ESearchCase::IgnoreCase))
	{
		// "SetByCaller" (SetByCaller.Damage), "SetByCaller.Heal", "SetByCaller(Heal)".
		FString Name = Text.Mid(11).Replace(TEXT("("), TEXT("")).Replace(TEXT(")"), TEXT("")).TrimStartAndEnd();
		Name.RemoveFromStart(TEXT("."));
		FSetByCallerFloat SetByCaller;
		if (Name.IsEmpty())
			SetByCaller.DataTag = TAG_UnrealAI_SetByCaller_Damage;
		else if (!EnsureTag(Name.Contains(TEXT(".")) ? Name : TEXT("SetByCaller.") + Name, SetByCaller.DataTag))
			return 0;
		Value = FGameplayEffectModifierMagnitude(SetByCaller);
	}
	else if (Text.StartsWith(TEXT("Attribute("), ESearchCase::IgnoreCase) && Text.EndsWith(TEXT(")")))
	{
		// "Attribute(Source.AttackPower, 1.5)": a captured attribute times a coefficient.
		TArray<FString> Parts = SplitList(Text.Mid(10, Text.Len() - 11));
		float Coefficient = 1.f;
		FString Source, Name;
		if (Parts.IsEmpty() || Parts.Num() > 2 || (Parts.Num() == 2 && !ParseNumber(Parts[1], Coefficient)) || !Parts[0].Split(TEXT("."), &Source, &Name)
			|| !(Source.Equals(TEXT("Source"), ESearchCase::IgnoreCase) || Source.Equals(TEXT("Target"), ESearchCase::IgnoreCase)))
			return Failed<int32>(FString::Printf(TEXT("'%s' is not Attribute(Source.<attribute>, <coefficient>) or Attribute(Target.<attribute>, <coefficient>)"), *Magnitude));
		const FGameplayAttribute Backing = UnrealAIGAS::FindAttribute(Name, UnrealAIGAS::LoadedAttributeSetClasses(), &Error);
		if (!Backing.IsValid())
			return Failed<int32>(Error);
		FAttributeBasedFloat Based;
		Based.Coefficient = FScalableFloat(Coefficient);
		Based.BackingAttribute = FGameplayEffectAttributeCaptureDefinition(Backing,
			Source.Equals(TEXT("Source"), ESearchCase::IgnoreCase) ? EGameplayEffectAttributeCaptureSource::Source : EGameplayEffectAttributeCaptureSource::Target, false);
		Value = FGameplayEffectModifierMagnitude(Based);
	}
	else
	{
		return Failed<int32>(FString::Printf(TEXT("Magnitude '%s' is not a number, \"SetByCaller\", \"SetByCaller.<Tag>\" or \"Attribute(Source.<attribute>, <coefficient>)\""), *Magnitude));
	}

	UGameplayEffect* Defaults = EditableDefaults<UGameplayEffect>(Effect, TEXT("create a Blueprint with gas | call CreateEffect(path)"));
	if (!Defaults)
		return 0;
	FGameplayModifierInfo Modifier;
	Modifier.Attribute = Target;
	Modifier.ModifierOp = ModOp;
	Modifier.ModifierMagnitude = Value;
	Defaults->Modifiers.Add(Modifier);
	DefaultsChanged(*Defaults);
	return Defaults->Modifiers.Num();
}

int32 UUnrealAIGASRqlApi::RemoveModifiers(TSubclassOf<UGameplayEffect> Effect, const FString& Attribute)
{
	FGameplayAttribute Target;
	if (!Attribute.TrimStartAndEnd().IsEmpty())
	{
		FString Error;
		Target = UnrealAIGAS::FindAttribute(Attribute, UnrealAIGAS::LoadedAttributeSetClasses(), &Error);
		if (!Target.IsValid())
			return Failed<int32>(Error);
	}
	UGameplayEffect* Defaults = EditableDefaults<UGameplayEffect>(Effect, TEXT("create a Blueprint with gas | call CreateEffect(path)"));
	if (!Defaults)
		return 0;
	const int32 Removed = Defaults->Modifiers.RemoveAll([&Target](const FGameplayModifierInfo& Modifier) { return !Target.IsValid() || Modifier.Attribute == Target; });
	DefaultsChanged(*Defaults);
	return Removed;
}

FString UUnrealAIGASRqlApi::SetEffectTags(TSubclassOf<UGameplayEffect> Effect, const FString& Kind, const FString& Tags)
{
	static const TCHAR* Kinds[] = { TEXT("Granted"), TEXT("Asset"), TEXT("BlockAbilities"), TEXT("ApplicationRequired"), TEXT("ApplicationBlocked") };
	int32 KindIndex = INDEX_NONE;
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Kinds); ++Index)
	{
		if (Kind.TrimStartAndEnd().Equals(Kinds[Index], ESearchCase::IgnoreCase))
			KindIndex = Index;
	}
	if (KindIndex == INDEX_NONE)
		return Failed<FString>(FString::Printf(TEXT("Kind '%s' is not Granted, Asset, BlockAbilities, ApplicationRequired or ApplicationBlocked"), *Kind));
	UGameplayEffect* Defaults = EditableDefaults<UGameplayEffect>(Effect, TEXT("create a Blueprint with gas | call CreateEffect(path)"));
	FGameplayTagContainer Container;
	if (!Defaults || !ParseTags(Tags, Container))
		return FString();

	FInheritedTagContainer Inherited;
	for (const FGameplayTag& Tag : Container)
		Inherited.AddTag(Tag);
	switch (KindIndex)
	{
	case 0:
	{
		UTargetTagsGameplayEffectComponent& Component = Defaults->FindOrAddComponent<UTargetTagsGameplayEffectComponent>();
		Component.Modify();
		Component.SetAndApplyTargetTagChanges(Inherited);
		break;
	}
	case 1:
	{
		UAssetTagsGameplayEffectComponent& Component = Defaults->FindOrAddComponent<UAssetTagsGameplayEffectComponent>();
		Component.Modify();
		Component.SetAndApplyAssetTagChanges(Inherited);
		break;
	}
	case 2:
	{
		UBlockAbilityTagsGameplayEffectComponent& Component = Defaults->FindOrAddComponent<UBlockAbilityTagsGameplayEffectComponent>();
		Component.Modify();
		Component.SetAndApplyBlockedAbilityTagChanges(Inherited);
		break;
	}
	default:
	{
		UTargetTagRequirementsGameplayEffectComponent& Component = Defaults->FindOrAddComponent<UTargetTagRequirementsGameplayEffectComponent>();
		Component.Modify();
		(KindIndex == 3 ? Component.ApplicationTagRequirements.RequireTags : Component.ApplicationTagRequirements.IgnoreTags) = Container;
		break;
	}
	}
	DefaultsChanged(*Defaults);
	return FString::Printf(TEXT("%s: %s"), Kinds[KindIndex], *TagsText(Container));
}

FString UUnrealAIGASRqlApi::SetStacking(TSubclassOf<UGameplayEffect> Effect, const FString& Type, int32 Limit, bool bRefreshDuration, bool bResetPeriod)
{
	const UEnum* Enum = StaticEnum<EGameplayEffectStackingType>();
	const int64 Value = Enum->GetValueByNameString(Type.TrimStartAndEnd());
	if (Value == INDEX_NONE)
		return Failed<FString>(FString::Printf(TEXT("Type '%s' is not None, AggregateBySource or AggregateByTarget"), *Type));
	if (Limit < 0)
		return Failed<FString>(TEXT("Limit must be 0 (no limit) or more"));
	UGameplayEffect* Defaults = EditableDefaults<UGameplayEffect>(Effect, TEXT("create a Blueprint with gas | call CreateEffect(path)"));
	if (!Defaults)
		return FString();
	if (Value != static_cast<int64>(EGameplayEffectStackingType::None) && Defaults->DurationPolicy == EGameplayEffectDurationType::Instant)
		return Failed<FString>(TEXT("an instant effect does not stack: give it a duration first (SetEffectDuration)"));
	// StackingType is deprecated for direct access (5.7) and its setter is not exported: write it as the Details panel does.
	if (uint8* Stacking = PropertyValue<uint8>(*Defaults, TEXT("StackingType")))
		*Stacking = static_cast<uint8>(Value);
	Defaults->StackLimitCount = Limit;
	Defaults->StackDurationRefreshPolicy = bRefreshDuration ? EGameplayEffectStackingDurationPolicy::RefreshOnSuccessfulApplication : EGameplayEffectStackingDurationPolicy::NeverRefresh;
	Defaults->StackPeriodResetPolicy = bResetPeriod ? EGameplayEffectStackingPeriodPolicy::ResetOnSuccessfulApplication : EGameplayEffectStackingPeriodPolicy::NeverReset;
	DefaultsChanged(*Defaults);
	return FString::Printf(TEXT("%s, limit %d%s%s"), *Enum->GetNameStringByValue(Value), Limit,
		bRefreshDuration ? TEXT(", refreshes duration") : TEXT(""), bResetPeriod ? TEXT(", resets period") : TEXT(""));
}

// ---------------------------------------------------------------- abilities

UBlueprint* UUnrealAIGASRqlApi::CreateAbility(const FString& Path, TSubclassOf<UGameplayAbility> Parent)
{
	UClass* ParentClass = Parent ? Parent.Get() : UUnrealAIGameplayAbility::StaticClass();
	if (!ParentClass->IsChildOf(UGameplayAbility::StaticClass()))
		return Failed<UBlueprint*>(FString::Printf(TEXT("%s is not a Gameplay Ability class"), *ParentClass->GetName()));
	return CreateBlueprintAsset(Path, ParentClass, UGameplayAbilityBlueprint::StaticClass());
}

FString UUnrealAIGASRqlApi::SetCost(TSubclassOf<UGameplayAbility> Ability, const FString& Attribute, float Amount, const FString& Effect)
{
	FString Error;
	const FGameplayAttribute Target = UnrealAIGAS::FindAttribute(Attribute, UnrealAIGAS::LoadedAttributeSetClasses(), &Error);
	if (!Target.IsValid())
		return Failed<FString>(Error);
	if (Amount < 0.f)
		return Failed<FString>(TEXT("Amount is what the ability costs: 0 or more"));
	UGameplayAbility* Defaults = EditableDefaults<UGameplayAbility>(Ability, TEXT("create a Blueprint with gas | call CreateAbility(path)"));
	if (!Defaults)
		return FString();
	const FString Path = Effect.TrimStartAndEnd().IsEmpty() ? AssetPath(*Ability.Get()) + TEXT("_Cost") : Effect.TrimStartAndEnd();
	UGameplayEffect* Cost = FindOrCreateEffect(Path);
	if (!Cost)
		return FString();
	SetDuration(*Cost, EGameplayEffectDurationType::Instant, 0.f, 0.f, true);
	Cost->Modifiers.Reset();
	FGameplayModifierInfo Modifier;
	Modifier.Attribute = Target;
	Modifier.ModifierOp = EGameplayModOp::Additive;
	Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(-Amount));
	Cost->Modifiers.Add(Modifier);
	DefaultsChanged(*Cost);
	SetClassProperty(*Defaults, TEXT("CostGameplayEffectClass"), Cost->GetClass());
	DefaultsChanged(*Defaults);
	return FString::Printf(TEXT("%s costs %g %s (%s)"), *Ability->GetName(), Amount, *UnrealAIGAS::QualifiedName(Target), *Path);
}

FString UUnrealAIGASRqlApi::SetCooldown(TSubclassOf<UGameplayAbility> Ability, float Seconds, const FString& Tag, const FString& Effect)
{
	if (Seconds <= 0.f)
		return Failed<FString>(TEXT("Seconds must be above 0 (to remove a cooldown: SetDefault(<ability>, \"CooldownGameplayEffectClass\", \"None\"))"));
	UGameplayAbility* Defaults = EditableDefaults<UGameplayAbility>(Ability, TEXT("create a Blueprint with gas | call CreateAbility(path)"));
	if (!Defaults)
		return FString();
	FString TagName = Tag.TrimStartAndEnd();
	if (TagName.IsEmpty())
	{
		FString Name = UBlueprint::GetBlueprintFromClass(Ability)->GetName();
		Name.RemoveFromStart(TEXT("GA_"));
		TagName = TEXT("Cooldown.") + Name;
	}
	FGameplayTag CooldownTag;
	if (!EnsureTag(TagName, CooldownTag))
		return FString();
	const FString Path = Effect.TrimStartAndEnd().IsEmpty() ? AssetPath(*Ability.Get()) + TEXT("_Cooldown") : Effect.TrimStartAndEnd();
	UGameplayEffect* Cooldown = FindOrCreateEffect(Path);
	if (!Cooldown)
		return FString();
	SetDuration(*Cooldown, EGameplayEffectDurationType::HasDuration, Seconds, 0.f, true);
	FInheritedTagContainer Granted;
	Granted.AddTag(CooldownTag);
	UTargetTagsGameplayEffectComponent& Component = Cooldown->FindOrAddComponent<UTargetTagsGameplayEffectComponent>();
	Component.Modify();
	Component.SetAndApplyTargetTagChanges(Granted);
	DefaultsChanged(*Cooldown);
	SetClassProperty(*Defaults, TEXT("CooldownGameplayEffectClass"), Cooldown->GetClass());
	DefaultsChanged(*Defaults);
	return FString::Printf(TEXT("%s has a %gs cooldown: %s grants %s"), *Ability->GetName(), Seconds, *Path, *CooldownTag.ToString());
}

FString UUnrealAIGASRqlApi::SetAbilityTags(TSubclassOf<UGameplayAbility> Ability, const FString& Kind, const FString& Tags)
{
	static const TCHAR* Kinds[] = { TEXT("Ability"), TEXT("ActivationOwned"), TEXT("ActivationRequired"), TEXT("ActivationBlocked"),
		TEXT("CancelAbilitiesWithTag"), TEXT("BlockAbilitiesWithTag") };
	static const TCHAR* Properties[] = { TEXT(""), TEXT("ActivationOwnedTags"), TEXT("ActivationRequiredTags"), TEXT("ActivationBlockedTags"),
		TEXT("CancelAbilitiesWithTag"), TEXT("BlockAbilitiesWithTag") };
	FString Wanted = Kind.TrimStartAndEnd();
	Wanted.RemoveFromEnd(TEXT("Tags"));
	int32 KindIndex = INDEX_NONE;
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Kinds); ++Index)
	{
		if (Wanted.Equals(Kinds[Index], ESearchCase::IgnoreCase) || (Index == 0 && (Wanted.Equals(TEXT("Asset"), ESearchCase::IgnoreCase))))
			KindIndex = Index;
	}
	if (KindIndex == INDEX_NONE)
		return Failed<FString>(FString::Printf(TEXT("Kind '%s' is not Ability, ActivationOwned, ActivationRequired, ActivationBlocked, CancelAbilitiesWithTag or BlockAbilitiesWithTag"), *Kind));
	UGameplayAbility* Defaults = EditableDefaults<UGameplayAbility>(Ability, TEXT("create a Blueprint with gas | call CreateAbility(path)"));
	FGameplayTagContainer Container;
	if (!Defaults || !ParseTags(Tags, Container))
		return FString();
	FGameplayTagContainer* Target = KindIndex == 0 ? &Defaults->EditorGetAssetTags() : PropertyValue<FGameplayTagContainer>(*Defaults, Properties[KindIndex]);
	if (!Target)
		return Failed<FString>(FString::Printf(TEXT("%s has no %s"), *Ability->GetName(), Properties[KindIndex]));
	*Target = Container;
	DefaultsChanged(*Defaults);
	return FString::Printf(TEXT("%s: %s"), Kinds[KindIndex], *TagsText(Container));
}

FString UUnrealAIGASRqlApi::SetAbilityProjectile(TSubclassOf<UGameplayAbility> Ability, TSubclassOf<UGameplayEffect> HitEffect, float Magnitude,
	TSubclassOf<AActor> Projectile, float Speed)
{
	if (Ability && !Ability->IsChildOf(UUnrealAIGameplayAbility::StaticClass()))
		return Failed<FString>(FString::Printf(TEXT("%s does not derive from UnrealAIGameplayAbility (create it with gas | call CreateAbility(path)), so it has no projectile settings"), *Ability->GetName()));
	if (Speed < 0.f)
		return Failed<FString>(TEXT("Speed must be 0 (the projectile's own) or more"));
	UUnrealAIGameplayAbility* Defaults = EditableDefaults<UUnrealAIGameplayAbility>(Ability, TEXT("create a Blueprint with gas | call CreateAbility(path)"));
	if (!Defaults)
		return FString();
	Defaults->ProjectileClass = Projectile ? Projectile.Get() : AUnrealAIGASProjectile::StaticClass();
	Defaults->HitEffect = HitEffect;
	Defaults->HitMagnitude = Magnitude;
	Defaults->ProjectileSpeed = Speed;
	DefaultsChanged(*Defaults);
	return FString::Printf(TEXT("%s fires %s applying %s (SetByCaller.Damage %g)%s"), *Ability->GetName(), *ClassName(Defaults->ProjectileClass),
		*ClassName(HitEffect), Magnitude, Speed > 0.f ? *FString::Printf(TEXT(" at %g cm/s"), Speed) : TEXT(""));
}

FString UUnrealAIGASRqlApi::SetDefault(UClass* Class, const FString& Property, const FString& Value)
{
	UObject* Defaults = EditableDefaults<UObject>(Class, TEXT("only a project Blueprint's defaults can be set"));
	if (!Defaults)
		return FString();
	FProperty* Target = FindFProperty<FProperty>(Class, FName(*Property.TrimStartAndEnd()));
	for (TFieldIterator<FProperty> It(Class); It && !Target; ++It)
	{
		if (It->GetName().Equals(Property.TrimStartAndEnd(), ESearchCase::IgnoreCase) || It->GetAuthoredName().Equals(Property.TrimStartAndEnd(), ESearchCase::IgnoreCase))
			Target = *It;
	}
	if (!Target || !Target->HasAnyPropertyFlags(CPF_Edit))
		return Failed<FString>(FString::Printf(TEXT("%s has no editable property '%s' (rql-schema %s lists its members)"), *Class->GetName(), *Property, *Class->GetName()));

	void* Data = Target->ContainerPtrToValuePtr<void>(Defaults);
	const FString Text = Value.TrimStartAndEnd();
	const bool bNone = Text.IsEmpty() || Text.Equals(TEXT("None"), ESearchCase::IgnoreCase);
	// Resolve references first (nothing changes when one fails), then write between Pre/PostEditChange as the Details panel does.
	TFunction<bool()> Write;
	if (const FStructProperty* Struct = CastField<FStructProperty>(Target); Struct && Struct->Struct == FGameplayTagContainer::StaticStruct())
	{
		FGameplayTagContainer Tags;
		if (!ParseTags(Text, Tags))
			return FString();
		Write = [Data, Tags]() { *static_cast<FGameplayTagContainer*>(Data) = Tags; return true; };
	}
	else if (Struct && Struct->Struct == FGameplayTag::StaticStruct())
	{
		FGameplayTag Tag;
		if (!bNone && !EnsureTag(Text, Tag))
			return FString();
		Write = [Data, Tag]() { *static_cast<FGameplayTag*>(Data) = Tag; return true; };
	}
	else if (const FObjectPropertyBase* Reference = CastField<FObjectPropertyBase>(Target))
	{
		UObject* Object = nullptr;
		if (!bNone)
		{
			const FClassProperty* ClassReference = CastField<FClassProperty>(Target);
			Object = ClassReference ? static_cast<UObject*>(UnrealMcpRql::ResolveClass(Text)) : UnrealMcpRql::LoadObject(Reference->PropertyClass, Text);
			if (!Object || (ClassReference && !CastChecked<UClass>(Object)->IsChildOf(ClassReference->MetaClass)) || !Object->IsA(Reference->PropertyClass))
				return Failed<FString>(FString::Printf(TEXT("'%s' is not a %s for %s"), *Text, *(ClassReference ? ClassReference->MetaClass : Reference->PropertyClass)->GetName(), *Target->GetName()));
		}
		Write = [Reference, Data, Object]() { Reference->SetObjectPropertyValue(Data, Object); return true; };
	}
	else
	{
		Write = [Target, Data, Text, Defaults]()
		{
			FStringOutputDevice ImportError;
			const TCHAR* End = Target->ImportText_Direct(*Text, Data, Defaults, PPF_None, &ImportError);
			if (!End || !ImportError.IsEmpty())
			{
				UnrealMcpRql::Fail(FString::Printf(TEXT("'%s' is not a value for %s (%s)%s"), *Text, *Target->GetName(), *Target->GetCPPType(),
					ImportError.IsEmpty() ? TEXT("") : *(TEXT(": ") + ImportError)));
				return false;
			}
			return true;
		};
	}
	Defaults->PreEditChange(Target);
	const bool bWritten = Write();
	FPropertyChangedEvent Event(Target, EPropertyChangeType::ValueSet);
	Defaults->PostEditChangeProperty(Event);
	DefaultsChanged(*Defaults);
	if (!bWritten)
		return FString();
	FString Exported;
	Target->ExportTextItem_Direct(Exported, Data, nullptr, Defaults, PPF_None);
	return Exported;
}

// ---------------------------------------------------------------- actors

UActorComponent* UUnrealAIGASRqlApi::AddAbilitySystem(UBlueprint* Blueprint, const FString& Attributes, TSubclassOf<UAttributeSet> AttributeSet)
{
	if (!Construction(*Blueprint))
		return nullptr;
	UClass* SetClass = AttributeSet ? AttributeSet.Get() : UUnrealAIAttributeSet::StaticClass();
	TArray<TPair<FString, float>> Values;
	if (!ParsePairs(Attributes, TEXT("Attribute"), Values))
		return nullptr;

	USCS_Node* Node = FindNode(*Blueprint, UAbilitySystemComponent::StaticClass());
	UUnrealAIAbilitySystemComponent* Existing = Node ? Cast<UUnrealAIAbilitySystemComponent>(Node->ComponentTemplate) : nullptr;
	if (Node && !Existing)
		return Failed<UActorComponent*>(FString::Printf(TEXT("%s already has an AbilitySystemComponent (%s) that is not an UnrealAIAbilitySystemComponent; remove it first or configure it in its own graph"),
			*Blueprint->GetName(), *Node->ComponentTemplate->GetClass()->GetName()));
	const AActor* ActorDefaults = Blueprint->GeneratedClass->GetDefaultObject<AActor>();
	if (const UAbilitySystemComponent* Native = ActorDefaults ? ActorDefaults->FindComponentByClass<UAbilitySystemComponent>() : nullptr; Native && !Existing)
		return Failed<UActorComponent*>(FString::Printf(TEXT("%s already has an AbilitySystemComponent from its parent class (%s): grant abilities with gas | call GrantAbility or its own setup"),
			*Blueprint->GetName(), *Native->GetName()));

	// The attribute names must exist in the component's sets (validated before any change).
	TArray<const UClass*> Sets = { SetClass };
	if (Existing)
	{
		for (const TSubclassOf<UAttributeSet>& Set : Existing->AttributeSets)
			Sets.AddUnique(Set.Get());
	}
	for (const TPair<FString, float>& Value : Values)
	{
		FString Error;
		if (!UnrealAIGAS::FindAttribute(Value.Key, Sets, &Error).IsValid())
			return Failed<UActorComponent*>(Error);
	}

	if (!Node)
		Node = AddNode(*Blueprint, UUnrealAIAbilitySystemComponent::StaticClass(), TEXT("AbilitySystem"));
	UUnrealAIAbilitySystemComponent* Template = CastChecked<UUnrealAIAbilitySystemComponent>(Node->ComponentTemplate);
	Template->Modify();
	Template->AttributeSets.AddUnique(SetClass);
	for (const TPair<FString, float>& Value : Values)
		Template->InitialAttributes.Add(FName(*Value.Key), Value.Value);
	Recompile(*Blueprint);
	return Node->ComponentTemplate;
}

FString UUnrealAIGASRqlApi::AddStartingAbility(UBlueprint* Blueprint, TSubclassOf<UGameplayAbility> Ability)
{
	UUnrealAIAbilitySystemComponent* Template = AbilitySystemTemplate(*Blueprint);
	if (!Template)
		return FString();
	Template->StartingAbilities.AddUnique(Ability);
	Recompile(*Blueprint);
	TArray<FString> Names;
	for (const TSubclassOf<UGameplayAbility>& Each : Template->StartingAbilities)
		Names.Add(ClassName(Each));
	return FString::Join(Names, TEXT(", "));
}

FString UUnrealAIGASRqlApi::AddStartingEffect(UBlueprint* Blueprint, TSubclassOf<UGameplayEffect> Effect)
{
	UUnrealAIAbilitySystemComponent* Template = AbilitySystemTemplate(*Blueprint);
	if (!Template)
		return FString();
	Template->StartingEffects.AddUnique(Effect);
	Recompile(*Blueprint);
	TArray<FString> Names;
	for (const TSubclassOf<UGameplayEffect>& Each : Template->StartingEffects)
		Names.Add(ClassName(Each));
	return FString::Join(Names, TEXT(", "));
}

FString UUnrealAIGASRqlApi::BindAbilityInput(UBlueprint* Blueprint, TSubclassOf<UGameplayAbility> Ability, const FString& InputAction, const FString& Key,
	UInputMappingContext* MappingContext)
{
	if (!AbilitySystemTemplate(*Blueprint))
		return FString();
	const FString KeyName = Key.TrimStartAndEnd();
	const FKey MappedKey(*KeyName);
	if (!KeyName.IsEmpty() && !MappedKey.IsValid())
		return Failed<FString>(FString::Printf(TEXT("'%s' is not a key name (e.g. One, Q, SpaceBar, LeftMouseButton, Gamepad_FaceButton_Top)"), *KeyName));
	const FString ActionPath = InputAction.TrimStartAndEnd();
	UInputAction* Action = Cast<UInputAction>(UnrealMcpRql::LoadObject(UInputAction::StaticClass(), ActionPath));
	if (!Action)
	{
		if (UnrealMcpRql::LoadObject(UObject::StaticClass(), ActionPath))
			return Failed<FString>(FString::Printf(TEXT("%s is not an Input Action"), *ActionPath));
		Action = Cast<UInputAction>(UnrealMcpRql::CreateAsset(UInputAction::StaticClass(), ActionPath));
		if (!Action)
			return FString();
	}
	UInputMappingContext* Context = MappingContext;
	if (!KeyName.IsEmpty() && !Context)
	{
		const FString ContextPath = FPackageName::GetLongPackagePath(Action->GetOutermost()->GetName()) + TEXT("/IMC_Abilities");
		Context = Cast<UInputMappingContext>(UnrealMcpRql::LoadObject(UInputMappingContext::StaticClass(), ContextPath));
		if (!Context)
			Context = Cast<UInputMappingContext>(UnrealMcpRql::CreateAsset(UInputMappingContext::StaticClass(), ContextPath));
		if (!Context)
			return FString();
	}
	if (!KeyName.IsEmpty())
	{
		const bool bMapped = Context->GetMappings().ContainsByPredicate([Action, &MappedKey](const FEnhancedActionKeyMapping& Mapping)
		{
			return Mapping.Action == Action && Mapping.Key == MappedKey;
		});
		if (!bMapped)
		{
			Context->Modify();
			Context->MapKey(Action, MappedKey);
			Context->MarkPackageDirty();
		}
	}

	UUnrealAIAbilitySystemComponent* Template = AbilitySystemTemplate(*Blueprint);
	FUnrealAIAbilityInput* Binding = Template->AbilityInputs.FindByPredicate([Action](const FUnrealAIAbilityInput& Input) { return Input.InputAction == Action; });
	if (!Binding)
	{
		Binding = &Template->AbilityInputs.AddDefaulted_GetRef();
		Binding->InputAction = Action;
	}
	Binding->Ability = Ability;
	if (Context)
		Template->InputMappingContext = Context;
	Recompile(*Blueprint);
	return FString::Printf(TEXT("%s%s activates %s for the player controlling %s"), *Action->GetName(),
		Context && !KeyName.IsEmpty() ? *FString::Printf(TEXT(" (%s in %s)"), *KeyName, *Context->GetName()) : TEXT(""), *ClassName(Ability), *Blueprint->GetName());
}

UActorComponent* UUnrealAIGASRqlApi::AddAttributeDisplay(UBlueprint* Blueprint, const FString& Format, FVector Offset)
{
	if (!Construction(*Blueprint))
		return nullptr;
	USCS_Node* Node = FindNode(*Blueprint, UUnrealAIAttributeTextComponent::StaticClass());
	if (!Node)
		Node = AddNode(*Blueprint, UUnrealAIAttributeTextComponent::StaticClass(), TEXT("AttributeText"));
	UUnrealAIAttributeTextComponent* Template = CastChecked<UUnrealAIAttributeTextComponent>(Node->ComponentTemplate);
	Template->Modify();
	Template->Format = Format;
	Template->SetRelativeLocation_Direct(Offset);
	Recompile(*Blueprint);
	return Node->ComponentTemplate;
}

FString UUnrealAIGASRqlApi::GrantAbility(AActor* Actor, TSubclassOf<UGameplayAbility> Ability, int32 Level)
{
	UAbilitySystemComponent* Asc = nullptr;
	if (const IAbilitySystemInterface* WithAsc = Cast<IAbilitySystemInterface>(Actor))
		Asc = WithAsc->GetAbilitySystemComponent();
	if (!Asc)
		Asc = Actor->FindComponentByClass<UAbilitySystemComponent>();
	if (!Asc)
		return Failed<FString>(FString::Printf(TEXT("%s has no AbilitySystemComponent"), *Actor->GetActorNameOrLabel()));
	if (!Asc->IsOwnerActorAuthoritative())
		return Failed<FString>(FString::Printf(TEXT("the AbilitySystemComponent of %s is not authoritative"), *Actor->GetActorNameOrLabel()));
	const FGameplayAbilitySpecHandle Handle = Asc->GiveAbility(FGameplayAbilitySpec(Ability, FMath::Max(Level, 1)));
	if (!Handle.IsValid())
		return Failed<FString>(FString::Printf(TEXT("could not grant %s"), *Ability->GetName()));
	return Handle.ToString();
}

// ---------------------------------------------------------------- the running game

FString UUnrealAIGASRqlApi::ActivateAbility(TSubclassOf<UGameplayAbility> Ability, const FString& Actor)
{
	AActor* Target = FindGameActor(Actor);
	UAbilitySystemComponent* Asc = Target ? FindAbilitySystem(*Target) : nullptr;
	if (!Asc)
		return FString();
	FGameplayAbilitySpec* Spec = Asc->FindAbilitySpecFromClass(Ability);
	if (!Spec)
	{
		TArray<FString> Granted;
		for (const FGameplayAbilitySpec& Each : Asc->GetActivatableAbilities())
			Granted.Add(Each.Ability ? Each.Ability->GetClass()->GetName() : FString());
		return Failed<FString>(FString::Printf(TEXT("%s is not granted to %s (granted: %s)"), *Ability->GetName(), *Target->GetActorNameOrLabel(),
			Granted.IsEmpty() ? TEXT("none") : *FString::Join(Granted, TEXT(", "))));
	}
	const UGameplayAbility* Definition = Spec->Ability;
	const FGameplayAbilityActorInfo* Info = Asc->AbilityActorInfo.Get();
	if (!Definition || !Info || !Info->AvatarActor.IsValid())
		return Failed<FString>(FString::Printf(TEXT("the AbilitySystemComponent of %s is not initialized"), *Target->GetActorNameOrLabel()));
	{
		if (!Definition->CheckCooldown(Spec->Handle, Info))
			return FString::Printf(TEXT("not activated: on cooldown (%s, %.1fs left)"), *TagsText(*Definition->GetCooldownTags()), Definition->GetCooldownTimeRemaining(Info));
		if (!Definition->CheckCost(Spec->Handle, Info))
			return TEXT("not activated: cannot pay the cost (see the Attributes member)");
	}
	const TMap<FString, float> Before = AttributeValues(*Asc);
	if (!Asc->TryActivateAbility(Spec->Handle))
		return FString::Printf(TEXT("not activated (blocked by tags or its CanActivateAbility; the owner has %s)"), *TagsText(Asc->GetOwnedGameplayTags()));
	TArray<FString> Changes;
	for (const TPair<FString, float>& After : AttributeValues(*Asc))
	{
		const float* Old = Before.Find(After.Key);
		if (Old && !FMath::IsNearlyEqual(*Old, After.Value))
			Changes.Add(FString::Printf(TEXT("%s %g -> %g"), *After.Key, *Old, After.Value));
	}
	return Changes.IsEmpty() ? FString(TEXT("activated")) : FString::Printf(TEXT("activated: %s"), *FString::Join(Changes, TEXT(", ")));
}

FString UUnrealAIGASRqlApi::ApplyEffect(TSubclassOf<UGameplayEffect> Effect, const FString& Actor, const FString& SetByCaller, float Level)
{
	TArray<TPair<FString, float>> Magnitudes;
	if (!ParsePairs(SetByCaller, TEXT("Tag"), Magnitudes))
		return FString();
	TArray<TPair<FGameplayTag, float>> Tags;
	for (const TPair<FString, float>& Magnitude : Magnitudes)
	{
		FGameplayTag Tag;
		if (!EnsureTag(Magnitude.Key.Contains(TEXT(".")) ? Magnitude.Key : TEXT("SetByCaller.") + Magnitude.Key, Tag))
			return FString();
		Tags.Emplace(Tag, Magnitude.Value);
	}
	AActor* Target = FindGameActor(Actor);
	UAbilitySystemComponent* Asc = Target ? FindAbilitySystem(*Target) : nullptr;
	if (!Asc)
		return FString();
	FGameplayEffectContextHandle Context = Asc->MakeEffectContext();
	Context.AddSourceObject(Target);
	const FGameplayEffectSpecHandle Spec = Asc->MakeOutgoingSpec(Effect, Level, Context);
	if (!Spec.IsValid())
		return Failed<FString>(FString::Printf(TEXT("could not make a spec of %s"), *Effect->GetName()));
	for (const TPair<FGameplayTag, float>& Tag : Tags)
		Spec.Data->SetSetByCallerMagnitude(Tag.Key, Tag.Value);
	const TMap<FString, float> Before = AttributeValues(*Asc);
	const FActiveGameplayEffectHandle Handle = Asc->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
	TArray<FString> Changes;
	for (const TPair<FString, float>& After : AttributeValues(*Asc))
	{
		const float* Old = Before.Find(After.Key);
		if (Old && !FMath::IsNearlyEqual(*Old, After.Value))
			Changes.Add(FString::Printf(TEXT("%s %g -> %g"), *After.Key, *Old, After.Value));
	}
	if (!Handle.WasSuccessfullyApplied())
		return FString::Printf(TEXT("%s was not applied to %s (its tag requirements, immunity or chance)"), *Effect->GetName(), *Target->GetActorNameOrLabel());
	return FString::Printf(TEXT("%s applied to %s%s"), *Effect->GetName(), *Target->GetActorNameOrLabel(),
		Changes.IsEmpty() ? TEXT("") : *(TEXT(": ") + FString::Join(Changes, TEXT(", "))));
}

float UUnrealAIGASRqlApi::SetAttribute(const FString& Attribute, float Value, const FString& Actor)
{
	AActor* Target = FindGameActor(Actor);
	UAbilitySystemComponent* Asc = Target ? FindAbilitySystem(*Target) : nullptr;
	if (!Asc)
		return 0.f;
	FString Error;
	const FGameplayAttribute Found = UnrealAIGAS::FindAttribute(*Asc, Attribute, &Error);
	if (!Found.IsValid())
		return Failed<float>(Error);
	Asc->SetNumericAttributeBase(Found, Value);
	return Asc->GetNumericAttribute(Found);
}
