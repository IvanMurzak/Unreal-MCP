// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "UnrealMcpRqlTypes.h"

#include <functional>
#include <string>

class FUnrealMcpRqlSession;
class UScriptStruct;

/** Most assets one unreal.assets(...) / asset source lists (they load lazily, as the query reads them). */
constexpr int32 UnrealMcpRqlMaxAssets = 200;

/** Assets of @p Class (and subclasses) under @p Path ("/Game" when empty), sorted by path, loaded when read. */
rql::Value UnrealMcpRqlAssetList(FUnrealMcpRqlTypes& Types, UClass* Class, const FString& Path, bool bAllRootsWhenEmpty = false);

/**
 * The class default objects of the subclasses of @p Base (loaded abstract classes left out; Blueprint classes that are not
 * loaded yet load as the query reads them), sorted by class name, at most UnrealMcpRqlMaxAssets.
 */
rql::Value UnrealMcpRqlClassDefaultsList(FUnrealMcpRqlTypes& Types, UClass* Base);

/**
 * " (did you mean /Engine/BasicShapes/Cube?)": up to three asset paths of @p Class whose name matches the last part of
 * @p Text ("Cube", "/Game/Mehses/Cube"), for a reference that resolved to nothing; "" when there is none (or for actor and
 * class references, which do not name assets).
 */
FString UnrealMcpRqlSimilarAssets(UClass* Class, const FString& Text);

/** Text and list literals of the math structs: "(1, 2, 3)", "X=1 Y=2 Z=3", [1, 2, 3], "#ff8800" (colors). False (and no
 * error) for other structs and value kinds; false + @p OutError for a malformed literal. FUnrealMcpRqlTypes::CoerceStruct uses it. */
bool UnrealMcpRqlParseMathStruct(UScriptStruct* Struct, const rql::Value& Value, void* Out, FString& OutError);

/**
 * An adapter `coerce` that builds a @p Struct value through FUnrealMcpRqlTypes::CoerceStruct (math literals, struct
 * parsers) from the value kinds @p Accepts takes; other values fall through to RQL's own coercion.
 */
std::function<rql::AdapterOutcome(const rql::Value&, const rql::TypeDescriptor&, rql::Value&, std::string&)>
	UnrealMcpRqlStructCoerce(FUnrealMcpRqlTypes& Types, UScriptStruct* Struct, std::function<bool(const rql::Value&)> Accepts);

/** RQL type adapters for the math structs: "(x, y, z)" output, text / list literals on writes. */
void UnrealMcpRqlAddStructAdapters(FUnrealMcpRqlSession& Session);

/** DataTable rows: UDataTable `Rows` (row name -> row struct), RowNames, RowCount, row calls; the `dataTables` source. */
void UnrealMcpRqlAddDataTables(FUnrealMcpRqlSession& Session);

/** User-defined structs: UUserDefinedStruct `Fields` and the Struct editor's AddField / RemoveField / RenameField / SetFieldDefault. */
void UnrealMcpRqlAddUserStructs(FUnrealMcpRqlSession& Session);

/** Gameplay tags as text ("A.B.C") / lists, validated against the tag tree; the `gameplayTags` source. */
void UnrealMcpRqlAddGameplayTags(FUnrealMcpRqlSession& Session);

/** Project Settings / Editor Preferences sections: the `settings` source and unreal.settings(name); writes saved to config. */
void UnrealMcpRqlAddSettings(FUnrealMcpRqlSession& Session);

/** Blueprint graphs: Graphs / Nodes / Pins, node creation, pin connections, Compile; the `blueprints` source. */
void UnrealMcpRqlAddBlueprints(FUnrealMcpRqlSession& Session);

/** UMG: Widget Blueprint widget trees (RootWidget, Widgets, AddWidget), panel Children, widget Parent / Destroy. */
void UnrealMcpRqlAddWidgets(FUnrealMcpRqlSession& Session);

/** Level Sequences: bindings, tracks, sections and channel keys in display frames; the `levelSequences` source. */
void UnrealMcpRqlAddSequences(FUnrealMcpRqlSession& Session);
