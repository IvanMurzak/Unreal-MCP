import { describe, it, expect, afterEach } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import { createHash } from 'node:crypto';
import { zipSync, strToU8 } from 'fflate';
import {
  downloadServer,
  findExtractedBinary,
  readVersionMarker,
  resolveServerBinaryPath,
  resolveServerOverride,
  serverDownloadUrl,
  serverExecutableName,
  serverInstallDir,
  SERVER_PATH_ENV_VAR,
} from '../src/lib/download-server.js';
import { serverZipAssetName } from '../src/lib/server-checksum.js';
import { SERVER_VERSION } from '../src/lib/server-version.js';
import { makeTempDir, rmTempDir } from './helpers.js';

const dirs: string[] = [];
afterEach(() => {
  while (dirs.length) rmTempDir(dirs.pop()!);
});
function tmp(): string {
  const d = makeTempDir();
  dirs.push(d);
  return d;
}

// --- synthetic release zips (ground truth: v8.0.0 asset layouts) ------------

const SIDECARS = ['appsettings.json', 'NLog.config', 'server.json', 'web.config'];

/** FLAT layout (the win zips): exe + sidecar files at the zip root. */
function flatWinZip(): Uint8Array {
  const entries: Record<string, Uint8Array> = {
    'gamedev-mcp-server.exe': strToU8('exe-bytes'),
  };
  for (const s of SIDECARS) entries[s] = strToU8(`content of ${s}`);
  return zipSync(entries);
}

/** `<rid>/`-foldered layout (the osx/linux zips). */
function folderedUnixZip(rid: string): Uint8Array {
  const entries: Record<string, Uint8Array> = {
    [`${rid}/gamedev-mcp-server`]: strToU8('elf-bytes'),
  };
  for (const s of SIDECARS) entries[`${rid}/${s}`] = strToU8(`content of ${s}`);
  return zipSync(entries);
}

/** SHA256 hex of bytes, matching the createHash('sha256') the downloader uses. */
function sha256Hex(bytes: Uint8Array): string {
  return createHash('sha256').update(bytes).digest('hex');
}

/**
 * Build a valid `SHA256SUMS` manifest for the given zip + RID — the digest the
 * fail-closed verify-gate must accept. `digestOverride` injects a WRONG digest
 * to exercise the tampered-rejection path; `omitEntry` produces a manifest with
 * no line for this RID (missing-entry path).
 */
function makeSha256Sums(
  bytes: Uint8Array,
  rid: string,
  opts: { digestOverride?: string; omitEntry?: boolean } = {},
): string {
  if (opts.omitEntry) {
    // A manifest with some-other-RID entry but NOT this one.
    return `${'0'.repeat(64)}  gamedev-mcp-server-some-other-rid.zip\n`;
  }
  const digest = opts.digestOverride ?? sha256Hex(bytes);
  return `${digest}  ${serverZipAssetName(rid)}\n`;
}

/**
 * A `fetch` stub serving BOTH the zip (the download URL) AND the SHA256SUMS
 * manifest (the integrity URL). The manifest is computed from the zip bytes so
 * the verify-gate passes by default. `sumsOpts` lets a test serve a tampered /
 * missing / unfetchable manifest; `rid` is the asset the manifest keys on.
 */
function fetchZip(
  bytes: Uint8Array,
  calls?: string[],
  rid = 'win-x64',
  sumsOpts: { digestOverride?: string; omitEntry?: boolean; sumsHttpError?: number } = {},
): typeof fetch {
  return (async (url: unknown) => {
    const u = String(url);
    calls?.push(u);
    if (u.endsWith('SHA256SUMS')) {
      if (sumsOpts.sumsHttpError) {
        return { ok: false, status: sumsOpts.sumsHttpError, statusText: 'err', text: async () => '' } as unknown as Response;
      }
      return {
        ok: true,
        status: 200,
        statusText: 'OK',
        text: async () => makeSha256Sums(bytes, rid, sumsOpts),
      } as unknown as Response;
    }
    return {
      ok: true,
      status: 200,
      statusText: 'OK',
      arrayBuffer: async () => bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength),
    } as unknown as Response;
  }) as typeof fetch;
}

/** A `fetch` stub returning an HTTP error (for the ZIP download — fails before verify). */
function fetch404(): typeof fetch {
  return (async () =>
    ({ ok: false, status: 404, statusText: 'Not Found', arrayBuffer: async () => new ArrayBuffer(0), text: async () => '' }) as unknown as Response) as typeof fetch;
}

// --- pure helpers ------------------------------------------------------------

describe('serverExecutableName / serverDownloadUrl / paths', () => {
  it('appends .exe on windows only', () => {
    expect(serverExecutableName('win32')).toBe('gamedev-mcp-server.exe');
    expect(serverExecutableName('linux')).toBe('gamedev-mcp-server');
    expect(serverExecutableName('darwin')).toBe('gamedev-mcp-server');
  });

  it('builds the v-prefixed GameDev-MCP-Server release-asset URL', () => {
    expect(serverDownloadUrl('win-x64', '8.0.0')).toBe(
      'https://github.com/IvanMurzak/GameDev-MCP-Server/releases/download/v8.0.0/gamedev-mcp-server-win-x64.zip',
    );
  });

  it('defaults to the pinned SERVER_VERSION', () => {
    expect(serverDownloadUrl('linux-x64')).toContain(`/v${SERVER_VERSION}/`);
  });

  it('resolves the §6 install layout', () => {
    const dir = serverInstallDir('/proj', 'win32', 'x64');
    expect(dir).toBe(path.join(path.resolve('/proj'), 'Intermediate', 'UnrealMCP', 'server', 'win-x64'));
    expect(resolveServerBinaryPath('/proj', 'win32', 'x64')).toBe(path.join(dir, 'gamedev-mcp-server.exe'));
  });
});

describe('resolveServerOverride', () => {
  it('returns the override when it points at an existing file', () => {
    const dir = tmp();
    const file = path.join(dir, 'gamedev-mcp-server.exe');
    fs.writeFileSync(file, 'x');
    expect(resolveServerOverride({ [SERVER_PATH_ENV_VAR]: file })).toBe(file);
  });

  it('ignores an unset, empty, or missing-file override', () => {
    expect(resolveServerOverride({})).toBeNull();
    expect(resolveServerOverride({ [SERVER_PATH_ENV_VAR]: '' })).toBeNull();
    expect(resolveServerOverride({ [SERVER_PATH_ENV_VAR]: path.join(tmp(), 'nope.exe') })).toBeNull();
  });
});

describe('findExtractedBinary', () => {
  it('prefers the shallowest match', () => {
    const dir = tmp();
    fs.mkdirSync(path.join(dir, 'nested', 'deeper'), { recursive: true });
    fs.writeFileSync(path.join(dir, 'nested', 'deeper', 'srv'), 'deep');
    fs.writeFileSync(path.join(dir, 'nested', 'srv'), 'shallow');
    expect(findExtractedBinary(dir, 'srv')).toBe(path.join(dir, 'nested', 'srv'));
  });

  it('returns null when absent', () => {
    expect(findExtractedBinary(tmp(), 'srv')).toBeNull();
  });
});

// --- downloadServer ----------------------------------------------------------

describe('downloadServer', () => {
  it('extracts a FLAT win zip: exe + sidecars + version marker land in server/<rid>/', async () => {
    const proj = tmp();
    const r = await downloadServer({ projectDir: proj, os: 'win32', arch: 'x64', env: {}, fetchImpl: fetchZip(flatWinZip()) });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.source).toBe('download');
    const installDir = serverInstallDir(proj, 'win32', 'x64');
    expect(r.serverPath).toBe(path.join(installDir, 'gamedev-mcp-server.exe'));
    expect(fs.readFileSync(r.serverPath, 'utf-8')).toBe('exe-bytes');
    // The sidecar files MUST land next to the binary.
    for (const s of SIDECARS) expect(fs.existsSync(path.join(installDir, s))).toBe(true);
    expect(readVersionMarker(installDir)).toBe(SERVER_VERSION);
  });

  it('extracts a <rid>/-foldered unix zip: files land DIRECTLY in server/<rid>/ (not nested)', async () => {
    const proj = tmp();
    const r = await downloadServer({
      projectDir: proj,
      os: 'linux',
      arch: 'x64',
      env: {},
      fetchImpl: fetchZip(folderedUnixZip('linux-x64'), undefined, 'linux-x64'),
    });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    const installDir = serverInstallDir(proj, 'linux', 'x64');
    expect(r.serverPath).toBe(path.join(installDir, 'gamedev-mcp-server'));
    expect(fs.existsSync(r.serverPath)).toBe(true);
    // No nested linux-x64/linux-x64 folder.
    expect(fs.existsSync(path.join(installDir, 'linux-x64'))).toBe(false);
    for (const s of SIDECARS) expect(fs.existsSync(path.join(installDir, s))).toBe(true);
    // Unix binary is marked executable (exec bits are not representable on a
    // Windows filesystem, so only assert where chmod is meaningful).
    if (process.platform !== 'win32') {
      expect(fs.statSync(r.serverPath).mode & 0o111).not.toBe(0);
    }
    expect(readVersionMarker(installDir)).toBe(SERVER_VERSION);
  });

  it('reuses the cache when the version marker matches (no fetch)', async () => {
    const proj = tmp();
    const calls: string[] = [];
    const first = await downloadServer({ projectDir: proj, os: 'win32', arch: 'x64', env: {}, fetchImpl: fetchZip(flatWinZip(), calls) });
    expect(first.kind).toBe('success');
    const second = await downloadServer({ projectDir: proj, os: 'win32', arch: 'x64', env: {}, fetchImpl: fetchZip(flatWinZip(), calls) });
    expect(second.kind).toBe('success');
    if (second.kind !== 'success') return;
    expect(second.source).toBe('cache');
    // First download = 2 fetches (zip + SHA256SUMS); the cache hit adds none.
    expect(calls).toHaveLength(2);
  });

  it('re-downloads when the version marker is stale', async () => {
    const proj = tmp();
    const installDir = serverInstallDir(proj, 'win32', 'x64');
    fs.mkdirSync(installDir, { recursive: true });
    fs.writeFileSync(path.join(installDir, 'gamedev-mcp-server.exe'), 'old-exe');
    fs.writeFileSync(path.join(installDir, 'version'), '7.9.0');
    const calls: string[] = [];
    const r = await downloadServer({ projectDir: proj, os: 'win32', arch: 'x64', env: {}, fetchImpl: fetchZip(flatWinZip(), calls) });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.source).toBe('download');
    // 2 fetches: the release zip + the SHA256SUMS integrity manifest.
    expect(calls).toHaveLength(2);
    expect(fs.readFileSync(r.serverPath, 'utf-8')).toBe('exe-bytes');
    expect(readVersionMarker(installDir)).toBe(SERVER_VERSION);
  });

  it('honors the UNREAL_MCP_SERVER_PATH override (no download, no version check)', async () => {
    const proj = tmp();
    const overrideDir = tmp();
    const override = path.join(overrideDir, 'gamedev-mcp-server.exe');
    fs.writeFileSync(override, 'local-build');
    const calls: string[] = [];
    const r = await downloadServer({
      projectDir: proj,
      os: 'win32',
      arch: 'x64',
      env: { [SERVER_PATH_ENV_VAR]: override },
      fetchImpl: fetchZip(flatWinZip(), calls),
    });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.source).toBe('override');
    expect(r.serverPath).toBe(override);
    expect(calls).toHaveLength(0);
  });

  it('falls through a set-but-missing override and downloads', async () => {
    const proj = tmp();
    const r = await downloadServer({
      projectDir: proj,
      os: 'win32',
      arch: 'x64',
      env: { [SERVER_PATH_ENV_VAR]: path.join(tmp(), 'missing.exe') },
      fetchImpl: fetchZip(flatWinZip()),
    });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.source).toBe('download');
  });

  it('returns failure (no throw) on an HTTP error and reports the URL', async () => {
    const r = await downloadServer({ projectDir: tmp(), os: 'win32', arch: 'x64', env: {}, fetchImpl: fetch404() });
    expect(r.kind).toBe('failure');
    if (r.kind !== 'failure') return;
    expect(r.error.message).toContain('404');
    expect(r.url).toContain('gamedev-mcp-server-win-x64.zip');
  });

  it('returns failure when the zip does not contain the server binary', async () => {
    const zip = zipSync({ 'README.md': strToU8('not a server') });
    const r = await downloadServer({ projectDir: tmp(), os: 'win32', arch: 'x64', env: {}, fetchImpl: fetchZip(zip) });
    expect(r.kind).toBe('failure');
    if (r.kind !== 'failure') return;
    expect(r.error.message).toContain('gamedev-mcp-server.exe');
  });

  // --- fail-closed integrity gate (issue #155) ------------------------------

  it('FAIL-CLOSED: a tampered zip (digest mismatch) is rejected and NEVER extracted', async () => {
    const proj = tmp();
    // The manifest advertises a WRONG digest for win-x64 → mismatch → refuse to extract.
    const r = await downloadServer({
      projectDir: proj,
      os: 'win32',
      arch: 'x64',
      env: {},
      fetchImpl: fetchZip(flatWinZip(), undefined, 'win-x64', { digestOverride: 'f'.repeat(64) }),
    });
    expect(r.kind).toBe('failure');
    if (r.kind !== 'failure') return;
    expect(r.error.message).toContain('did not match');
    // Nothing was extracted — the install dir holds no binary.
    expect(fs.existsSync(resolveServerBinaryPath(proj, 'win32', 'x64'))).toBe(false);
  });

  it('FAIL-CLOSED: a manifest missing this RID entry is rejected and NEVER extracted', async () => {
    const proj = tmp();
    const r = await downloadServer({
      projectDir: proj,
      os: 'win32',
      arch: 'x64',
      env: {},
      fetchImpl: fetchZip(flatWinZip(), undefined, 'win-x64', { omitEntry: true }),
    });
    expect(r.kind).toBe('failure');
    if (r.kind !== 'failure') return;
    expect(r.error.message).toContain('no entry for');
    expect(fs.existsSync(resolveServerBinaryPath(proj, 'win32', 'x64'))).toBe(false);
  });

  it('FAIL-CLOSED: an unfetchable SHA256SUMS manifest (persistent HTTP error) is rejected', async () => {
    const proj = tmp();
    const r = await downloadServer({
      projectDir: proj,
      os: 'win32',
      arch: 'x64',
      env: {},
      fetchImpl: fetchZip(flatWinZip(), undefined, 'win-x64', { sumsHttpError: 500 }),
    });
    expect(r.kind).toBe('failure');
    if (r.kind !== 'failure') return;
    expect(r.error.message).toContain('SHA256SUMS');
    expect(fs.existsSync(resolveServerBinaryPath(proj, 'win32', 'x64'))).toBe(false);
  });

  it('verifies the REAL win-x64 release digest end-to-end (node:crypto SHA256 of fixed bytes == manifest)', async () => {
    // Synthesize a zip whose real SHA256 is what the (test-built) manifest advertises, proving the
    // createHash('sha256') compute path agrees with the manifest comparison through the whole download.
    const proj = tmp();
    const zip = flatWinZip();
    const realDigest = sha256Hex(zip);
    const calls: string[] = [];
    const r = await downloadServer({
      projectDir: proj,
      os: 'win32',
      arch: 'x64',
      env: {},
      // digestOverride set to the REAL computed digest — equivalent to the genuine manifest.
      fetchImpl: fetchZip(zip, calls, 'win-x64', { digestOverride: realDigest }),
    });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.source).toBe('download');
    expect(fs.existsSync(r.serverPath)).toBe(true);
  });
});

// --- --server-source escape hatch (offline / CI) ----------------------------

/** A fetch that FAILS the test if called — asserts the source path never hits the network. */
function fetchNever(): typeof fetch {
  return (async () => {
    throw new Error('fetch must not be called for a local --server-source');
  }) as typeof fetch;
}

describe('downloadServer --server-source', () => {
  it('installs from a local .zip into the managed dir WITHOUT the SHA256SUMS gate', async () => {
    const proj = tmp();
    const src = tmp();
    const zipPath = path.join(src, 'server.zip');
    fs.writeFileSync(zipPath, flatWinZip());

    const r = await downloadServer({
      projectDir: proj,
      os: 'win32',
      arch: 'x64',
      env: {},
      source: zipPath,
      fetchImpl: fetchNever(), // no network — no SHA256SUMS fetch, no release download
    });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.source).toBe('source');
    const installDir = serverInstallDir(proj, 'win32', 'x64');
    expect(r.serverPath).toBe(path.join(installDir, 'gamedev-mcp-server.exe'));
    expect(fs.existsSync(r.serverPath)).toBe(true);
    // Sidecars moved beside the binary, and the version marker written.
    expect(fs.existsSync(path.join(installDir, 'appsettings.json'))).toBe(true);
    expect(readVersionMarker(installDir)).toBe(SERVER_VERSION);
  });

  it('installs from an already-extracted directory (copy, source left intact)', async () => {
    const proj = tmp();
    const src = tmp();
    fs.writeFileSync(path.join(src, 'gamedev-mcp-server.exe'), 'exe-bytes');
    fs.writeFileSync(path.join(src, 'appsettings.json'), '{}');

    const r = await downloadServer({
      projectDir: proj,
      os: 'win32',
      arch: 'x64',
      env: {},
      source: src,
      fetchImpl: fetchNever(),
    });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.source).toBe('source');
    expect(fs.existsSync(r.serverPath)).toBe(true);
    // The source directory is preserved (copy, not move).
    expect(fs.existsSync(path.join(src, 'gamedev-mcp-server.exe'))).toBe(true);
  });

  it('installs from a bare binary file path', async () => {
    const proj = tmp();
    const src = tmp();
    const bin = path.join(src, 'gamedev-mcp-server.exe');
    fs.writeFileSync(bin, 'exe-bytes');

    const r = await downloadServer({ projectDir: proj, os: 'win32', arch: 'x64', env: {}, source: bin, fetchImpl: fetchNever() });
    expect(r.kind).toBe('success');
    if (r.kind === 'success') expect(r.source).toBe('source');
  });

  it('downloads from a --server-source URL WITHOUT fetching SHA256SUMS', async () => {
    const proj = tmp();
    const zip = flatWinZip();
    const calls: string[] = [];
    const fetchImpl = (async (url: unknown) => {
      calls.push(String(url));
      return {
        ok: true,
        status: 200,
        statusText: 'OK',
        arrayBuffer: async () => zip.buffer.slice(zip.byteOffset, zip.byteOffset + zip.byteLength),
      } as unknown as Response;
    }) as typeof fetch;

    const r = await downloadServer({
      projectDir: proj,
      os: 'win32',
      arch: 'x64',
      env: {},
      source: 'https://example.com/custom-server.zip',
      fetchImpl,
    });
    expect(r.kind).toBe('success');
    if (r.kind === 'success') expect(r.source).toBe('source');
    // Exactly one fetch — the source URL. No SHA256SUMS request.
    expect(calls).toEqual(['https://example.com/custom-server.zip']);
    expect(calls.some((u) => u.endsWith('SHA256SUMS'))).toBe(false);
  });

  it('fails cleanly for a non-existent --server-source path', async () => {
    const r = await downloadServer({
      projectDir: tmp(),
      os: 'win32',
      arch: 'x64',
      env: {},
      source: path.join(tmp(), 'does-not-exist.zip'),
      fetchImpl: fetchNever(),
    });
    expect(r.kind).toBe('failure');
    if (r.kind === 'failure') expect(r.error.message).toMatch(/does not exist/i);
  });

  it('the UNREAL_MCP_SERVER_PATH override still wins over --server-source', async () => {
    const proj = tmp();
    const overrideBin = path.join(tmp(), 'gamedev-mcp-server.exe');
    fs.writeFileSync(overrideBin, 'x');
    const r = await downloadServer({
      projectDir: proj,
      os: 'win32',
      arch: 'x64',
      env: { [SERVER_PATH_ENV_VAR]: overrideBin },
      source: path.join(tmp(), 'ignored.zip'),
      fetchImpl: fetchNever(),
    });
    expect(r.kind).toBe('success');
    if (r.kind === 'success') {
      expect(r.source).toBe('override');
      expect(r.serverPath).toBe(overrideBin);
    }
  });
});
