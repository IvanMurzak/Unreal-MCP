import { describe, it, expect, afterEach } from 'vitest';
import { openProject, buildOpenEnv } from '../src/lib/open.js';
import { editorBinaryPath } from '../src/utils/engine.js';
import type { EngineInstallation } from '../src/utils/launcher.js';
import * as fs from 'fs';
import * as path from 'path';
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

/**
 * Build a fake engine install whose UnrealEditor binary actually exists on
 * disk (for the host platform), so resolveEngine's existence check passes
 * deterministically regardless of what is installed on the test machine.
 */
function fakeEngine(association: string): EngineInstallation {
  const root = tmp();
  const bin = editorBinaryPath(root, process.platform);
  fs.mkdirSync(path.dirname(bin), { recursive: true });
  fs.writeFileSync(bin, '', 'utf-8');
  return { appName: `UE_${association}`, appVersion: `${association}.0`, installLocation: root, engineAssociation: association };
}

describe('buildOpenEnv', () => {
  it('maps options to UNREAL_MCP_* vars', () => {
    const env = buildOpenEnv({ host: 'http://h', token: 't', keepConnected: true, auth: 'required', transport: 'http' });
    expect(env).toEqual({
      UNREAL_MCP_HOST: 'http://h',
      UNREAL_MCP_TOKEN: 't',
      UNREAL_MCP_KEEP_CONNECTED: 'true',
      UNREAL_MCP_AUTH_OPTION: 'required',
      UNREAL_MCP_TRANSPORT: 'http',
    });
  });

  it('returns undefined for --no-connect', () => {
    expect(buildOpenEnv({ noConnect: true, host: 'http://h' })).toBeUndefined();
  });

  it('throws on bad auth/transport enums', () => {
    expect(() => buildOpenEnv({ auth: 'bogus' as never })).toThrow();
    expect(() => buildOpenEnv({ transport: 'bogus' as never })).toThrow();
  });
});

describe('openProject', () => {
  it('resolves the engine and spawns the editor (injected engines + spawn)', async () => {
    const dir = tmp();
    writeUProject(dir, 'MyGame', '5.7');
    let spawnedWith: { editorPath: string; args: string[]; env: NodeJS.ProcessEnv } | null = null;
    const r = await openProject({
      projectDir: dir,
      host: 'http://localhost:5220',
      enginesImpl: () => [fakeEngine('5.7')],
      spawnImpl: (editorPath, args, env) => {
        spawnedWith = { editorPath, args, env };
        return { pid: 4242 };
      },
    });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.editorPid).toBe(4242);
    expect(r.editorPath).toContain('UnrealEditor');
    expect(r.envVars['UNREAL_MCP_HOST']).toBe('http://localhost:5220');
    expect(spawnedWith).not.toBeNull();
    expect(spawnedWith!.env['UNREAL_MCP_HOST']).toBe('http://localhost:5220');
  });

  it('warns that connection options are ignored under --no-connect', async () => {
    const dir = tmp();
    writeUProject(dir, 'MyGame', '5.7');
    const r = await openProject({
      projectDir: dir,
      noConnect: true,
      host: 'http://h',
      auth: 'required',
      enginesImpl: () => [fakeEngine('5.7')],
      spawnImpl: () => ({ pid: 7 }),
    });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.envVars).toEqual({}); // nothing propagated under --no-connect
    expect(r.warnings.some((w) => /ignored under --no-connect/.test(w))).toBe(true);
  });

  it('fails when no .uproject is present', async () => {
    const dir = tmp();
    const r = await openProject({ projectDir: dir, enginesImpl: () => [fakeEngine('5.7')] });
    expect(r.kind).toBe('failure');
    if (r.kind === 'failure') expect(r.errorMessage).toContain('.uproject');
  });

  it('fails when the engine association is not installed', async () => {
    const dir = tmp();
    writeUProject(dir, 'MyGame', '4.27');
    const r = await openProject({ projectDir: dir, enginesImpl: () => [fakeEngine('5.7')], spawnImpl: () => ({ pid: 1 }) });
    expect(r.kind).toBe('failure');
  });
});
