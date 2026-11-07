// Copyright (c) 2026 IvanMurzak/Unreal-AI-GAS. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemComponent.h"
#include "AttributeSet.h"

#include "UnrealAIAttributeSet.generated.h"

/**
 * A ready-made attribute set with the attributes most games start with, so projects need no C++ AttributeSet:
 * Health / MaxHealth, Mana / MaxMana, Stamina / MaxStamina, and the meta attributes Damage and Healing.
 *
 * - Health, Mana and Stamina stay within [0, Max...] (base and current values; lowering a maximum lowers the value).
 * - Damage and Healing are "meta" attributes: a Gameplay Effect that adds to Damage (e.g. with a SetByCaller magnitude)
 *   subtracts that much from Health, one that adds to Healing adds to Health; both are reset to 0 afterwards.
 * - Every value starts at 100 (Damage and Healing at 0); UUnrealAIAbilitySystemComponent::InitialAttributes overrides them.
 */
UCLASS(BlueprintType, meta = (DisplayName = "Attribute Set (Unreal AI)"))
class UNREALAIGASRUNTIME_API UUnrealAIAttributeSet : public UAttributeSet
{
	GENERATED_BODY()

public:
	UUnrealAIAttributeSet();

	UPROPERTY(BlueprintReadOnly, Category = "Attributes", ReplicatedUsing = OnRep_Health)
	FGameplayAttributeData Health;
	ATTRIBUTE_ACCESSORS_BASIC(UUnrealAIAttributeSet, Health)

	UPROPERTY(BlueprintReadOnly, Category = "Attributes", ReplicatedUsing = OnRep_MaxHealth)
	FGameplayAttributeData MaxHealth;
	ATTRIBUTE_ACCESSORS_BASIC(UUnrealAIAttributeSet, MaxHealth)

	UPROPERTY(BlueprintReadOnly, Category = "Attributes", ReplicatedUsing = OnRep_Mana)
	FGameplayAttributeData Mana;
	ATTRIBUTE_ACCESSORS_BASIC(UUnrealAIAttributeSet, Mana)

	UPROPERTY(BlueprintReadOnly, Category = "Attributes", ReplicatedUsing = OnRep_MaxMana)
	FGameplayAttributeData MaxMana;
	ATTRIBUTE_ACCESSORS_BASIC(UUnrealAIAttributeSet, MaxMana)

	UPROPERTY(BlueprintReadOnly, Category = "Attributes", ReplicatedUsing = OnRep_Stamina)
	FGameplayAttributeData Stamina;
	ATTRIBUTE_ACCESSORS_BASIC(UUnrealAIAttributeSet, Stamina)

	UPROPERTY(BlueprintReadOnly, Category = "Attributes", ReplicatedUsing = OnRep_MaxStamina)
	FGameplayAttributeData MaxStamina;
	ATTRIBUTE_ACCESSORS_BASIC(UUnrealAIAttributeSet, MaxStamina)

	/** Meta attribute: an instant effect that adds to it subtracts that much from Health (server only, not replicated). */
	UPROPERTY(BlueprintReadOnly, Category = "Meta")
	FGameplayAttributeData Damage;
	ATTRIBUTE_ACCESSORS_BASIC(UUnrealAIAttributeSet, Damage)

	/** Meta attribute: an instant effect that adds to it adds that much to Health (server only, not replicated). */
	UPROPERTY(BlueprintReadOnly, Category = "Meta")
	FGameplayAttributeData Healing;
	ATTRIBUTE_ACCESSORS_BASIC(UUnrealAIAttributeSet, Healing)

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void PreAttributeBaseChange(const FGameplayAttribute& Attribute, float& NewValue) const override;
	virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;
	virtual void PostAttributeChange(const FGameplayAttribute& Attribute, float OldValue, float NewValue) override;
	virtual void PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data) override;

protected:
	UFUNCTION()
	void OnRep_Health(const FGameplayAttributeData& OldValue);
	UFUNCTION()
	void OnRep_MaxHealth(const FGameplayAttributeData& OldValue);
	UFUNCTION()
	void OnRep_Mana(const FGameplayAttributeData& OldValue);
	UFUNCTION()
	void OnRep_MaxMana(const FGameplayAttributeData& OldValue);
	UFUNCTION()
	void OnRep_Stamina(const FGameplayAttributeData& OldValue);
	UFUNCTION()
	void OnRep_MaxStamina(const FGameplayAttributeData& OldValue);

private:
	/** Clamp a value attribute to [0, its maximum] (maximums to >= 0). */
	void Clamp(const FGameplayAttribute& Attribute, float& NewValue) const;
};
