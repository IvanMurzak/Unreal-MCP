// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

class AActor;
class UClass;
class UObject;
class UWorld;

/**
 * Object-reference resolution for tool handlers (docs/ARCHITECTURE.md §3.2 — the "path-or-name string
 * ref" rule). The analog of Unity-MCP's `ObjectRef`: a single string identifies a class, an asset, an
 * actor, or a component, resolved with a label → FindObject → soft-path-load fallback chain.
 *
 * Shared by the actor family (and reused by the asset/blueprint families as they land), so it lives in
 * a self-contained Private/Tools header. Every method runs ON the game thread (it touches the editor
 * world and the UObject graph); call it only from a dispatched tool body (§4).
 */
namespace FUnrealMcpObjectRef
{
	/** The editor world the tool family operates on (GEditor's editor-context world). Null outside the editor. */
	UNREALMCPRUNTIME_API UWorld* GetEditorWorld();

	/**
	 * An object of @p Class by path: a package ("/Game/Materials/M_Red", its main asset) or an object path
	 * ("/Game/Materials/M_Red.M_Red"). Already-loaded objects first; otherwise the package is probed before it is loaded,
	 * so a miss logs nothing (loading a missing package logs an engine error even with LOAD_NoWarn). Null when none.
	 */
	UNREALMCPRUNTIME_API UObject* LoadObjectQuiet(UClass* Class, const FString& Path);

	/**
	 * Resolve a class reference to a UClass*: a class path (`/Script/Engine.PointLight`,
	 * `/Game/BP/BP_Foo.BP_Foo_C`), a Blueprint asset path (`/Game/BP/BP_Foo`, its generated class) or a short type
	 * name (`PointLight`, `AStaticMeshActor`: the C++ A/U prefix is tolerated). Quiet: a miss logs nothing.
	 */
	UNREALMCPRUNTIME_API UClass* ResolveClass(const FString& ClassRef);

	/**
	 * Resolve an actor reference within @p World. Matches (case-sensitive first, then case-insensitive
	 * on the label) the actor label, the object name, or the full path name. Returns null when no actor
	 * matches. When @p World is null the editor world is used.
	 */
	UNREALMCPRUNTIME_API AActor* ResolveActor(const FString& ActorRef, UWorld* World = nullptr);

	/**
	 * The one text -> object rule for a reference of type @p Wanted (an edit field's rule): a class reference by class
	 * name or path (ResolveClass), an actor by label / name / path (ResolveActor), anything else by asset path
	 * (LoadObjectQuiet). Null when nothing matches or the match is not a @p Wanted.
	 */
	UNREALMCPRUNTIME_API UObject* ResolveReference(UClass* Wanted, const FString& Text);

	/**
	 * Resolve a generic UObject by an actor reference first (so a live actor can be targeted by label), then by
	 * object path (LoadObjectQuiet). Returns null when nothing matches.
	 */
	UNREALMCPRUNTIME_API UObject* ResolveObject(const FString& ObjectRef, UWorld* World = nullptr);
}
