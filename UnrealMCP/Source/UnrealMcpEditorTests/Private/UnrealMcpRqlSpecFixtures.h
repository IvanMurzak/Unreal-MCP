// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "Engine/DataTable.h"
#include "Engine/DeveloperSettings.h"
#include "GameFramework/Actor.h"
#include "GameplayTagContainer.h"
#include "Math/Color.h"
#include "Math/IntPoint.h"
#include "Math/IntVector.h"
#include "Templates/SubclassOf.h"
#include "UObject/Object.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"

#include "UnrealMcpRqlSpecFixtures.generated.h"

/**
 * Test-only types of the RQL specs (UnrealMcpRqlAdaptersSpec / UnrealMcpRqlDataModelsSpec): one property of every kind the
 * Unreal provider and its adapters convert, a DataTable row struct and a settings class. The test module is never
 * shipped (it is added to the descriptor only for the Automation pass), so these types never reach a project.
 */
UENUM()
enum class EUnrealMcpRqlSpecMode : uint8
{
	Idle,
	// Display names are RQL enum aliases ("Sprint" = Run); Walk's "Run" spells another entry's name and is skipped.
	Walk UMETA(DisplayName = "Run"),
	Run UMETA(DisplayName = "Sprint"),
};

USTRUCT()
struct FUnrealMcpRqlSpecRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Spec")
	int32 Damage = 0;

	UPROPERTY(EditAnywhere, Category = "Spec")
	float Speed = 1.0f;

	UPROPERTY(EditAnywhere, Category = "Spec")
	FString Label;

	UPROPERTY(EditAnywhere, Category = "Spec")
	FVector Offset = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, Category = "Spec")
	FGameplayTag Tag;

	UPROPERTY(EditAnywhere, Category = "Spec")
	FGameplayTagContainer Tags;

	UPROPERTY(EditAnywhere, Category = "Spec")
	EUnrealMcpRqlSpecMode Mode = EUnrealMcpRqlSpecMode::Idle;
};

UCLASS()
class UUnrealMcpRqlSpecFixture : public UObject
{
	GENERATED_BODY()

public:
	// The math structs with RQL adapters.
	UPROPERTY(EditAnywhere, Category = "Spec") FVector Vector = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, Category = "Spec") FVector2D Vector2D = FVector2D::ZeroVector;
	UPROPERTY(EditAnywhere, Category = "Spec") FVector4 Vector4 = FVector4(0, 0, 0, 0);
	UPROPERTY(EditAnywhere, Category = "Spec") FRotator Rotator = FRotator::ZeroRotator;
	UPROPERTY(EditAnywhere, Category = "Spec") FQuat Quat = FQuat::Identity;
	UPROPERTY(EditAnywhere, Category = "Spec") FLinearColor LinearColor = FLinearColor::Black;
	UPROPERTY(EditAnywhere, Category = "Spec") FColor Color = FColor::Black;
	UPROPERTY(EditAnywhere, Category = "Spec") FIntPoint IntPoint = FIntPoint::ZeroValue;
	UPROPERTY(EditAnywhere, Category = "Spec") FIntVector IntVector = FIntVector::ZeroValue;

	// Containers (FArrayAdapter, sets, FMapAdapter).
	UPROPERTY(EditAnywhere, Category = "Spec") TArray<int32> Numbers;
	UPROPERTY(EditAnywhere, Category = "Spec") TArray<FVector> Points;
	UPROPERTY(EditAnywhere, Category = "Spec") TSet<FName> Names;
	UPROPERTY(EditAnywhere, Category = "Spec") TMap<FName, int32> Scores;
	UPROPERTY(EditAnywhere, Category = "Spec") TMap<FString, FVector> Spots;

	// References (the UObject references adapter and soft references).
	UPROPERTY(EditAnywhere, Category = "Spec") TObjectPtr<UObject> Reference;
	UPROPERTY(EditAnywhere, Category = "Spec") TSubclassOf<AActor> ActorClass;
	UPROPERTY(EditAnywhere, Category = "Spec") TSoftObjectPtr<UObject> Soft;
	UPROPERTY(EditAnywhere, Category = "Spec") FSoftObjectPath SoftPath;
	UPROPERTY(EditAnywhere, Category = "Spec") FSoftClassPath SoftClass;

	// Gameplay tags.
	UPROPERTY(EditAnywhere, Category = "Spec") FGameplayTag Tag;
	UPROPERTY(EditAnywhere, Category = "Spec") FGameplayTagContainer Tags;
	UPROPERTY(EditAnywhere, Category = "Spec") FGameplayTagQuery Query;

	// Scalars, enums, a nested struct.
	UPROPERTY(EditAnywhere, Category = "Spec") FUnrealMcpRqlSpecRow Row;
	UPROPERTY(EditAnywhere, Category = "Spec") EUnrealMcpRqlSpecMode Mode = EUnrealMcpRqlSpecMode::Idle;
	UPROPERTY(EditAnywhere, Category = "Spec") uint8 Small = 0;
	UPROPERTY(EditAnywhere, Category = "Spec") int64 Big = 0;
	UPROPERTY(EditAnywhere, Category = "Spec") bool bFlag = false;
	UPROPERTY(EditAnywhere, Category = "Spec") FText Caption;
	UPROPERTY(EditAnywhere, Category = "Spec") FName Key;
	UPROPERTY(VisibleAnywhere, Category = "Spec") int32 ReadOnlyValue = 7;
	/** Not exposed by reflection (no Edit / Blueprint flags): IUnrealMcpRqlModule::ExposeProperties makes it a member. */
	UPROPERTY() int32 HiddenValue = 0;

	/** PostEditChangeProperty calls (writes go through the editor's property-change path). */
	int32 PostEditCount = 0;

	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override
	{
		Super::PostEditChangeProperty(Event);
		++PostEditCount;
	}
};

/**
 * A scene component that does not re-register around a property edit (as UNiagaraComponent): a raw write of its
 * RelativeLocation moves it only when the writer recomputes the world transform itself.
 */
UCLASS(ClassGroup = Spec, meta = (BlueprintSpawnableComponent))
class UUnrealMcpRqlSpecStillComponent : public USceneComponent
{
	GENERATED_BODY()

public:
#if WITH_EDITOR
	using USceneComponent::PreEditChange;
	virtual void PreEditChange(FProperty* Property) override { UObject::PreEditChange(Property); }
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override { UObject::PostEditChangeProperty(Event); }
#endif
};

/** Operations of the specs' RQL module (AddApiSource): defaults recorded by UHT, failures through UnrealMcpRql::Fail. */
UCLASS(Transient)
class UUnrealMcpRqlSpecApi : public UObject
{
	GENERATED_BODY()

public:
	/** add two numbers */
	UFUNCTION(BlueprintCallable, Category = "Spec", meta = (BlueprintInternalUseOnly = "true"))
	int32 Add(int32 A, int32 B = 2) { return A + B; }

	/** always fails with the given message */
	UFUNCTION(BlueprintCallable, Category = "Spec", meta = (BlueprintInternalUseOnly = "true"))
	void Boom(const FString& Message);
};

/** A Project Settings section of the specs, saved to its own Config/DefaultUnrealMcpRqlSpec.ini (removed after the run). */
UCLASS(config = UnrealMcpRqlSpec, defaultconfig, meta = (DisplayName = "Unreal MCP RQL Spec"))
class UUnrealMcpRqlSpecSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UPROPERTY(config, EditAnywhere, Category = "Spec") int32 Level = 1;
	UPROPERTY(config, EditAnywhere, Category = "Spec") FString Greeting = TEXT("hello");
	UPROPERTY(config, EditAnywhere, Category = "Spec") TArray<FString> Items;
	UPROPERTY(EditAnywhere, Category = "Spec") int32 NotSaved = 0;

	virtual FName GetContainerName() const override { return TEXT("Project"); }
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
	virtual FName GetSectionName() const override { return TEXT("UnrealMcpRqlSpec"); }
};
