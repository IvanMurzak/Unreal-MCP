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
 *  - The RUNTIME bootstrap subsystem (R3, not yet present) will install a resolver returning the live
 *    game world (`GetGameInstance()->GetWorld()`).
 *
 * When no resolver is installed, GetActiveWorld() returns null — exactly the GEditor==null behaviour the
 * old FUnrealMcpObjectRef::GetEditorWorld() had outside the editor. Every caller already null-checks the
 * returned world, so an un-wired runtime module is safe (it simply resolves nothing).
 *
 * The resolver is invoked ON the game thread (tool bodies run there via the §4 dispatcher); installation
 * happens once during plugin startup, also on the game thread, so no extra synchronization is required.
 */
namespace FUnrealMcpWorldProvider
{
	/** The world tool bodies operate on, via the installed resolver. Null when no resolver is set (or it returns null). */
	UNREALMCPRUNTIME_API UWorld* GetActiveWorld();

	/** Install the world resolver. Called once on startup by the editor coordinator (R1) or the runtime subsystem (R3). */
	UNREALMCPRUNTIME_API void SetWorldResolver(TFunction<UWorld*()> Resolver);

	/** Clear the installed resolver (teardown). Idempotent. */
	UNREALMCPRUNTIME_API void ClearWorldResolver();
}
