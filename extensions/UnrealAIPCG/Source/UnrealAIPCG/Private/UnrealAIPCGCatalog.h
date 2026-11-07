// Copyright (c) 2026 IvanMurzak/Unreal-AI-PCG. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Templates/SubclassOf.h"
#include "UObject/Object.h"
#include "Utils/PCGPreconfiguration.h"

#include "UnrealAIPCGCatalog.generated.h"

class UPCGSettings;

/**
 * One row of the `pcgNodeTypes` source: a node the PCG graph editor's palette offers (a settings class, or one of its
 * preconfigured variants), with the default pins of a new node. AddNode takes its Name or Type.
 */
UCLASS(Transient)
class UUnrealAIPCGNodeType : public UObject
{
	GENERATED_BODY()

public:
	/** the palette title, e.g. Surface Sampler (a preconfigured variant: its own title) */
	UPROPERTY(VisibleAnywhere, Category = "Node")
	FString Name;

	/** the settings class without its PCG prefix and Settings suffix, e.g. SurfaceSampler */
	UPROPERTY(VisibleAnywhere, Category = "Node")
	FString Type;

	/** the palette category: Sampler, Filter, Spatial, Spawner, Metadata, Param, Generic, ... */
	UPROPERTY(VisibleAnywhere, Category = "Node")
	FString Category;

	/** what the node does (the palette tooltip) */
	UPROPERTY(VisibleAnywhere, Category = "Node")
	FString Description;

	/** the input pins of a new node (override pins of settings left out: the node's Settings member names them) */
	UPROPERTY(VisibleAnywhere, Category = "Node")
	TArray<FString> Inputs;

	/** the output pins of a new node */
	UPROPERTY(VisibleAnywhere, Category = "Node")
	TArray<FString> Outputs;

	/** other names the palette finds the node by */
	UPROPERTY(VisibleAnywhere, Category = "Node")
	TArray<FString> Aliases;
};

/** One row of the `pcgTemplates` source: a PCG graph template (engine or project) that CreateGraph copies. */
UCLASS(Transient)
class UUnrealAIPCGTemplate : public UObject
{
	GENERATED_BODY()

public:
	/** the template's asset name, e.g. TPL_Showcase_SimpleForest */
	UPROPERTY(VisibleAnywhere, Category = "Template")
	FString Name;

	/** the asset path to pass to CreateGraph, e.g. /PCG/GraphTemplates/TPL_Showcase_SimpleForest */
	UPROPERTY(VisibleAnywhere, Category = "Template")
	FString Path;

	/** the template's category */
	UPROPERTY(VisibleAnywhere, Category = "Template")
	FString Category;

	/** what the template makes */
	UPROPERTY(VisibleAnywhere, Category = "Template")
	FString Description;
};

namespace UnrealAIPCGCatalog
{
	/** A node type AddNode can create: the settings class and, for a preconfigured variant, the variant to apply. */
	struct FNodeType
	{
		TSubclassOf<UPCGSettings> Class;
		TOptional<FPCGPreConfiguredSettingsInfo> Preset;
	};

	/** The `pcgNodeTypes` rows, sorted by category and name. */
	TArray<UObject*> NodeTypes();

	/** The `pcgTemplates` rows: the PCG graphs marked as templates, sorted by name. */
	TArray<UObject*> Templates();

	/** The node type @p Name names (title, Type, class, alias or preconfigured title; case and spaces ignored); Fail()s naming similar ones. */
	bool FindNodeType(const FString& Name, FNodeType& Out);

	/** The settings class without its PCG prefix and Settings suffix (UPCGSurfaceSamplerSettings: SurfaceSampler). */
	FString TypeName(const UClass* SettingsClass);
}
