import { afterEach, describe, expect, it, vi } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import {
  awaitingInputMessage,
  dialogFile,
  findProjectDialogs,
  listEditorDialogs,
  nodeDialogFileIo,
  parseDialog,
  pressEditorDialogButton,
  publishedDialogs,
  withBlockingDialogs,
  type DialogFileIo,
  type EditorDialog,
} from '../src/lib/editor-dialogs.js';
import { buildRunToolCommand } from '../src/commands/run-tool-builder.js';
import { getStatus } from '../src/lib/status.js';
import { FAILURE } from '../src/utils/output.js';
import * as ui from '../src/utils/ui.js';
import { makeTempDir, rmTempDir, writeUProject } from './helpers.js';

const PROJECT = path.join('C:', 'Games', 'MyGame');
const SAVE: EditorDialog = { id: '3', pid: 4242, title: 'Save Content', message: 'Save these packages?\n/Game/Map', buttons: ['Save Selected', "Don't Save", 'Cancel'] };

/**
 * In-memory files with a stand-in for the plugin's modal loop: on every sleep it takes a pending press request of a
 * published dialog and answers it as FUnrealMcpDialogControl does (unless `answer` is false: an editor that never looks).
 */
function fakeEditor(dialogs: EditorDialog[], answer = true, running: number[] = dialogs.map((d) => d.pid), project = PROJECT) {
  const files = new Map<string, string>();
  for (const d of dialogs) files.set(dialogFile(project, d.pid), JSON.stringify(d));
  const pressed: Array<{ pid: number; button: string }> = [];
  let clock = 0;
  /** When set, the editor takes a request only from this time on (a busy modal loop). */
  let takeLate: number | undefined;
  const io: DialogFileIo = {
    read: (file) => files.get(file),
    write: (file, text) => void files.set(file, text),
    remove: (file) => void files.delete(file),
    list: (dir) => [...files.keys()].filter((f) => path.dirname(f) === dir).map((f) => path.basename(f)),
    alive: (pid) => running.includes(pid),
    now: () => clock,
    sleep: async (ms) => {
      clock += ms;
      if (!answer) return;
      if (takeLate !== undefined && clock < takeLate) return;
      for (const d of dialogs) {
        const request = files.get(dialogFile(PROJECT, d.pid, '.press.json'));
        if (request === undefined) continue;
        files.delete(dialogFile(PROJECT, d.pid, '.press.json'));
        const r = JSON.parse(request) as { request: string; button: string; id?: string };
        const match = d.buttons.find((b) => b.toLowerCase() === r.button.trim().toLowerCase());
        const ok = match !== undefined && (!r.id || r.id === d.id);
        if (ok) {
          pressed.push({ pid: d.pid, button: match });
          files.delete(dialogFile(PROJECT, d.pid));
        }
        files.set(dialogFile(PROJECT, d.pid, '.result.json'), JSON.stringify({
          request: r.request, result: ok ? 'pressed' : 'not-found', button: r.button, message: ok ? '' : `The dialog "${d.title}" has no enabled "${r.button}" button.`,
        }));
      }
    },
  };
  return { io, files, pressed, takeFrom: (ms: number) => { takeLate = ms; } };
}

const dirs: string[] = [];
const originalExitCode = process.exitCode;
afterEach(() => {
  for (const dir of dirs.splice(0)) rmTempDir(dir);
  process.exitCode = originalExitCode;
  vi.restoreAllMocks();
});

describe('editor dialog files', () => {
  it('reads the dialog of each running editor from Saved/UnrealMCP/Dialogs/<pid>.json', () => {
    const { io } = fakeEditor([SAVE]);
    expect(dialogFile(PROJECT, 4242)).toBe(path.join(PROJECT, 'Saved', 'UnrealMCP', 'Dialogs', '4242.json'));
    expect(listEditorDialogs(PROJECT, [4242, 99], io)).toEqual([SAVE]);
    expect(listEditorDialogs(PROJECT, [99], io)).toEqual([]);
  });

  it('ignores files that are not a published dialog of that process', () => {
    expect(parseDialog('{"id":"1","pid":7,"title":"T"', 7)).toBeUndefined(); // half written
    expect(parseDialog(JSON.stringify({ ...SAVE, pid: 1 }), 4242)).toBeUndefined(); // another process
    expect(parseDialog(JSON.stringify({ ...SAVE, id: '1; rm -rf' }), 4242)).toBeUndefined();
    expect(parseDialog(JSON.stringify({ id: '1', pid: 5, title: 'Picker' }), 5)).toEqual({ id: '1', pid: 5, title: 'Picker', message: '', buttons: [] });
  });

  it('presses a button by label (any case) through a request the editor answers', async () => {
    const { io, files, pressed } = fakeEditor([SAVE]);
    const result = await pressEditorDialogButton(PROJECT, [4242], "don't save", { io });
    expect(result).toMatchObject({ kind: 'pressed', dialog: { title: 'Save Content' } });
    expect(pressed).toEqual([{ pid: 4242, button: "Don't Save" }]);
    expect(files.has(dialogFile(PROJECT, 4242, '.result.json'))).toBe(false); // the answer is consumed
  });

  it('carries the label as JSON data, whatever it contains', async () => {
    const { io } = fakeEditor([SAVE], false);
    const write = vi.spyOn(io, 'write');
    const label = 'Save"; Remove-Item C:\\ -Recurse; "$(calc)';
    await pressEditorDialogButton(PROJECT, [4242], label, { id: '3', io, timeoutMs: 0 });
    expect(write).toHaveBeenCalledWith(dialogFile(PROJECT, 4242, '.press.json'), expect.any(String));
    expect(JSON.parse(write.mock.calls[0]![1])).toMatchObject({ button: label, id: '3' });
  });

  it('names the open dialogs when the button or the id does not match', async () => {
    const { io, pressed } = fakeEditor([SAVE]);
    const noButton = await pressEditorDialogButton(PROJECT, [4242], 'Maybe', { io });
    expect(noButton.kind).toBe('not-found');
    expect(noButton.kind === 'not-found' && noButton.message).toContain('"Save Content"');
    const noId = await pressEditorDialogButton(PROJECT, [4242], 'Cancel', { id: '9', io });
    expect(noId.kind === 'not-found' && noId.message).toContain('Dialog 9 is not open');
    expect(pressed).toEqual([]);
  });

  it('reports the editor\'s refusal (the dialog changed under the request)', async () => {
    const { io } = fakeEditor([SAVE]);
    io.write(dialogFile(PROJECT, 4242), JSON.stringify({ ...SAVE, buttons: [...SAVE.buttons, 'Gone'] }));
    const result = await pressEditorDialogButton(PROJECT, [4242], 'Gone', { io });
    expect(result).toEqual({ kind: 'not-found', message: 'The dialog "Save Content" has no enabled "Gone" button.' });
  });

  it('gives up and withdraws the request when the editor does not answer', async () => {
    const { io, files } = fakeEditor([SAVE], false);
    const result = await pressEditorDialogButton(PROJECT, [4242], 'Cancel', { io, timeoutMs: 1_000 });
    expect(result.kind).toBe('no-answer');
    expect(files.has(dialogFile(PROJECT, 4242, '.press.json'))).toBe(false);
  });

  it('says no dialog is open when none is published', async () => {
    const { io } = fakeEditor([]);
    const result = await pressEditorDialogButton(PROJECT, [4242], 'OK', { io });
    expect(result.kind === 'not-found' && result.message).toContain('No modal dialog is open');
  });

  it('writes the request atomically on disk', () => {
    const dir = makeTempDir(); dirs.push(dir);
    const file = dialogFile(dir, 1, '.press.json');
    nodeDialogFileIo.write(file, '{"a":1}');
    expect(nodeDialogFileIo.read(file)).toBe('{"a":1}');
    expect(fs.readdirSync(path.dirname(file))).toEqual(['1.press.json']);
    nodeDialogFileIo.remove(file);
    expect(nodeDialogFileIo.read(file)).toBeUndefined();
  });
});

describe('awaiting input', () => {
  it('describes the dialogs and how to answer them', () => {
    const message = awaitingInputMessage([SAVE]);
    expect(message).toContain('"Save Content": Save these packages? /Game/Map [Save Selected | Don\'t Save | Cancel]');
    expect(message).toContain('unreal-mcp-cli dialog --press <button>');
    expect(awaitingInputMessage([{ ...SAVE, buttons: [] }])).not.toContain('--press');
  });

  it('turns only the failures a dialog can cause into AWAITING_INPUT', () => {
    const find = vi.fn(() => [SAVE]);
    const failed = { kind: FAILURE.toolFailed, message: 'boom', data: { tool: 'x' } };
    expect(withBlockingDialogs(PROJECT, failed, find)).toBe(failed);
    expect(find).not.toHaveBeenCalled();
    for (const kind of [FAILURE.timeout, FAILURE.busy, FAILURE.unreachable]) {
      expect(withBlockingDialogs(PROJECT, { kind, message: 'late', data: { tool: 'x' } }, find))
        .toEqual({ kind: FAILURE.awaitingInput, message: awaitingInputMessage([SAVE]), data: { tool: 'x', dialogs: [SAVE] } });
    }
    const ended = withBlockingDialogs(PROJECT, { kind: FAILURE.awaitingInput, message: 'The editor is showing the modal dialog', data: {} }, find);
    expect(ended.message).toBe('The editor is showing the modal dialog'); // the watch's own reason stays
    expect(ended.data).toEqual({ dialogs: [SAVE] });
    const timeout = { kind: FAILURE.timeout, message: 'late', data: {} };
    expect(withBlockingDialogs(PROJECT, timeout, () => [])).toBe(timeout);
  });

  it('finds published dialogs of running editors of the project, listing processes only when a file is there', () => {
    const dir = makeTempDir(); dirs.push(dir);
    const uproject = writeUProject(dir, 'MyGame');
    const stale = { ...SAVE, pid: 99, id: '1' }; // a crashed editor's file; pid 99 now belongs to another program
    const gone = { ...SAVE, pid: 98, id: '1' }; // a crashed editor's file; no process 98
    const { io } = fakeEditor([SAVE, stale, gone], true, [4242, 99], dir);
    io.write(dialogFile(dir, 4242, '.result.json'), '{}');
    const listProcessesImpl = vi.fn(() => [{ pid: 4242, commandLine: `UnrealEditor.exe "${uproject}"` }, { pid: 99, commandLine: 'notepad.exe' }]);
    expect(publishedDialogs(uproject, { io, listProcessesImpl })).toEqual([SAVE]); // the .uproject path works as well
    expect(listProcessesImpl).toHaveBeenCalledTimes(1);

    const empty = fakeEditor([], true, [], dir);
    expect(publishedDialogs(dir, { io: empty.io, listProcessesImpl })).toEqual([]);
    expect(listProcessesImpl).toHaveBeenCalledTimes(1); // nothing published: no listing
  });

  it('refuses an id that names a dialog in more than one editor', async () => {
    const { io, pressed } = fakeEditor([SAVE, { ...SAVE, pid: 5 }]);
    const result = await pressEditorDialogButton(PROJECT, [4242, 5], 'Cancel', { id: '3', io });
    expect(result.kind === 'not-found' && result.message).toContain('open in several editors');
    expect(pressed).toEqual([]);
  });

  it('waits a moment for the answer to a request the editor took at the deadline', async () => {
    const editor = fakeEditor([SAVE]);
    editor.takeFrom(1_000);
    const result = await pressEditorDialogButton(PROJECT, [4242], 'Cancel', { io: editor.io, timeoutMs: 1_000 });
    expect(result.kind).toBe('pressed');
  });

  it('finds the dialogs of the editors running the project', () => {
    const dir = makeTempDir(); dirs.push(dir);
    const uproject = writeUProject(dir, 'MyGame');
    nodeDialogFileIo.write(dialogFile(dir, 4242), JSON.stringify(SAVE));
    const editor = { pid: 4242, commandLine: `"C:/UE/UnrealEditor.exe" "${uproject}"` };
    const found = findProjectDialogs(dir, { listProcessesImpl: () => [editor, { pid: 7, commandLine: 'notepad.exe' }] });
    expect(found?.pids).toEqual([4242]);
    expect(found?.dialogs).toEqual([SAVE]);
    expect(findProjectDialogs(path.join(dir, 'missing'))).toBeUndefined();
  });

  it('status reports the dialogs with their buttons', async () => {
    const dir = makeTempDir(); dirs.push(dir);
    const uproject = writeUProject(dir, 'MyGame');
    nodeDialogFileIo.write(dialogFile(dir, 4242), JSON.stringify(SAVE));
    const r = await getStatus({
      projectDir: dir,
      noProbe: true,
      listProcessesImpl: () => [{ pid: 4242, commandLine: `UnrealEditor.exe "${uproject}"` }],
      readLogTail: () => undefined,
    });
    expect(r.editor).toEqual({ running: true, pids: [4242], modalDialog: 'Save Content', dialogs: [SAVE] });
  });

  it('run-tool exits 18 with the dialogs when a call times out behind one', async () => {
    vi.spyOn(ui, 'error').mockImplementation(() => {});
    const run = vi.fn(async () => ({
      result: { kind: 'failure' as const, success: false as const, endpoint: 'http://localhost/api/tools/x', reason: 'timeout' as const, message: 'timed out' },
    }));
    const cli = buildRunToolCommand({ name: 'run-tool', description: 'Test', errorNoun: 'tool', invoke: vi.fn(), run, findDialogs: () => [SAVE] });
    const stdout = vi.spyOn(process.stdout, 'write').mockImplementation(() => true);
    await cli.parseAsync(['node', 'cli', 'x', '--format', 'json']);
    const envelope = JSON.parse(String(stdout.mock.calls.at(-1)?.[0])) as { errors: Array<{ code: string; message: string }>; data: { dialogs: EditorDialog[] } };
    expect(process.exitCode).toBe(18);
    expect(envelope.errors[0]).toMatchObject({ code: 'AWAITING_INPUT' });
    expect(envelope.errors[0]!.message).toContain('"Save Content"');
    expect(envelope.data.dialogs).toEqual([SAVE]);
  });

  it('run-tool adds the dialogs to a call the modal watch ended', async () => {
    vi.spyOn(ui, 'error').mockImplementation(() => {});
    const run = vi.fn(async () => ({
      result: { kind: 'failure' as const, success: false as const, endpoint: 'http://localhost/api/tools/x', reason: 'http-error' as const, httpStatus: 500,
        data: { error: 'The editor is showing the modal dialog "Save Content" (open for 3 s): its game thread waits' }, message: 'HTTP 500' },
    }));
    const cli = buildRunToolCommand({ name: 'run-tool', description: 'Test', errorNoun: 'tool', invoke: vi.fn(), run, findDialogs: () => [SAVE] });
    const stdout = vi.spyOn(process.stdout, 'write').mockImplementation(() => true);
    await cli.parseAsync(['node', 'cli', 'x', '--format', 'json']);
    const envelope = JSON.parse(String(stdout.mock.calls.at(-1)?.[0])) as { errors: Array<{ code: string; message: string; httpStatus?: number }>; data: { dialogs: EditorDialog[] } };
    expect(process.exitCode).toBe(18);
    // One error with the server's status, its message said once.
    expect(envelope.errors).toHaveLength(1);
    expect(envelope.errors[0]).toMatchObject({ code: 'AWAITING_INPUT', httpStatus: 500 });
    expect(envelope.errors[0]!.message.match(/Failed to run tool/g)).toHaveLength(1);
    expect(envelope.data.dialogs).toEqual([SAVE]);
  });

  it('run-tool keeps its own failure when no dialog is open', async () => {
    vi.spyOn(ui, 'error').mockImplementation(() => {});
    vi.spyOn(ui, 'json').mockImplementation(() => {});
    const run = vi.fn(async () => ({
      result: { kind: 'failure' as const, success: false as const, endpoint: 'http://localhost/api/tools/x', reason: 'timeout' as const, message: 'timed out' },
    }));
    const cli = buildRunToolCommand({ name: 'run-tool', description: 'Test', errorNoun: 'tool', invoke: vi.fn(), run, findDialogs: () => [] });
    await cli.parseAsync(['node', 'cli', 'x']);
    expect(process.exitCode).toBe(14);
  });
});
