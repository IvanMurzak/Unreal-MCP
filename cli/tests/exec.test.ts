import { afterEach, describe, expect, it, vi } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import type { SessionProcessOps } from '@baizor/gamedev-cli-core';
import { execPython, readPythonResponse, toolErrorText, PYTHON_EXECUTE_TOOL } from '../src/lib/exec.js';
import {
  closeOwnedSession,
  createUnrealSession,
  EDITOR_QUIT_TOOL,
  readSessionLease,
  resolveSessionConnection,
  sessionEditorEnv,
} from '../src/lib/session.js';
import { readCode } from '../src/commands/exec.js';
import { PLUGIN_CONFIG_RELATIVE_PATH, resolveConnection } from '../src/utils/config.js';
import { generatePortFromDirectory } from '../src/utils/port.js';
import type { RunToolOptions, RunToolResult } from '../src/lib/types.js';
import { makeTempDir, rmTempDir, writeUProject } from './helpers.js';

const dirs: string[] = [];
afterEach(() => {
  while (dirs.length) rmTempDir(dirs.pop()!);
});

function project(): string {
  const dir = makeTempDir();
  dirs.push(dir);
  writeUProject(dir, 'Game');
  return dir;
}

/**
 * A fake process table: `spawnDetached` adds an editor that becomes "ready" after `readyAfterProbes` readiness
 * probes; `requestClose` / `forceKill` remove it.
 */
function fakeEditors(readyAfterProbes = 1) {
  const alive = new Set<number>();
  const spawned: Array<{ command: string; args: readonly string[]; env: NodeJS.ProcessEnv; hideWindow: boolean }> = [];
  let nextPid = 1000;
  let probes = 0;
  const ops: Partial<SessionProcessOps> = {
    spawnDetached: async (command, args, options) => {
      spawned.push({ command, args, env: options.env, hideWindow: options.hideWindow });
      const pid = nextPid++;
      alive.add(pid);
      return pid;
    },
    isAlive: (pid) => alive.has(pid),
    processStart: async (pid) => (alive.has(pid) ? `start-${pid}` : undefined),
    requestClose: async (pid) => alive.delete(pid),
    forceKill: async (pid) => alive.delete(pid),
    sleep: async () => {},
  };
  const probeReady = async (): Promise<string | undefined> => {
    if (alive.size === 0) return undefined;
    probes += 1;
    return probes >= readyAfterProbes ? 'http://localhost:5199' : undefined;
  };
  return { ops, probeReady, spawned, alive };
}

function okTool(data: unknown = { status: 'success', structured: { success: true, value: '3', logs: [] } }) {
  return vi.fn(async (_options: RunToolOptions): Promise<RunToolResult> => ({
    kind: 'success', success: true, endpoint: 'x', httpStatus: 200, data,
  }));
}

const CONNECTION = { url: 'http://localhost:5199' };
const noBuild = async (): Promise<void> => {};

describe('execPython', () => {
  it('reuses a running editor without launching one', async () => {
    const dir = project();
    const editors = fakeEditors();
    const invoke = okTool();
    const result = await execPython({
      projectDir: dir,
      code: '1 + 2',
      url: CONNECTION.url,
      invoke,
      sessionOptions: { probeReady: async () => 'http://localhost:5199', processOps: editors.ops, editorBinary: 'UnrealEditor.exe', beforeLaunch: noBuild },
    });
    expect(result.kind).toBe('success');
    if (result.kind === 'success') expect(result.source).toBe('running');
    expect(editors.spawned).toHaveLength(0);
    expect(invoke).toHaveBeenCalledWith(expect.objectContaining({
      toolName: PYTHON_EXECUTE_TOOL, url: 'http://localhost:5199', input: { code: '1 + 2' },
    }));
  });

  it('starts a headless editor with -RenderOffscreen, the session env and a local server, then runs the code', async () => {
    const dir = project();
    const editors = fakeEditors(2);
    const invoke = okTool();
    const result = await execPython({
      projectDir: dir,
      code: 'print(1)',
      include: ['helpers'],
      url: CONNECTION.url,
      idleSeconds: 120,
      invoke,
      sessionOptions: { probeReady: editors.probeReady, processOps: editors.ops, editorBinary: 'UnrealEditor.exe', beforeLaunch: noBuild, listProcessesImpl: () => [] },
    });
    expect(result.kind).toBe('success');
    if (result.kind === 'success') expect(result.source).toBe('started');
    expect(editors.spawned).toHaveLength(1);
    const [launch] = editors.spawned;
    expect(launch!.command).toBe('UnrealEditor.exe');
    expect(launch!.args).toEqual([path.join(dir, 'Game.uproject'), '-RenderOffscreen', '-unattended', '-nosplash']);
    expect(launch!.hideWindow).toBe(true);
    expect(launch!.env).toMatchObject({
      AIGD_SESSION_IDLE_SECONDS: '120',
      AIGD_SESSION_OWNER: 'cli',
      UNREAL_MCP_CONNECTION_MODE: 'Custom',
      UNREAL_MCP_HOST: 'http://localhost:5199',
      UNREAL_MCP_START_SERVER: 'true',
      UNREAL_MCP_TRANSPORT: 'http',
    });
    const lease = readSessionLease(dir);
    expect(lease).toMatchObject({ pid: 1000, mode: 'headless', engine: 'unreal', ownedBy: 'cli' });
    expect(launch!.env['AIGD_SESSION_NONCE']).toBe(lease!.nonce);
    expect(invoke).toHaveBeenCalledWith(expect.objectContaining({ input: { code: 'print(1)', include: ['helpers'] } }));
  });

  it('starts a windowed editor with --window', async () => {
    const dir = project();
    const editors = fakeEditors();
    await execPython({
      projectDir: dir,
      code: '1',
      url: CONNECTION.url,
      window: true,
      invoke: okTool(),
      sessionOptions: { probeReady: editors.probeReady, processOps: editors.ops, editorBinary: 'UnrealEditor.exe', beforeLaunch: noBuild, listProcessesImpl: () => [] },
    });
    expect(editors.spawned[0]!.args).toEqual([path.join(dir, 'Game.uproject')]);
    expect(editors.spawned[0]!.hideWindow).toBe(false);
    expect(readSessionLease(dir)?.mode).toBe('window');
  });

  it('launches one editor for two parallel cold calls', async () => {
    const dir = project();
    const editors = fakeEditors(3);
    const run = () => execPython({
      projectDir: dir,
      code: '1',
      url: CONNECTION.url,
      invoke: okTool(),
      sessionOptions: { probeReady: editors.probeReady, processOps: editors.ops, editorBinary: 'UnrealEditor.exe', beforeLaunch: noBuild, listProcessesImpl: () => [] },
    });
    const results = await Promise.all([run(), run()]);
    expect(results.map((r) => r.kind)).toEqual(['success', 'success']);
    expect(editors.spawned).toHaveLength(1);
    expect(results.map((r) => (r.kind === 'success' ? r.source : r.reason)).sort()).toEqual(['owned', 'started']);
  });

  it('waits for an editor the user is opening instead of starting a second one', async () => {
    const dir = project();
    let probes = 0;
    const editors = fakeEditors();
    editors.alive.add(42);
    const result = await execPython({
      projectDir: dir,
      code: '1',
      url: CONNECTION.url,
      invoke: okTool(),
      sessionOptions: {
        probeReady: async () => (++probes >= 3 ? 'http://localhost:5199' : undefined),
        processOps: editors.ops,
        editorBinary: 'UnrealEditor.exe', beforeLaunch: noBuild,
        listProcessesImpl: () => [{ pid: 42, commandLine: `"C:/UE/UnrealEditor.exe" "${path.join(dir, 'Game.uproject')}"` }],
      },
    });
    expect(result.kind).toBe('success');
    if (result.kind === 'success') expect(result.source).toBe('foreign');
    expect(editors.spawned).toHaveLength(0);
  });

  it('reports a session failure when the editor exits before it is ready', async () => {
    const dir = project();
    const editors = fakeEditors(Number.POSITIVE_INFINITY);
    editors.ops.spawnDetached = async () => 7; // never alive
    const result = await execPython({
      projectDir: dir,
      code: '1',
      url: CONNECTION.url,
      invoke: okTool(),
      sessionOptions: { probeReady: editors.probeReady, processOps: editors.ops, editorBinary: 'UnrealEditor.exe', beforeLaunch: noBuild, listProcessesImpl: () => [] },
    });
    expect(result).toMatchObject({ kind: 'failure', reason: 'session', sessionReason: 'exited' });
    expect(readSessionLease(dir)).toBeUndefined();
  });

  it('runs the pre-launch build once, under the startup lock, for two parallel cold calls', async () => {
    const dir = project();
    const editors = fakeEditors(3);
    let builds = 0;
    let building = false;
    const build = async (): Promise<void> => {
      expect(building).toBe(false);
      building = true;
      builds += 1;
      await new Promise((resolve) => setTimeout(resolve, 50));
      building = false;
    };
    const run = () => execPython({
      projectDir: dir,
      code: '1',
      url: CONNECTION.url,
      invoke: okTool(),
      sessionOptions: { probeReady: editors.probeReady, processOps: editors.ops, editorBinary: 'UnrealEditor.exe', beforeLaunch: build, listProcessesImpl: () => [] },
    });
    const results = await Promise.all([run(), run()]);
    expect(results.map((r) => r.kind)).toEqual(['success', 'success']);
    expect(builds).toBe(1);
    expect(editors.spawned).toHaveLength(1);
  });

  it('does not launch when the pre-launch build fails', async () => {
    const dir = project();
    const editors = fakeEditors();
    const result = await execPython({
      projectDir: dir,
      code: '1',
      url: CONNECTION.url,
      invoke: okTool(),
      sessionOptions: {
        probeReady: editors.probeReady, processOps: editors.ops, editorBinary: 'UnrealEditor.exe', listProcessesImpl: () => [],
        beforeLaunch: async () => { throw new Error('Pre-launch build failed: UBT exited with code 6'); },
      },
    });
    expect(result).toMatchObject({ kind: 'failure', reason: 'session' });
    expect(result.kind === 'failure' && result.message).toMatch(/Pre-launch build failed/);
    expect(editors.spawned).toHaveLength(0);
    expect(readSessionLease(dir)).toBeUndefined();
  });

  it('records the server URL and connection mode in the lease and keeps the session files out of git', async () => {
    const dir = project();
    const editors = fakeEditors();
    await execPython({
      projectDir: dir,
      code: '1',
      url: 'http://127.0.0.1:5188',
      invoke: okTool(),
      sessionOptions: { probeReady: editors.probeReady, processOps: editors.ops, editorBinary: 'UnrealEditor.exe', beforeLaunch: noBuild, listProcessesImpl: () => [] },
    });
    expect(readSessionLease(dir)).toMatchObject({ serverUrl: 'http://127.0.0.1:5188', connectionMode: 'Custom' });
    const gitignore = fs.readFileSync(path.join(dir, '.ai-game-dev', '.gitignore'), 'utf-8');
    expect(gitignore).toMatch(/session\.json/);
  });

  it('fails fast when an editor the user opened never answers on the local server (e.g. Cloud mode)', async () => {
    const dir = project();
    const editors = fakeEditors();
    editors.alive.add(42);
    const started = Date.now();
    const result = await execPython({
      projectDir: dir,
      code: '1',
      url: CONNECTION.url,
      invoke: okTool(),
      sessionOptions: {
        probeReady: async () => undefined,
        processOps: editors.ops,
        editorBinary: 'UnrealEditor.exe',
        beforeLaunch: noBuild,
        foreignReadyTimeoutMs: 50,
        listProcessesImpl: () => [{ pid: 42, commandLine: `"C:/UE/UnrealEditor.exe" "${path.join(dir, 'Game.uproject')}"` }],
      },
    });
    expect(Date.now() - started).toBeLessThan(5_000);
    expect(result).toMatchObject({ kind: 'failure', reason: 'session', sessionReason: 'not-ready', sessionSource: 'foreign' });
    expect(result.kind === 'failure' && result.message).toMatch(/Cloud mode/);
    expect(editors.spawned).toHaveLength(0);
  });

  it('rejects a remote --url (readiness must probe the local server)', async () => {
    const result = await execPython({ projectDir: project(), code: '1', url: 'https://ai-game.dev/mcp' });
    expect(result).toMatchObject({ kind: 'failure', reason: 'invalid-input' });
  });

  it('fails without a .uproject', async () => {
    const dir = makeTempDir();
    dirs.push(dir);
    expect(await execPython({ projectDir: dir, code: '1' })).toMatchObject({ kind: 'failure', reason: 'invalid-input' });
  });
});

describe('closeOwned (editor-quit)', () => {
  it('asks the plugin to save and quit, proving ownership with the lease nonce', async () => {
    const dir = project();
    const editors = fakeEditors();
    const quit = vi.fn(async (options: RunToolOptions): Promise<RunToolResult> => {
      editors.alive.clear();
      return { kind: 'success', success: true, endpoint: options.toolName, httpStatus: 200, data: { status: 'success' } };
    });
    const session = createUnrealSession({
      projectDir: dir, connection: CONNECTION, probeReady: editors.probeReady, processOps: editors.ops,
      editorBinary: 'UnrealEditor.exe', beforeLaunch: noBuild, listProcessesImpl: () => [], invokeSystemTool: quit,
    });
    await session.ensure();
    const nonce = readSessionLease(dir)!.nonce;
    const requestClose = vi.spyOn(editors.ops, 'requestClose');

    const result = await session.closeOwned({ timeoutMs: 1000 });
    expect(result).toMatchObject({ kind: 'closed', pid: 1000, forced: false });
    expect(quit).toHaveBeenCalledWith(expect.objectContaining({ toolName: EDITOR_QUIT_TOOL, input: { nonce } }));
    expect(requestClose).not.toHaveBeenCalled();
    expect(readSessionLease(dir)).toBeUndefined();
  });

  it('falls back to the OS polite quit when the plugin cannot be reached', async () => {
    const dir = project();
    const editors = fakeEditors();
    const unreachable = vi.fn(async (): Promise<RunToolResult> => ({
      kind: 'failure', success: false, endpoint: 'x', reason: 'connection-refused', message: 'ECONNREFUSED',
    }) as RunToolResult);
    const session = createUnrealSession({
      projectDir: dir, connection: CONNECTION, probeReady: editors.probeReady, processOps: editors.ops,
      editorBinary: 'UnrealEditor.exe', beforeLaunch: noBuild, listProcessesImpl: () => [], invokeSystemTool: unreachable,
    });
    await session.ensure();
    expect(await session.closeOwned({ timeoutMs: 1000 })).toMatchObject({ kind: 'closed' });
    expect(editors.alive.size).toBe(0);
  });

  it('closes without --url: editor-quit goes to the server named by the lease', async () => {
    const dir = project();
    const editors = fakeEditors();
    const quit = vi.fn(async (options: RunToolOptions): Promise<RunToolResult> => {
      editors.alive.clear();
      return { kind: 'success', success: true, endpoint: options.toolName, httpStatus: 200, data: { status: 'success' } };
    });
    const seams = { processOps: editors.ops, listProcessesImpl: () => [], invokeSystemTool: quit };
    const started = await execPython({
      projectDir: dir,
      code: '1',
      url: 'http://127.0.0.1:5177',
      invoke: okTool(),
      sessionOptions: { ...seams, probeReady: editors.probeReady, editorBinary: 'UnrealEditor.exe', beforeLaunch: noBuild },
    });
    expect(started.kind).toBe('success');
    const result = await closeOwnedSession({ projectDir: dir, timeoutMs: 1000, ...seams });
    expect(result).toMatchObject({ kind: 'success', result: { kind: 'closed', pid: 1000 } });
    expect(quit).toHaveBeenCalledWith(expect.objectContaining({ toolName: EDITOR_QUIT_TOOL, url: 'http://127.0.0.1:5177' }));
  });

  it('never touches an editor without our lease', async () => {
    const dir = project();
    const result = await closeOwnedSession({ projectDir: dir });
    expect(result).toMatchObject({ kind: 'success', result: { kind: 'not-owned', staleLeaseRemoved: false } });
  });
});

describe('session connection', () => {
  it('keeps a loopback URL and builds the started editor env from it', async () => {
    const dir = project();
    const connection = await resolveSessionConnection(dir, { url: 'http://127.0.0.1:5188/', token: 't' });
    expect(connection).toEqual({ url: 'http://127.0.0.1:5188', token: 't' });
    expect(sessionEditorEnv(connection, 't')).toEqual({
      UNREAL_MCP_CONNECTION_MODE: 'Custom',
      UNREAL_MCP_HOST: 'http://127.0.0.1:5188',
      UNREAL_MCP_TRANSPORT: 'http',
      UNREAL_MCP_START_SERVER: 'true',
      UNREAL_MCP_KEEP_CONNECTED: 'true',
      UNREAL_MCP_TOKEN: 't',
    });
  });

  it('fills in the bound port for a port-less --url', async () => {
    const dir = project();
    const connection = await resolveSessionConnection(dir, { url: 'http://localhost' });
    expect(connection.url).toBe(`http://localhost:${generatePortFromDirectory(dir)}`);
  });

  it('rejects a --url whose port the project portOverride overrules, and follows the override otherwise', async () => {
    const dir = project();
    fs.mkdirSync(path.join(dir, '.ai-game-dev'), { recursive: true });
    fs.writeFileSync(path.join(dir, '.ai-game-dev', 'project.json'), JSON.stringify({ portOverride: 27001 }));
    await expect(resolveSessionConnection(dir, { url: 'http://localhost:5199' })).rejects.toThrow(/portOverride/);
    expect((await resolveSessionConnection(dir, { url: 'http://localhost:27001' })).url).toBe('http://localhost:27001');
    expect((await resolveSessionConnection(dir, { url: 'http://localhost' })).url).toBe('http://localhost:27001');
    expect((await resolveSessionConnection(dir)).url).toBe('http://localhost:27001');
  });

  it('uses the deterministic local port for a Cloud-mode project, never the cloud URL or its token', async () => {
    const dir = project();
    const configPath = path.join(dir, PLUGIN_CONFIG_RELATIVE_PATH);
    fs.mkdirSync(path.dirname(configPath), { recursive: true });
    fs.writeFileSync(configPath, JSON.stringify({ connectionMode: 'Cloud', cloudUrl: 'https://ai-game.dev', cloudToken: 'cloud-secret' }));
    expect((await resolveConnection({ projectDir: dir, processEnv: {} })).url).toBe('https://ai-game.dev/mcp'); // the fixture is live
    const connection = await resolveSessionConnection(dir);
    expect(connection.url).toMatch(/^http:\/\/localhost:\d+$/);
    expect(connection.token).toBeUndefined();
  });
});

describe('readPythonResponse', () => {
  it('returns the value and the logs of a success', () => {
    expect(readPythonResponse({
      status: 'success',
      structured: { success: true, value: '3', logs: [{ level: 'info', message: 'hi' }] },
    })).toEqual({ ok: true, value: '3', logs: [{ level: 'info', message: 'hi' }] });
  });

  it('returns the error text of a failure', () => {
    expect(readPythonResponse({ status: 'error', content: [{ type: 'text', text: 'RuntimeError: boom' }] }))
      .toEqual({ ok: false, value: 'RuntimeError: boom', logs: [] });
  });

  it('prints null for a statement without a value', () => {
    expect(readPythonResponse({ status: 'success', structured: { success: true, logs: [] } }).value).toBe('null');
  });
});

describe('readCode', () => {
  const tty = { isTTY: true, read: () => '' };

  it('takes the argument', () => {
    expect(readCode('1 + 2', undefined, tty)).toBe('1 + 2');
  });

  it('reads a file and stdin', () => {
    const dir = makeTempDir();
    dirs.push(dir);
    const file = path.join(dir, 'a.py');
    fs.writeFileSync(file, 'x = 1\nx');
    expect(readCode(undefined, file, tty)).toBe('x = 1\nx');
    expect(readCode(undefined, '-', { isTTY: true, read: () => 'piped' })).toBe('piped');
    expect(readCode(undefined, undefined, { isTTY: false, read: () => 'piped' })).toBe('piped');
  });

  it('rejects both, none and empty input', () => {
    expect(() => readCode('1', 'a.py', tty)).toThrow(/not both/);
    expect(() => readCode(undefined, undefined, tty)).toThrow(/No code given/);
    expect(() => readCode(undefined, undefined, { isTTY: false, read: () => '  ' })).toThrow(/stdin/);
  });
});

describe('toolErrorText', () => {
  it('prefers the error the server returned for a failed tool', () => {
    const failure = { kind: 'failure', success: false, endpoint: 'x', reason: 'http-error', httpStatus: 500, message: 'Internal Server Error' } as const;
    expect(toolErrorText({ ...failure, data: { error: 'Runtime: ZeroDivisionError: division by zero' } })).toBe('Runtime: ZeroDivisionError: division by zero');
    expect(toolErrorText(failure)).toBe('Internal Server Error');
  });
});
