import { describe, it, expect, afterEach } from 'vitest';
import * as path from 'path';
import { close, selectEditorProcesses, type RunningProcess } from '../src/lib/close.js';
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

describe('selectEditorProcesses', () => {
  const projectDir = path.resolve('/work/MyGame');
  const uproject = path.join(projectDir, 'MyGame.uproject');
  const procs: RunningProcess[] = [
    { pid: 1, commandLine: `"C:/UE/UnrealEditor.exe" "${uproject}"` },
    { pid: 2, commandLine: 'chrome.exe --foo' },
    { pid: 3, commandLine: `UnrealEditor.exe "C:/work/MyGame/Other.uproject"` },
    { pid: 4, commandLine: 'notepad.exe MyGame.uproject' },
  ];

  it('matches editor processes referencing the project (.uproject or dir)', () => {
    const matched = selectEditorProcesses(procs, projectDir, uproject);
    const pids = matched.map((p) => p.pid).sort();
    expect(pids).toContain(1);
    expect(pids).toContain(3); // same dir, different uproject
    expect(pids).not.toContain(2);
    expect(pids).not.toContain(4); // not an UnrealEditor process
  });
});

describe('close', () => {
  it('terminates matched editor processes via injected kill', async () => {
    const dir = tmp();
    writeUProject(dir, 'MyGame', '5.7');
    const killed: number[] = [];
    const r = await close({
      projectDir: dir,
      listProcessesImpl: () => [{ pid: 99, commandLine: `UnrealEditor.exe "${path.join(dir, 'MyGame.uproject')}"` }],
      killImpl: (pid) => killed.push(pid),
    });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.wasRunning).toBe(true);
    expect(killed).toEqual([99]);
  });

  it('reports wasRunning=false when nothing matches', async () => {
    const dir = tmp();
    writeUProject(dir, 'MyGame', '5.7');
    const r = await close({ projectDir: dir, listProcessesImpl: () => [], killImpl: () => {} });
    expect(r.kind).toBe('success');
    if (r.kind === 'success') expect(r.wasRunning).toBe(false);
  });
});
