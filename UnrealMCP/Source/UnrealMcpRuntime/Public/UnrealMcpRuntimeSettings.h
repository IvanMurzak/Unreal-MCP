// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "UnrealMcpRuntimeSettings.generated.h"

/**
 * Project-level kill switch for the runtime (in-game) MCP connection (docs/ARCHITECTURE.md §12.8 #4).
 *
 * Persisted in DefaultGame.ini under [/Script/UnrealMcpRuntime.UnrealMcpRuntimeSettings] and surfaced in
 * Project Settings → Plugins → "Unreal MCP (Runtime)". The single setting `bRuntimeMcpEnabled` defaults
 * to FALSE: a shipped/packaged game that ships with the plugin does NOT expose its in-game MCP remote-control
 * surface unless the developer flips this flag on purpose. UUnrealMcpRuntimeSubsystem::Connect() short-circuits
 * (returns false + logs) while the flag is off — one of the five layered runtime-security mitigations (the
 * others: explicit-opt-in-only Connect, loopback IPC + one-shot stdin token, the bUnrealMcpAllowShipping Build
 * flag, and the loopback-host default that rejects remote hosts without bAllowRemoteHost).
 *
 * This is the analog of Unity's runtime opt-in gate. It is a Game-config setting (not Editor) so it travels
 * into a packaged build, exactly where the gate must take effect.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Unreal MCP (Runtime)"))
class UNREALMCPRUNTIME_API UUnrealMcpRuntimeSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UUnrealMcpRuntimeSettings();

	/**
	 * Master enable for the runtime (in-game) MCP connection. Default FALSE (§12.8 #4): while off, every
	 * Connect() call is rejected with a log line and no sidecar is spawned. Turning it on does NOT cause an
	 * auto-connect — a connection still requires an explicit Connect() call (the §12.8 #1 invariant).
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Runtime",
		meta = (DisplayName = "Enable Runtime MCP",
			ToolTip = "When off (default), in-game UUnrealMcpRuntimeSubsystem::Connect() is rejected. This is a security kill switch; even when on, a connection still requires an explicit Connect() call (the plugin never auto-connects)."))
	bool bRuntimeMcpEnabled = false;

	/**
	 * Whether the runtime (in-game) subsystem registers the built-in core tool families
	 * (ping, actor/component, console+reflection, screenshot, level-get-data). Default TRUE — existing
	 * users see no behavior change. Turn it OFF to ship a game that exposes ONLY its own custom tools
	 * (registered via IUnrealMcpToolProvider / RegisterToolProvider); the built-ins are skipped at
	 * Initialize() and never reach the runtime registry.
	 *
	 * SECURITY (docs/ARCHITECTURE.md §12.8): the built-in set includes `reflection-method-call` and
	 * `console-run-command`, which together are effectively remote arbitrary-code-execution over the
	 * network. A shipped game that only wants its own narrow tool surface should turn this off so that
	 * surface is never exposed. This gates the RUNTIME path only — the editor toolset is unaffected.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Runtime",
		meta = (DisplayName = "Register Built-in Runtime Tools",
			ToolTip = "When on (default), the runtime subsystem registers the built-in core tool families. Turn off to ship a game that exposes ONLY its own custom tools. The built-ins include reflection-method-call and console-run-command (effectively remote code execution), so disabling them shrinks the in-game remote-control surface. Runtime path only; the editor toolset is unaffected."))
	bool bRegisterBuiltinRuntimeTools = true;

	/** Settings-category placement helpers (Project Settings → Plugins). */
	virtual FName GetCategoryName() const override;
};
