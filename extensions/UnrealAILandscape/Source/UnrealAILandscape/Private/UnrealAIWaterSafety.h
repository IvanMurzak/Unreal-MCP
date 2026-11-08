// Copyright (c) 2026 IvanMurzak/Unreal-AI-Landscape. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

class AWaterBody;
class UMaterialInterface;
class UWorld;

/**
 * Keeps the Water plugin usable from tool calls, however a water body is placed (the water operations,
 * editor | call SpawnActor, Python):
 *
 * - No modal dialog. A landscape-affecting body makes the Water editor spawn a WaterBrushManager, which asks the
 *   Landscape editor for a "Water" edit layer; with ULandscapeSettings::bShowDialogForAutomaticLayerCreation on, that
 *   creates the layer and then opens the modal "Insert New Landscape Edit Layer" dialog on the game thread, blocking the
 *   editor and every tool call until a human closes it. While the extension is loaded the setting is off (in memory,
 *   never saved): the layer is still created, where the dialog's OK puts it, and can be reordered in Landscape mode.
 * - Water defaults. A body spawned without the Water actor factory (SpawnActor, Python) has no material and no water
 *   info mesh: it is invisible, and assigning a material later (set_water_material) makes the water zone draw a body
 *   without that mesh ("Ensure condition failed: StaticMesh", WaterMeshComponent.cpp). On the next tick such a body gets
 *   the defaults the Place Actors panel gives (the factory's PostSpawnActor, keeping its spline) and its mesh is built.
 */
namespace UnrealAIWaterSafety
{
	/**
	 * Whether @p Material can render water (its base material has "Used with Water"). Read-only: CheckMaterialUsage would
	 * instead switch the flag on and recompile the material (even an engine one) in the editor.
	 */
	bool IsWaterMaterial(const UMaterialInterface* Material);

	/** Rebuild @p Body now (shape, collision, zone, water info mesh), as the editor does after an edit. */
	void Rebuild(AWaterBody* Body);

	/**
	 * Give @p Body the Place Actors defaults of its type (materials, waves, terrain curves; its spline points are kept)
	 * and rebuild it; false when the Water plugin has no actor factory for it.
	 */
	bool ApplyWaterDefaults(AWaterBody* Body);

	/** Start on module startup (the dialog setting, the defaults repair), Stop on shutdown. */
	void Start();
	void Stop();

	/**
	 * While alive (a water operation's spawn in @p World): a water body initialised there does not affect the landscape
	 * (no water brush, no "Water" edit layer, no GPU brush pass) and is left to its operation (no defaults repair).
	 */
	class FWaterSpawnScope
	{
	public:
		explicit FWaterSpawnScope(UWorld* World);
		~FWaterSpawnScope();
		FWaterSpawnScope(const FWaterSpawnScope&) = delete;
		FWaterSpawnScope& operator=(const FWaterSpawnScope&) = delete;

	private:
		TWeakObjectPtr<UWorld> World;
		FDelegateHandle Hook;
	};
}
