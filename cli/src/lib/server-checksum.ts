// `server-checksum` — pure (no I/O, no network) download-integrity logic for
// the shared `gamedev-mcp-server` binary: the `SHA256SUMS` manifest URL
// builder, the coreutils-format parser, the exact-key RID digest lookup, the
// case-insensitive digest compare, and the single fail-closed `verifyZip`
// verdict. `download-server.ts`'s `downloadServer` calls `verifyZip` BETWEEN
// the zip download (`arrayBuffer()`) and `unzipSync` — so a downloaded server
// zip is NEVER extracted or executed unless its SHA256 matches the release's
// published `SHA256SUMS` manifest (issue #155). A compromised release asset or
// a trusted-CA MITM would otherwise yield arbitrary code execution.
//
// Keeping this here — rather than inline in `download-server.ts` — makes every
// decision unit-testable with no real download: each function below is a
// deterministic string/enum/map transform. The `node:crypto` SHA256 compute and
// the HTTP fetch that surround this verdict live in `download-server.ts`.
// Mirrors Unity-MCP's `McpServerChecksum` (PR #842) and Godot-MCP's checksum
// seam (PR #193); same parser/verify/test shape, TS idioms.

import { SERVER_VERSION } from './server-version.js';
import { SERVER_BINARY_BASENAME } from './download-server.js';

/** GitHub repo the server binaries (and the SHA256SUMS manifest) are released from. */
const SERVER_RELEASE_REPO = 'IvanMurzak/GameDev-MCP-Server';

/**
 * The name of the integrity manifest asset attached to every GameDev-MCP-Server
 * release: a standard coreutils `sha256sum` output file listing one
 * `<hex>␠␠<filename>` line per per-RID server zip. LIVE on the pinned `v8.0.0`
 * release (and every future release).
 */
export const SHA256SUMS_ASSET_NAME = 'SHA256SUMS';

/**
 * The URL of a release's `SHA256SUMS` manifest — the SIBLING of the per-RID zip
 * (`serverDownloadUrl`) under the SAME `v<version>` release tag:
 * `https://github.com/IvanMurzak/GameDev-MCP-Server/releases/download/v<version>/SHA256SUMS`.
 * The downloaded zip's SHA256 is verified against this manifest BEFORE
 * extraction/execution (fail-closed). Pure.
 */
export function serverChecksumsUrl(version: string = SERVER_VERSION): string {
  return `https://github.com/${SERVER_RELEASE_REPO}/releases/download/v${version}/${SHA256SUMS_ASSET_NAME}`;
}

/**
 * The per-RID server zip asset name, e.g. `gamedev-mcp-server-win-x64.zip`. This
 * is the EXACT key looked up in the parsed `SHA256SUMS` map — it matches the
 * trailing segment of `serverDownloadUrl` so the verified asset name can never
 * drift from the downloaded asset. Pure.
 */
export function serverZipAssetName(rid: string): string {
  return `${SERVER_BINARY_BASENAME}-${rid}.zip`;
}

/** True when `value` is exactly 64 ASCII hex characters (a SHA256 hex digest). */
function isHex64(value: string): boolean {
  return /^[0-9a-fA-F]{64}$/.test(value);
}

/**
 * Parse a coreutils `sha256sum` manifest into a `{ filename → lowercase-hex }`
 * map. The exact LIVE format is one line per file: a 64-character lowercase hex
 * digest, then TWO spaces (the coreutils text-mode separator), then the file
 * name — `844d4ad8…53319␠␠gamedev-mcp-server-linux-x64.zip`. Tolerances applied
 * (so a hand-edited or CRLF manifest still parses, while a malformed one yields
 * no usable entry):
 *   - CRLF and bare-LF line endings; blank lines skipped.
 *   - Leading/trailing whitespace on each line trimmed.
 *   - The coreutils binary-mode `'*'` marker before the filename
 *     (`<hex> *<name>`) is stripped.
 *   - A line whose first token is NOT a 64-char hex string, or which has no
 *     filename, is SKIPPED (it never produces a spurious entry — fail-closed at
 *     the lookup layer).
 * Digests are normalized to lowercase; filenames kept verbatim (case-sensitive,
 * matching the asset names). On a duplicate filename the LAST entry wins. Never
 * throws — a null/empty/garbage input yields an empty map. Pure.
 */
export function parseSha256Sums(sha256SumsText: string | null | undefined): Map<string, string> {
  const map = new Map<string, string>();
  if (!sha256SumsText) return map;

  for (const rawLine of sha256SumsText.replace(/\r\n/g, '\n').split('\n')) {
    const line = rawLine.trim();
    if (line.length === 0) continue;

    // Split on the FIRST run of whitespace: the digest token is fixed-width
    // 64-hex, the filename is everything after (so a single-space/tab variant
    // still parses, not only the canonical two-space separator).
    const sepMatch = /\s/.exec(line);
    if (!sepMatch || sepMatch.index <= 0) continue;
    const sepIndex = sepMatch.index;

    const digestToken = line.slice(0, sepIndex);
    if (!isHex64(digestToken)) continue;

    let fileName = line.slice(sepIndex).trimStart();
    // coreutils binary-mode marker: `<hex> *<name>`. Strip a single leading '*'.
    if (fileName.startsWith('*')) fileName = fileName.slice(1);
    fileName = fileName.trim();
    if (fileName.length === 0) continue;

    map.set(fileName, digestToken.toLowerCase());
  }

  return map;
}

/**
 * Look up the expected SHA256 digest for `assetZipName` (e.g.
 * `gamedev-mcp-server-win-x64.zip`) in a parsed `parseSha256Sums` map. The
 * lookup is EXACT-key — `linux-x64` never cross-matches `linux-arm64`,
 * `win-x64` never cross-matches `win-x86` (no substring/prefix match). Returns
 * the lowercase hex digest, or `null` when the manifest has no entry for that
 * asset (the MISSING-entry fail-closed case). Pure.
 */
export function lookupDigest(parsed: Map<string, string>, assetZipName: string): string | null {
  if (!parsed || !assetZipName) return null;
  return parsed.get(assetZipName) ?? null;
}

/**
 * Case-insensitive hex-digest equality (both sides trimmed). A null/empty/
 * whitespace digest on either side is NEVER a match (fail-closed: an unknown
 * digest must not pass). Pure.
 */
export function verifyDigest(expectedHexDigest: string | null | undefined, actualHexDigest: string | null | undefined): boolean {
  const expected = (expectedHexDigest ?? '').trim();
  const actual = (actualHexDigest ?? '').trim();
  if (expected.length === 0 || actual.length === 0) return false;
  return expected.toLowerCase() === actual.toLowerCase();
}

/** The verdict of verifying a downloaded zip against a release `SHA256SUMS` manifest. */
export type ChecksumVerdict =
  /** The manifest parsed, contained this asset's entry, and the digest matched. SAFE to extract/execute. */
  | 'verified'
  /** The manifest text was missing/empty/unparsable (no usable entries). Fail-closed. */
  | 'manifest-unparsable'
  /** The manifest parsed but had no line for this asset's zip name. Fail-closed. */
  | 'missing-entry'
  /** The manifest's entry for this asset did NOT match the downloaded zip's digest. Fail-closed. */
  | 'digest-mismatch';

/**
 * The single fail-closed integrity decision `downloadServer` calls BEFORE
 * `unzipSync`: parse the release's `SHA256SUMS`, find the entry for
 * `assetZipName`, and compare it (case-insensitive hex) against the
 * locally-computed SHA256 of the downloaded zip (`actualZipHexDigest`). Returns
 * `'verified'` ONLY when the manifest parsed, contained the asset, and the
 * digest matched; every other outcome is a distinct fail-closed verdict the
 * caller MUST treat as "do NOT extract, do NOT launch". Pure — unit-tested with
 * no real download. Never throws.
 */
export function verifyZip(sha256SumsText: string | null | undefined, assetZipName: string, actualZipHexDigest: string | null | undefined): ChecksumVerdict {
  const parsed = parseSha256Sums(sha256SumsText);
  if (parsed.size === 0) return 'manifest-unparsable';

  const expected = lookupDigest(parsed, assetZipName);
  if (expected === null) return 'missing-entry';

  return verifyDigest(expected, actualZipHexDigest) ? 'verified' : 'digest-mismatch';
}

/** A short, actionable human-readable reason for a non-`'verified'` verdict. Pure. */
export function checksumFailureReason(verdict: ChecksumVerdict, assetZipName: string): string {
  switch (verdict) {
    case 'manifest-unparsable':
      return `the downloaded ${SHA256SUMS_ASSET_NAME} manifest was empty or unparsable`;
    case 'missing-entry':
      return `the ${SHA256SUMS_ASSET_NAME} manifest has no entry for '${assetZipName}'`;
    case 'digest-mismatch':
      return `the downloaded '${assetZipName}' SHA256 did not match the ${SHA256SUMS_ASSET_NAME} manifest entry`;
    default:
      return 'the checksum was verified';
  }
}
