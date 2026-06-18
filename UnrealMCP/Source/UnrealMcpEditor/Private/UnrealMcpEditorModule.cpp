// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "UnrealMcpLog.h"
#include "UnrealMcpEditorCoordinator.h"
#include "UI/FUnrealMcpStyle.h"

/**
 * Editor module for the Unreal-MCP plugin (docs/ARCHITECTURE.md §0, §12.3 Model A). Owns the
 * plugin-lifetime FUnrealMcpEditorCoordinator, which builds the runtime-owned subsystems (tool registry §2,
 * game-thread dispatcher §4, IPC bridge server §1, sidecar manager §6 — all in the UnrealMcpRuntime module)
 * and layers the editor-only families + Slate UI (§7) + config (§8) on top of the same registry.
 */
class FUnrealMcpEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// The canonical boot line — the headless smoke test greps for it. Logged FIRST so a coordinator
		// startup hiccup never hides the proof that the module itself loaded.
		UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] plugin loaded"));

		// The "AI Game Developer" Slate style set (§7) — must be registered before any §7 window is built.
		FUnrealMcpStyle::Initialize();

		Coordinator = MakeUnique<FUnrealMcpEditorCoordinator>();
		Coordinator->Startup();
	}

	virtual void ShutdownModule() override
	{
		if (Coordinator.IsValid())
		{
			Coordinator->Shutdown();
			Coordinator.Reset();
		}
		FUnrealMcpStyle::Shutdown();
		UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] plugin shutting down"));
	}

private:
	TUniquePtr<FUnrealMcpEditorCoordinator> Coordinator;
};

IMPLEMENT_MODULE(FUnrealMcpEditorModule, UnrealMcpEditor)
