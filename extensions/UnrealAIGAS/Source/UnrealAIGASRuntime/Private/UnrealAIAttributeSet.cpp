// Copyright (c) 2026 IvanMurzak/Unreal-AI-GAS. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIAttributeSet.h"

#include "GameplayEffectExtension.h"
#include "Net/UnrealNetwork.h"

UUnrealAIAttributeSet::UUnrealAIAttributeSet()
{
	InitHealth(100.f);
	InitMaxHealth(100.f);
	InitMana(100.f);
	InitMaxMana(100.f);
	InitStamina(100.f);
	InitMaxStamina(100.f);
	InitDamage(0.f);
	InitHealing(0.f);
}

void UUnrealAIAttributeSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION_NOTIFY(UUnrealAIAttributeSet, Health, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UUnrealAIAttributeSet, MaxHealth, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UUnrealAIAttributeSet, Mana, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UUnrealAIAttributeSet, MaxMana, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UUnrealAIAttributeSet, Stamina, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UUnrealAIAttributeSet, MaxStamina, COND_None, REPNOTIFY_Always);
}

void UUnrealAIAttributeSet::Clamp(const FGameplayAttribute& Attribute, float& NewValue) const
{
	if (Attribute == GetHealthAttribute())
		NewValue = FMath::Clamp(NewValue, 0.f, FMath::Max(GetMaxHealth(), 0.f));
	else if (Attribute == GetManaAttribute())
		NewValue = FMath::Clamp(NewValue, 0.f, FMath::Max(GetMaxMana(), 0.f));
	else if (Attribute == GetStaminaAttribute())
		NewValue = FMath::Clamp(NewValue, 0.f, FMath::Max(GetMaxStamina(), 0.f));
	else if (Attribute == GetMaxHealthAttribute() || Attribute == GetMaxManaAttribute() || Attribute == GetMaxStaminaAttribute())
		NewValue = FMath::Max(NewValue, 0.f);
}

void UUnrealAIAttributeSet::PreAttributeBaseChange(const FGameplayAttribute& Attribute, float& NewValue) const
{
	Super::PreAttributeBaseChange(Attribute, NewValue);
	Clamp(Attribute, NewValue);
}

void UUnrealAIAttributeSet::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue)
{
	Super::PreAttributeChange(Attribute, NewValue);
	Clamp(Attribute, NewValue);
}

void UUnrealAIAttributeSet::PostAttributeChange(const FGameplayAttribute& Attribute, float OldValue, float NewValue)
{
	Super::PostAttributeChange(Attribute, OldValue, NewValue);
	// A lower maximum lowers the value it bounds.
	UAbilitySystemComponent* Asc = GetOwningAbilitySystemComponent();
	if (!Asc)
		return;
	if (Attribute == GetMaxHealthAttribute() && GetHealth() > NewValue)
		Asc->SetNumericAttributeBase(GetHealthAttribute(), NewValue);
	else if (Attribute == GetMaxManaAttribute() && GetMana() > NewValue)
		Asc->SetNumericAttributeBase(GetManaAttribute(), NewValue);
	else if (Attribute == GetMaxStaminaAttribute() && GetStamina() > NewValue)
		Asc->SetNumericAttributeBase(GetStaminaAttribute(), NewValue);
}

void UUnrealAIAttributeSet::PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data)
{
	Super::PostGameplayEffectExecute(Data);
	if (Data.EvaluatedData.Attribute == GetDamageAttribute())
	{
		const float Amount = GetDamage();
		SetDamage(0.f);
		if (Amount > 0.f)
			SetHealth(GetHealth() - Amount);
	}
	else if (Data.EvaluatedData.Attribute == GetHealingAttribute())
	{
		const float Amount = GetHealing();
		SetHealing(0.f);
		if (Amount > 0.f)
			SetHealth(GetHealth() + Amount);
	}
}

void UUnrealAIAttributeSet::OnRep_Health(const FGameplayAttributeData& OldValue)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UUnrealAIAttributeSet, Health, OldValue);
}

void UUnrealAIAttributeSet::OnRep_MaxHealth(const FGameplayAttributeData& OldValue)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UUnrealAIAttributeSet, MaxHealth, OldValue);
}

void UUnrealAIAttributeSet::OnRep_Mana(const FGameplayAttributeData& OldValue)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UUnrealAIAttributeSet, Mana, OldValue);
}

void UUnrealAIAttributeSet::OnRep_MaxMana(const FGameplayAttributeData& OldValue)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UUnrealAIAttributeSet, MaxMana, OldValue);
}

void UUnrealAIAttributeSet::OnRep_Stamina(const FGameplayAttributeData& OldValue)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UUnrealAIAttributeSet, Stamina, OldValue);
}

void UUnrealAIAttributeSet::OnRep_MaxStamina(const FGameplayAttributeData& OldValue)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UUnrealAIAttributeSet, MaxStamina, OldValue);
}
