// Copyright (c) 2026 IvanMurzak/Unreal-AI-GAS. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemComponent.h"
#include "Templates/SubclassOf.h"

#include "UnrealAIAbilitySystemComponent.generated.h"

class AController;
class APawn;
class APlayerController;
class UEnhancedInputComponent;
class UGameplayAbility;
class UGameplayEffect;
class UInputAction;
class UInputMappingContext;

/** An Enhanced Input action that activates a Gameplay Ability (UUnrealAIAbilitySystemComponent::AbilityInputs). */
USTRUCT(BlueprintType)
struct UNREALAIGASRUNTIME_API FUnrealAIAbilityInput
{
	GENERATED_BODY()

	/** The input action (bound to a key in an Input Mapping Context) that activates Ability when it starts */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	TObjectPtr<UInputAction> InputAction;

	/** The ability to activate (granted at BeginPlay) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	TSubclassOf<UGameplayAbility> Ability;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FUnrealAIAttributeChangedSignature, FName, Attribute, float, NewValue, float, OldValue);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FUnrealAIOutOfHealthSignature);

/**
 * An Ability System Component configured with data only, so a Blueprint actor or character gets the Gameplay Ability
 * System without C++: it creates its attribute sets (UnrealAIAttributeSet by default) with initial values, grants
 * abilities and applies effects at BeginPlay (e.g. a periodic mana regeneration effect), binds abilities to Enhanced
 * Input actions for the controlling player, and reports attribute changes to Blueprints.
 *
 * Actors find it without IAbilitySystemInterface (UAbilitySystemGlobals looks for the component).
 */
UCLASS(ClassGroup = AbilitySystem, meta = (BlueprintSpawnableComponent, DisplayName = "Ability System (Unreal AI)"))
class UNREALAIGASRUNTIME_API UUnrealAIAbilitySystemComponent : public UAbilitySystemComponent
{
	GENERATED_BODY()

public:
	UUnrealAIAbilitySystemComponent();

	/** The attribute sets created for the owner at startup (server) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Unreal AI|Attributes")
	TArray<TSubclassOf<UAttributeSet>> AttributeSets;

	/** Initial base values by attribute name ("Health", or "UnrealAIAttributeSet.Health"); maximums are applied first */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Unreal AI|Attributes")
	TMap<FName, float> InitialAttributes;

	/** Abilities granted at BeginPlay (server) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Unreal AI|Abilities")
	TArray<TSubclassOf<UGameplayAbility>> StartingAbilities;

	/** Effects applied to the owner at BeginPlay (server): regeneration, passive buffs, ... */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Unreal AI|Abilities")
	TArray<TSubclassOf<UGameplayEffect>> StartingEffects;

	/** Input actions that activate abilities for the player controlling the owning pawn (their abilities are granted too) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Unreal AI|Input")
	TArray<FUnrealAIAbilityInput> AbilityInputs;

	/** An Input Mapping Context added for the controlling player (the keys of AbilityInputs' actions); optional */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Unreal AI|Input")
	TObjectPtr<UInputMappingContext> InputMappingContext;

	/** The priority InputMappingContext is added with */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Unreal AI|Input")
	int32 InputMappingPriority = 1;

	/** Any attribute of the owner's sets changed (Attribute is its name, e.g. Health) */
	UPROPERTY(BlueprintAssignable, Category = "Unreal AI|Attributes")
	FUnrealAIAttributeChangedSignature OnAttributeChanged;

	/** Health reached 0 */
	UPROPERTY(BlueprintAssignable, Category = "Unreal AI|Attributes")
	FUnrealAIOutOfHealthSignature OnOutOfHealth;

	/** The current value of the attribute Name ("Health"); 0 and bFound false when the owner has no such attribute */
	UFUNCTION(BlueprintPure, Category = "Unreal AI|Attributes")
	float GetAttributeValue(FName Name, bool& bFound) const;

	/** Set the base value of the attribute Name; false when the owner has no such attribute */
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Unreal AI|Attributes")
	bool SetAttributeBaseValue(FName Name, float Value);

	virtual void InitializeComponent() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void CreateAttributeSets();
	void GrantStartingData();
	void BindAttributeEvents();
	void HandleAttributeChanged(const FOnAttributeChangeData& Data);

	/** (Re)bind AbilityInputs for the player controlling the owning pawn. */
	void BindInput();
	void UnbindInput();
	void HandleAbilityInput(int32 Index);

	UFUNCTION()
	void HandleControllerChanged(APawn* Pawn, AController* OldController, AController* NewController);

	UPROPERTY(Transient)
	TObjectPtr<UEnhancedInputComponent> AbilityInputComponent;

	TWeakObjectPtr<APlayerController> InputController;
	TSet<const UClass*> BoundAttributeSets;
	int32 AttributeBindRetries = 0;
	bool bAddedMappingContext = false;
};
