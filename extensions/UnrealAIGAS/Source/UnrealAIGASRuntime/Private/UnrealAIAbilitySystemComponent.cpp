// Copyright (c) 2026 IvanMurzak/Unreal-AI-GAS. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIAbilitySystemComponent.h"

#include "UnrealAIAttributeSet.h"
#include "UnrealAIGASAttributes.h"

#include "Abilities/GameplayAbility.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "Components/InputComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameplayAbilitySpec.h"
#include "GameplayEffect.h"
#include "InputAction.h"
#include "InputMappingContext.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealAIGAS, Log, All);

UUnrealAIAbilitySystemComponent::UUnrealAIAbilitySystemComponent()
{
	AttributeSets.Add(UUnrealAIAttributeSet::StaticClass());
}

void UUnrealAIAbilitySystemComponent::InitializeComponent()
{
	Super::InitializeComponent();
	// Before the owner's BeginPlay, so its graph already reads the initial values.
	CreateAttributeSets();
}

void UUnrealAIAbilitySystemComponent::CreateAttributeSets()
{
	const AActor* Owner = GetOwner();
	if (!Owner || !Owner->HasAuthority())
		return; // clients receive the server's sets

	for (const TSubclassOf<UAttributeSet>& SetClass : AttributeSets)
	{
		if (SetClass)
			GetOrCreateAttributeSubobject(SetClass);
	}

	// Maximums first, so a value is not clamped by the maximum it is about to get.
	TArray<TPair<FName, float>> Values;
	for (const TPair<FName, float>& Entry : InitialAttributes)
		Values.Add(Entry);
	Values.StableSort([](const TPair<FName, float>& A, const TPair<FName, float>& B)
	{
		const auto IsMax = [](const FName Name)
		{
			FString Attribute = Name.ToString();
			Attribute.Split(TEXT("."), nullptr, &Attribute, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			return Attribute.StartsWith(TEXT("Max"), ESearchCase::IgnoreCase);
		};
		return IsMax(A.Key) && !IsMax(B.Key);
	});
	for (const TPair<FName, float>& Entry : Values)
	{
		FString Error;
		const FGameplayAttribute Attribute = UnrealAIGAS::FindAttribute(*this, Entry.Key.ToString(), &Error);
		if (Attribute.IsValid())
			SetNumericAttributeBase(Attribute, Entry.Value);
		else
			UE_LOG(LogUnrealAIGAS, Warning, TEXT("%s: InitialAttributes: %s"), *GetPathName(), *Error);
	}
}

void UUnrealAIAbilitySystemComponent::BeginPlay()
{
	Super::BeginPlay();
	GrantStartingData();
	BindAttributeEvents();
	if (APawn* Pawn = Cast<APawn>(GetOwner()))
	{
		Pawn->ReceiveControllerChangedDelegate.AddUniqueDynamic(this, &UUnrealAIAbilitySystemComponent::HandleControllerChanged);
		// The actor info was made at InitializeComponent, usually before possession: pick up the controller now and on changes.
		RefreshAbilityActorInfo();
		BindInput();
	}
}

void UUnrealAIAbilitySystemComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnbindInput();
	if (APawn* Pawn = Cast<APawn>(GetOwner()))
		Pawn->ReceiveControllerChangedDelegate.RemoveDynamic(this, &UUnrealAIAbilitySystemComponent::HandleControllerChanged);
	Super::EndPlay(EndPlayReason);
}

void UUnrealAIAbilitySystemComponent::GrantStartingData()
{
	if (!IsOwnerActorAuthoritative())
		return;
	TArray<TSubclassOf<UGameplayAbility>> Abilities = StartingAbilities;
	for (const FUnrealAIAbilityInput& Input : AbilityInputs)
		Abilities.AddUnique(Input.Ability);
	for (const TSubclassOf<UGameplayAbility>& Ability : Abilities)
	{
		if (Ability && !FindAbilitySpecFromClass(Ability))
			GiveAbility(FGameplayAbilitySpec(Ability, 1, INDEX_NONE, GetOwner()));
	}
	for (const TSubclassOf<UGameplayEffect>& Effect : StartingEffects)
	{
		if (Effect)
		{
			FGameplayEffectContextHandle Context = MakeEffectContext();
			Context.AddSourceObject(GetOwner());
			ApplyGameplayEffectToSelf(Effect->GetDefaultObject<UGameplayEffect>(), 1.f, Context);
		}
	}
}

void UUnrealAIAbilitySystemComponent::BindAttributeEvents()
{
	// Clients receive their sets by replication, possibly after BeginPlay: bind each set once, whenever it is there.
	for (const UAttributeSet* Set : GetSpawnedAttributes())
	{
		if (!Set || BoundAttributeSets.Contains(Set->GetClass()))
			continue;
		BoundAttributeSets.Add(Set->GetClass());
		for (const FGameplayAttribute& Attribute : UnrealAIGAS::AttributesOf(Set->GetClass()))
			GetGameplayAttributeValueChangeDelegate(Attribute).AddUObject(this, &UUnrealAIAbilitySystemComponent::HandleAttributeChanged);
	}
	// Not replicated yet (a client): look again shortly, for a few seconds.
	UWorld* World = GetWorld();
	if (BoundAttributeSets.IsEmpty() && !AttributeSets.IsEmpty() && World && AttributeBindRetries++ < 20)
	{
		FTimerHandle Retry;
		World->GetTimerManager().SetTimer(Retry, FTimerDelegate::CreateUObject(this, &UUnrealAIAbilitySystemComponent::BindAttributeEvents), 0.25f, false);
	}
}

void UUnrealAIAbilitySystemComponent::HandleAttributeChanged(const FOnAttributeChangeData& Data)
{
	const FName Name(*Data.Attribute.GetName());
	OnAttributeChanged.Broadcast(Name, Data.NewValue, Data.OldValue);
	if (Name == TEXT("Health") && Data.NewValue <= 0.f && Data.OldValue > 0.f)
		OnOutOfHealth.Broadcast();
}

float UUnrealAIAbilitySystemComponent::GetAttributeValue(FName Name, bool& bFound) const
{
	const FGameplayAttribute Attribute = UnrealAIGAS::FindAttribute(*this, Name.ToString());
	bFound = Attribute.IsValid();
	return bFound ? GetNumericAttribute(Attribute) : 0.f;
}

bool UUnrealAIAbilitySystemComponent::SetAttributeBaseValue(FName Name, float Value)
{
	const FGameplayAttribute Attribute = UnrealAIGAS::FindAttribute(*this, Name.ToString());
	if (!Attribute.IsValid())
		return false;
	SetNumericAttributeBase(Attribute, Value);
	return true;
}

void UUnrealAIAbilitySystemComponent::HandleControllerChanged(APawn* Pawn, AController* OldController, AController* NewController)
{
	RefreshAbilityActorInfo();
	BindAttributeEvents();
	BindInput();
}

void UUnrealAIAbilitySystemComponent::UnbindInput()
{
	APlayerController* Controller = InputController.Get();
	if (Controller && AbilityInputComponent)
		Controller->PopInputComponent(AbilityInputComponent);
	if (Controller && InputMappingContext && bAddedMappingContext)
	{
		if (const ULocalPlayer* Player = Controller->GetLocalPlayer())
		{
			if (UEnhancedInputLocalPlayerSubsystem* Input = Player->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
				Input->RemoveMappingContext(InputMappingContext);
		}
	}
	if (AbilityInputComponent)
	{
		AbilityInputComponent->ClearActionBindings();
		AbilityInputComponent->DestroyComponent();
		AbilityInputComponent = nullptr;
	}
	InputController.Reset();
	bAddedMappingContext = false;
}

void UUnrealAIAbilitySystemComponent::BindInput()
{
	UnbindInput();
	const APawn* Pawn = Cast<APawn>(GetOwner());
	APlayerController* Controller = Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
	if ((AbilityInputs.IsEmpty() && !InputMappingContext) || !Controller || !Controller->IsLocalController())
		return;

	// The pawn's own input component exists only while possessed; a separate pushed component works for any pawn.
	AbilityInputComponent = NewObject<UEnhancedInputComponent>(GetOwner(), UEnhancedInputComponent::StaticClass(), NAME_None, RF_Transient);
	AbilityInputComponent->RegisterComponent();
	for (int32 Index = 0; Index < AbilityInputs.Num(); ++Index)
	{
		if (AbilityInputs[Index].InputAction && AbilityInputs[Index].Ability)
			AbilityInputComponent->BindAction(AbilityInputs[Index].InputAction.Get(), ETriggerEvent::Started, this, &UUnrealAIAbilitySystemComponent::HandleAbilityInput, Index);
	}
	Controller->PushInputComponent(AbilityInputComponent);
	if (InputMappingContext)
	{
		if (const ULocalPlayer* Player = Controller->GetLocalPlayer())
		{
			UEnhancedInputLocalPlayerSubsystem* Input = Player->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
			// A context the pawn (or anything else) already added stays theirs: only what this component added is removed.
			if (Input && !Input->HasMappingContext(InputMappingContext))
			{
				Input->AddMappingContext(InputMappingContext, InputMappingPriority);
				bAddedMappingContext = true;
			}
		}
	}
	InputController = Controller;
}

void UUnrealAIAbilitySystemComponent::HandleAbilityInput(int32 Index)
{
	if (AbilityInputs.IsValidIndex(Index) && AbilityInputs[Index].Ability)
		TryActivateAbilityByClass(AbilityInputs[Index].Ability);
}
