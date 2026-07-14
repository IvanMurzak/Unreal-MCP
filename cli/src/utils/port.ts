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
