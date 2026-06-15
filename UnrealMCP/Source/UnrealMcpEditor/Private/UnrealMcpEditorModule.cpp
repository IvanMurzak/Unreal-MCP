// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "UnrealMcpLog.h"
#include "UnrealMcpRuntime.h"
#include "UI/FUnrealMcpStyle.h"

/**
 * Editor module for the Unreal-MCP plugin (docs/ARCHITECTURE.md §0). Owns the plugin-lifetime
 * FUnrealMcpRuntime, which wires the tool registry (§2), the game-thread dispatcher (§4), the IPC
 * bridge server (§1) and the sidecar manager (§6). The remaining subsystems (schema generator §3,
 * extensions §5, Slate UI §7, config §8) land in later tasks.
 */
class FUnrealMcpEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// The canonical boot line — the headless smoke test greps for it. Logged FIRST so a runtime
		// startup hiccup never hides the proof that the module itself loaded.
		UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] plugin loaded"));

		// The "AI Game Developer" Slate style set (§7) — must be registered before any §7 window is built.
		FUnrealMcpStyle::Initialize();

		Runtime = MakeUnique<FUnrealMcpRuntime>();
		Runtime->Startup();
	}

	virtual void ShutdownModule() override
	{
		if (Runtime.IsValid())
		{
			Runtime->Shutdown();
			Runtime.Reset();
		}
		FUnrealMcpStyle::Shutdown();
		UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] plugin shutting down"));
	}

private:
	TUniquePtr<FUnrealMcpRuntime> Runtime;
};

IMPLEMENT_MODULE(FUnrealMcpEditorModule, UnrealMcpEditor)
