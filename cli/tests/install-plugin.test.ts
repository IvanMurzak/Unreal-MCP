import { describe, it, expect, afterEach } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import { zipSync, strToU8 } from 'fflate';
import { installPlugin, removePlugin } from '../src/lib/install-plugin.js';
import { PACKAGE_VERSION } from '../src/version.js';
import { makeTempDir, rmTempDir, writeUProject } from './helpers.js';

const dirs: string[] = [];
afterEach(() => {
  while (dirs.length) rmTempDir(dirs.pop()!);
});
function tmp(): string {
  const d = makeTempDir();
  dirs.push(d);
  return d;
}

function makePluginSource(): string {
  const dir = tmp();
  fs.writeFileSync(path.join(dir, 'UnrealMCP.uplugin'), JSON.stringify({ VersionName: '0.1.0' }), 'utf-8');
  fs.mkdirSync(path.join(dir, 'Source'), { recursive: true });
  fs.writeFileSync(path.join(dir, 'Source', 'marker.txt'), 'hello', 'utf-8');
  return dir;
}

function zipResponse(entries: Record<string, Uint8Array>): typeof fetch {
  const bytes = zipSync(entries);
  return (async () =>
    ({
      ok: true,
      status: 200,
      statusText: 'OK',
      arrayBuffer: async () => bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength),
    }) as unknown as Response) as typeof fetch;
}

describe('installPlugin (copy)', () => {
  it('copies the plugin source into <project>/Plugins/UnrealMCP', async () => {
    const project = tmp();
    const source = makePluginSource();
    const r = await installPlugin({ projectDir: project, pluginSourceDir: source });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.mode).toBe('copy');
    expect(fs.existsSync(path.join(r.installedPath, 'UnrealMCP.uplugin'))).toBe(true);
    expect(fs.existsSync(path.join(r.installedPath, 'Source', 'marker.txt'))).toBe(true);
  });

  it('is idempotent — a second install replaces the prior one', async () => {
    const project = tmp();
    const source = makePluginSource();
    await installPlugin({ projectDir: project, pluginSourceDir: source });
    const r = await installPlugin({ projectDir: project, pluginSourceDir: source });
    expect(r.kind).toBe('success');
  });

  it('fails (no throw) when the plugin source is missing', async () => {
    const project = tmp();
    const r = await installPlugin({ projectDir: project, pluginSourceDir: path.join(tmp(), 'nope') });
    expect(r.kind).toBe('failure');
  });

  it('excludes the source checkout\'s stale Win64/Intermediate build cache but keeps the sidecar (issue #73)', async () => {
    const project = tmp();
    const source = makePluginSource();
    // Extra kept content alongside the .uplugin / Source.
    fs.mkdirSync(path.join(source, 'Resources'), { recursive: true });
    fs.writeFileSync(path.join(source, 'Resources', 'Icon.png'), 'PNG', 'utf-8');
    // The bundled sidecar — SHOULD be copied.
    const bridge = path.join(source, 'Binaries', 'ThirdParty', 'UnrealMcpBridge', 'win-x64');
    fs.mkdirSync(bridge, { recursive: true });
    fs.writeFileSync(path.join(bridge, 'unreal-mcp-bridge.exe'), 'BRIDGE', 'utf-8');
    // Stale dev build cache — should NOT be copied.
    const win64 = path.join(source, 'Binaries', 'Win64');
    fs.mkdirSync(win64, { recursive: true });
    fs.writeFileSync(path.join(win64, 'UnrealEditor-UnrealMcpEditor.dll'), 'DLL', 'utf-8');
    fs.writeFileSync(path.join(win64, 'UnrealEditor.modules'), '{}', 'utf-8');
    const inter = path.join(source, 'Intermediate', 'Build', 'Win64');
    fs.mkdirSync(inter, { recursive: true });
    fs.writeFileSync(path.join(inter, 'x.obj'), 'OBJ', 'utf-8');

    const r = await installPlugin({ projectDir: project, pluginSourceDir: source });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    const p = r.installedPath;
    // Kept:
    expect(fs.existsSync(path.join(p, 'UnrealMCP.uplugin'))).toBe(true);
    expect(fs.existsSync(path.join(p, 'Source', 'marker.txt'))).toBe(true);
    expect(fs.existsSync(path.join(p, 'Resources', 'Icon.png'))).toBe(true);
    expect(
      fs.existsSync(path.join(p, 'Binaries', 'ThirdParty', 'UnrealMcpBridge', 'win-x64', 'unreal-mcp-bridge.exe')),
    ).toBe(true);
    // Excluded:
    expect(fs.existsSync(path.join(p, 'Binaries', 'Win64'))).toBe(false);
    expect(fs.existsSync(path.join(p, 'Intermediate'))).toBe(false);
  });

  it('preserves a previously-bundled sidecar bridge across a copy re-install (issue #58)', async () => {
    // First install ships a release plugin WITH the bundled bridge.
    const project = tmp();
    const released = makePluginSource();
    const bridge = path.join(released, 'Binaries', 'ThirdParty', 'UnrealMcpBridge', 'win-x64');
    fs.mkdirSync(bridge, { recursive: true });
    fs.writeFileSync(path.join(bridge, 'unreal-mcp-bridge.exe'), 'BRIDGE-PAYLOAD', 'utf-8');
    await installPlugin({ projectDir: project, pluginSourceDir: released });

    // Re-install from a SOURCE checkout that has NO bundled bridge.
    const sourceCheckout = makePluginSource();
    const r = await installPlugin({ projectDir: project, pluginSourceDir: sourceCheckout });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    const installedBridge = path.join(r.installedPath, 'Binaries', 'ThirdParty', 'UnrealMcpBridge', 'win-x64', 'unreal-mcp-bridge.exe');
    expect(fs.existsSync(installedBridge)).toBe(true);
    expect(fs.readFileSync(installedBridge, 'utf-8')).toBe('BRIDGE-PAYLOAD');
  });

  it('downloads the dedicated source asset from GitHub when no local source is available', async () => {
    const project = tmp();
    writeUProject(project, 'DownloadInstall');
    const fetchImpl = zipResponse({
      'UnrealMCP/UnrealMCP.uplugin': strToU8(JSON.stringify({ VersionName: PACKAGE_VERSION, Installed: false })),
      'UnrealMCP/Source/marker.txt': strToU8('downloaded'),
      'UnrealMCP/Source/ThirdParty/UnrealMcpBridge/win-x64/unreal-mcp-bridge.exe': strToU8('BRIDGE'),
    });
    const r = await installPlugin({ projectDir: project, fetchImpl });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.mode).toBe('copy');
    expect(fs.existsSync(path.join(r.installedPath, 'UnrealMCP.uplugin'))).toBe(true);
    expect(fs.existsSync(path.join(r.installedPath, 'Source', 'marker.txt'))).toBe(true);
    const descriptor = JSON.parse(
      fs.readFileSync(path.join(r.installedPath, 'UnrealMCP.uplugin'), 'utf-8'),
    ) as Record<string, unknown>;
    expect(descriptor['EngineVersion']).toBeUndefined();
    expect(
      fs.existsSync(
        path.join(r.installedPath, 'Source', 'ThirdParty', 'UnrealMcpBridge', 'win-x64', 'unreal-mcp-bridge.exe'),
      ),
    ).toBe(true);
    expect(
      fs.existsSync(
        path.join(r.installedPath, 'Binaries', 'ThirdParty', 'UnrealMcpBridge', 'win-x64', 'unreal-mcp-bridge.exe'),
      ),
    ).toBe(true);
  });

  it('falls back to copy when --junction targets a downloaded GitHub release', async () => {
    const project = tmp();
    writeUProject(project, 'DownloadJunction');
    const fetchImpl = zipResponse({
      'UnrealMCP/UnrealMCP.uplugin': strToU8(JSON.stringify({ VersionName: PACKAGE_VERSION, Installed: false })),
      'UnrealMCP/Source/marker.txt': strToU8('downloaded'),
    });
    const r = await installPlugin({ projectDir: project, junction: true, fetchImpl });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.mode).toBe('copy');
    expect(r.warnings.some((w) => /stable local plugin source/i.test(w))).toBe(true);
  });

  it('refreshes a legacy packaged install with the source-asset bridge payload instead of restoring the stale bridge', async () => {
    const project = tmp();
    const legacy = makePluginSource();
    const oldBridge = path.join(legacy, 'Binaries', 'ThirdParty', 'UnrealMcpBridge', 'win-x64');
    fs.mkdirSync(oldBridge, { recursive: true });
    fs.writeFileSync(path.join(oldBridge, 'unreal-mcp-bridge.exe'), 'OLD-BRIDGE', 'utf-8');
    await installPlugin({ projectDir: project, pluginSourceDir: legacy });

    const sourceAsset = makePluginSource();
    const newBridge = path.join(sourceAsset, 'Source', 'ThirdParty', 'UnrealMcpBridge', 'win-x64');
    fs.mkdirSync(newBridge, { recursive: true });
    fs.writeFileSync(path.join(newBridge, 'unreal-mcp-bridge.exe'), 'NEW-BRIDGE', 'utf-8');

    const r = await installPlugin({ projectDir: project, pluginSourceDir: sourceAsset });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    const installedBridge = path.join(
      r.installedPath,
      'Binaries',
      'ThirdParty',
      'UnrealMcpBridge',
      'win-x64',
      'unreal-mcp-bridge.exe',
    );
    expect(fs.existsSync(installedBridge)).toBe(true);
    expect(fs.readFileSync(installedBridge, 'utf-8')).toBe('NEW-BRIDGE');
  });
});

describe('removePlugin', () => {
  it('removes an installed plugin', async () => {
    const project = tmp();
    const source = makePluginSource();
    await installPlugin({ projectDir: project, pluginSourceDir: source });
    const r = await removePlugin({ projectDir: project });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.removed).toBe(true);
    expect(fs.existsSync(r.installedPath)).toBe(false);
  });

  it('reports removed=false when nothing is installed', async () => {
    const project = tmp();
    const r = await removePlugin({ projectDir: project });
    expect(r.kind).toBe('success');
    if (r.kind === 'success') expect(r.removed).toBe(false);
  });
});

describe('installPlugin --with-server', () => {
  it('downloads the server after the plugin install and records its path', async () => {
    const project = tmp();
    const source = makePluginSource();
    let called: unknown;
    const r = await installPlugin({
      projectDir: project,
      pluginSourceDir: source,
      withServer: true,
      serverVersion: '9.0.0',
      downloadServerImpl: async (opts) => {
        called = opts;
        return { kind: 'success', success: true, serverPath: '/managed/gamedev-mcp-server.exe', source: 'download', version: '9.0.0', warnings: [] };
      },
    });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.serverPath).toBe('/managed/gamedev-mcp-server.exe');
    expect(r.serverVersion).toBe('9.0.0');
    expect((called as { projectDir: string }).projectDir).toBe(project);
    expect((called as { version?: string }).version).toBe('9.0.0');
  });

  it('forwards --server-source to the download resolver', async () => {
    const project = tmp();
    const source = makePluginSource();
    let seenSource: string | undefined;
    await installPlugin({
      projectDir: project,
      pluginSourceDir: source,
      withServer: true,
      serverSource: '/local/server.zip',
      downloadServerImpl: async (opts) => {
        seenSource = opts.source;
        return { kind: 'success', success: true, serverPath: '/x', source: 'source', version: '9.0.0', warnings: [] };
      },
    });
    expect(seenSource).toBe('/local/server.zip');
  });

  it('degrades a server-download failure to a warning (plugin still installed)', async () => {
    const project = tmp();
    const source = makePluginSource();
    const r = await installPlugin({
      projectDir: project,
      pluginSourceDir: source,
      withServer: true,
      downloadServerImpl: async () => ({ kind: 'failure', success: false, warnings: [], error: new Error('offline') }),
    });
    expect(r.kind).toBe('success');
    if (r.kind === 'success') {
      expect(r.serverPath).toBeUndefined();
      expect(r.warnings.join(' ')).toMatch(/--with-server/);
    }
  });
});

describe('installPlugin --enroll', () => {
  it('redeems the code after the plugin install and records enrollment fields', async () => {
    const project = tmp();
    const source = makePluginSource();
    let seenCode: string | undefined;
    const r = await installPlugin({
      projectDir: project,
      pluginSourceDir: source,
      enrollCode: 'ABCD-1234',
      enrollImpl: async (opts) => {
        seenCode = opts.enrollCode;
        return {
          kind: 'success',
          success: true,
          token: 't',
          serverTarget: 'https://ai-game.dev',
          credentialPath: '/store/credentials.json',
          markerPath: path.join(opts.projectDir, '.ai-game-dev', 'project.json'),
          pin: 'abcd1234',
          pinnedConfigFiles: [path.join(opts.projectDir, '.mcp.json')],
          warnings: [],
        };
      },
    });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(seenCode).toBe('ABCD-1234');
    expect(r.enrolled).toBe(true);
    expect(r.serverTarget).toBe('https://ai-game.dev');
    expect(r.pin).toBe('abcd1234');
  });

  it('surfaces an enroll failure as an overall failure (noting the plugin was installed)', async () => {
    const project = tmp();
    const source = makePluginSource();
    const r = await installPlugin({
      projectDir: project,
      pluginSourceDir: source,
      enrollCode: 'spent',
      enrollImpl: async () => ({ kind: 'failure', success: false, reason: 'invalid_code', error: new Error('bad code') }),
    });
    expect(r.kind).toBe('failure');
    if (r.kind === 'failure') {
      expect(r.error.message).toMatch(/Plugin installed/);
      expect(r.error.message).toMatch(/enrollment failed/i);
    }
    // The plugin was still materialized on disk.
    expect(fs.existsSync(path.join(project, 'Plugins', 'UnrealMCP', 'UnrealMCP.uplugin'))).toBe(true);
  });
});
