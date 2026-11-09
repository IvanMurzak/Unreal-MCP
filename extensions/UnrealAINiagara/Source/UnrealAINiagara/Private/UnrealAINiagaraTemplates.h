// Copyright (c) 2026 IvanMurzak/Unreal-AI-Niagara. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "UnrealAINiagaraTemplates.generated.h"

/**
 * One row of the `niagaraTemplates` source: a Niagara system or emitter template the engine or an enabled plugin ships
 * (read from the asset registry, nothing is loaded), or a row for an installed but disabled template plugin.
 */
UCLASS(Transient)
class UUnrealAINiagaraTemplate : public UObject
{
	GENERATED_BODY()

public:
	/** the template's asset name, e.g. Grid3D_Gas_Fire */
	UPROPERTY(VisibleAnywhere, Category = "Template")
	FString Name;

	/** the asset path to pass to CreateSystem / AddEmitter, e.g. /NiagaraFluids/Templates/Gas/3D/Systems/Grid3D_Gas_Fire (empty when not Available) */
	UPROPERTY(VisibleAnywhere, Category = "Template")
	FString Path;

	/** System (a whole effect: CreateSystem copies it), Emitter (one layer: CreateSystem or AddEmitter), or Plugin (a disabled template plugin) */
	UPROPERTY(VisibleAnywhere, Category = "Template")
	FString Kind;

	/** the folder below the plugin's template root, e.g. Gas/3D/Systems */
	UPROPERTY(VisibleAnywhere, Category = "Template")
	FString Category;

	/** the plugin that ships it: Niagara, NiagaraFluids, ... */
	UPROPERTY(VisibleAnywhere, Category = "Template")
	FString Plugin;

	/** what it makes */
	UPROPERTY(VisibleAnywhere, Category = "Template")
	FString Description;

	/** false for a disabled plugin's row: its Description says how to enable it */
	UPROPERTY(VisibleAnywhere, Category = "Template")
	bool Available = true;
};

namespace UnrealAINiagaraTemplates
{
	/** The template rows, sorted by plugin, kind, category and name. */
	TArray<UObject*> List();

	/**
	 * The Path of the row of @p Rows (see List) that @p Name names, ignoring case, spaces and underscores. Empty with
	 * @p OutError listing the candidates when no row or several rows match.
	 */
	FString ResolveName(const TArray<UObject*>& Rows, const FString& Name, FString& OutError);

	/** Whether the NiagaraFluids plugin is enabled in this project. */
	bool IsFluidsEnabled();

	/** Whether the NiagaraFluids plugin is installed with the engine (enabled or not). */
	bool IsFluidsInstalled();

	/** The message for a path into a template plugin that is not enabled; empty when @p Path is not one. */
	FString DisabledPluginMessage(const FString& Path);
}
