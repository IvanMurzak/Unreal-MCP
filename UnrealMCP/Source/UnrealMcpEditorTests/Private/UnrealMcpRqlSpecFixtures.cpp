// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRqlSpecFixtures.h"

#include "Components/BoxComponent.h"
#include "GameFramework/PlayerController.h"
#include "IUnrealMcpRqlModule.h"
#include "InputCoreTypes.h"

void UUnrealMcpRqlSpecApi::Boom(const FString& Message)
{
	UnrealMcpRql::Fail(Message);
}

FUnrealMcpRqlSpecRow& UUnrealMcpRqlSpecApi::RowRef(const FString& Label)
{
	Stored.Label = Label;
	return Stored;
}

void UUnrealMcpRqlSpecApi::Double(int32& Value)
{
	Value *= 2;
}

void UUnrealMcpRqlSpecApi::MakeRow(int32 Damage, FUnrealMcpRqlSpecRow& Row)
{
	Row = FUnrealMcpRqlSpecRow();
	Row.Damage = Damage;
}

AUnrealMcpRqlSpecSelfMovingPawn::AUnrealMcpRqlSpecSelfMovingPawn()
{
	PrimaryActorTick.bCanEverTick = true;
	UBoxComponent* Box = CreateDefaultSubobject<UBoxComponent>(TEXT("Box"));
	Box->InitBoxExtent(FVector(50.0));
	Box->SetCollisionProfileName(TEXT("BlockAll"));
	SetRootComponent(Box);
}

void AUnrealMcpRqlSpecSelfMovingPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const APlayerController* Player = Cast<APlayerController>(GetController());
	if (!Player)
		return;
	if (Player->IsInputKeyDown(FKey(TEXT("W"))))
		AddActorWorldOffset(GetActorForwardVector() * 300.0 * DeltaSeconds, /*bSweep*/ true);
	if (Player->IsInputKeyDown(FKey(TEXT("A"))))
		AddActorWorldRotation(FRotator(0.0, -90.0 * DeltaSeconds, 0.0));
}
