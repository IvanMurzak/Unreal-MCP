// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Config/UnrealMcpConfig.h"

class FJsonObject;

/**
 * The live SignalR/connection state surfaced to the main window (docs/ARCHITECTURE.md §7, mapping
 * Unity's GetConnectionStatusClass tri-state). Drives the status dot colour + label + the Connect/
 * Disconnect/Stop button text.
 */
enum class EUnrealMcpConnectionState : uint8
{
	/** No SignalR link and not trying to establish one (keepConnected=false or never started). */
	Disconnected,
	/** Attempting to connect / re-connect (sidecar dialing the MCP server). */
	Connecting,
	/** SignalR connected; tools are reachable end-to-end. */
	Connected,
	/** Was connected, lost the link, and is auto-retrying in the background (keepConnected=true). */
	Degraded
};

/** Cloud device-code authorization progress (docs/ARCHITECTURE.md §7 item 4 / §1.3 `device-auth`). */
enum class EUnrealMcpDeviceAuthState : uint8
{
	/** No device-code flow in progress. */
	Idle,
	/** Authorize pressed; awaiting the user to complete verification in their browser. */
	Pending,
	/** The device-code flow succeeded and a cloud token was stored. */
	Authorized,
	/** The flow failed, was denied, or expired. */
	Failed
};

/**
 * Game-thread-only view-model behind the "AI Game Developer" main window (docs/ARCHITECTURE.md §7).
 * It is the single owner of UI state: it wraps an FUnrealMcpConfig (the §8 config store), tracks the
 * live connection / device-auth state delivered by the sidecar's `status` / `device-auth` IPC feed
 * (marshalled to the game thread by the caller before it reaches ApplyStatus / ApplyDeviceAuth), and
 * exposes the mutators the Slate widgets bind to.
 *
 * Side effects (persisting the config, pushing the §1.3 `config` message, sending `auth-*` messages)
 * are delivered through injectable TFunction sinks so the whole view-model is drivable from the
 * UnrealMcpEditorTests Automation specs with NO live bridge, editor world, or real files. The Slate
 * window wires the sinks to the real FUnrealMcpBridgeServer + on-disk config; a spec wires recording
 * stubs. All symbols are UNREALMCPEDITOR_API-exported so the Tests module links against them.
 *
 * Token discipline (§8): the masked token is the ONLY representation the UI ever renders or copies by
 * default; the raw value never reaches a log line (every diagnostic uses FUnrealMcpConfig::MaskSecret).
 */
class FUnrealMcpEditorViewModel
{
public:
	UNREALMCPEDITOR_API FUnrealMcpEditorViewModel();

	// --- Side-effect sinks (injected by the window; recording stubs in specs). ---

	/** Persist the current config to its on-disk store (§8). Optional; a spec may leave it unset. */
	TFunction<void(const FUnrealMcpConfig&)> OnPersistConfig;
	/** Push the §1.3 `config` message built from the current config to the connected sidecar. */
	TFunction<void(const FUnrealMcpConfig&)> OnPushConfig;
	/**
	 * Send a §1.3 auth message (auth-start / auth-cancel / auth-revoke) to the sidecar. Returns true when the
	 * frame was actually handed to a connected/handshaken sidecar; false when there is no sidecar to send to
	 * (so Authorize() can avoid wedging the UI in a code-less Pending state). A spec may leave it unset.
	 */
	TFunction<bool(const FString& /*AuthType*/)> OnSendAuth;
	/** Open a verification URL in the system browser (device-code flow). */
	TFunction<void(const FString& /*Url*/)> OnOpenBrowser;
	/**
	 * Apply the §7 per-tool enable-map change: the runtime re-applies the disabled set to the tool registry and
	 * re-pushes the manifest so the sidecar's tools/list drops/restores the toggled tool over the wire. Invoked
	 * with the CURRENT persisted disabled-tool name list. Optional; a spec may wire a recording stub. This is
	 * distinct from OnPushConfig: tool enablement travels via the tool-manifest (§2.2), NOT the §1.3 `config`.
	 */
	TFunction<void(const TArray<FString>& /*DisabledTools*/)> OnToolEnablementChanged;

	// --- Local MCP-server (gamedev-mcp-server) control sinks (§7 in-UI Start, issue #95). ---

	/**
	 * Start the LOCAL gamedev-mcp-server (the MCP-server card's "Start"). Wired by the runtime to
	 * FUnrealMcpServerManager::Start(); a spec leaves it unset. ONLY invoked when the launch is gated-in
	 * (Custom mode + http transport) — see IsLocalServerLaunchable / ToggleLocalServer. Returns true on a
	 * successful (or already-running) start.
	 */
	TFunction<bool()> OnStartLocalServer;
	/** Stop the LOCAL gamedev-mcp-server (the MCP-server card's "Stop"). Wired to FUnrealMcpServerManager::Stop(). */
	TFunction<void()> OnStopLocalServer;
	/** Report whether the LOCAL gamedev-mcp-server is currently running. Wired to FUnrealMcpServerManager::IsRunning(); unset → false. */
	TFunction<bool()> IsLocalServerRunningSink;

	// --- Local MCP-server gating + control (pure view-model surface over the sinks above). ---

	/**
	 * Whether the in-UI local-server Start affordance is offered at all: ONLY in Custom mode with http transport
	 * (mirrors FUnrealMcpServerManager::IsLaunchAllowed). In Cloud mode, or Custom+stdio, the Start button is
	 * hidden/disabled and a click is a no-op — the server is never launched. Pure (reads the config only).
	 */
	UNREALMCPEDITOR_API bool IsLocalServerLaunchable() const;
	/** Whether the local server is currently running (false when no sink is wired). Game-thread only. */
	UNREALMCPEDITOR_API bool IsLocalServerRunning() const;
	/**
	 * The MCP-server card's Start/Stop toggle: when launchable, start the server if stopped, else stop it. A
	 * NO-OP (returns false, never touches the sinks) when NOT launchable (Cloud / Custom+stdio) — the gating
	 * guard that guarantees a stray click cannot spawn a server in a non-Custom+http mode. Game-thread only.
	 * Returns true when an action was taken.
	 */
	UNREALMCPEDITOR_API bool ToggleLocalServer();
	/** The MCP-server card's button label for the current state: "Start" when stopped, "Stop" when running. */
	UNREALMCPEDITOR_API FText GetLocalServerButtonText() const;

	/**
	 * Fires whenever a connection-affecting setting changes — connection mode (Cloud↔Custom), Custom host,
	 * auth option, the Custom/Cloud token. The AI Agent Configurators panel subscribes to this so it can
	 * invalidate the active configurator's cached STDIO/HTTP config and rebuild itself, keeping the shown
	 * snippet/status AND what Configure() writes in sync with the live connection WITHOUT the user reselecting
	 * the agent (the C++/Slate analog of Unity's InvalidateAndReloadAgentUI). Broadcast-only and side-effect
	 * free in the view-model; subscribers do the invalidation. Specs can bind a recording lambda to assert the
	 * notification fires. Subscribers MUST unbind (via the returned handle) before they are destroyed.
	 */
	FSimpleMulticastDelegate OnConnectionSettingsChanged;

	// --- Config accessors (the working §8 config the UI edits). ---

	const FUnrealMcpConfig& GetConfig() const { return Config; }
	UNREALMCPEDITOR_API void InitializeConfig(const FUnrealMcpConfig& InConfig);

	/** The persisted log level string ("Trace"/"Debug"/.../"None"); falls back to the §8 default when unset. */
	UNREALMCPEDITOR_API FString GetLogLevel() const;
	/**
	 * Set the log level (the §7 Log Level dropdown). Persists + pushes the §1.3 config so the sidecar adopts the new
	 * verbosity. A no-op change does nothing. Game-thread only. The accepted values mirror Unity's LogLevel enum
	 * (Trace, Debug, Info, Warning, Error, Exception, None); an empty value resets to the §8 default.
	 */
	UNREALMCPEDITOR_API void SetLogLevel(const FString& InLevel);

	EUnrealMcpConnectionMode GetConnectionMode() const { return Config.ConnectionMode; }
	UNREALMCPEDITOR_API void SetConnectionMode(EUnrealMcpConnectionMode InMode);

	const FString& GetCustomHost() const { return Config.CustomHost; }
	/** Set the Custom-mode server URL; persists + pushes only when it is a valid URL (§7 validated field). */
	UNREALMCPEDITOR_API void SetCustomHost(const FString& InHost);

	EUnrealMcpAuthOption GetAuthOption() const { return Config.AuthOption; }
	UNREALMCPEDITOR_API void SetAuthOption(EUnrealMcpAuthOption InOption);

	// --- Transport selector (§7 — the C++ analog of Unity's SetupAiAgentSection segmented control). ---

	/** The persisted (raw) transport intent. NOT connection-aware — see GetEffectiveTransport for the locked value. */
	EUnrealMcpTransportMethod GetTransportMethod() const { return Config.GetTransportMethod(); }
	/**
	 * The connection-aware transport the UI offers + the snippets reflect: Cloud is LOCKED to Http (the cloud is
	 * HTTP-only); Custom uses the stored choice. This is the value the agent configurators render a single transport
	 * for (mirrors Unity's "Cloud forces streamableHttp" + the agent panel reading the active transport).
	 */
	EUnrealMcpTransportMethod GetEffectiveTransport() const { return Config.ResolveEffectiveTransport(); }
	/** Whether the transport selector is user-editable (only in Custom mode; Cloud locks it to Http). */
	bool IsTransportSelectable() const { return Config.ConnectionMode == EUnrealMcpConnectionMode::Custom; }
	/**
	 * Set the Custom-mode transport (the §7 stdio/http selector). Persists + fires OnConnectionSettingsChanged so
	 * the agent panel re-resolves the now-selected transport's snippet/status. A no-op change does nothing. In Cloud
	 * mode the selector is locked, so a stray set is ignored (the effective transport stays Http). Game-thread only.
	 */
	UNREALMCPEDITOR_API void SetTransportMethod(EUnrealMcpTransportMethod InMethod);

	const FString& GetCustomToken() const { return Config.CustomToken; }
	UNREALMCPEDITOR_API void SetCustomToken(const FString& InToken);
	/** Replace the Custom-mode token with a fresh CSPRNG value (§7 Generate button), then persist + push. */
	UNREALMCPEDITOR_API void GenerateCustomToken();

	// --- Connection control. ---

	EUnrealMcpConnectionState GetConnectionState() const { return ConnectionState; }

	/** §7 Connect: keepConnected=true, optimistic Connecting state, push the config (sidecar (re)dials). */
	UNREALMCPEDITOR_API void Connect();
	/**
	 * §7 Disconnect: the Godot M9b lesson — genuinely STOP the reconnect loop. Sets keepConnected=false
	 * and pushes the config so the sidecar fully tears down the SignalR client (not merely drops one
	 * connection); the UI drops to Disconnected and stays there until the next explicit Connect.
	 */
	UNREALMCPEDITOR_API void Disconnect();
	/** Whether a background reconnect loop is currently armed (keepConnected). False after Disconnect(). */
	bool IsReconnectArmed() const { return Config.bKeepConnected; }

	// --- Cloud device-code auth (§7 item 4 / §1.3). ---

	EUnrealMcpDeviceAuthState GetDeviceAuthState() const { return DeviceAuthState; }
	const FString& GetDeviceVerificationUrl() const { return DeviceVerificationUrl; }
	const FString& GetDeviceUserCode() const { return DeviceUserCode; }
	/** The human-readable failure reason surfaced while the device-code flow is Failed (empty otherwise). */
	const FString& GetDeviceAuthError() const { return DeviceAuthError; }

	/** Authorize: begin the device-code flow (sends `auth-start`; the sidecar drives the real flow). */
	UNREALMCPEDITOR_API void Authorize();
	/** Cancel an in-progress device-code flow (sends `auth-cancel`). */
	UNREALMCPEDITOR_API void CancelAuth();
	/** Revoke + clear the stored cloud token (sends `auth-revoke`, clears CloudToken, persists). */
	UNREALMCPEDITOR_API void Revoke();
	/** Whether a cloud bearer is currently stored (drives the Authorize/Revoke button visibility). */
	bool HasCloudToken() const { return !Config.CloudToken.IsEmpty(); }

	// --- Live status feed (caller marshals these onto the game thread first, §7). ---

	/** Apply a §1.3 `status` message: connectionState / keepConnected / aiAgents. */
	UNREALMCPEDITOR_API void ApplyStatus(const TSharedPtr<FJsonObject>& Status);
	/** Apply a §1.3 `device-auth` message: verificationUrl / userCode / state (+ open browser on first url). */
	UNREALMCPEDITOR_API void ApplyDeviceAuth(const TSharedPtr<FJsonObject>& DeviceAuth);

	/** Connected AI-agent labels surfaced in the §7 AI-agents section (from the last `status`). */
	const TArray<FString>& GetAiAgents() const { return AiAgents; }

	// --- §7 per-tool enable-map (the MCP Tools window). ---

	/** The persisted §8 disabled-tool blocklist (names the user turned off; empty = all enabled). */
	const TArray<FString>& GetDisabledTools() const { return Config.DisabledTools; }
	/** Whether @p ToolName is currently disabled (present in the §8 disabled-tool blocklist). */
	UNREALMCPEDITOR_API bool IsToolDisabled(const FString& ToolName) const;
	/**
	 * Toggle one tool's enabled state (the §7 MCP Tools window per-tool checkbox). Mutates the §8 disabled-tool
	 * blocklist, persists the config (OnPersistConfig — so the choice survives an editor restart), then fires
	 * OnToolEnablementChanged so the runtime excludes/restores the tool in the served manifest. A no-op change
	 * (already in the requested state) does nothing. Game-thread only.
	 */
	UNREALMCPEDITOR_API void SetToolEnabled(const FString& ToolName, bool bEnabled);

	// --- §7 AI agent configurators panel (persisted dropdown selection). ---

	/** The persisted AI-agent-configurator dropdown selection (e.g. "claude-code"). */
	const FString& GetSelectedAgentId() const { return Config.SelectedAgentId; }
	/**
	 * Persist the AI-agent-configurator dropdown selection (the §7 Agent Configurators panel). Pure presentation
	 * state: persists via OnPersistConfig so the choice survives an editor restart, but does NOT push the §1.3
	 * `config` (the selection has no effect on the live connection). A no-op change does nothing. Game-thread only.
	 */
	UNREALMCPEDITOR_API void SetSelectedAgentId(const FString& InAgentId);

	// --- §7 per-agent skills generation (the Agent Configurators skills toggle, issue #53 Phase C). ---

	/** Whether the user enabled per-agent "auto-generate skills" for @p AgentId (the C++ analog of Unity's IsAutoGenerateSkills). */
	UNREALMCPEDITOR_API bool IsAutoGenerateSkills(const FString& AgentId) const;
	/**
	 * Toggle per-agent "auto-generate skills" for @p AgentId (mirrors Unity's SetAutoGenerateSkills). Mutates the
	 * §8 skill-auto-generate agent set and persists the config (OnPersistConfig — so the choice survives a restart).
	 * Pure UI persistence: like SetSelectedAgentId it deliberately does NOT push the §1.3 `config` (skills generation
	 * has no effect on the live connection). A no-op change does nothing. Does NOT itself write skill files — the
	 * caller (the panel) regenerates after enabling. Game-thread only.
	 */
	UNREALMCPEDITOR_API void SetAutoGenerateSkills(const FString& AgentId, bool bEnabled);

	/** The user-overridable Custom-agent skills folder (project-relative; only the Custom configurator reads it). */
	const FString& GetSkillsPath() const { return Config.SkillsPath; }
	/**
	 * Persist the user-overridable Custom-agent skills folder (mirrors Unity's editable SkillsPath setter). Pure UI
	 * persistence: persists via OnPersistConfig, never pushes the §1.3 `config`. An empty value resets to the default
	 * ".claude/skills" so the Custom agent always keeps a valid skills path. A no-op change does nothing. Game-thread only.
	 */
	UNREALMCPEDITOR_API void SetSkillsPath(const FString& InPath);

	// --- Pure helpers (no instance state; the spec-friendly heart of the view-model). ---

	/**
	 * Validate a Custom-mode server URL (§7 validated field): non-empty, an http:// or https:// scheme,
	 * and a non-empty host after the scheme. Returns true when valid; otherwise false + a short reason in
	 * @p OutError. Pure — no network, no DNS.
	 */
	static UNREALMCPEDITOR_API bool ValidateServerUrl(const FString& Url, FString& OutError);

	/**
	 * The masked display form of a token (§8 — never rendered unmasked by default): empty stays empty,
	 * otherwise a fixed-width dot mask when @p bReveal is false (the length is NOT leaked), or the raw value
	 * when @p bReveal is true. This is the masking contract the specs lock and the form to use for any
	 * read-only token surface. NOTE: the editable Custom-mode token field in SUnrealMcpMainWindow cannot route
	 * through this helper — committing a mask string back would overwrite the real token — so it masks
	 * natively via SEditableTextBox::IsPassword (revealed only while the Reveal button is held). Either path
	 * keeps the raw value off-screen by default and out of every log line (diagnostics use MaskSecret).
	 */
	static UNREALMCPEDITOR_API FString MaskTokenForDisplay(const FString& Token, bool bReveal);

	/** The Connect/Disconnect/Stop button label for a connection state (Unity GetButtonText tri-state). */
	static UNREALMCPEDITOR_API FText GetButtonText(EUnrealMcpConnectionState State);
	/** The "Unreal: <status>" label for a connection state (§7 connection panel). */
	static UNREALMCPEDITOR_API FText GetStatusLabel(EUnrealMcpConnectionState State);
	/** The status-dot colour for a connection state (green Connected / amber transitional / red off). */
	static UNREALMCPEDITOR_API FLinearColor GetStatusColor(EUnrealMcpConnectionState State);

	/** Parse a §1.3 connectionState string ("Connected"/"Connecting"/"Degraded"/…) into the enum. */
	static UNREALMCPEDITOR_API EUnrealMcpConnectionState ParseConnectionState(const FString& Raw, bool bKeepConnected);

private:
	FUnrealMcpConfig Config;

	EUnrealMcpConnectionState ConnectionState = EUnrealMcpConnectionState::Disconnected;
	EUnrealMcpDeviceAuthState DeviceAuthState = EUnrealMcpDeviceAuthState::Idle;
	FString DeviceVerificationUrl;
	FString DeviceUserCode;
	FString DeviceAuthError;
	TArray<FString> AiAgents;

	// Persist (if a sink is wired) then push the §1.3 config to the sidecar (if a sink is wired).
	void PersistAndPush();
};
