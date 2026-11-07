// Copyright (c) 2026 IvanMurzak/Unreal-AI-PCG. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "UnrealAIPCGRqlApi.generated.h"

class AActor;
class UPCGComponent;
class UPCGGraph;

/** The result of `pcg | call Generate(actor)`: what the PCG components of the actor generated. */
USTRUCT(BlueprintType)
struct FUnrealAIPCGGenerateResult
{
	GENERATED_BODY()

	/** true when generation finished within the timeout */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PCG")
	bool Generated = false;

	/** the instanced static meshes spawned (every Static Mesh Spawner) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PCG")
	int32 Instances = 0;

	/** "<mesh>: <instances>" per mesh, most first */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PCG")
	TArray<FString> Meshes;

	/** the points that reached the graph's Output node */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PCG")
	int32 Points = 0;

	/** generated components (instanced mesh components included) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PCG")
	int32 Components = 0;

	/** spawned actors (Spawn Actor nodes) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PCG")
	int32 Actors = 0;

	/** the component seed the run used */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PCG")
	int32 Seed = 0;

	/** how long generation took */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PCG")
	double Seconds = 0.0;

	/** PCG errors logged while generating (a node without input, an invalid bounds, a missing mesh, ...) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PCG")
	TArray<FString> Errors;

	/** PCG warnings logged while generating */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PCG")
	TArray<FString> Warnings;
};

/**
 * The `pcg` RQL source (type "PCGOperations"): author PCG graphs (nodes, connections, settings, graph parameters), put
 * them on actors and volumes, generate and read back what they made, e.g.
 * `pcg | call AddNode("/Game/PCG/PCG_Forest", "Surface Sampler")`. Every UFUNCTION here is callable from rql-mutate
 * (Unreal-MCP's IUnrealMcpRqlModule::AddApiClass); the doc comment is the description agents see. BlueprintCallable only
 * so UHT records the C++ default arguments; BlueprintInternalUseOnly keeps them out of Blueprint menus. Node, pin, setting
 * and parameter names ignore case, spaces and underscores; a miss fails naming what exists.
 */
UCLASS(Transient)
class UUnrealAIPCGRqlApi : public UObject
{
	GENERATED_BODY()

public:
	/** create a PCG graph asset at Path ("/Game/PCG/PCG_Forest"), empty (Input and Output nodes) or a copy of Template (a path from pcgTemplates); returns the graph (unsaved) */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|PCG", meta = (BlueprintInternalUseOnly = "true"))
	UPCGGraph* CreateGraph(const FString& Path, const FString& Template = TEXT(""));

	/** add a node of Type (a Name or Type from pcgNodeTypes: "Surface Sampler", "Density Filter", "Static Mesh Spawner", ...) to Graph, named Name (optional; else the type's name made unique); returns the node's Name, which the other operations take */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|PCG", meta = (BlueprintInternalUseOnly = "true"))
	FString AddNode(UPCGGraph* Graph, const FString& Type, const FString& Name = TEXT(""));

	/** remove the node Node (its Name or title) and its connections from Graph */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|PCG", meta = (BlueprintInternalUseOnly = "true"))
	void RemoveNode(UPCGGraph* Graph, const FString& Node);

	/** connect the output pin FromPin of node From to the input pin ToPin of node To ("" = the node's main pin; "Input" / "Output" are the graph's own nodes; a setting's name reaches its override pin); returns "From.Pin -> To.Pin" */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|PCG", meta = (BlueprintInternalUseOnly = "true"))
	FString Connect(UPCGGraph* Graph, const FString& From, const FString& To, const FString& FromPin = TEXT(""), const FString& ToPin = TEXT(""));

	/** remove the connections from node From to node To (only FromPin / ToPin when given); returns how many were removed */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|PCG", meta = (BlueprintInternalUseOnly = "true"))
	int32 Disconnect(UPCGGraph* Graph, const FString& From, const FString& To, const FString& FromPin = TEXT(""), const FString& ToPin = TEXT(""));

	/** set the setting Setting of node Node from text (see the node's Settings member: "PointsPerSquaredMeter" "0.05", "LowerBound" "0.5", "Mode" an enum entry, "(x, y, z)", "TargetAttribute" "$Position.Z", "ActorSelector.ActorSelectionTag" "Road", a Static Mesh Spawner's "Meshes" "/Game/SM_Pine: 3, /Game/SM_Birch: 1", "bEnabled" "false"); returns the new value */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|PCG", meta = (BlueprintInternalUseOnly = "true"))
	FString SetNodeSetting(UPCGGraph* Graph, const FString& Node, const FString& Setting, const FString& Value);

	/** set the default of the graph parameter Name (user parameter); a new one is added with Type (double, float, int, int64, bool, string, name, Vector, Vector2, Vector4, Rotator, Transform, or an asset class such as StaticMesh; "" = from the value); components override it per instance; returns the new value */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|PCG", meta = (BlueprintInternalUseOnly = "true"))
	FString SetParameter(UPCGGraph* Graph, const FString& Name, const FString& Value, const FString& Type = TEXT(""));

	/** remove the graph parameter Name */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|PCG", meta = (BlueprintInternalUseOnly = "true"))
	void RemoveParameter(UPCGGraph* Graph, const FString& Name);

	/** drive the setting Setting of node Node by the graph parameter Parameter (a Get Graph Parameter node wired to the setting's override pin), e.g. BindParameter(graph, "Density", "Surface Sampler", "PointsPerSquaredMeter"); returns "Parameter -> Node.Pin" */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|PCG", meta = (BlueprintInternalUseOnly = "true"))
	FString BindParameter(UPCGGraph* Graph, const FString& Parameter, const FString& Node, const FString& Setting);

	/** add a PCG component to an actor (label, name or ref), bound to a PCG graph when one is given (undoable); returns the component */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|PCG", meta = (BlueprintInternalUseOnly = "true"))
	UPCGComponent* AddComponent(AActor* Actor, UPCGGraph* Graph = nullptr);

	/** bind Graph to the PCG component of Actor (adds one when it has none; undoable); returns the component */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|PCG", meta = (BlueprintInternalUseOnly = "true"))
	UPCGComponent* SetGraph(AActor* Actor, UPCGGraph* Graph);

	/** place a PCG Volume running Graph in the current level: centred on Location, Size the full box in cm ("(50000, 50000, 20000)" = 500 x 500 m, 200 m tall: cover the terrain's heights), labelled Label (optional; undoable); returns the volume */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|PCG", meta = (BlueprintInternalUseOnly = "true"))
	AActor* SpawnVolume(UPCGGraph* Graph, FVector Location, FVector Size = FVector(10000.0, 10000.0, 10000.0), const FString& Label = TEXT(""));

	/** run PCG generation on the PCG components of Actor and wait for it (up to TimeoutSeconds); Seed (not -1) sets the component seed first: another seed, another variant; returns {Generated, Instances, Meshes, Points, Components, Actors, Seed, Seconds, Errors, Warnings} */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|PCG", meta = (BlueprintInternalUseOnly = "true"))
	FUnrealAIPCGGenerateResult Generate(AActor* Actor, int32 Seed = -1, double TimeoutSeconds = 120.0);

	/** remove what the PCG components of Actor generated (instances, components, actors); returns how many instances were removed */
	UFUNCTION(BlueprintCallable, Category = "Unreal AI|PCG", meta = (BlueprintInternalUseOnly = "true"))
	int32 Cleanup(AActor* Actor);
};
