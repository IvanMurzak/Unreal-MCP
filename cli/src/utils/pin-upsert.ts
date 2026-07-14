// Upsert the D14 routing pin (`/p/<pin>`) into any EXISTING project-local agent
// config entry that points at the enrolled ai-game.dev MCP server (mcp-authorize
// design 06 — "the CLI … upserts the D14 pin into any existing project-local
// agent config entry, so a hosted server added manually in step 1 of workflow 1A
// becomes pinned, and the plugin boots pointed at the right hub").
//
// The classic 1A case: the user ran `claude mcp add --transport http
// ai-game-developer https://ai-game.dev/mcp` (writing a project-local `.mcp.json`)
// BEFORE enrolling. `install-plugin --enroll <code>` then rewrites that server's
// URL to carry the `/p/<pin>` path segment so agent sessions launched in this
// project folder route strictly to this project's engine (D14 strict pin routing).
//
// Scope (design 06, "Project-scoped configs only, D14"): only **project-local**
// config files (those whose path is under the project root) are touched — the
// golden path never rewrites a user-global config. The rewrite is:
//   • format-preserving (a targeted string substitution on the URL token, never a
//     whole-file reparse/reformat), so it works for JSON and TOML configs alike;
//   • host-scoped (only URLs whose origin matches the enrolled `serverTarget` —
//     or, when no target is known, ai-game.dev + loopback hosts — are rewritten);
//   • idempotent (an already-pinned URL's stale `/p/<hex8>` segment is stripped
//     and re-appended, so re-running never produces `/p/x/p/y`).
//
// Library-safe: never throws past the boundary; an unreadable/garbage config is
// skipped, not fatal.

import * as fs from 'fs';
import * as path from 'path';
import { agentRegistry } from './agents.js';

/** Matches a trailing `/p/<8-hex>` routing-pin segment (trailing slash tolerated). */
const TRAILING_PIN_RE = /\/p\/[0-9a-fA-F]{8}\/?$/;

/**
 * URL/serverUrl key → quoted value, for BOTH JSON (`"url": "…"`) and TOML
 * (`url = "…"`). The optional `\1` backreference makes the key quotes match
 * (present in JSON, absent in TOML); the value token excludes quotes/whitespace
 * so it captures exactly one URL.
 */
const URL_FIELD_RE = /(["']?)(url|serverUrl)\1\s*[:=]\s*(["'])([^"'\s]+)\3/g;

export interface UpsertPinOptions {
  /**
   * The enrolled server target URL. Only config URLs whose ORIGIN matches this
   * target's origin are pinned. When omitted, ai-game.dev + loopback hosts match
   * (a defensive default; the enroll flow always supplies the redeemed target).
   */
  serverTarget?: string;
  /**
   * Explicit list of config file paths to scan. Defaults to the PROJECT-LOCAL
   * subset of the agent registry's config paths (those under the project root).
   * Test injection.
   */
  configPaths?: string[];
}

export interface UpsertPinResult {
  /** Absolute paths of the config files whose URL was rewritten this run. */
  updatedFiles: string[];
}

/**
 * The pinned form of `rawUrl`: any existing trailing `/p/<8hex>` segment stripped,
 * then `/p/<pin>` appended to the path. Preserves scheme/host/port and any prefix
 * path (e.g. hosted `…/mcp` stays, local origin gets a bare `/p/<pin>`). Pure.
 * A non-absolute / unparsable URL is returned unchanged.
 */
export function pinnedUrl(rawUrl: string, pin: string): string {
  let u: URL;
  try {
    u = new URL(rawUrl);
  } catch {
    return rawUrl;
  }
  let pathname = u.pathname.replace(TRAILING_PIN_RE, '').replace(/\/+$/, '');
  u.pathname = `${pathname}/p/${pin}`;
  // Drop a trailing slash `URL` may re-add for an empty search/hash.
  return u.toString().replace(/\/$/, '');
}

/**
 * True when `rawUrl`'s origin should be pinned for this enrollment. With a
 * `serverTarget`, the origins (scheme+host+port, case-insensitive) must match.
 * Without one, ai-game.dev (or a subdomain) and loopback hosts match. Pure.
 */
export function originMatches(rawUrl: string, serverTarget?: string): boolean {
  let u: URL;
  try {
    u = new URL(rawUrl);
  } catch {
    return false;
  }
  if (serverTarget && serverTarget.trim().length > 0) {
    try {
      return u.origin.toLowerCase() === new URL(serverTarget).origin.toLowerCase();
    } catch {
      return false;
    }
  }
  const host = u.hostname.toLowerCase();
  return (
    host === 'ai-game.dev' ||
    host.endsWith('.ai-game.dev') ||
    host === 'localhost' ||
    host === '127.0.0.1' ||
    host === '[::1]'
  );
}

/**
 * Rewrite matching `url`/`serverUrl` string values in `text` to carry the pin,
 * returning the new text plus whether anything changed. Format-preserving:
 * substitutes only the URL token, leaving surrounding whitespace/formatting
 * intact. Pure.
 */
export function applyPinToConfigText(
  text: string,
  pin: string,
  serverTarget?: string,
): { text: string; changed: boolean } {
  let changed = false;
  const out = text.replace(URL_FIELD_RE, (full, _q1, _key, quote, url) => {
    if (!originMatches(url, serverTarget)) return full;
    const pinned = pinnedUrl(url, pin);
    if (pinned === url) return full;
    changed = true;
    return full.replace(`${quote}${url}${quote}`, `${quote}${pinned}${quote}`);
  });
  return { text: out, changed };
}

/**
 * The default set of config files the enroll pin-upsert scans: every agent
 * registry config path that resolves UNDER `projectDir` (project-local). Global
 * / home-dir configs (Claude Desktop, Antigravity, Cline, Copilot CLI) are
 * excluded by construction. Pure-ish (no I/O). De-duplicated.
 */
export function projectLocalAgentConfigPaths(projectDir: string): string[] {
  const root = path.resolve(projectDir);
  const withSep = root.endsWith(path.sep) ? root : root + path.sep;
  const seen = new Set<string>();
  for (const agent of agentRegistry) {
    let configPath: string;
    try {
      configPath = path.resolve(agent.getConfigPath(root));
    } catch {
      continue;
    }
    if (configPath === root || configPath.startsWith(withSep)) {
      seen.add(configPath);
    }
  }
  return [...seen];
}

/**
 * Upsert the `/p/<pin>` routing segment into every existing project-local agent
 * config whose ai-game.dev/target-origin server URL is not already pinned to
 * `pin`. Returns the files that changed. Never throws — an unreadable/garbage
 * config is skipped.
 */
export function upsertProjectPin(projectDir: string, pin: string, opts: UpsertPinOptions = {}): UpsertPinResult {
  const updatedFiles: string[] = [];
  const paths = opts.configPaths ?? projectLocalAgentConfigPaths(projectDir);
  for (const configPath of paths) {
    let text: string;
    try {
      if (!fs.existsSync(configPath)) continue;
      text = fs.readFileSync(configPath, 'utf-8');
    } catch {
      continue;
    }
    const { text: next, changed } = applyPinToConfigText(text, pin, opts.serverTarget);
    if (!changed) continue;
    try {
      fs.writeFileSync(configPath, next);
      updatedFiles.push(configPath);
    } catch {
      // Best-effort — a read-only config must not fail the enrollment.
    }
  }
  return { updatedFiles };
}
