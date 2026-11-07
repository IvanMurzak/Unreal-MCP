// Copyright (c) 2026 IvanMurzak/Unreal-AI-GAS. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Components/TextRenderComponent.h"

#include "UnrealAIAttributeTextComponent.generated.h"

/**
 * World-space text showing the owner's attributes while the game runs, e.g. "HP {Health} / {MaxHealth}" above a
 * training dummy or the player: every {Name} is replaced with that attribute's current value (any attribute of the
 * owner's Ability System Component). Updates ten times a second; turns to the player's camera.
 */
UCLASS(ClassGroup = AbilitySystem, meta = (BlueprintSpawnableComponent, DisplayName = "Attribute Text (Unreal AI)"))
class UNREALAIGASRUNTIME_API UUnrealAIAttributeTextComponent : public UTextRenderComponent
{
	GENERATED_BODY()

public:
	UUnrealAIAttributeTextComponent();

	/** The text; {Health} is replaced with the Health attribute's current value */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Unreal AI")
	FString Format = TEXT("HP {Health} / {MaxHealth}");

	/** Turn toward the first player's camera */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Unreal AI")
	bool bFaceCamera = true;

	/** Format with the owner's current attribute values ({Name} left as is when the owner has no such attribute) */
	UFUNCTION(BlueprintPure, Category = "Unreal AI")
	FString FormatAttributes() const;

	virtual void OnRegister() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
};
