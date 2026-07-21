// Connection resolution for the HTTP commands (`run-tool`,
// `run-system-tool`, `status`, `wait-for-ready`).
//
// Precedence mirrors docs/ARCHITECTURE.md §8 (highest wins):
//   explicit override -> process env -> project `.env` -> plugin JSON
//   config -> deterministic localhost port.
//
// The plugin owns the on-disk JSON config under `Saved/Config/UnrealMcp/`
// (FUnrealMcpConfig::DefaultConfigFilePath). It is the ONLY place a Cloud-mode
// project records its connection — Cloud mode writes no `.env`, so without
// reading this file the CLI falls through to the deterministic localhost port
// where no server runs (issue #71). We mirror Unity's CLI, which reads its own
// plugin config in the same precedence slot.

import * as fs from 'fs';
import * as path from 'path';
import { readEnvFile } from './env-file.js';
import { generatePortFromDirectory, isAbsoluteLoopbackHost, resolveLocalBindPort } from './port.js';
import { readProjectMarker } from './project-marker.js';

export interface ResolvedConnection {
  /** Base URL with no trailing slash. */
  url: string;
  /** Bearer token, when one is configured. */
  token: string | undefined;
  /** Where the URL came from — for diagnostics / `status`. */
  source: 'override' | 'process-env' | 'env-file' | 'plugin-config' | 'deterministic-port';
}

export interface ResolveConnectionOptions {
  /** Absolute project dir — used for `.env` read + deterministic port. */
  projectDir?: string;
  /** Explicit URL override (wins over everything). */
  url?: string;
  /** Explicit token override (wins over everything). */
  token?: string;
  /** Injectable env source (defaults to `process.env`). */
  processEnv?: NodeJS.ProcessEnv;
}

/**
 * Resolve the MCP server URL + token to use for a project. Pure given an
 * injected `processEnv`; reads the project `.env` via the filesystem.
 *
 * Throws only when neither a `projectDir` nor a `url` override is supplied
 * — every other path degrades to the deterministic localhost port.
 */
export function resolveConnection(opts: ResolveConnectionOptions): ResolvedConnection {
  const env = opts.processEnv ?? process.env;
  const envToken = nonEmpty(env['UNREAL_MCP_TOKEN']);

  if (opts.url && opts.url.trim().length > 0) {
    return { url: stripTrailingSlash(opts.url), token: opts.token ?? envToken, source: 'override' };
  }

  if (!opts.projectDir) {
    throw new Error('Either url or projectDir must be provided to resolve a connection.');
  }
  const projectDir = path.resolve(opts.projectDir);

  const fileEnv = readEnvFile(path.join(projectDir, '.env'));
  const fileToken = nonEmpty(fileEnv['UNREAL_MCP_TOKEN']);

  // Token precedence is layered independently of which layer supplied the
  // URL (§8: explicit override -> process env -> project `.env`). Resolving
  // it once here avoids the asymmetry of, e.g., a process-env HOST ignoring a
  // `.env` token or a `.env` HOST ignoring a process-env token.
  const token = opts.token ?? envToken ?? fileToken;

  // Process env beats the file (live override, never persisted — §8).
  const envHost = nonEmpty(env['UNREAL_MCP_HOST']);
  if (envHost) {
    return { url: stripTrailingSlash(envHost), token, source: 'process-env' };
  }

  const fileHost = nonEmpty(fileEnv['UNREAL_MCP_HOST']);
  if (fileHost) {
    return { url: stripTrailingSlash(fileHost), token, source: 'env-file' };
  }

  // Plugin JSON config — the only record of a Cloud-mode connection (issue #71).
  // The `.env` token layer still wins over the config's token (it sits higher in
  // §8 precedence); the config supplies a token only when no `.env`/process-env
  // token is present.
  const pluginConfig = readPluginConfig(projectDir);
  if (pluginConfig) {
    const fromConfig = resolveConnectionFromPluginConfig(pluginConfig);
    if (fromConfig.url) {
      return {
        url: resolveCustomHostUrl(fromConfig.url, projectDir),
        token: token ?? fromConfig.token,
        source: 'plugin-config',
      };
    }
  }

  // No host anywhere → deterministic localhost fallback, still honoring a marker
  // portOverride (level 1) over the v1 derivation (level 3); there is no host so the
  // typed-host level 2 is N/A. Mirrors the binder's default-host resolution.
  const marker = readProjectMarker(projectDir);
  const { port } = resolveLocalBindPort({ projectDir, markerPortOverride: marker?.portOverride });
  return { url: `http://localhost:${port}`, token, source: 'deterministic-port' };
}

/** Trim a value and return `undefined` when it is absent or blank. */
function nonEmpty(value: string | undefined): string | undefined {
  if (value === undefined) return undefined;
  const trimmed = value.trim();
  return trimmed.length > 0 ? trimmed : undefined;
}

export function stripTrailingSlash(url: string): string {
  return url.trim().replace(/\/+$/, '');
}

/**
 * Resolve the effective connection URL for a Custom-mode plugin-config `host` (defect D
 * / D21). A LOOPBACK host is pointed at the port the sidecar actually binds via the
 * three-level precedence (marker `portOverride` → typed loopback port → v1 derivation),
 * so a port-less `http://localhost` (or a legacy default `:8080`) no longer connects to
 * the wrong port. A NON-loopback host (a LAN IP / remote target) is used verbatim — the
 * local `gamedev-mcp-server` binds no such authority, mirroring the bridge binder's
 * deliberate levels-1+3-only divergence for non-loopback hosts. Pure given the marker on
 * disk.
 */
function resolveCustomHostUrl(host: string, projectDir: string): string {
  const stripped = stripTrailingSlash(host);
  if (!isAbsoluteLoopbackHost(stripped)) return stripped;

  const marker = readProjectMarker(projectDir);
  const { port } = resolveLocalBindPort({ projectDir, host: stripped, markerPortOverride: marker?.portOverride });

  // Rebuild <scheme>://<host>:<port>, preserving any path/query the host carried. Building
  // the authority by hand (rather than URL.port =) keeps the resolved port explicit even
  // when it equals a scheme default that WHATWG would otherwise elide.
  const url = new URL(stripped);
  const authoritativePath = url.pathname === '/' ? '' : url.pathname;
  return stripTrailingSlash(`${url.protocol}//${url.hostname}:${port}${authoritativePath}${url.search}`);
}

// --- Plugin on-disk JSON config (issue #71) -------------------------------
//
// Path + field names mirror the plugin's FUnrealMcpConfig
// (UnrealMCP/Source/UnrealMcpEditor/Private/Config/UnrealMcpConfig.cpp): the
// file lives at `<project>/Saved/Config/UnrealMcp/ai-game-developer-config.json`
// and uses camelCase keys. We read only the connection-relevant subset.

/** Project-relative path to the plugin's persisted JSON config. */
export const PLUGIN_CONFIG_RELATIVE_PATH = path.join(
  'Saved',
  'Config',
  'UnrealMcp',
  'ai-game-developer-config.json',
);

/** The connection-relevant subset of the plugin's JSON config. */
export interface PluginConnectionConfig {
  /** "Cloud" | "Custom" (case-insensitive; anything else is treated as Custom). */
  connectionMode?: string;
  /** Cloud base URL (e.g. `https://ai-game.dev`); the `/mcp` prefix is appended. */
  cloudUrl?: string;
  /** Bearer token for Cloud mode. */
  cloudToken?: string;
  /** Custom-mode host (full base URL). */
  host?: string;
  /** Bearer token for Custom mode (only on the wire when authOption resolves to Token). */
  token?: string;
  /** "None" | "Token" | "Oauth" (legacy "Required" → Token) — Custom-mode auth gate for `token`. */
  authOption?: string;
}

/**
 * Read `<projectDir>/Saved/Config/UnrealMcp/ai-game-developer-config.json`.
 * Returns `undefined` when the file is absent, unreadable, or not valid JSON —
 * never throws (library-safe). Tolerates a UTF-8 BOM.
 */
export function readPluginConfig(projectDir: string): PluginConnectionConfig | undefined {
  const configPath = path.join(path.resolve(projectDir), PLUGIN_CONFIG_RELATIVE_PATH);
  let raw: string;
  try {
    if (!fs.existsSync(configPath)) return undefined;
    raw = fs.readFileSync(configPath, 'utf-8');
  } catch {
    return undefined;
  }
  // Strip a leading UTF-8 BOM (FFileHelper::SaveStringToFile may emit one).
  if (raw.charCodeAt(0) === 0xfeff) raw = raw.slice(1);
  let parsed: unknown;
  try {
    parsed = JSON.parse(raw);
  } catch {
    return undefined;
  }
  if (parsed === null || typeof parsed !== 'object') return undefined;
  const obj = parsed as Record<string, unknown>;
  return {
    connectionMode: asString(obj.connectionMode),
    cloudUrl: asString(obj.cloudUrl),
    cloudToken: asString(obj.cloudToken),
    host: asString(obj.host),
    token: asString(obj.token),
    authOption: asString(obj.authOption),
  };
}

/** True when the config selects Cloud mode (case-insensitive). */
export function isCloudMode(config: PluginConnectionConfig): boolean {
  return (config.connectionMode ?? '').trim().toLowerCase() === 'cloud';
}

/**
 * Resolve a `{ url, token }` from a plugin config, mirroring the plugin's
 * routing (FUnrealMcpConfig):
 *   - Cloud  -> url = appendMcp(cloudUrl), token = cloudToken
 *   - Custom -> url = host,                token = (authOption resolves to Token ? token : undefined)
 * `url` may be undefined when the relevant field is blank. The Custom `url` here is the RAW
 * host; loopback port resolution (defect D) happens in `resolveConnection`, which has the
 * project dir + marker to apply the three-level bind-port precedence.
 */
export function resolveConnectionFromPluginConfig(config: PluginConnectionConfig): {
  url: string | undefined;
  token: string | undefined;
} {
  if (isCloudMode(config)) {
    const cloudUrl = nonEmpty(config.cloudUrl);
    return {
      url: cloudUrl ? appendMcp(cloudUrl) : undefined,
      token: nonEmpty(config.cloudToken),
    };
  }
  // Custom mode: the token only goes on the wire in `Token` auth mode — `None`
  // (anonymous/loopback) and `Oauth` (native OAuth, no static bearer) send NOTHING,
  // regardless of any stored token (mirrors FUnrealMcpConfig::ResolveEffectiveToken).
  return {
    url: nonEmpty(config.host),
    token: parseCustomAuthMode(config.authOption) === 'token' ? nonEmpty(config.token) : undefined,
  };
}

/** The Custom-mode auth modes the plugin persists (PascalCase on disk, matched case-insensitively). */
export type CustomAuthMode = 'none' | 'token' | 'oauth';

/**
 * Map a persisted `authOption` string to the Custom-mode auth mode, mirroring the plugin's
 * `FUnrealMcpConfig::TryParseAuthOption` (`UnrealMcpConfig.cpp:625-651`): `None`/`Token`/`Oauth`
 * plus the g5 migration `Required` → `Token` (the retired shared-secret gate). Anything absent
 * or unrecognized resolves to `none` — so no bearer is ever sent for a config the plugin did not
 * mark as Token. Replaces the retired `authOption === 'required'` gate (defect C). Pure.
 */
export function parseCustomAuthMode(raw: string | undefined): CustomAuthMode {
  const v = (raw ?? '').trim().toLowerCase();
  if (v === 'token' || v === 'required') return 'token'; // 'required' is the legacy alias for Token (g5 migration)
  if (v === 'oauth') return 'oauth';
  return 'none';
}

/**
 * Idempotently ensure a URL ends with exactly one `/mcp` segment: strip a
 * trailing `/mcp` (case-insensitive, trailing-slash tolerant) then re-append
 * `/mcp`. `https://ai-game.dev` -> `https://ai-game.dev/mcp`;
 * `https://ai-game.dev/mcp/` -> `https://ai-game.dev/mcp` (never `/mcp/mcp`).
 */
export function appendMcp(url: string): string {
  const base = stripTrailingSlash(url).replace(/\/mcp$/i, '');
  return `${base}/mcp`;
}

/** Coerce an unknown JSON field to a trimmed non-empty string, else undefined. */
function asString(value: unknown): string | undefined {
  return typeof value === 'string' ? nonEmpty(value) : undefined;
}
