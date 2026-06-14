import { describe, it, expect, afterEach } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import { installPlugin, removePlugin } from '../src/lib/install-plugin.js';
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

function makePluginSource(): string {
  const dir = tmp();
  fs.writeFileSync(path.join(dir, 'UnrealMCP.uplugin'), JSON.stringify({ VersionName: '0.1.0' }), 'utf-8');
  fs.mkdirSync(path.join(dir, 'Source'), { recursive: true });
  fs.writeFileSync(path.join(dir, 'Source', 'marker.txt'), 'hello', 'utf-8');
  return dir;
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
