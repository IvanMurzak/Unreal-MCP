import { describe, it, expect, afterEach } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import { getStatus } from '../src/lib/status.js';
import { makeTempDir, rmTempDir, writeUProject, fakeResponse } from './helpers.js';

const dirs: string[] = [];
afterEach(() => {
  while (dirs.length) rmTempDir(dirs.pop()!);
});
function tmp(): string {
  const d = makeTempDir();
  dirs.push(d);
  return d;
}

describe('getStatus', () => {
  it('returns package identity with no project', async () => {
    const r = await getStatus({ noProbe: true });
    expect(r.name).toBe('unreal-cli');
    expect(r.version).toMatch(/^\d+\.\d+\.\d+$/);
    expect(r.project).toBeUndefined();
  });

  it('reports project + plugin state and resolves a deterministic-port connection', async () => {
    const dir = tmp();
    writeUProject(dir, 'MyGame', '5.7');
    fs.mkdirSync(path.join(dir, 'Plugins', 'UnrealMCP'), { recursive: true });
    fs.writeFileSync(path.join(dir, 'Plugins', 'UnrealMCP', 'UnrealMCP.uplugin'), '{}', 'utf-8');

    const r = await getStatus({ projectDir: dir, noProbe: true });
    expect(r.project?.projectName).toBe('MyGame');
    expect(r.project?.engineAssociation).toBe('5.7');
    expect(r.project?.pluginInstalled).toBe(true);
    expect(r.connection.source).toBe('deterministic-port');
    expect(r.connection.url).toMatch(/^http:\/\/localhost:\d+$/);
  });

  it('probes reachability when not disabled', async () => {
    const dir = tmp();
    writeUProject(dir, 'MyGame', '5.7');
    const fetchImpl = (async () => fakeResponse({ ok: true, status: 200, body: '{}' })) as unknown as typeof fetch;
    const r = await getStatus({ projectDir: dir, fetchImpl, probeTimeoutMs: 1000 });
    expect(r.reachable).toBe(true);
  });

  it('reports unreachable on connection refused', async () => {
    const dir = tmp();
    writeUProject(dir, 'MyGame', '5.7');
    const fetchImpl = (async () => {
      const err = new Error('refused');
      (err as Error & { cause?: unknown }).cause = { code: 'ECONNREFUSED' };
      throw err;
    }) as unknown as typeof fetch;
    const r = await getStatus({ projectDir: dir, fetchImpl, probeTimeoutMs: 1000 });
    expect(r.reachable).toBe(false);
    expect(r.probeReason).toBe('connection refused');
  });
});
