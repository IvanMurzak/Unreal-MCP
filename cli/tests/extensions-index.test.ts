import { afterEach, describe, expect, it } from 'vitest';
import * as fs from 'node:fs';
import * as path from 'node:path';
import { createHash } from 'node:crypto';
import { zipSync } from 'fflate';
import { makeTempDir, rmTempDir, writeUProject } from './helpers.js';
import { EXTENSIONS_INDEX_URL, loadExtensionsIndex, parseExtensionsIndex } from '../src/utils/extensions-index.js';
import { installExtension } from '../src/lib/install-extension.js';

const dirs: string[] = [];
function temp() { const d = makeTempDir(); dirs.push(d); return d; }
afterEach(() => { while (dirs.length) rmTempDir(dirs.pop()!); });

function fixture() {
  return { schemaVersion: 1, extensions: [{
    extensionId: 'com.test.new-extension', name: 'New Tools', description: 'Newly published',
    pluginName: 'UnrealAINew', repo: 'IvanMurzak/Unreal-MCP', version: '1.2.0',
    minCoreVersion: '0.20.2', enginePlugins: ['Niagara'], tools: [],
    releases: {
      '1.1.0': { repo: 'IvanMurzak/Unreal-MCP', tag: 'new-v1.1.0' },
      '1.2.0': { repo: 'IvanMurzak/Unreal-MCP', tag: 'new-v1.2.0' },
    },
  }] };
}

function online(index: object, archiveVersion?: string) {
  const requests: string[] = [];
  const fetchImpl = (async (input: string | URL | Request) => {
    const url = String(input);
    requests.push(url);
    if (url === EXTENSIONS_INDEX_URL) return new Response(JSON.stringify(index));
    const m = /\/new-v([\d.]+)\/UnrealAINew-([\d.]+)\.zip$/.exec(url);
    if (!m || m[1] !== m[2]) return new Response('', { status: 404 });
    const bytes = zipSync({
      'UnrealAINew/UnrealAINew.uplugin': new TextEncoder().encode(JSON.stringify({ FileVersion: 3, VersionName: archiveVersion ?? m[1] })),
      'UnrealAINew/Source/marker.cpp': new TextEncoder().encode('// installed'),
    });
    return new Response(bytes as unknown as BodyInit);
  }) as typeof fetch;
  return { requests, fetchImpl };
}

describe('independent component index', () => {
  it('installs a newly added extension without a CLI/catalog release and never asks for repository latest', async () => {
    const project = temp(); writeUProject(project, 'Game', '5.8');
    const github = online(fixture());
    const result = await installExtension({ projectDir: project, extensionId: 'UnrealAINew',
      fetchImpl: github.fetchImpl, indexCachePath: path.join(temp(), 'index.json') });
    expect(result.kind).toBe('success');
    if (result.kind !== 'success') throw result.error;
    expect(result.toVersion).toBe('1.2.0');
    expect(result.versionSource).toBe('index');
    expect(fs.existsSync(path.join(project, 'Plugins/UnrealAINew/Source/marker.cpp'))).toBe(true);
    expect(github.requests).toEqual([EXTENSIONS_INDEX_URL,
      'https://github.com/IvanMurzak/Unreal-MCP/releases/download/new-v1.2.0/UnrealAINew-1.2.0.zip']);
  });

  it('installs an explicit older version from its own historical tag', async () => {
    const project = temp(); writeUProject(project, 'Game', '5.8');
    const github = online(fixture());
    const result = await installExtension({ projectDir: project, extensionId: 'UnrealAINew', version: '1.1.0',
      fetchImpl: github.fetchImpl, indexCachePath: path.join(temp(), 'index.json') });
    expect(result.kind === 'success' && result.toVersion).toBe('1.1.0');
    expect(github.requests[1]).toContain('/new-v1.1.0/');
  });

  it('uses the selected historical version dependencies instead of latest dependencies', async () => {
    const project = temp(); writeUProject(project, 'Game', '5.8');
    const index = fixture();
    Object.assign(index.extensions[0].releases['1.1.0'], { enginePlugins: [], minCoreVersion: null });
    const result = await installExtension({ projectDir: project, extensionId: 'UnrealAINew', version: '1.1.0',
      fetchImpl: online(index).fetchImpl, indexCachePath: path.join(temp(), 'index.json') });
    expect(result.kind).toBe('success');
    const plugins = JSON.parse(fs.readFileSync(path.join(project, 'Game.uproject'), 'utf8')).Plugins;
    expect(plugins.some((p: { Name: string }) => p.Name === 'Niagara')).toBe(false);
  });

  it('rejects a wrong archive version before replacing the installed plugin', async () => {
    const project = temp(); writeUProject(project, 'Game', '5.8');
    const installed = path.join(project, 'Plugins/UnrealAINew'); fs.mkdirSync(installed, { recursive: true });
    const descriptor = path.join(installed, 'UnrealAINew.uplugin');
    fs.writeFileSync(descriptor, JSON.stringify({ VersionName: '1.1.0' }));
    const result = await installExtension({ projectDir: project, extensionId: 'UnrealAINew',
      fetchImpl: online(fixture(), '9.9.9').fetchImpl, indexCachePath: path.join(temp(), 'index.json') });
    expect(result.kind).toBe('failure');
    expect(JSON.parse(fs.readFileSync(descriptor, 'utf8')).VersionName).toBe('1.1.0');
  });

  it('rejects an unpublished explicit version without downloading another asset', async () => {
    const project = temp(); writeUProject(project, 'Game', '5.8');
    const github = online(fixture());
    const result = await installExtension({ projectDir: project, extensionId: 'UnrealAINew', version: '9.0.0',
      fetchImpl: github.fetchImpl, indexCachePath: path.join(temp(), 'index.json') });
    expect(result.kind).toBe('failure');
    expect(github.requests).toEqual([EXTENSIONS_INDEX_URL]);
  });

  it('keeps an installed newer version even with --force', async () => {
    const project = temp(); writeUProject(project, 'Game', '5.8');
    const installed = path.join(project, 'Plugins/UnrealAINew'); fs.mkdirSync(installed, { recursive: true });
    fs.writeFileSync(path.join(installed, 'UnrealAINew.uplugin'), JSON.stringify({ VersionName: '2.0.0' }));
    const github = online(fixture());
    const result = await installExtension({ projectDir: project, extensionId: 'UnrealAINew', force: true,
      fetchImpl: github.fetchImpl, indexCachePath: path.join(temp(), 'index.json') });
    expect(result.kind === 'success' && result.versionSource).toBe('installed');
    expect(github.requests).toEqual([EXTENSIONS_INDEX_URL]);
  });

  it('uses a validated cache with a visible stale-version warning', async () => {
    const cachePath = path.join(temp(), 'index.json');
    await loadExtensionsIndex({ cachePath, fetchImpl: online(fixture()).fetchImpl });
    const result = await loadExtensionsIndex({ cachePath, fetchImpl: (async () => { throw new Error('offline'); }) as typeof fetch });
    expect(result.index?.extensions[0].version).toBe('1.2.0');
    expect(result.warning).toContain('Newer releases may exist');
  });

  it('does not poison a good cache with an invalid response', async () => {
    const cachePath = path.join(temp(), 'index.json');
    const fetchImpl = online(fixture()).fetchImpl;
    await loadExtensionsIndex({ cachePath, fetchImpl });
    const result = await loadExtensionsIndex({ cachePath, fetchImpl: online({ schemaVersion: 999 }).fetchImpl });
    expect(result.index?.extensions).toHaveLength(1);
    expect(JSON.parse(fs.readFileSync(cachePath, 'utf8')).schemaVersion).toBe(1);
  });

  it('checks the archive hash before changing an existing installation', async () => {
    const project = temp(); writeUProject(project, 'Game', '5.8');
    const index = fixture();
    Object.assign(index.extensions[0].releases['1.2.0'], { sha256: createHash('sha256').update('wrong').digest('hex') });
    const installed = path.join(project, 'Plugins/UnrealAINew'); fs.mkdirSync(installed, { recursive: true });
    const descriptor = path.join(installed, 'UnrealAINew.uplugin');
    fs.writeFileSync(descriptor, JSON.stringify({ VersionName: '1.1.0' }));
    const before = fs.readFileSync(descriptor, 'utf8');
    const result = await installExtension({ projectDir: project, extensionId: 'UnrealAINew',
      fetchImpl: online(index).fetchImpl, indexCachePath: path.join(temp(), 'index.json') });
    expect(result.kind).toBe('failure');
    expect(fs.readFileSync(descriptor, 'utf8')).toBe(before);
  });

  it.each(['../Escape', 'Bad/Name', 'C:Drive'])('rejects unsafe plugin name %s', pluginName => {
    const index = fixture(); index.extensions[0].pluginName = pluginName;
    expect(() => parseExtensionsIndex(index)).toThrow();
  });

  it('rejects duplicate identities and foreign release repositories', () => {
    const duplicate = fixture(); duplicate.extensions.push(duplicate.extensions[0]);
    expect(() => parseExtensionsIndex(duplicate)).toThrow();
    const foreign = fixture(); foreign.extensions[0].releases['1.1.0'].repo = 'attacker/Unreal-MCP';
    expect(() => parseExtensionsIndex(foreign)).toThrow();
  });
});
