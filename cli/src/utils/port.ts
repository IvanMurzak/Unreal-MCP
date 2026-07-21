import { createHash } from 'crypto';

const MIN_PORT = 20000;
const MAX_PORT = 29999;
const PORT_RANGE = MAX_PORT - MIN_PORT + 1;

/**
 * Characters where JS `String.prototype.toLowerCase()` diverges from C#
 * `string.ToLowerInvariant()`. The shared `ProjectIdentity` reference hashes
 * `ToLowerInvariant()` output, so a naive TS `toLowerCase()` port would compute
 * a DIFFERENT hash for these code points and break cross-language parity. The
 * golden vectors (`ProjectIdentity.GoldenVectors.json` § unicodeDivergence) pin
 * the one that matters for path hashing: U+0130 (İ) — C# leaves it unchanged;
 * JS expands it to `i` + U+0307 (COMBINING DOT ABOVE). Extend this map if a new
 * divergence surfaces in the vectors.
 */
const TO_LOWER_INVARIANT_OVERRIDES: Readonly<Record<string, string>> = {
  'İ': 'İ', // LATIN CAPITAL LETTER I WITH DOT ABOVE — unchanged under ToLowerInvariant
};

/**
 * Lowercase a string the way C# `string.ToLowerInvariant()` does. Iterating by
 * code point (not code unit) and lowercasing each independently reproduces the
 * invariant SIMPLE (1:1) case mapping — closer to `ToLowerInvariant` than a
 * whole-string `toLowerCase()`, which applies context-sensitive / expanding full
 * case folding — with the documented divergent code points overridden. Pure.
 */
function toLowerInvariant(s: string): string {
  let out = '';
  for (const ch of s) {
    const override = TO_LOWER_INVARIANT_OVERRIDES[ch];
    out += override !== undefined ? override : ch.toLowerCase();
  }
  return out;
}

/**
 * Trim trailing directory separators (`/` and `\`) so `/a/b` and `/a/b/` are the
 * same project. Keeps at least one character (mirrors the C# reference, which
 * loops while `end > 1`). Separators are NEVER converted — `C:\a` and `C:/a`
 * intentionally hash differently. Pure.
 */
function trimTrailingSeparators(p: string): string {
  let end = p.length;
  while (end > 1 && (p[end - 1] === '/' || p[end - 1] === '\\')) end--;
  return end === p.length ? p : p.slice(0, end);
}

/**
 * The exact string that gets UTF-8/SHA-256 hashed: the project root with trailing
 * separators trimmed, then `ToLowerInvariant`-lowercased. Exposed so tests and
 * callers can reproduce the pre-hash string. Pure.
 */
export function normalizeProjectRoot(dir: string): string {
  return toLowerInvariant(trimTrailingSeparators(dir));
}

/**
 * Generate a deterministic localhost port from a project directory.
 *
 * The single canonical `ProjectIdentity` derivation shared with the .NET/Unity/
 * Godot plugins (`McpPlugin/src/AgentConfig/ProjectIdentity.cs`), gated
 * byte-for-byte by the committed golden vectors:
 *   1. Trim trailing separators, then `ToLowerInvariant` the project root.
 *   2. UTF-8 encode, then SHA-256 hash.
 *   3. port = 20000 + (littleEndianUInt32(first 4 bytes) % 10000). Range 20000–29999.
 *
 * No probing, no I/O — so the CLI reaches a project's local MCP server without
 * reading any config, matching the port the plugin derives for the same path.
 * Pure.
 */
export function generatePortFromDirectory(dir: string): number {
  const hash = createHash('sha256').update(normalizeProjectRoot(dir), 'utf-8').digest();
  const uint32 = hash.readUInt32LE(0);
  return MIN_PORT + (uint32 % PORT_RANGE);
}

/** Number of hex characters in the routing pin (first 4 bytes of the hash). */
export const PROJECT_PIN_LENGTH = 8;

/**
 * The D14 routing **pin** for a project directory: the first 4 bytes of the
 * SHA-256 of the normalized project root, rendered as 8 lowercase hex chars.
 * The pin is NEVER affected by a port override — it is purely hash-derived.
 *
 * Byte-for-byte identical to the shared `ProjectIdentity.DerivePin`
 * (`McpPlugin/src/AgentConfig/ProjectIdentity.cs`, `ToHex(hash, 4)`), gated by
 * the same golden vectors that pin `generatePortFromDirectory`, so the pin the
 * CLI writes into an agent config's `/p/<pin>` URL segment routes to the SAME
 * project's engine the plugin reports in its hub instance-metadata handshake.
 * Pure.
 */
export function deriveProjectPin(dir: string): string {
  const hash = createHash('sha256').update(normalizeProjectRoot(dir), 'utf-8').digest();
  return hash.subarray(0, PROJECT_PIN_LENGTH / 2).toString('hex');
}

// --- Local-server bind-port precedence (defect D / D21) -------------------
//
// The sidecar resolves the port its local `gamedev-mcp-server` BINDS with a
// three-level precedence (bridge `ProjectConnectionResolver.Resolve`, owner
// ruling 2026-07-19). The CLI must resolve the SAME port so a `run-tool` /
// `status` reaches the server the plugin actually bound — instead of taking a
// Custom-mode host verbatim (port-less `http://localhost` → :80, legacy
// `:8080`), which named a port the sidecar does not listen on. Highest wins:
//   1. the committable project marker's `portOverride` (a deliberate pin);
//   2. an explicit port the user typed into the Custom-mode LOOPBACK host;
//   3. the deterministic `ProjectIdentity` v1 derivation (generatePortFromDirectory).
// Mirror of `bridge/src/Host/ProjectConnectionResolver.cs:100-139`.

/** The inclusive maximum TCP port — mirrors McpPlugin `Consts.Hub.MaxPort`. */
export const MAX_TCP_PORT = 65535;

/** Characters that terminate a URL's authority component (path / query / fragment). */
const AUTHORITY_TERMINATORS = new Set(['/', '?', '#']);

/** Which precedence level supplied the resolved local bind port (diagnostic). */
export type LocalServerPortSource = 'marker-override' | 'typed-host' | 'derived';

export interface LocalBindPortResolution {
  /** The resolved local-server bind port. */
  port: number;
  /** Which precedence level decided it. */
  source: LocalServerPortSource;
}

/**
 * True when `host` parses as an ABSOLUTE LOOPBACK URL (`localhost`, `127.0.0.0/8`,
 * or `::1`). Mirrors the bridge binder's `Uri.TryCreate(Absolute) && uri.IsLoopback`
 * gate: a bare `localhost:27618` (no `//` authority) or a non-loopback host is not a
 * match, so level 2 is skipped and the derived port is used. Pure; never throws.
 */
export function isAbsoluteLoopbackHost(host: string | undefined): boolean {
  if (host == null) return false;
  let url: URL;
  try {
    url = new URL(host);
  } catch {
    return false; // not an absolute URL (or an invalid port that WHATWG rejects — e.g. :abc / :70000)
  }
  const hostname = url.hostname.toLowerCase();
  if (hostname.length === 0) return false; // e.g. `localhost:27618` parses as scheme `localhost:` with no authority
  if (hostname === 'localhost') return true;
  if (hostname === '::1' || hostname === '[::1]') return true; // IPv6 loopback (Node keeps brackets on hostname)
  return /^127(?:\.\d{1,3}){3}$/.test(hostname); // IPv4 loopback 127.0.0.0/8
}

/**
 * Read an EXPLICITLY-typed port out of a host string, or `undefined` when there is
 * none. Parses the RAW string rather than a synthesised scheme default, so "the user
 * typed no port" (`http://localhost` → undefined, NOT 80) stays distinct from "the
 * user typed 80". Step-for-step mirror of the bridge binder's `TryGetExplicitPort`
 * (`ProjectConnectionResolver.cs:199-228`) including its `> 0 && <= MaxPort` guard and
 * strict-digit validation, so the CLI and the sidecar never disagree on a port. Pure.
 */
export function tryGetExplicitPort(host: string | undefined): number | undefined {
  if (host == null || host.trim().length === 0) return undefined;

  // Isolate the authority: after "scheme://" (if present), up to the first '/', '?' or '#'.
  const schemeEnd = host.indexOf('://');
  const start = schemeEnd >= 0 ? schemeEnd + 3 : 0;
  let end = -1;
  for (let i = start; i < host.length; i++) {
    if (AUTHORITY_TERMINATORS.has(host[i]!)) {
      end = i;
      break;
    }
  }
  let authority = end >= 0 ? host.substring(start, end) : host.substring(start);

  // Drop any userinfo ("user:pass@host:port") — its colon is not a port separator.
  const at = authority.lastIndexOf('@');
  if (at >= 0) authority = authority.substring(at + 1);

  // For an IPv6 literal the port follows the closing bracket ("[::1]:8080"); the colons
  // inside the brackets belong to the address. lastIndexOf(']') is -1 for a normal host,
  // so the search then starts at 0 and finds a plain "host:port" colon.
  const colon = authority.indexOf(':', authority.lastIndexOf(']') + 1);
  if (colon < 0 || colon === authority.length - 1) return undefined;

  const portStr = authority.substring(colon + 1);
  // ASCII digits only — mirrors `NumberStyles.None` (no sign, whitespace, or separators).
  if (!/^[0-9]+$/.test(portStr)) return undefined;
  const port = Number(portStr);
  if (!Number.isSafeInteger(port)) return undefined; // int.TryParse overflow → null on the C# side
  return port > 0 && port <= MAX_TCP_PORT ? port : undefined;
}

/**
 * The port the user explicitly typed into a Custom-mode LOOPBACK `host`, or `undefined`
 * when there is none — precedence level 2. Mirror of the bridge binder's
 * `TryGetExplicitLoopbackPort` (`ProjectConnectionResolver.cs:175-182`): a non-loopback
 * host (a LAN IP / remote) yields `undefined` so it falls through to the derived port,
 * matching the writer's `ConnectionMode.Local && uri.IsLoopback` gate. Pure.
 */
export function tryGetExplicitLoopbackPort(host: string | undefined): number | undefined {
  if (!isAbsoluteLoopbackHost(host)) return undefined;
  return tryGetExplicitPort(host);
}

/**
 * Resolve the local-server bind port for a project via the three-level precedence
 * (marker `portOverride` → typed loopback-host port → v1 derivation). Mirror of the
 * bridge binder's `ProjectConnectionResolver.Resolve` port block
 * (`ProjectConnectionResolver.cs:135-139`). `host` is the Custom-mode host (level 2
 * source); pass `undefined` when there is no host (Cloud / default fallback). Pure.
 */
export function resolveLocalBindPort(opts: {
  projectDir: string;
  host?: string;
  markerPortOverride?: number;
}): LocalBindPortResolution {
  const override = opts.markerPortOverride;
  if (typeof override === 'number' && Number.isInteger(override) && override > 0 && override <= MAX_TCP_PORT) {
    return { port: override, source: 'marker-override' }; // 1. marker portOverride wins outright
  }
  const typed = tryGetExplicitLoopbackPort(opts.host);
  if (typed !== undefined) {
    return { port: typed, source: 'typed-host' }; // 2. port typed into the loopback host
  }
  return { port: generatePortFromDirectory(opts.projectDir), source: 'derived' }; // 3. deterministic derivation
}
