// Copyright (c) 2026 IvanMurzak/Unreal-AI-Landscape. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

class UMaterialInterface;
class UWorld;

/**
 * Keeps the Water plugin from opening modal UI on the game thread. A water body that affects the landscape makes the
 * Water editor spawn a WaterBrushManager, whose SetTargetLandscape asks the Landscape editor for a "Water" edit layer;
 * when the landscape has none, that call creates it and then shows the modal "Insert New Landscape Edit Layer" dialog
 * (ULandscapeSettings::bShowDialogForAutomaticLayerCreation), which blocks every tool call until a human closes it.
 * Creating the "Water" layer first (what the dialog's OK does) means the dialog is never needed.
 */
namespace UnrealAIWaterSafety
{
	/**
	 * Whether @p Material can render water (its base material has "Used with Water"). Read-only: CheckMaterialUsage would
	 * instead switch the flag on and recompile the material (even an engine one) in the editor.
	 */
	bool IsWaterMaterial(const UMaterialInterface* Material);

	/** Add a "Water" edit layer to every landscape of @p World that has none; how many it added. */
	int32 PrepareWaterEditLayers(UWorld* World);

	/**
	 * Watch every editor world: before a water body that affects the landscape (or a landscape brush) is initialised,
	 * prepare the "Water" edit layers. Covers every way of placing one: these operations, editor | call SpawnActor,
	 * Python. Start on module startup, Stop on shutdown.
	 */
	void Start();
	void Stop();

	/**
	 * While alive (the water operations' spawns in @p World): a water body initialised in @p World does not affect the
	 * landscape (no water brush, no "Water" edit layer, no GPU brush pass), and the Landscape editor creates any automatic
	 * edit layer without its modal dialog.
	 */
	class FWaterSpawnScope
	{
	public:
		explicit FWaterSpawnScope(UWorld* World);
		~FWaterSpawnScope();
		FWaterSpawnScope(const FWaterSpawnScope&) = delete;
		FWaterSpawnScope& operator=(const FWaterSpawnScope&) = delete;

	private:
		bool bPreviousDialog = true;
	};
}
