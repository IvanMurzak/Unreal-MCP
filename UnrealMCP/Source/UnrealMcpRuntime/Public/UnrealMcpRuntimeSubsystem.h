// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Templates/PimplPtr.h"
#include "UnrealMcpRuntimeSubsystem.generated.h"

// Forward-declared: RegisterToolProvider/UnregisterToolProvider take it by pointer only (§12.9), so the
// PUBLIC header needs no extension-contract include — callers include IUnrealMcpToolProvider.h themselves.
class IUnrealMcpToolProvider;
// Forward-declared for the same reason: RegisterPromptProvider/UnregisterPromptProvider take it by pointer.
class IUnrealMcpPromptProvider;

// The runtime-owned machinery (registry/dispatcher/bridge/sidecar/extensions) is held behind a PImpl
// (FRuntimeImpl, defined in the .cpp) so this PUBLIC header forward-declares nothing module-private and the
// TUniquePtr<incomplete-type> members never reach UHT's generated vtable-helper ctor in the .gen.cpp (which,
// in the Game-target / non-unity BuildPlugin compile, would otherwise C4150 on incomplete member types).
// TPimplPtr is purpose-built for forward-declared PImpl: it captures a typed deleter at MakePimpl time, so a
// TPimplPtr<FRuntimeImpl> member destroys correctly even where FRuntimeImpl is incomplete.

/**
 * Which MCP server a runtime Connect() targets (docs/ARCHITECTURE.md §12.4). Blueprint-exposed analog of
 * the internal FUnrealMcpConfig EUnrealMcpConnectionMode (which is a plain C++ enum, not UENUM). Custom is
 * the default for runtime: a game typically dials a developer-supplied loopback server.
 */
UENUM(BlueprintType)
enum class EUnrealMcpRuntimeConnectionMode : uint8
{
	/** A developer-supplied server URL (local dev server, self-hosted, …). The runtime default. */
	Custom,
	/** The hosted ai-game.dev cloud server. */
	Cloud
};

/**
 * The runtime (in-game) bootstrap for Unreal-MCP (docs/ARCHITECTURE.md §12.4) — the UE analog of Unity's
 * `UnityMcpPluginRuntime.Initialize().Build().Connect()`. A UGameInstanceSubsystem, so it is auto-instantiated
 * once per UGameInstance (PIE, Standalone, packaged Development/Shipping) — but it NEVER auto-connects.
 *
 * Lifecycle:
 *  - Initialize(): builds the registry + runtime-safe tool families + game-thread dispatcher + IPC bridge
 *    server, generates the one-shot token, and ARMS the loopback listener (BridgeServer->Start). It does
 *    NOT spawn the sidecar and does NOT connect. It also installs the §12.6 RUNTIME world resolver
 *    (`GetGameInstance()->GetWorld()`) into FUnrealMcpWorldProvider so runtime tool bodies resolve the live
 *    game world.
 *  - Connect(): the single explicit opt-in entry point. Spawns the sidecar (SidecarManager->StartForPort)
 *    and pushes the §1.3 `config` IPC message with keepConnected:true and the resolved host/token/mode.
 *  - Disconnect(): tears the sidecar down (orphan-safe).
 *  - Deinitialize(): graceful teardown — stops the sidecar, tears down the bridge, clears the world resolver.
 *
 * SECURITY (§12.8 — RCE-class surface if reachable; all mitigations enforced in Connect):
 *  1. Explicit opt-in only — auto-instantiated, never auto-connecting. NO RuntimeInitializeOnLoadMethod
 *     auto-dial, NO config-asset auto-connect. (Review-gated invariant.)
 *  2. Loopback IPC + one-shot stdin token (inherited from FUnrealMcpBridgeServer / FUnrealMcpSidecarManager).
 *  3. Build-config gating: bUnrealMcpAllowShipping Build.cs flag → UNREAL_MCP_ALLOW_SHIPPING. With 0, Connect
 *     in a Shipping build logs + returns false.
 *  4. Kill switch: UUnrealMcpRuntimeSettings::bRuntimeMcpEnabled default FALSE → Connect short-circuits.
 *  5. Loopback-host default: Connect rejects non-loopback hosts unless bAllowRemoteHost is passed.
 *
 * QA convenience without a recompile: the console commands `UnrealMcp.Connect <host> [token]` and
 * `UnrealMcp.Disconnect` (registered in Initialize, de-registered in Deinitialize) drive Connect/Disconnect
 * on the active game instance's subsystem.
 */
UCLASS()
class UNREALMCPRUNTIME_API UUnrealMcpRuntimeSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	// UGameInstanceSubsystem ----------------------------------------------------------------------
	/** Build registry/dispatcher/bridge, arm the listener, install the runtime world resolver. NO connect. */
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	/** Orphan-safe sidecar teardown + bridge shutdown + world-resolver clear. */
	virtual void Deinitialize() override;

	/**
	 * Explicit opt-in connect (§12.4) — the ONLY path that spawns the sidecar and connects. Spawns the
	 * bridge sidecar and pushes the §1.3 `config` (keepConnected:true) with the resolved host/token/mode.
	 *
	 * Returns false (and connects nothing) when ANY security gate rejects:
	 *  - the UUnrealMcpRuntimeSettings kill switch is off (§12.8 #4);
	 *  - the build is Shipping and UNREAL_MCP_ALLOW_SHIPPING == 0 (§12.8 #3);
	 *  - @p Host is non-loopback and @p bAllowRemoteHost is false (§12.8 #5);
	 *  - the listener never armed (Initialize failed to bind a port);
	 *  - the sidecar binary cannot be resolved (packaged-without-<rid> / unset UNREAL_MCP_BRIDGE_PATH).
	 *
	 * @param Host             the server URL to connect to (e.g. "http://localhost:8080").
	 * @param Token            optional bearer token; empty means anonymous (Custom+None).
	 * @param Mode             Custom (default) or Cloud.
	 * @param bAllowRemoteHost set true to permit a non-loopback @p Host (off by default, §12.8 #5).
	 */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP",
		meta = (AdvancedDisplay = "Token,Mode,bAllowRemoteHost", AutoCreateRefTerm = "Token"))
	bool Connect(const FString& Host, const FString& Token = TEXT(""),
		EUnrealMcpRuntimeConnectionMode Mode = EUnrealMcpRuntimeConnectionMode::Custom,
		bool bAllowRemoteHost = false);

	/** Tear down the sidecar (orphan-safe). Idempotent — a no-op when not connected. */
	UFUNCTION(BlueprintCallable, Category = "Unreal MCP")
	void Disconnect();

	/** True when a sidecar is connected and handshaken over the runtime bridge. */
	UFUNCTION(BlueprintPure, Category = "Unreal MCP")
	bool IsConnected() const;

	/**
	 * The runtime subsystem for @p WorldContext's game instance, or null when none (no world / no game
	 * instance / subsystem not created). Convenience for the §12.4 BeginPlay snippet and console commands.
	 */
	UFUNCTION(BlueprintPure, Category = "Unreal MCP", meta = (WorldContext = "WorldContext"))
	static UUnrealMcpRuntimeSubsystem* Get(const UObject* WorldContext);

	/**
	 * Register a custom gameplay tool provider at runtime (docs/ARCHITECTURE.md §12.9) — the ergonomic
	 * UE analog of Unity's `WithToolsFromAssembly` / `[AiTool]` scan (README Chess example). A game module
	 * registers an IUnrealMcpToolProvider it owns and its tools merge into the live registry; the §5
	 * extension manager rebuilds the registry and re-pushes the manifest so a CONNECTED sidecar's
	 * `tools/list` gains the provider's tools without the developer touching IModularFeatures directly.
	 *
	 * This is a thin, discoverable wrapper over
	 * `IModularFeatures::Get().RegisterModularFeature(IUnrealMcpToolProvider::GetModularFeatureName(), Provider)`:
	 * the manager subscribed to the modular-feature register/unregister events in Initialize(), so the
	 * registration is observed and the manifest re-pushed exactly as a plugin-load-time registration is
	 * (docs/EXTENSIONS.md "Runtime usage"). The same provider may be registered via either path.
	 *
	 * OWNERSHIP: @p Provider is NOT owned by the subsystem — the caller keeps it alive for as long as it is
	 * registered (typically a member of the game module/subsystem) and MUST call UnregisterToolProvider
	 * (or unregister the modular feature) before destroying it. A null @p Provider is a no-op.
	 *
	 * Callable from C++ and Blueprint is NOT offered (a raw interface pointer is not a Blueprint type);
	 * Blueprint authors expose tools by registering their provider in C++ module startup as today.
	 */
	void RegisterToolProvider(IUnrealMcpToolProvider* Provider);

	/**
	 * Unregister a gameplay tool provider previously registered via RegisterToolProvider (or directly via
	 * IModularFeatures). The §5 manager observes the unregister event, rebuilds the registry, and re-pushes
	 * the manifest so a connected sidecar's `tools/list` drops the provider's tools. A null @p Provider, or
	 * one that was never registered, is a harmless no-op (IModularFeatures tolerates an unknown unregister).
	 */
	void UnregisterToolProvider(IUnrealMcpToolProvider* Provider);

	/**
	 * Register a custom gameplay PROMPT provider at runtime (§A.2) — the prompt sibling of
	 * RegisterToolProvider. A thin wrapper over
	 * `IModularFeatures::Get().RegisterModularFeature(IUnrealMcpPromptProvider::GetModularFeatureName(), Provider)`:
	 * the extension manager subscribed to the prompt modular-feature events in Initialize(), so the
	 * registration rebuilds the prompt registry and re-pushes the manifest. OWNERSHIP: not owned — the caller
	 * keeps @p Provider alive and must UnregisterPromptProvider before destroying it. Null is a no-op.
	 */
	void RegisterPromptProvider(IUnrealMcpPromptProvider* Provider);

	/** Unregister a prompt provider previously registered via RegisterPromptProvider (or directly via IModularFeatures). Null / unknown is a harmless no-op. */
	void UnregisterPromptProvider(IUnrealMcpPromptProvider* Provider);

	/** Whether the loopback listener armed (a port is bound). False means Initialize failed to bind. */
	bool IsListenerArmed() const { return BoundPort > 0; }

	/**
	 * True when @p Host resolves to a loopback address (localhost / the 127.0.0.0/8 block / ::1). Pure +
	 * static + exported so the §12.8 #5 loopback-host policy is unit-testable without standing up a sidecar.
	 * An empty host counts as loopback (it resolves to the §8 DefaultCustomHost, which is localhost).
	 */
	static bool IsLoopbackHost(const FString& Host);

private:
	/** Register / unregister the QA console commands (UnrealMcp.Connect / UnrealMcp.Disconnect). */
	void RegisterConsoleCommands();
	void UnregisterConsoleCommands();

	/** `UnrealMcp.Connect <host> [token]` handler: parse args and call Connect (loopback-only, Custom mode). */
	void HandleConnectCommand(const TArray<FString>& Args);

	// Runtime-owned machinery (registry/dispatcher/bridge/sidecar/extensions — mirrors FUnrealMcpEditorCoordinator,
	// §12.3 Model A) held behind a PImpl defined in the .cpp. See the file-top note for why this is a PImpl and
	// not a set of forward-declared TUniquePtr members.
	struct FRuntimeImpl;
	TPimplPtr<FRuntimeImpl> Impl;

	/** The one-shot IPC token generated in Initialize; reused by every Connect (bridge + sidecar agree, §1.4). */
	FString IpcToken;
	/** The bound loopback port from BridgeServer->Start; > 0 once the listener armed. */
	int32 BoundPort = -1;

	/** The console-command objects (owned by IConsoleManager; we hold them to deregister in Deinitialize). */
	IConsoleObject* ConnectCommand = nullptr;
	IConsoleObject* DisconnectCommand = nullptr;
};
