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

	/** Settings-category placement helpers (Project Settings → Plugins). */
	virtual FName GetCategoryName() const override;
};
