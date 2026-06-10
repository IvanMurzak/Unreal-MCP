// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/** Which MCP server the plugin connects to (docs/ARCHITECTURE.md §8). Mirrors Godot/Unity. */
enum class EUnrealMcpConnectionMode : uint8
{
	/** A user-supplied server URL (local dev server, self-hosted, …). */
	Custom,
	/** The hosted ai-game.dev cloud server. */
	Cloud
};

/**
 * Whether a Custom-mode connection sends a bearer token (§8). Only meaningful in
 * EUnrealMcpConnectionMode::Custom — Cloud-mode auth is the device-code flow (a later UI task).
 */
enum class EUnrealMcpAuthOption : uint8
{
	/** No bearer token is sent (the Custom server accepts anonymous connections). */
	None,
	/** A bearer token is sent (the Custom server requires authorization). */
	Required
};

/**
 * Connection + environment configuration for the UnrealMCP plugin (docs/ARCHITECTURE.md §8). The C++
 * analog of Godot's GodotMcpConfig + GodotMcpEnvFile + Unity's EnvironmentUtils.OverrideRecord:
 *
 *  - Persisted on disk as camelCase JSON at <Project>/Saved/Config/UnrealMcp/ai-game-developer-config.json
 *    (Saved/ is gitignored by every UE template, so tokens never land in VCS by default).
 *  - Layered with a project-root <Project>/.env file (parsed with EXACTLY GodotMcpEnvFile's rules) and the
 *    process environment, with precedence — highest wins — process env > .env > config file > defaults.
 *  - Env/.env overrides are NEVER persisted back: the Unity OverrideRecord baseline-restore pattern is
 *    carried over so Save() round-trips the on-disk baseline while the in-memory config keeps the override.
 *  - Tokens are NEVER logged at any level (see MaskSecret); only their presence may surface.
 *
 * The effective connection config reaches the sidecar EXCLUSIVELY via the §1.3 `config` IPC message
 * (and the handshake-ack), built by BuildEffectiveConnectionConfig() — the plugin resolves Cloud/Custom
 * host, token and mode here; the sidecar never re-resolves it (§1.5).
 *
 * The parsing / precedence / persistence surface is pure (CoreMinimal + Json only, env/file readers are
 * injectable) so it is fully drivable from the UnrealMcpEditorTests Automation specs with no real process
 * env or files. All symbols are UNREALMCPEDITOR_API-exported so the Tests module links against them.
 */
class FUnrealMcpConfig
{
public:
	// --- Recognized environment-variable names (§8). ---
	static UNREALMCPEDITOR_API const TCHAR* EnvConnectionMode; // UNREAL_MCP_CONNECTION_MODE
	static UNREALMCPEDITOR_API const TCHAR* EnvHost;           // UNREAL_MCP_HOST
	static UNREALMCPEDITOR_API const TCHAR* EnvCloudUrl;       // UNREAL_MCP_CLOUD_URL
	static UNREALMCPEDITOR_API const TCHAR* EnvToken;          // UNREAL_MCP_TOKEN
	static UNREALMCPEDITOR_API const TCHAR* EnvAuthOption;     // UNREAL_MCP_AUTH_OPTION
	static UNREALMCPEDITOR_API const TCHAR* EnvKeepConnected;  // UNREAL_MCP_KEEP_CONNECTED
	static UNREALMCPEDITOR_API const TCHAR* EnvTools;          // UNREAL_MCP_TOOLS
	static UNREALMCPEDITOR_API const TCHAR* EnvStartServer;    // UNREAL_MCP_START_SERVER
	static UNREALMCPEDITOR_API const TCHAR* EnvTransport;      // UNREAL_MCP_TRANSPORT
	static UNREALMCPEDITOR_API const TCHAR* EnvLogLevel;       // UNREAL_MCP_LOG_LEVEL
	static UNREALMCPEDITOR_API const TCHAR* EnvBridgePath;     // UNREAL_MCP_BRIDGE_PATH (dev-only, §6)

	// --- Defaults. ---
	static UNREALMCPEDITOR_API const TCHAR* DefaultCloudBaseUrl; // https://ai-game.dev
	static UNREALMCPEDITOR_API const TCHAR* DefaultCustomHost;   // http://localhost:8080
	static UNREALMCPEDITOR_API const TCHAR* DefaultLogLevel;     // Info
	static UNREALMCPEDITOR_API const TCHAR* DefaultTransport;    // http

	// --- Live (post-override) backing fields, serialized to the camelCase JSON keys in the comments. ---
	EUnrealMcpConnectionMode ConnectionMode = EUnrealMcpConnectionMode::Cloud; // "connectionMode"
	FString CustomHost;        // "host"
	FString CustomToken;       // "token"
	FString CloudToken;        // "cloudToken"
	FString CloudUrl;          // "cloudUrl"  (cloud base URL override)
	EUnrealMcpAuthOption AuthOption = EUnrealMcpAuthOption::None; // "authOption"
	bool bKeepConnected = true;                                  // "keepConnected"
	FString LogLevel;          // "logLevel"
	FString Transport;         // "transport"
	bool bStartServer = false; // "startServer"
	TArray<FString> EnabledTools; // "enabledTools" (UNREAL_MCP_TOOLS override; empty = no override)

	UNREALMCPEDITOR_API FUnrealMcpConfig();

	// --- Standard on-disk locations (production paths; resolved from the live project). ---
	/** <Project>/Saved/Config/UnrealMcp/ai-game-developer-config.json (§8). */
	static UNREALMCPEDITOR_API FString DefaultConfigFilePath();
	/** <Project>/.env (§8). */
	static UNREALMCPEDITOR_API FString DefaultEnvFilePath();

	/**
	 * Production convenience: load the on-disk config (if any), parse <Project>/.env, then apply env/.env
	 * overrides with full precedence, and return the resolved config. Reads real process env + files.
	 */
	static UNREALMCPEDITOR_API FUnrealMcpConfig LoadAndResolve();

	/**
	 * Same as LoadAndResolve() but with an already-parsed @p DotEnv map, so a caller that needs the parsed
	 * .env for another purpose (e.g. ExportDotEnvToProcessEnv) does not parse the file twice.
	 */
	static UNREALMCPEDITOR_API FUnrealMcpConfig LoadAndResolve(const TMap<FString, FString>& DotEnv);

	/**
	 * Load the persisted config JSON at @p Path into the backing fields, and snapshot the result as the
	 * disk baseline (used by Save() to round-trip env/.env overrides away). A missing/empty/corrupt file
	 * is the common first-run case — fields keep their defaults and the baseline is the default snapshot.
	 * Never throws.
	 */
	UNREALMCPEDITOR_API void LoadFromFile(const FString& Path);

	/** Set the disk baseline directly from a parsed JSON object (or defaults when null). LoadFromFile delegates here; also the injectable seam for tests. */
	UNREALMCPEDITOR_API void LoadFromJson(const TSharedPtr<FJsonObject>& Json);

	/**
	 * Apply the .env values (lower precedence) then the process env (higher precedence) on top of the
	 * current fields, recording which keys were overridden so Save() can restore the baseline. @p EnvReader
	 * resolves a process-env var (returns false when unset); inject a stub in tests. @p DotEnv is the parsed
	 * .env map (see ParseEnvLines / LoadEnvFile); pass an empty map to disable the file layer.
	 */
	UNREALMCPEDITOR_API void ApplyOverrides(const TMap<FString, FString>& DotEnv, const TFunction<bool(const FString&, FString&)>& EnvReader);

	/** Serialize the CURRENT (post-override) field values to a camelCase JSON object. */
	UNREALMCPEDITOR_API TSharedPtr<FJsonObject> ToJson() const;

	/**
	 * Serialize to @p Path, restoring the disk baseline for every env/.env-overridden key first so an
	 * override is never persisted (Unity OverrideRecord pattern). The in-memory fields are unchanged.
	 * Creates the parent directory. Returns false on a genuine write failure.
	 */
	UNREALMCPEDITOR_API bool Save(const FString& Path) const;

	/**
	 * Build the effective connection config the plugin pushes to the sidecar (§1.3 `config` / handshake-ack):
	 * resolves mode → host/cloudUrl, and mode+auth → token. Keys: mode, host, cloudUrl, token, keepConnected.
	 * The token is the resolved bearer (empty in Custom+None) — the sidecar sends it verbatim, never re-resolves.
	 */
	UNREALMCPEDITOR_API TSharedPtr<FJsonObject> BuildEffectiveConnectionConfig() const;

	/** The resolved cloud base URL (CloudUrl override or DefaultCloudBaseUrl), trailing slash trimmed. */
	UNREALMCPEDITOR_API FString ResolveCloudBaseUrl() const;
	/** The resolved Custom-mode host (CustomHost or DefaultCustomHost), trailing slash trimmed. */
	UNREALMCPEDITOR_API FString ResolveCustomHost() const;
	/** The mode+auth-resolved bearer token: Cloud→CloudToken, Custom→(None?empty:CustomToken). */
	UNREALMCPEDITOR_API FString ResolveEffectiveToken() const;

	/** Whether ApplyOverrides recorded any env/.env override (for diagnostics/tests). */
	bool HasOverrides() const { return OverriddenKeys.Num() > 0; }
	bool IsOverridden(const FString& JsonKey) const { return OverriddenKeys.Contains(JsonKey); }

	// --- Pure .env parsing (EXACTLY GodotMcpEnvFile's rules, §8). ---

	/**
	 * Parse .env-style lines into the recognized UNREAL_MCP_* values: skip blank lines and `#` comments,
	 * split on the FIRST `=`, trim the key, keep only recognized keys (case-sensitive), sanitize the value
	 * (trim whitespace + one pair of wrapping single OR double quotes), skip a blank value, last occurrence
	 * wins. The returned map is keyed by the full UNREAL_MCP_* name.
	 */
	static UNREALMCPEDITOR_API TMap<FString, FString> ParseEnvLines(const TArray<FString>& Lines);

	/** Read + parse the .env file at @p Path. A missing/unreadable file returns an empty map (never throws). */
	static UNREALMCPEDITOR_API TMap<FString, FString> LoadEnvFile(const FString& Path);

	/**
	 * Export the UNREAL_MCP_BRIDGE_PATH .env value into the editor's process environment (and ONLY that key),
	 * set-if-absent so the §8 precedence "process env > .env" is preserved. That is the single var the editor
	 * process itself must read out-of-band (FUnrealMcpSidecarManager resolves the sidecar binary from it, §6),
	 * and it is not a secret. HOST/CLOUD_URL/TOKEN are intentionally NOT exported: the sidecar gets them via
	 * the authoritative §1.3 `config` push, so process-env export would only leak the bearer token into every
	 * editor child and pin stale .env values at process-env precedence on a later re-resolve.
	 */
	static UNREALMCPEDITOR_API void ExportDotEnvToProcessEnv(const TMap<FString, FString>& DotEnv);

	/**
	 * Mask a secret for logging: "<unset>" when empty, otherwise "***" (the value never appears). Use this
	 * EVERYWHERE a token could otherwise reach a log line (§8 — tokens never logged at any level).
	 */
	static UNREALMCPEDITOR_API FString MaskSecret(const FString& Secret);

	/** Sanitize a value identically to a process-env value: trim whitespace + one wrapping quote pair. */
	static UNREALMCPEDITOR_API FString SanitizeValue(const FString& Raw);

private:
	// The disk-baseline JSON snapshot taken at LoadFromFile/LoadFromJson time — the values Save() persists
	// for any key an env/.env layer overrode. Keys not overridden persist their live (current) value.
	TSharedPtr<FJsonObject> DiskBaselineJson;
	// JSON keys that an env/.env layer overrode (so Save() restores their baseline).
	TSet<FString> OverriddenKeys;

	// Apply a single resolved value (from .env or process env) onto the matching field, recording the
	// override against its JSON key. @p Source is the effective override value (already sanitized).
	void ApplyOne(const FString& EnvName, const FString& Source);

	static bool TryParseMode(const FString& Raw, EUnrealMcpConnectionMode& OutMode);
	static bool TryParseAuthOption(const FString& Raw, EUnrealMcpAuthOption& OutOption);
	static bool TryParseBool(const FString& Raw, bool& OutValue);
};
