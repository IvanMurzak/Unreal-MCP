import { describe, it, expect, afterEach } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import { zipSync, strToU8 } from 'fflate';
import { update, readPluginVersion } from '../src/lib/update.js';
import { PACKAGE_VERSION } from '../src/version.js';
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

function makeSource(version: string): string {
  const dir = tmp();
  fs.writeFileSync(path.join(dir, 'UnrealMCP.uplugin'), JSON.stringify({ VersionName: version }), 'utf-8');
  return dir;
}

function zipResponse(entries: Record<string, Uint8Array>, calls?: string[]): typeof fetch {
  const bytes = zipSync(entries);
  return (async (url: string) => {
    calls?.push(url);
    return {
      ok: true,
      status: 200,
      statusText: 'OK',
      arrayBuffer: async () => bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength),
    } as unknown as Response;
  }) as typeof fetch;
}

/**
 * A plugin SOURCE that itself ships a stale C++ build cache (Intermediate/ +
 * Binaries/Win64). Copying this into the install reproduces the cache there, so
 * only the EXPLICIT clean step (not installPlugin's rm-then-copy) can remove it
 * — this makes the "clean wiped the cache" assertion non-vacuous.
 */
function makeSourceWithCache(version: string): string {
  const dir = makeSource(version);
  fs.mkdirSync(path.join(dir, 'Intermediate', 'Build'), { recursive: true });
  fs.writeFileSync(path.join(dir, 'Intermediate', 'Build', 'stale.obj'), 'stale', 'utf-8');
  fs.mkdirSync(path.join(dir, 'Binaries', 'Win64'), { recursive: true });
  fs.writeFileSync(path.join(dir, 'Binaries', 'Win64', 'UnrealEditor-UnrealMcpEditor.dll'), 'stale', 'utf-8');
  return dir;
}

/**
 * Seed an already-installed plugin under `<project>/Plugins/UnrealMCP` carrying
 * a stale build cache (Intermediate/ + C++ Binaries/) AND a bundled bridge
 * under Binaries/ThirdParty/ — the post-compile state a real release install
 * reaches before an `update`.
 */
function seedInstalledWithCache(project: string, version: string): string {
  const installed = path.join(project, 'Plugins', 'UnrealMCP');
  fs.mkdirSync(installed, { recursive: true });
  fs.writeFileSync(path.join(installed, 'UnrealMCP.uplugin'), JSON.stringify({ VersionName: version }), 'utf-8');
  fs.mkdirSync(path.join(installed, 'Intermediate', 'Build'), { recursive: true });
  fs.writeFileSync(path.join(installed, 'Intermediate', 'Build', 'stale.obj'), 'stale', 'utf-8');
  fs.mkdirSync(path.join(installed, 'Binaries', 'Win64'), { recursive: true });
  fs.writeFileSync(path.join(installed, 'Binaries', 'Win64', 'UnrealEditor-UnrealMcpEditor.dll'), 'stale', 'utf-8');
  const bridge = path.join(installed, 'Binaries', 'ThirdParty', 'UnrealMcpBridge', 'win-x64');
  fs.mkdirSync(bridge, { recursive: true });
  fs.writeFileSync(path.join(bridge, 'unreal-mcp-bridge.exe'), 'BRIDGE-PAYLOAD', 'utf-8');
  return installed;
}

describe('readPluginVersion', () => {
  it('reads VersionName, null on miss', () => {
    const dir = makeSource('0.2.0');
    expect(readPluginVersion(path.join(dir, 'UnrealMCP.uplugin'))).toBe('0.2.0');
    expect(readPluginVersion(path.join(dir, 'nope.uplugin'))).toBeNull();
  });
});

describe('update', () => {
  it('installs when not already present', async () => {
    const project = tmp();
    const source = makeSource('0.1.0');
    const r = await update({ projectDir: project, pluginSourceDir: source });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.updated).toBe(true);
    expect(r.fromVersion).toBeNull();
    expect(r.toVersion).toBe('0.1.0');
    expect(fs.existsSync(path.join(project, 'Plugins', 'UnrealMCP', 'UnrealMCP.uplugin'))).toBe(true);
  });

  it('is a no-op when versions match', async () => {
    const project = tmp();
    const source = makeSource('0.1.0');
    await update({ projectDir: project, pluginSourceDir: source });
    const r = await update({ projectDir: project, pluginSourceDir: source });
    expect(r.kind).toBe('success');
    if (r.kind === 'success') expect(r.updated).toBe(false);
  });

  it('installs when not present even if the source version is unreadable', async () => {
    // Regression: a not-installed plugin (fromVersion === null) against a
    // source whose UnrealMCP.uplugin is unreadable (toVersion === null) must
    // still install — `null !== null` is false, so the old guard short-
    // circuited to "already up to date" and never copied anything.
    const project = tmp();
    const source = tmp(); // a real dir, but with no UnrealMCP.uplugin
    fs.writeFileSync(path.join(source, 'README.md'), 'plugin contents', 'utf-8');
    const r = await update({ projectDir: project, pluginSourceDir: source });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.fromVersion).toBeNull();
    expect(r.toVersion).toBeNull();
    expect(r.updated).toBe(true);
    expect(r.warnings.some((w) => /could not read versionname/i.test(w))).toBe(true);
    expect(fs.existsSync(path.join(project, 'Plugins', 'UnrealMCP', 'README.md'))).toBe(true);
  });

  it('re-installs when versions differ', async () => {
    const project = tmp();
    await update({ projectDir: project, pluginSourceDir: makeSource('0.1.0') });
    const r = await update({ projectDir: project, pluginSourceDir: makeSource('0.2.0') });
    expect(r.kind).toBe('success');
    if (r.kind === 'success') {
      expect(r.fromVersion).toBe('0.1.0');
      expect(r.toVersion).toBe('0.2.0');
      expect(r.updated).toBe(true);
    }
  });

  it('does not download when the installed version already matches the CLI version', async () => {
    const project = tmp();
    await update({ projectDir: project, pluginSourceDir: makeSource(PACKAGE_VERSION) });
    const calls: string[] = [];
    const r = await update({
      projectDir: project,
      fetchImpl: (async (url: string) => {
        calls.push(url);
        throw new Error('fetch should not have been called');
      }) as typeof fetch,
    });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.updated).toBe(false);
    expect(calls).toEqual([]);
  });

  it('downloads the dedicated source asset when no local source exists and the installed version differs', async () => {
    const project = tmp();
    await update({ projectDir: project, pluginSourceDir: makeSource('0.1.0') });
    const calls: string[] = [];
    const r = await update({
      projectDir: project,
      fetchImpl: zipResponse(
        {
          'UnrealMCP/UnrealMCP.uplugin': strToU8(JSON.stringify({ VersionName: PACKAGE_VERSION, Installed: false })),
          'UnrealMCP/Source/marker.txt': strToU8('downloaded'),
          'UnrealMCP/Source/ThirdParty/UnrealMcpBridge/win-x64/unreal-mcp-bridge.exe': strToU8('BRIDGE'),
        },
        calls,
      ),
    });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.updated).toBe(true);
    expect(r.fromVersion).toBe('0.1.0');
    expect(r.toVersion).toBe(PACKAGE_VERSION);
    expect(fs.existsSync(path.join(project, 'Plugins', 'UnrealMCP', 'Source', 'marker.txt'))).toBe(true);
    expect(
      fs.existsSync(
        path.join(project, 'Plugins', 'UnrealMCP', 'Binaries', 'ThirdParty', 'UnrealMcpBridge', 'win-x64', 'unreal-mcp-bridge.exe'),
      ),
    ).toBe(true);
    expect(calls.some((url) => /unreal-mcp-plugin-source-/.test(url))).toBe(true);
  });

  // --- issue #58: auto-clean stale build cache on update -------------------

  it('on a version-change copy update, wipes Intermediate/+C++ Binaries/ and preserves the bundled bridge', async () => {
    const project = tmp();
    const installed = seedInstalledWithCache(project, '0.1.0');
    const r = await update({ projectDir: project, pluginSourceDir: makeSource('0.2.0') });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.updated).toBe(true);
    expect(r.cleaned).toBe(true);
    // New source landed.
    expect(readPluginVersion(path.join(installed, 'UnrealMCP.uplugin'))).toBe('0.2.0');
    // Stale C++ cache gone.
    expect(fs.existsSync(path.join(installed, 'Intermediate'))).toBe(false);
    expect(fs.existsSync(path.join(installed, 'Binaries', 'Win64'))).toBe(false);
    // Bundled bridge SURVIVED both the re-copy and the clean, payload intact.
    const bridgeExe = path.join(installed, 'Binaries', 'ThirdParty', 'UnrealMcpBridge', 'win-x64', 'unreal-mcp-bridge.exe');
    expect(fs.existsSync(bridgeExe)).toBe(true);
    expect(fs.readFileSync(bridgeExe, 'utf-8')).toBe('BRIDGE-PAYLOAD');
  });

  it('a SOURCE that ships a stale cache never lands it in the install (issue #73 copy filter)', async () => {
    // Since #73, installPlugin's copy EXCLUDES Intermediate/+Binaries/<platform>
    // (keeping only Binaries/ThirdParty), so even a NEW SOURCE that ships the
    // stale cache cannot reintroduce it into the install. The explicit clean on
    // top then has nothing left to do — the cache is gone either way.
    const project = tmp();
    seedInstalledWithCache(project, '0.1.0');
    const installed = path.join(project, 'Plugins', 'UnrealMCP');
    const r = await update({ projectDir: project, pluginSourceDir: makeSourceWithCache('0.2.0') });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.cleaned).toBe(true);
    // No stale cache in the install: the copy filter dropped it before any clean.
    expect(fs.existsSync(path.join(installed, 'Intermediate'))).toBe(false);
    expect(fs.existsSync(path.join(installed, 'Binaries', 'Win64'))).toBe(false);
  });

  it('--no-clean still keeps the source-shipped cache out (the copy filter, not the clean, removes it since #73)', async () => {
    // Before #73 the copy landed the source's cache and only the explicit clean
    // removed it. Now the copy filter excludes it up front, so even with noClean
    // (no explicit clean step) the install has no stale Intermediate/+Binaries/Win64.
    const project = tmp();
    seedInstalledWithCache(project, '0.1.0');
    const installed = path.join(project, 'Plugins', 'UnrealMCP');
    const r = await update({ projectDir: project, pluginSourceDir: makeSourceWithCache('0.2.0'), noClean: true });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.cleaned).toBe(false);
    // The copy filter kept the cache out even though no explicit clean ran.
    expect(fs.existsSync(path.join(installed, 'Intermediate'))).toBe(false);
    expect(fs.existsSync(path.join(installed, 'Binaries', 'Win64'))).toBe(false);
  });

  it('--no-clean (noClean) skips the explicit clean step but still preserves the bridge', async () => {
    const project = tmp();
    const installed = seedInstalledWithCache(project, '0.1.0');
    const r = await update({ projectDir: project, pluginSourceDir: makeSource('0.2.0'), noClean: true });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.updated).toBe(true);
    // The opt-out: no explicit clean was performed this update.
    expect(r.cleaned).toBe(false);
    // The bundled bridge is preserved across the re-copy regardless of noClean
    // (installPlugin stashes+restores it; only the explicit clean is gated).
    const bridgeExe = path.join(installed, 'Binaries', 'ThirdParty', 'UnrealMcpBridge', 'win-x64', 'unreal-mcp-bridge.exe');
    expect(fs.existsSync(bridgeExe)).toBe(true);
    expect(fs.readFileSync(bridgeExe, 'utf-8')).toBe('BRIDGE-PAYLOAD');
  });

  it('junction (dev) install is NOT cleaned — the live source build outputs survive', async () => {
    if (process.platform !== 'win32') return; // junction install path is Windows-only
    const project = tmp();
    fs.mkdirSync(path.join(project, 'Plugins'), { recursive: true });
    // Live dev source carrying its own build cache.
    const source = tmp();
    fs.writeFileSync(path.join(source, 'UnrealMCP.uplugin'), JSON.stringify({ VersionName: '0.1.0' }), 'utf-8');
    fs.mkdirSync(path.join(source, 'Intermediate'), { recursive: true });
    fs.writeFileSync(path.join(source, 'Intermediate', 'live.obj'), 'live', 'utf-8');
    fs.mkdirSync(path.join(source, 'Binaries', 'Win64'), { recursive: true });
    fs.writeFileSync(path.join(source, 'Binaries', 'Win64', 'live.dll'), 'live', 'utf-8');
    const link = path.join(project, 'Plugins', 'UnrealMCP');
    try {
      fs.symlinkSync(source, link, 'junction');
    } catch {
      return; // privilege-gated symlink creation — skip gracefully
    }
    // force an update against a 0.2.0 source while the install is a junction.
    const newSource = makeSource('0.2.0');
    const r = await update({ projectDir: project, pluginSourceDir: newSource, force: true });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.cleaned).toBe(false);
    // The dev source's build outputs are untouched.
    expect(fs.existsSync(path.join(source, 'Intermediate', 'live.obj'))).toBe(true);
    expect(fs.existsSync(path.join(source, 'Binaries', 'Win64', 'live.dll'))).toBe(true);
  });
});
