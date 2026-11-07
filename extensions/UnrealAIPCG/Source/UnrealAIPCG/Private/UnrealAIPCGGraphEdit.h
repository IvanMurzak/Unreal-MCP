// Copyright (c) 2026 IvanMurzak/Unreal-AI-PCG. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"

class FProperty;
class UPCGComponent;
class UPCGGraph;
class UPCGNode;
class UPCGPin;

/**
 * What the `pcg` operations and members share: finding nodes and pins by the names agents use, reading a node's settings
 * as text and writing them from text (with errors that name what exists), graph parameters, and generation results.
 * Names ignore case, spaces and underscores; a miss Fail()s (UnrealMcpRql::Fail) listing the valid names.
 */
namespace UnrealAIPCG
{
	/** Lower case without spaces, underscores or dashes: "Points Per Squared Meter" == "PointsPerSquaredMeter". */
	FString Normalize(const FString& Name);

	/** A node's title as the graph editor shows it (its custom title, else the node type's). */
	FString NodeTitle(const UPCGNode& Node);

	/** The node @p Name names in @p Graph: its Name (as AddNode returned it), its title, or Input / Output; Fail()s listing the nodes. */
	UPCGNode* FindNode(UPCGGraph& Graph, const FString& Name);

	/** The pin @p Label of @p Node ("" = its first pin that is not an override pin); Fail()s listing its pins. */
	UPCGPin* FindPin(UPCGNode& Node, const FString& Label, bool bOutput);

	/** "Node.Pin" for messages and the Edges member. */
	FString PinPath(const UPCGPin& Pin);

	/** The node's pins: Pin, Type, Connections ("Node.Pin"), Override (the setting an override pin sets). */
	TSharedPtr<FJsonValue> PinsJson(const UPCGNode& Node, bool bOutput);

	/** The node's settings: Name, Type, Value (text SetNodeSetting takes), Pin (its override pin, when it has one). */
	TSharedPtr<FJsonValue> SettingsJson(const UPCGNode& Node);

	/** Set the setting @p Path ("PointsPerSquaredMeter", "ActorSelector.ActorSelectionTag", the title shown in the editor) from text; returns the new value; "" after Fail(). */
	FString SetSetting(UPCGNode& Node, const FString& Path, const FString& Value);

	/** The override pin of @p Node that sets @p Setting; null after Fail(). */
	UPCGPin* FindOverridePin(UPCGNode& Node, const FString& Setting);

	/** Parse @p Text into the value at @p Address of @p Property (numbers, "(x, y, z)", "#rrggbb", enum entries, true / false, asset paths, "$Position.Z" attribute selectors, UE text); false with @p Error naming what it takes. */
	bool ParseValue(const FProperty& Property, void* Address, const FString& Text, FString& Error);

	/** The value at @p Address as the text ParseValue takes. */
	FString FormatValue(const FProperty& Property, const void* Address);

	/** The graph's user parameters: Name, Type, Value. */
	TSharedPtr<FJsonValue> ParametersJson(const UPCGGraph& Graph);

	/** Every edge: "From.Pin -> To.Pin". */
	TSharedPtr<FJsonValue> EdgesJson(const UPCGGraph& Graph);

	/** After a change: the asset is unsaved and an open PCG graph editor shows the change (it is reopened on a rebuilt graph). */
	void Changed(UPCGGraph& Graph);

	/** What a PCG component generated: managed instanced meshes (per mesh), components, actors, and the points of the graph output. */
	struct FGenerated
	{
		int32 Points = 0;
		int32 Instances = 0;
		int32 Components = 0;
		int32 Actors = 0;
		TMap<FString, int32> Meshes;

		/** "SM_Pine: 812" per mesh, most instances first. */
		TArray<FString> MeshLines() const;
	};

	/** Count what @p Component (and its local components, when partitioned) generated. */
	FGenerated CountGenerated(UPCGComponent& Component);

	/** The Generated member: {Instances, Meshes, Components, Actors, Points, IsGenerating}. */
	TSharedPtr<FJsonValue> GeneratedJson(UPCGComponent& Component);
}
