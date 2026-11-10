import { afterEach, describe, expect, it } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import { filterTools, listTools, parseToolList, readToolsCache, toolsCacheKey, toolsCachePath } from '../src/lib/tools.js';
import type { RunToolSessionOptions, RunToolSessionResult } from '../src/lib/run-tool-session.js';
import { makeTempDir, rmTempDir, writeUProject } from './helpers.js';

const dirs: string[] = [];
afterEach(() => {
  while (dirs.length) rmTempDir(dirs.pop()!);
});

function project(): string {
  const dir = makeTempDir();
  dirs.push(dir);
  writeUProject(dir, 'Game');
  const plugin = path.join(dir, 'Plugins', 'UnrealMCP');
  fs.mkdirSync(plugin, { recursive: true });
  fs.writeFileSync(path.join(plugin, 'UnrealMCP.uplugin'), JSON.stringify({ VersionName: '0.30.0' }));
  return dir;
}

const RESPONSE = {
  status: 'success',
  structured: {
    tools: [
      { name: 'rql-query', title: 'RQL Query', visible: true, extensionId: 'core', description: 'Read the editor with RQL', inputs: 'query: string (required)' },
      { name: 'python-execute', title: 'Python', visible: false, extensionId: 'core', description: 'Run Python' },
      { name: 'niagara-spawn', title: 'Spawn Niagara', visible: true, extensionId: 'UnrealAINiagara' },
    ],
    availableExtensions: [{ id: 'UnrealAIAudio', state: 'not-installed', line: 'UnrealAIAudio: audio tools (install-extension ...)' }],
  },
};

function fakeRun(response: unknown = RESPONSE) {
  const calls: RunToolSessionOptions[] = [];
  const run = async (o: RunToolSessionOptions): Promise<RunToolSessionResult> => {
    calls.push(o);
    return { result: { kind: 'success', success: true, endpoint: 'http://localhost:1/api/tools/unreal-tool-list', httpStatus: 200, data: response }, source: 'started' };
  };
  return { calls, run };
}

describe('tools', () => {
  it('asks the editor (descriptions and inputs) and caches the list for the installed plugins', async () => {
    const dir = project();
    const { calls, run } = fakeRun();
    const r = await listTools({ projectDir: dir, run });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.source).toBe('editor');
    expect(calls[0]).toMatchObject({ toolName: 'unreal-tool-list', input: { includeDescription: true, includeInputs: true } });
    expect(r.cache.tools.map((t) => t.name)).toEqual(['rql-query', 'python-execute', 'niagara-spawn']);
    expect(r.cache.tools[1]!.visible).toBe(false);
    expect(r.cache.tools[0]!.extensionId).toBeUndefined(); // core
    expect(readToolsCache(dir)?.key).toBe(toolsCacheKey(dir));
    expect(fs.readFileSync(path.join(dir, '.ai-game-dev', '.gitignore'), 'utf-8')).toContain('tools.json');
  });

  it('answers from the cache with the editor closed; --refresh asks again; a changed plugin set invalidates it', async () => {
    const dir = project();
    await listTools({ projectDir: dir, run: fakeRun().run });
    const second = fakeRun();
    const cached = await listTools({ projectDir: dir, run: second.run });
    expect(cached).toMatchObject({ kind: 'success', source: 'cache', cachePath: toolsCachePath(dir) });
    expect(second.calls).toEqual([]);
    expect((await listTools({ projectDir: dir, refresh: true, run: second.run })).kind).toBe('success');
    expect(second.calls).toHaveLength(1);
    fs.writeFileSync(path.join(dir, 'Plugins', 'UnrealMCP', 'UnrealMCP.uplugin'), JSON.stringify({ VersionName: '0.31.0' }));
    const third = fakeRun();
    expect(await listTools({ projectDir: dir, run: third.run })).toMatchObject({ source: 'editor' });
  });

  it('no tool list from the editor: a failure carrying the session outcome', async () => {
    const run = async (): Promise<RunToolSessionResult> => ({ sessionFailure: { message: 'no editor', reason: 'not-ready' } });
    const r = await listTools({ projectDir: project(), run });
    expect(r).toMatchObject({ kind: 'failure', session: { sessionFailure: { reason: 'not-ready' } } });
  });

  it('parses and filters rows', () => {
    expect(parseToolList({ status: 'success' })).toBeUndefined();
    const { tools } = parseToolList(RESPONSE)!;
    expect(filterTools(tools, 'rql').map((t) => t.name)).toEqual(['rql-query']);
    expect(filterTools(tools, 'Python').map((t) => t.name)).toEqual(['python-execute']);
    expect(filterTools([...tools, { name: 'x', title: 'X', visible: true, description: 'calls rql-query' }], 'rql-query').map((t) => t.name))
      .toEqual(['rql-query']); // the exact name wins over mentions
    expect(filterTools(tools, 'python|niagara').map((t) => t.name)).toEqual(['python-execute', 'niagara-spawn']);
    expect(filterTools(tools, '(unclosed').map((t) => t.name)).toEqual([]); // a bad regex is a substring
  });
});
