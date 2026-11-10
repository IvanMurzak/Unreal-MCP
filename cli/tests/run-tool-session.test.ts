import { afterEach, describe, expect, it } from 'vitest';
import type { SessionProcessOps } from '@baizor/gamedev-cli-core';
import { isUnreachableFailure, runToolWithSession } from '../src/lib/run-tool-session.js';
import { classifySessionFailure, classifyToolFailure, classifyToolResponseError, toolResponseError } from '../src/lib/tool-outcome.js';
import { EDITOR_QUIT_TOOL } from '../src/lib/session.js';
import type { RunToolFailure, RunToolOptions, RunToolResult } from '../src/lib/types.js';
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

const refused: RunToolFailure = { kind: 'failure', success: false, endpoint: 'http://localhost:1/api/tools/x', reason: 'connection-refused', message: 'ECONNREFUSED' };
const ok = (data: unknown = { status: 'success' }): RunToolResult => ({ kind: 'success', success: true, endpoint: 'e', httpStatus: 200, data });

/** Fake editors: a spawned one is ready at once; `editor-quit` (the plugin's close) removes it. */
function fakeSession() {
  const alive = new Set<number>();
  const spawned: Array<readonly string[]> = [];
  const ops: Partial<SessionProcessOps> = {
    spawnDetached: async (_command, args) => { spawned.push(args); alive.add(1000); return 1000; },
    isAlive: (pid) => alive.has(pid),
    processStart: async (pid) => (alive.has(pid) ? 'start' : undefined),
    requestClose: async (pid) => alive.delete(pid),
    forceKill: async (pid) => alive.delete(pid),
    sleep: async () => {},
  };
  return {
    alive,
    spawned,
    sessionOptions: {
      processOps: ops,
      probeReady: async () => (alive.size > 0 ? 'http://localhost:5199' : undefined),
      editorBinary: 'UnrealEditor',
      beforeLaunch: async () => {},
      listProcessesImpl: () => [],
      invokeSystemTool: async (o: RunToolOptions) => {
        if (o.toolName === EDITOR_QUIT_TOOL) alive.clear();
        return ok();
      },
    },
  };
}

describe('runToolWithSession', () => {
  it('calls the tool directly when an editor answers (no session)', async () => {
    const calls: RunToolOptions[] = [];
    const r = await runToolWithSession({ invoke: async (o) => { calls.push(o); return ok(); }, toolName: 'x', projectDir: project() });
    expect(r.result?.kind).toBe('success');
    expect(r.source).toBeUndefined();
    expect(calls).toHaveLength(1);
  });

  it('starts a headless editor when none answers, and with --once closes the one it started', async () => {
    const fake = fakeSession();
    const calls: RunToolOptions[] = [];
    const invoke = async (o: RunToolOptions): Promise<RunToolResult> => { calls.push(o); return calls.length === 1 ? refused : ok({ status: 'success', value: 1 }); };
    const r = await runToolWithSession({ invoke, toolName: 'x', projectDir: project(), once: true, nullrhi: true, sessionOptions: fake.sessionOptions });
    expect(r.source).toBe('started');
    expect(r.result).toMatchObject({ kind: 'success', data: { value: 1 } });
    expect(calls[1]!.url).toBe('http://localhost:5199');
    expect(fake.spawned[0]).toEqual(expect.arrayContaining(['-RenderOffscreen', '-nullrhi']));
    expect(r.closed).toMatchObject({ kind: 'closed', pid: 1000 });
    expect(fake.alive.size).toBe(0);
  });

  it('--once closes the editor it started even when it never became ready', async () => {
    const fake = fakeSession();
    const r = await runToolWithSession({
      invoke: async () => refused,
      toolName: 'x',
      projectDir: project(),
      once: true,
      readyTimeoutMs: 1,
      sessionOptions: { ...fake.sessionOptions, probeReady: async () => undefined },
    });
    expect(r.sessionFailure?.reason).toBe('not-ready');
    expect(r.closed?.kind).toBe('closed');
    expect(fake.alive.size).toBe(0);
  });

  it('--once never closes an editor it did not start', async () => {
    const fake = fakeSession();
    fake.alive.add(1000); // e.g. the user's editor, answering only after the first call failed
    const invoke = async (): Promise<RunToolResult> => ok();
    let first = true;
    const r = await runToolWithSession({
      invoke: async (o) => { if (first) { first = false; return refused; } return invoke(); },
      toolName: 'x', projectDir: project(), once: true, sessionOptions: fake.sessionOptions,
    });
    expect(r.source).toBe('running');
    expect(r.closed).toBeUndefined();
    expect(fake.alive.size).toBe(1);
  });

  it('--no-start returns the unreachable failure', async () => {
    const r = await runToolWithSession({ invoke: async () => refused, toolName: 'x', projectDir: project(), start: false });
    expect(r.result).toBe(refused);
  });

  it('a remote --url is never served by a started editor', async () => {
    const r = await runToolWithSession({ invoke: async () => refused, toolName: 'x', url: 'https://ai-game.dev/mcp' });
    expect(r.result).toBe(refused);
  });

  it('no project to start: a session failure', async () => {
    const dir = makeTempDir();
    dirs.push(dir);
    const r = await runToolWithSession({ invoke: async () => refused, toolName: 'x', projectDir: dir });
    expect(r.sessionFailure?.message).toMatch(/No \.uproject/);
  });
});

describe('tool outcome classification', () => {
  const failure = (fields: Partial<RunToolFailure>): RunToolFailure => ({ ...refused, ...fields });

  it('maps failures to the shared exit codes', () => {
    expect(classifyToolFailure(failure({ reason: 'invalid-input' })).exit).toBe(2);
    expect(classifyToolFailure(refused).exit).toBe(7);
    expect(classifyToolFailure(failure({ reason: 'timeout' })).exit).toBe(14);
    expect(classifyToolFailure(failure({ reason: 'http-error', httpStatus: 503 })).exit).toBe(17);
    expect(classifyToolFailure(failure({ reason: 'http-error', httpStatus: 404 }))).toMatchObject({ exit: 6, code: 'NOT_FOUND' });
    expect(classifyToolFailure(failure({ reason: 'http-error', httpStatus: 504 })).exit).toBe(14);
    expect(classifyToolFailure(failure({ reason: 'http-error', httpStatus: 502 })).exit).toBe(7);
    expect(classifySessionFailure(undefined, true)).toMatchObject({ exit: 2, code: 'NOT_FOUND' });
    expect(classifyToolFailure(failure({ reason: 'http-error', httpStatus: 500, message: 'No connected clients' })).exit).toBe(7);
    expect(classifyToolFailure(failure({ reason: 'http-error', httpStatus: 500, data: { error: 'bad args' } })).exit).toBe(6);
    const modal = 'The editor is showing the modal dialog "Save Content" (open for 3 s): its game thread waits';
    expect(classifyToolFailure(failure({ reason: 'http-error', httpStatus: 500, data: { error: modal } }))).toMatchObject({ exit: 18, code: 'AWAITING_INPUT' });
    expect(classifySessionFailure('lock-busy').exit).toBe(17);
    expect(classifySessionFailure('not-ready').exit).toBe(7);
  });

  it('a response with status error is a failed command', () => {
    expect(toolResponseError({ status: 'success' })).toBeUndefined();
    expect(toolResponseError({ status: 'error', content: [{ type: 'text', text: 'No such actor' }] })).toBe('No such actor');
    expect(classifyToolResponseError('No such actor')).toMatchObject({ exit: 6, code: 'TOOL_FAILED' });
  });

  it('only "nobody answered" failures start an editor', () => {
    expect(isUnreachableFailure(refused)).toBe(true);
    expect(isUnreachableFailure(failure({ reason: 'http-error', httpStatus: 503, message: 'busy' }))).toBe(false);
    expect(isUnreachableFailure(failure({ reason: 'timeout' }))).toBe(false);
  });
});
