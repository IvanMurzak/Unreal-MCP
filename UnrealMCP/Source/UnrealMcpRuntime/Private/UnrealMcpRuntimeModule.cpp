// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"

/**
 * Runtime module for the Unreal-MCP plugin (docs/ARCHITECTURE.md §12). Type `Runtime`, so it loads in
 * the editor, PIE, Standalone and packaged builds. It owns the engine-agnostic machinery (IPC bridge
 * server §1, game-thread dispatcher §4, tool registry §2, schema/object-ref resolution §3, sidecar
 * manager §6, config §8, extension bus §5, log collector + the runtime-safe `ping` core tool).
 *
 * In R1 this module carries no own bootstrap — the editor module (UnrealMcpEditor) still constructs and
 * drives FUnrealMcpEditorCoordinator, which builds the registry/dispatcher/bridge from these moved-down
 * types (Model A, §12.3). The opt-in runtime bootstrap subsystem (UUnrealMcpRuntimeSubsystem) lands in R3.
 */
class FUnrealMcpRuntimeModule : public IModuleInterface
{
};

IMPLEMENT_MODULE(FUnrealMcpRuntimeModule, UnrealMcpRuntime)
