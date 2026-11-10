import { afterEach, describe, expect, it } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import { blockingDialogInLog, blockingDialogOfProject, editorLogPath, modalDialogInToolError, readFileTail } from '../src/lib/editor-modal.js';
import { getStatus } from '../src/lib/status.js';
import { makeTempDir, rmTempDir, writeUProject } from './helpers.js';

const dirs: string[] = [];
afterEach(() => {
  while (dirs.length) rmTempDir(dirs.pop()!);
});

// The plugin's wording (UnrealMcpModalWatch.cpp).
const BLOCKED = '[2026.11.09-10.00.00:000][  0]LogUnrealMcp: Error: [Unreal-MCP] Editor blocked by modal dialog "Save Content": its game thread waits for the dialog to close, so MCP tool calls cannot run.';
const CLOSED = '[2026.11.09-10.00.05:000][  1]LogUnrealMcp: Display: [Unreal-MCP] Modal dialog "Save Content" closed; tool calls run again.';

describe('modal dialog detection', () => {
  it('finds the dialog the log reports as blocking, until it closes', () => {
    expect(blockingDialogInLog(`a\n${BLOCKED}\nb`)).toBe('Save Content');
    expect(blockingDialogInLog(`${BLOCKED}\n${CLOSED}`)).toBeUndefined();
    expect(blockingDialogInLog(`${BLOCKED}\n${CLOSED}\n${BLOCKED.replace('Save Content', 'Delete Assets')}`)).toBe('Delete Assets');
    expect(blockingDialogInLog('LogInit: Display: Engine is initialized.')).toBeUndefined();
  });

  it('reads the dialog from a tool error', () => {
    expect(modalDialogInToolError('The editor is showing the modal dialog "Save Content" (open for 3 s): its game thread')).toBe('Save Content');
    expect(modalDialogInToolError('No such actor')).toBeUndefined();
  });

  it('reads the tail of the project log', () => {
    const dir = makeTempDir();
    dirs.push(dir);
    const project = { projectDir: dir, projectName: 'Game' };
    fs.mkdirSync(path.dirname(editorLogPath(project)), { recursive: true });
    fs.writeFileSync(editorLogPath(project), `${'x'.repeat(100)}\n${BLOCKED}\n`);
    expect(blockingDialogOfProject(project)).toBe('Save Content');
    expect(readFileTail(editorLogPath(project), 10)).toHaveLength(10);
    expect(readFileTail(path.join(dir, 'missing.log'))).toBeUndefined();
  });

  it('status reports the blocking dialog of a running editor only', async () => {
    const dir = makeTempDir();
    dirs.push(dir);
    const uproject = writeUProject(dir, 'Game');
    const running = [{ pid: 9, commandLine: `UnrealEditor.exe "${uproject}"` }];
    const readLogTail = () => BLOCKED;
    const open = await getStatus({ projectDir: dir, noProbe: true, listProcessesImpl: () => running, readLogTail });
    expect(open.editor).toEqual({ running: true, pids: [9], modalDialog: 'Save Content' });
    const closed = await getStatus({ projectDir: dir, noProbe: true, listProcessesImpl: () => [], readLogTail });
    expect(closed.editor).toEqual({ running: false, pids: [] });
  });
});
