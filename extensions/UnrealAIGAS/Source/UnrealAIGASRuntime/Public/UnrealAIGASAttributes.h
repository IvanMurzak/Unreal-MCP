// Copyright (c) 2026 IvanMurzak/Unreal-AI-GAS. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "AttributeSet.h"
#include "NativeGameplayTags.h"

class UAbilitySystemComponent;

/** "SetByCaller.Damage": the SetByCaller magnitude the ability and projectile pass to their hit effect. */
UNREALAIGASRUNTIME_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_UnrealAI_SetByCaller_Damage);

namespace UnrealAIGAS
{
	/**
	 * Every attribute (FGameplayAttributeData property) of @p SetClass. Attribute sets name their attributes by property
	 * name; "Set.Attribute" (QualifiedName) names one when two sets share the name.
	 */
	UNREALAIGASRUNTIME_API TArray<FGameplayAttribute> AttributesOf(const UClass* SetClass);

	/** "UnrealAIAttributeSet.Health": the attribute set class name (no U prefix, Blueprint _C kept) and the attribute. */
	UNREALAIGASRUNTIME_API FString QualifiedName(const FGameplayAttribute& Attribute);

	/**
	 * The attribute @p Name ("Health", "UnrealAIAttributeSet.Health", "UUnrealAIAttributeSet.Health"; case-insensitive)
	 * among @p SetClasses. Invalid when none or several match; @p OutError then says why and lists the candidates.
	 */
	UNREALAIGASRUNTIME_API FGameplayAttribute FindAttribute(const FString& Name, const TArray<const UClass*>& SetClasses, FString* OutError = nullptr);

	/** FindAttribute among the attribute sets @p Asc has spawned. */
	UNREALAIGASRUNTIME_API FGameplayAttribute FindAttribute(const UAbilitySystemComponent& Asc, const FString& Name, FString* OutError = nullptr);

	/** Every loaded attribute set class except abstract ones and the engine's test set (for authoring: modifiers, costs). */
	UNREALAIGASRUNTIME_API TArray<const UClass*> LoadedAttributeSetClasses();
}
