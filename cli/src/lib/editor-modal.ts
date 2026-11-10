// A modal dialog that blocks the editor's tool calls. The plugin's FUnrealMcpModalWatch ends the waiting calls with
// a fixed reason and logs `[Unreal-MCP] Editor blocked by modal dialog "<title>": ...` (and, when the dialog stack
// closes, `[Unreal-MCP] Modal dialog "<title>" closed; tool calls run again.`). The wording is a contract with
// the plugin (UnrealMcpModalWatch.cpp). Library-safe.

import * as fs from 'fs';
import * as path from 'path';
import type { UProjectInfo } from '../utils/project.js';

const BLOCKED_LINE = /\[Unreal-MCP\] Editor blocked by modal dialog "(.*)": /g;
const CLOSED_LINE = /\[Unreal-MCP\] Modal dialog "(.*)" closed; tool calls run again\./g;
/** The reason a tool call ends with while a modal dialog holds the game thread. */
const TOOL_BLOCKED = /The editor is showing the modal dialog "(.*?)" \(open for/;
/** How much of the end of the editor log is read. */
const LOG_TAIL_BYTES = 512 * 1024;

/** The title of the dialog a tool call's error text names, if it ended because of one. */
export function modalDialogInToolError(text: string): string | undefined {
  return TOOL_BLOCKED.exec(text)?.[1];
}

/** The title of the dialog that blocks the editor according to its log: the last "blocked" line with no "closed" after it. */
export function blockingDialogInLog(text: string): string | undefined {
  let blocked: { index: number; title: string } | undefined;
  for (const m of text.matchAll(BLOCKED_LINE)) blocked = { index: m.index ?? 0, title: m[1]! };
  if (!blocked) return undefined;
  let closedAt = -1;
  for (const m of text.matchAll(CLOSED_LINE)) closedAt = m.index ?? 0;
  return closedAt > blocked.index ? undefined : blocked.title;
}

/** The editor log of a project (`Saved/Logs/<Project>.log`, where an editor without `-abslog` writes). */
export function editorLogPath(project: Pick<UProjectInfo, 'projectDir' | 'projectName'>): string {
  return path.join(project.projectDir, 'Saved', 'Logs', `${project.projectName}.log`);
}

/** The last {@link LOG_TAIL_BYTES} of a file; `undefined` when it cannot be read. */
export function readFileTail(file: string, bytes = LOG_TAIL_BYTES): string | undefined {
  let fd: number | undefined;
  try {
    fd = fs.openSync(file, 'r');
    const size = fs.fstatSync(fd).size;
    const length = Math.min(size, bytes);
    const buffer = Buffer.alloc(length);
    fs.readSync(fd, buffer, 0, length, size - length);
    return buffer.toString('utf-8');
  } catch {
    return undefined;
  } finally {
    if (fd !== undefined) fs.closeSync(fd);
  }
}

/**
 * The dialog blocking a RUNNING editor of `project`, from its log. Only meaningful while an editor runs: a new editor
 * starts a new log, so the caller checks that first.
 */
export function blockingDialogOfProject(
  project: Pick<UProjectInfo, 'projectDir' | 'projectName'>,
  readTail: (file: string) => string | undefined = readFileTail,
): string | undefined {
  const text = readTail(editorLogPath(project));
  return text === undefined ? undefined : blockingDialogInLog(text);
}
