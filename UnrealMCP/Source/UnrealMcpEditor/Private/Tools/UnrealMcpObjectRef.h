// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

class AActor;
class UActorComponent;
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
	UNREALMCPEDITOR_API UWorld* GetEditorWorld();

	/**
	 * Resolve a class reference to a UClass*. Accepts, in order:
	 *   - a soft class path (`/Script/Engine.PointLight`, `/Game/BP/BP_Foo.BP_Foo_C`),
	 *   - a loadable object path that is itself a UClass or a UBlueprint (returns its GeneratedClass),
	 *   - a short native type name (`StaticMeshActor`, `PointLight`).
	 * Returns null when nothing matches.
	 */
	UNREALMCPEDITOR_API UClass* ResolveClass(const FString& ClassRef);

	/**
	 * Resolve an actor reference within @p World. Matches (case-sensitive first, then case-insensitive
	 * on the label) the actor label, the object name, or the full path name. Returns null when no actor
	 * matches. When @p World is null the editor world is used.
	 */
	UNREALMCPEDITOR_API AActor* ResolveActor(const FString& ActorRef, UWorld* World = nullptr);

	/** Resolve a component on @p Actor by component name or readable name. Null when not found. */
	UNREALMCPEDITOR_API UActorComponent* ResolveComponent(AActor* Actor, const FString& ComponentRef);

	/**
	 * Resolve a generic UObject by object path (`/Game/Folder/Asset.Asset`) or, when @p World is given,
	 * by an actor reference first (so `object-get-data` can target a live actor by label). Returns null
	 * when nothing matches.
	 */
	UNREALMCPEDITOR_API UObject* ResolveObject(const FString& ObjectRef, UWorld* World = nullptr);

	/** A short JSON identity block for an actor: { name, label, class, path }. */
	UNREALMCPEDITOR_API TSharedPtr<FJsonObject> ActorIdentity(const AActor* Actor);

	/** A short JSON identity block for a component: { name, class, path }. */
	UNREALMCPEDITOR_API TSharedPtr<FJsonObject> ComponentIdentity(const UActorComponent* Component);
}
