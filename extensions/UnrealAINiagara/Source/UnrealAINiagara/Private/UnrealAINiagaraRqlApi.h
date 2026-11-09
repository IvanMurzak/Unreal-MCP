// Copyright (c) 2026 IvanMurzak/Unreal-AI-Niagara. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "UnrealAINiagaraRqlApi.generated.h"

class AActor;
class UMaterialInterface;
class UNiagaraEmitter;
class UNiagaraSystem;

/** The result of `niagara | call Compile(system)`: a failed compile is a result, not an error. */
USTRUCT(BlueprintType)
struct FUnrealAINiagaraCompileResult
{
	GENERATED_BODY()

	/** true when every enabled script compiled without errors */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Niagara")
	bool Succeeded = false;

	/** "<emitter> <script>: <message>" for each compile error */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Niagara")
	TArray<FString> Errors;

	/** "<emitter> <script>: <message>" for each compile warning */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Niagara")
	TArray<FString> Warnings;
};

/**
 * The `niagara` RQL source (type "NiagaraOperations"): author Niagara systems from templates, tune emitters and module
 * inputs, add collision, set materials, compile and place them, e.g.
 * `niagara | call CreateSystem("/Game/VFX/NS_Fire", "/Niagara/DefaultAssets/Templates/Emitters/Fountain")`. Every
 * UFUNCTION here is callable from rql-mutate (Unreal-MCP's IUnrealMcpRqlModule::AddApiClass); the doc comment is the
 * description agents see. BlueprintCallable only so UHT records the C++ default arguments; BlueprintInternalUseOnly keeps
 * them out of menus. Emitter, module and input names ignore case, spaces and underscores; a miss fails naming what exists.
 */
UCLASS(Transient)
class UUnrealAINiagaraRqlApi : public UObject
{
	GENERATED_BODY()

public:
	/** create a Niagara system asset at Path ("/Game/VFX/NS_Fire") from Template: a system template (copied whole), an emitter template or emitter asset (the system gets that one emitter), each by asset path or by its Name in niagaraTemplates, or "" (an empty system); returns the system (unsaved) */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Niagara", meta = (BlueprintInternalUseOnly = "true"))
	UNiagaraSystem* CreateSystem(const FString& Path, const FString& Template = TEXT(""));

	/** add an emitter (a template from niagaraTemplates or any emitter asset) to System, named Name (optional, unique); returns the emitter's name in the system */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Niagara", meta = (BlueprintInternalUseOnly = "true"))
	FString AddEmitter(UNiagaraSystem* System, UNiagaraEmitter* Emitter, const FString& Name = TEXT(""));

	/** remove the emitter named Emitter from System */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Niagara", meta = (BlueprintInternalUseOnly = "true"))
	void RemoveEmitter(UNiagaraSystem* System, const FString& Emitter);

	/** rename the emitter Emitter of System to NewName; returns the name it got (unique in the system) */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Niagara", meta = (BlueprintInternalUseOnly = "true"))
	FString RenameEmitter(UNiagaraSystem* System, const FString& Emitter, const FString& NewName);

	/** enable or disable the emitter Emitter of System */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Niagara", meta = (BlueprintInternalUseOnly = "true"))
	void SetEmitterEnabled(UNiagaraSystem* System, const FString& Emitter, bool bEnabled = true);

	/** set an emitter property (the stack's Emitter Properties: SimTarget "CPU" / "GPU", bLocalSpace, CalculateBoundsMode "Fixed", FixedBounds "(Min=(X=-500,Y=-500,Z=0),Max=(X=500,Y=500,Z=800))", RandomSeed, ...) from text; returns the new value */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Niagara", meta = (BlueprintInternalUseOnly = "true"))
	FString SetEmitterProperty(UNiagaraSystem* System, const FString& Emitter, const FString& Property, const FString& Value);

	/** add a module script (a name such as "CurlNoiseForce", "Drag", "Collision", or a NiagaraScript path) to a stage ("Particle Update", "Particle Spawn", "Emitter Update", "System Update", a simulation stage) of Emitter ("" = the system stack); Index -1 = the end of the stage (the Inputs member shows its inputs); returns "Stage/Module" */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Niagara", meta = (BlueprintInternalUseOnly = "true"))
	FString AddModule(UNiagaraSystem* System, const FString& Emitter, const FString& Stage, const FString& Module, int32 Index = -1);

	/** remove the module Module ("Curl Noise Force", or "Particle Update/Curl Noise Force") from Emitter ("" = the system stack) */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Niagara", meta = (BlueprintInternalUseOnly = "true"))
	void RemoveModule(UNiagaraSystem* System, const FString& Emitter, const FString& Module);

	/** enable or disable the module Module of Emitter ("" = the system stack) */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Niagara", meta = (BlueprintInternalUseOnly = "true"))
	void SetModuleEnabled(UNiagaraSystem* System, const FString& Emitter, const FString& Module, bool bEnabled = true);

	/** set a module input (see the Inputs member for names, types and current values) of Emitter ("" = the system stack) from text: "200", "(0, 0, 300)", "#ff6a00", an enum entry, "true", "random(1, 2)", "curve(0: 1, 1: 0)" (over the particle's life), "@User.Intensity" (link a parameter), an asset path, or "default"; returns the new value */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Niagara", meta = (BlueprintInternalUseOnly = "true"))
	FString SetModuleInput(UNiagaraSystem* System, const FString& Emitter, const FString& Module, const FString& Input, const FString& Value);

	/** set the default of a user (exposed) parameter of System ("Intensity" or "User.Intensity"); a new one is added with Type (float, int, bool, Vector, Vector2, Vector4, LinearColor, Position, or an object class such as StaticMesh; "" = from the value); returns the new value */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Niagara", meta = (BlueprintInternalUseOnly = "true"))
	FString SetUserParameter(UNiagaraSystem* System, const FString& Name, const FString& Value, const FString& Type = TEXT(""));

	/** make the particles of Emitter ("" = every emitter) collide with the scene: Mode "Auto", "SceneDepth" (GPU, what the camera sees), "DistanceField" (GPU, mesh distance fields), "RayTrace" (CPU, physics traces) or "None"; Niagara Fluids grids get their world collision instead; returns what was changed and what the project still needs */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Niagara", meta = (BlueprintInternalUseOnly = "true"))
	FString EnableCollision(UNiagaraSystem* System, const FString& Emitter = TEXT(""), const FString& Mode = TEXT("Auto"));

	/** set Material on the renderers of Emitter (Renderer -1 = every renderer that takes a material: sprite, ribbon, mesh override, volume; or its index in the Renderers member); returns how many renderers changed */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Niagara", meta = (BlueprintInternalUseOnly = "true"))
	int32 SetMaterial(UNiagaraSystem* System, const FString& Emitter, UMaterialInterface* Material, int32 Renderer = -1);

	/** compile System now and wait; returns {Succeeded, Errors, Warnings} (placed effects restart with the result) */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Niagara", meta = (BlueprintInternalUseOnly = "true"))
	FUnrealAINiagaraCompileResult Compile(UNiagaraSystem* System);

	/** place a Niagara system in the current level as a Niagara actor at Location (Label optional, undoable); returns the actor */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Niagara", meta = (BlueprintInternalUseOnly = "true"))
	AActor* SpawnSystem(UNiagaraSystem* System, FVector Location = FVector::ZeroVector, const FString& Label = TEXT(""),
		bool bAutoActivate = true);

	/** (re)start the Niagara effects on Actor in the editor viewport (bReset: from the beginning, picking up asset edits); returns how many components started */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Niagara", meta = (BlueprintInternalUseOnly = "true"))
	int32 Activate(AActor* Actor, bool bReset = true);

	/** stop the Niagara effects on Actor; returns how many components stopped */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Niagara", meta = (BlueprintInternalUseOnly = "true"))
	int32 Deactivate(AActor* Actor);

	/** enable the engine's Niagara Fluids plugin (volumetric fire / smoke / liquid templates) in the project file, and Generate Mesh Distance Fields in the project settings (the 3D gas and liquid templates do not compile without it); takes effect after an editor restart; not undoable; returns what to do next */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|Niagara", meta = (BlueprintInternalUseOnly = "true"))
	FString EnableFluids();
};
