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
