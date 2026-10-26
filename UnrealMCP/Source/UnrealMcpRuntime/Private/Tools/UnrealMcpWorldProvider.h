// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

class UWorld;

/**
 * The runtime world resolver (docs/ARCHITECTURE.md §12.6). The runtime module owns no GEditor reference,
 * so the "which UWorld do tool bodies operate on?" decision is injected by whoever bootstraps the plugin:
 *
 *  - The EDITOR coordinator (FUnrealMcpEditorCoordinator) installs a resolver that returns the editor
 *    world (`GEditor->GetEditorWorldContext().World()`), preserving today's behaviour byte-for-byte.
 *  - The RUNTIME bootstrap subsystem (UUnrealMcpRuntimeSubsystem) installs a resolver returning the live
 *    game world (`GetGameInstance()->GetWorld()`).
 *
 * Resolvers are keyed by their owner and stacked: the latest installed one answers, and clearing an owner's
 * resolver hands the world back to the one installed before it. In the editor, PIE creates a game instance
 * whose runtime subsystem installs a game-world resolver on top of the coordinator's editor resolver; when
 * PIE ends it clears only its own, so the editor tools resolve the editor world again.
 *
 * When no resolver is installed, GetActiveWorld() returns null — exactly the GEditor==null behaviour the
 * old FUnrealMcpObjectRef::GetEditorWorld() had outside the editor. Every caller already null-checks the
 * returned world, so an un-wired runtime module is safe (it simply resolves nothing).
 *
 * The resolver is invoked ON the game thread (tool bodies run there via the §4 dispatcher); installation
 * happens on the game thread too, so no extra synchronization is required.
 */
namespace FUnrealMcpWorldProvider
{
	/** The world tool bodies operate on, via the latest installed resolver. Null when none is set (or it returns null). */
	UNREALMCPRUNTIME_API UWorld* GetActiveWorld();

	/** Install @p Owner's resolver on top of the others (replacing @p Owner's previous one). */
	UNREALMCPRUNTIME_API void SetWorldResolver(const void* Owner, TFunction<UWorld*()> Resolver);

	/** Remove @p Owner's resolver; the one installed before it answers again. Idempotent. */
	UNREALMCPRUNTIME_API void ClearWorldResolver(const void* Owner);
}
