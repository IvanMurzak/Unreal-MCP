import { describe, it, expect, afterEach, vi } from 'vitest';
import { EventEmitter } from 'events';
import * as fs from 'fs';
import * as path from 'path';
import { spawn } from 'child_process';
import { openProject } from '../src/lib/open.js';
import type { ProgressEvent } from '../src/lib/types.js';
import { editorBinaryPath } from '../src/utils/engine.js';
import type { EngineInstallation } from '../src/utils/launcher.js';
import { makeTempDir, rmTempDir, writeUProject } from './helpers.js';

// The default (detached `child_process.spawn`) launch, with a child whose pid is reported only by its
// `spawn` event, as a launch routed elsewhere reports it: the pid is waited for, never logged as "unknown".
vi.mock('child_process', async (importOriginal) => ({
  ...(await importOriginal<typeof import('child_process')>()),
  spawn: vi.fn(),
}));
const spawnMock = vi.mocked(spawn);

const dirs: string[] = [];
afterEach(() => {
  while (dirs.length) rmTempDir(dirs.pop()!);
  spawnMock.mockReset();
});
function tmp(): string {
  const d = makeTempDir();
  dirs.push(d);
  return d;
}
function fakeEngine(association: string): EngineInstallation {
  const root = tmp();
  const bin = editorBinaryPath(root, process.platform);
  fs.mkdirSync(path.dirname(bin), { recursive: true });
  fs.writeFileSync(bin, '', 'utf-8');
  return { appName: `UE_${association}`, appVersion: `${association}.0`, installLocation: root, engineAssociation: association };
}

/** A detached child with no pid yet: `spawn` (with `pid`) after `delayMs`, or `error` when `pid` is undefined. */
function lateChild(pid: number | undefined, delayMs: number): ReturnType<typeof spawn> {
  const child = Object.assign(new EventEmitter(), { pid: undefined as number | undefined, unref: () => child });
  setTimeout(() => {
    if (pid === undefined) child.emit('error', new Error('spawn ENOENT'));
    else {
      child.pid = pid;
      child.emit('spawn');
    }
  }, delayMs);
  return child as unknown as ReturnType<typeof spawn>;
}

async function open(progress: ProgressEvent[]) {
  const dir = tmp();
  writeUProject(dir, 'MyGame', '5.7');
  return openProject({
    projectDir: dir,
    autoDismissStartupDialogs: false,
    build: false,
    noCache: true,
    enginesImpl: () => [fakeEngine('5.7')],
    onProgress: (e) => progress.push(e),
  });
}

describe('openProject — default spawn, pid reported by the spawn event', () => {
  it('waits for the spawn event and logs the pid', async () => {
    spawnMock.mockImplementation(() => lateChild(5151, 100));
    const progress: ProgressEvent[] = [];
    const r = await open(progress);
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.editorPid).toBe(5151);
    expect(progress).toContainEqual({ phase: 'launched', message: 'Launched (PID: 5151)', pid: 5151 });
    expect(spawnMock.mock.calls[0][2]).toMatchObject({ detached: true, stdio: 'ignore' });
  });

  it('a spawn that errors reports no pid, and no "unknown"', async () => {
    spawnMock.mockImplementation(() => lateChild(undefined, 10));
    const progress: ProgressEvent[] = [];
    const r = await open(progress);
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.editorPid).toBeUndefined();
    expect(progress).toContainEqual({ phase: 'launched', message: 'Launched', pid: undefined });
  });
});
