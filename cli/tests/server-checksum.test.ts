import { describe, it, expect } from 'vitest';
import { createHash } from 'node:crypto';
import {
  serverChecksumsUrl,
  serverZipAssetName,
  parseSha256Sums,
  lookupDigest,
  verifyDigest,
  verifyZip,
  checksumFailureReason,
  SHA256SUMS_ASSET_NAME,
} from '../src/lib/server-checksum.js';
import { SERVER_VERSION } from '../src/lib/server-version.js';

// The VERBATIM live v8.0.0 SHA256SUMS manifest (downloaded via
// `gh release download v8.0.0 --repo IvanMurzak/GameDev-MCP-Server --pattern SHA256SUMS`).
// Standard coreutils format: <64-lowercase-hex>␠␠<filename>. All 7 RID zips. The
// test fixture and the production contract are byte-identical, so this suite proves
// ACCEPT-real / REJECT-tampered against ground truth.
const LIVE_SHA256SUMS =
  '5f17508e92812fbf9522eb552641d21dc2383fc2f6cf371f5413ad06c9820282  gamedev-mcp-server-linux-arm64.zip\n' +
  '844d4ad8cd152df44287341235ca2ae67cdb69b496252678eb6491f0bdc53319  gamedev-mcp-server-linux-x64.zip\n' +
  'ad0f50042dfa1edde26a9f26968538146ba792cc0188a47f6bfc1ae573bb513e  gamedev-mcp-server-osx-arm64.zip\n' +
  'd25993216e610401c8925716d9ad0f8ecaf3dc93443b12cfd057a75495ef9952  gamedev-mcp-server-osx-x64.zip\n' +
  '702f1d708c25dde6a58d3335c7adb92aa5fe36be618003821ceb040a9b59c51b  gamedev-mcp-server-win-arm64.zip\n' +
  '7383638dbc1cad84cf3b85617405c29c7885a51e34b1ef7b8b8864d0656814cb  gamedev-mcp-server-win-x64.zip\n' +
  'b171e1d8318d0ce4e88d30a5e86ad1cac1acea946ef1a71cd410a27f917c9799  gamedev-mcp-server-win-x86.zip\n';

const WIN_X64_DIGEST = '7383638dbc1cad84cf3b85617405c29c7885a51e34b1ef7b8b8864d0656814cb';

describe('serverChecksumsUrl / serverZipAssetName', () => {
  it('builds the v-prefixed SHA256SUMS sibling URL under the same release tag', () => {
    expect(serverChecksumsUrl('8.0.0')).toBe(
      'https://github.com/IvanMurzak/GameDev-MCP-Server/releases/download/v8.0.0/SHA256SUMS',
    );
  });

  it('defaults to the pinned SERVER_VERSION', () => {
    expect(serverChecksumsUrl()).toContain(`/v${SERVER_VERSION}/SHA256SUMS`);
  });

  it('builds the per-RID zip asset name (the exact lookup key)', () => {
    expect(serverZipAssetName('win-x64')).toBe('gamedev-mcp-server-win-x64.zip');
    expect(serverZipAssetName('linux-arm64')).toBe('gamedev-mcp-server-linux-arm64.zip');
  });
});

describe('parseSha256Sums', () => {
  it('parses the two-space coreutils format into all 7 RID entries (live manifest)', () => {
    const parsed = parseSha256Sums(LIVE_SHA256SUMS);
    expect(parsed.size).toBe(7);
    expect(parsed.get('gamedev-mcp-server-win-x64.zip')).toBe(WIN_X64_DIGEST);
    expect(parsed.get('gamedev-mcp-server-linux-x64.zip')).toBe(
      '844d4ad8cd152df44287341235ca2ae67cdb69b496252678eb6491f0bdc53319',
    );
    for (const rid of ['linux-arm64', 'linux-x64', 'osx-arm64', 'osx-x64', 'win-arm64', 'win-x64', 'win-x86']) {
      expect(parsed.has(`gamedev-mcp-server-${rid}.zip`)).toBe(true);
    }
  });

  it('tolerates CRLF endings, the * binary marker, and lowercases uppercase digests', () => {
    const manifest =
      '7383638DBC1CAD84CF3B85617405C29C7885A51E34B1EF7B8B8864D0656814CB *gamedev-mcp-server-win-x64.zip\r\n' +
      '844d4ad8cd152df44287341235ca2ae67cdb69b496252678eb6491f0bdc53319  gamedev-mcp-server-linux-x64.zip\r\n';
    const parsed = parseSha256Sums(manifest);
    expect(parsed.size).toBe(2);
    expect(parsed.get('gamedev-mcp-server-win-x64.zip')).toBe(WIN_X64_DIGEST);
  });

  it('skips blank lines and garbage (non-64-hex / no-filename) without spurious entries', () => {
    const manifest =
      '\n' +
      'not-a-hex-digest  some-file.zip\n' +
      'deadbeef  too-short.zip\n' +
      '7383638dbc1cad84cf3b85617405c29c7885a51e34b1ef7b8b8864d0656814cb\n' +
      '   \n' +
      '844d4ad8cd152df44287341235ca2ae67cdb69b496252678eb6491f0bdc53319  gamedev-mcp-server-linux-x64.zip\n';
    const parsed = parseSha256Sums(manifest);
    expect(parsed.size).toBe(1);
    expect(parsed.has('gamedev-mcp-server-linux-x64.zip')).toBe(true);
  });

  it('yields an empty map for empty / null / undefined input', () => {
    expect(parseSha256Sums('').size).toBe(0);
    expect(parseSha256Sums(null).size).toBe(0);
    expect(parseSha256Sums(undefined).size).toBe(0);
  });
});

describe('lookupDigest (exact-key, no cross-match among the 7 RIDs)', () => {
  const parsed = parseSha256Sums(LIVE_SHA256SUMS);

  it('returns the correct digest per RID', () => {
    expect(lookupDigest(parsed, 'gamedev-mcp-server-win-x64.zip')).toBe(WIN_X64_DIGEST);
  });

  it('never cross-matches a sibling RID (win-x64 vs win-x86 vs win-arm64 are distinct)', () => {
    const x64 = lookupDigest(parsed, 'gamedev-mcp-server-win-x64.zip');
    const x86 = lookupDigest(parsed, 'gamedev-mcp-server-win-x86.zip');
    const arm = lookupDigest(parsed, 'gamedev-mcp-server-win-arm64.zip');
    expect(x64).not.toBe(x86);
    expect(x64).not.toBe(arm);
    expect(x86).not.toBe(arm);
  });

  it('returns null for an asset with no manifest entry (missing-RID fail-closed)', () => {
    expect(lookupDigest(parsed, 'gamedev-mcp-server-solaris-sparc.zip')).toBeNull();
  });
});

describe('verifyDigest (case-insensitive, fail-closed on empty)', () => {
  it('matches case-insensitively and trims, but never matches an empty digest', () => {
    expect(verifyDigest(WIN_X64_DIGEST, WIN_X64_DIGEST)).toBe(true);
    expect(verifyDigest(WIN_X64_DIGEST.toUpperCase(), WIN_X64_DIGEST)).toBe(true);
    expect(verifyDigest(`  ${WIN_X64_DIGEST}  `, WIN_X64_DIGEST)).toBe(true);
    expect(verifyDigest(WIN_X64_DIGEST, '844d4ad8cd152df44287341235ca2ae67cdb69b496252678eb6491f0bdc53319')).toBe(false);
    expect(verifyDigest('', WIN_X64_DIGEST)).toBe(false);
    expect(verifyDigest(WIN_X64_DIGEST, '')).toBe(false);
    expect(verifyDigest(null, WIN_X64_DIGEST)).toBe(false);
    expect(verifyDigest(WIN_X64_DIGEST, undefined)).toBe(false);
  });
});

describe('verifyZip (the fail-closed gate, against the live manifest)', () => {
  it('verified when the manifest has the RID and the digest matches (valid passes)', () => {
    expect(verifyZip(LIVE_SHA256SUMS, 'gamedev-mcp-server-win-x64.zip', WIN_X64_DIGEST)).toBe('verified');
  });

  it('digest-mismatch when the computed digest differs (tampered rejected)', () => {
    expect(
      verifyZip(LIVE_SHA256SUMS, 'gamedev-mcp-server-win-x64.zip', '844d4ad8cd152df44287341235ca2ae67cdb69b496252678eb6491f0bdc53319'),
    ).toBe('digest-mismatch');
  });

  it('missing-entry when the manifest has no line for this RID (missing-entry rejected)', () => {
    expect(verifyZip(LIVE_SHA256SUMS, 'gamedev-mcp-server-solaris-sparc.zip', WIN_X64_DIGEST)).toBe('missing-entry');
  });

  it('manifest-unparsable for empty or all-garbage manifest text (malformed rejected)', () => {
    expect(verifyZip('', 'gamedev-mcp-server-win-x64.zip', WIN_X64_DIGEST)).toBe('manifest-unparsable');
    expect(verifyZip('just\nnoise\nno digests', 'gamedev-mcp-server-win-x64.zip', WIN_X64_DIGEST)).toBe('manifest-unparsable');
  });

  it('rejects a CORRECT digest looked up under the WRONG RID (no cross-RID acceptance)', () => {
    // win-x64 digest is genuine, but under the win-x86 asset name it must NOT verify.
    expect(verifyZip(LIVE_SHA256SUMS, 'gamedev-mcp-server-win-x86.zip', WIN_X64_DIGEST)).toBe('digest-mismatch');
  });

  it('verifies a real-bytes round-trip: node:crypto SHA256 of fixed bytes against a crafted manifest', () => {
    // Prove the createHash('sha256') path the downloader uses agrees with verifyZip end to end.
    const bytes = new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]);
    const digest = createHash('sha256').update(bytes).digest('hex');
    const manifest = `${digest}  gamedev-mcp-server-win-x64.zip\n`;
    expect(verifyZip(manifest, 'gamedev-mcp-server-win-x64.zip', digest)).toBe('verified');
    // A single flipped byte → different digest → rejected.
    const tampered = createHash('sha256').update(new Uint8Array([1, 2, 3, 4, 5, 6, 7, 9])).digest('hex');
    expect(verifyZip(manifest, 'gamedev-mcp-server-win-x64.zip', tampered)).toBe('digest-mismatch');
  });
});

describe('checksumFailureReason', () => {
  it('renders a distinct actionable reason per fail-closed verdict', () => {
    expect(checksumFailureReason('manifest-unparsable', 'x.zip')).toContain('unparsable');
    expect(checksumFailureReason('missing-entry', 'gamedev-mcp-server-win-x64.zip')).toContain('gamedev-mcp-server-win-x64.zip');
    expect(checksumFailureReason('digest-mismatch', 'x.zip')).toContain('SHA256');
    expect(checksumFailureReason('manifest-unparsable', 'x.zip')).toContain(SHA256SUMS_ASSET_NAME);
  });
});
