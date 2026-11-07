// Copyright (c) 2026 IvanMurzak/Unreal-AI-GAS. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIAttributeTextComponent.h"

#include "UnrealAIGASAttributes.h"

#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"

UUnrealAIAttributeTextComponent::UUnrealAIAttributeTextComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickInterval = 0.1f;
	SetHorizontalAlignment(EHTA_Center);
	SetVerticalAlignment(EVRTA_TextCenter);
	SetWorldSize(26.f);
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void UUnrealAIAttributeTextComponent::OnRegister()
{
	Super::OnRegister();
	SetText(FText::FromString(Format));
}

FString UUnrealAIAttributeTextComponent::FormatAttributes() const
{
	const UAbilitySystemComponent* Asc = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(GetOwner());
	FString Out;
	int32 Index = 0;
	while (Index < Format.Len())
	{
		const int32 Close = Format[Index] == TEXT('{') ? Format.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Index) : INDEX_NONE;
		if (Close != INDEX_NONE && Asc)
		{
			const FGameplayAttribute Attribute = UnrealAIGAS::FindAttribute(*Asc, Format.Mid(Index + 1, Close - Index - 1));
			if (Attribute.IsValid())
			{
				const float Value = Asc->GetNumericAttribute(Attribute);
				Out += FMath::IsNearlyEqual(Value, FMath::RoundToFloat(Value), 0.05f)
					? FString::FromInt(FMath::RoundToInt(Value)) : FString::Printf(TEXT("%.1f"), Value);
				Index = Close + 1;
				continue;
			}
		}
		Out.AppendChar(Format[Index]);
		++Index;
	}
	return Out;
}

void UUnrealAIAttributeTextComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	SetText(FText::FromString(FormatAttributes()));
	if (!bFaceCamera)
		return;
	const APlayerController* Player = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (Player && Player->PlayerCameraManager)
	{
		FRotator Facing = (Player->PlayerCameraManager->GetCameraLocation() - GetComponentLocation()).Rotation();
		Facing.Pitch = 0.f;
		SetWorldRotation(Facing);
	}
}
