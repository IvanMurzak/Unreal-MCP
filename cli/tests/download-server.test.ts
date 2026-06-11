import { describe, it, expect, afterEach } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
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

/** A `fetch` stub returning the given zip bytes (200 OK). */
function fetchZip(bytes: Uint8Array, calls?: string[]): typeof fetch {
  return (async (url: unknown) => {
    calls?.push(String(url));
    return {
      ok: true,
      status: 200,
      statusText: 'OK',
      arrayBuffer: async () => bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength),
    } as unknown as Response;
  }) as typeof fetch;
}

/** A `fetch` stub returning an HTTP error. */
function fetch404(): typeof fetch {
  return (async () =>
    ({ ok: false, status: 404, statusText: 'Not Found', arrayBuffer: async () => new ArrayBuffer(0) }) as unknown as Response) as typeof fetch;
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
      fetchImpl: fetchZip(folderedUnixZip('linux-x64')),
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
    expect(calls).toHaveLength(1);
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
    expect(calls).toHaveLength(1);
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
});
