// Modal dialogs of a running Unreal Editor: a dialog holds the editor's game thread, so every tool call waits until it is
// answered. Slate dialogs are invisible to Windows UI Automation (Slate's accessibility is off by default and, even on,
// leaves a modal window without children), so the plugin serves them from its modal loop (FUnrealMcpDialogControl)
// through files in <Project>/Saved/UnrealMCP/Dialogs/:
//
// - `<pid>.json`: the dialog on top in editor <pid>, `{"pid", "id", "title", "message", "buttons"}` (gone when none is open);
// - `<pid>.press.json`: a press request written here, `{"request", "button", "id"}`, taken by the plugin within 0.1 s;
// - `<pid>.result.json`: the plugin's answer, `{"request", "result": "pressed" | "not-found", "button", "message"}`.
//
// Works in every connection mode (Cloud too) and on every desktop OS, with no server. A dialog shown before the plugin
// loaded, or a native message box, is not served. Library-safe: never throws, never prints.

import { randomUUID } from 'crypto';
import * as fs from 'fs';
import * as path from 'path';
import { isProcessAlive, writeFileAtomicSync } from '@baizor/gamedev-cli-core';
import { listProcesses, selectEditorProcesses, type RunningProcess } from './close.js';
import { FAILURE, type FailureKind } from '../utils/output.js';
import { readUProject, type UProjectInfo } from '../utils/project.js';

export interface EditorDialog {
  /** Stable while the dialog stays on top in its editor; pass it to `pressEditorDialogButton`. Decimal digits. */
  id: string;
  pid: number;
  title: string;
  /** The dialog's visible texts, one per line. */
  message: string;
  /** Labels of its buttons (icon-only buttons have none and are left out). */
  buttons: string[];
}

type PressDialogResult =
  | { kind: 'pressed'; button: string; dialog: EditorDialog }
  | { kind: 'not-found'; message: string }
  /** The request was written but the editor did not take it in time (a dialog it does not serve, or a busy disk). */
  | { kind: 'no-answer'; message: string };

/** File access of the dialog exchange; injected by tests. */
export interface DialogFileIo {
  /** The file's text; `undefined` when it does not exist or cannot be read. */
  read(file: string): string | undefined;
  /** Replace the file atomically (a reader never sees half of it). */
  write(file: string, text: string): void;
  remove(file: string): void;
  /** The names of the files in a directory (none when it does not exist). */
  list(dir: string): string[];
  /** Whether process `pid` is running. */
  alive(pid: number): boolean;
  sleep(ms: number): Promise<void>;
  now(): number;
}

export const nodeDialogFileIo: DialogFileIo = {
  read: (file) => {
    try {
      return fs.readFileSync(file, 'utf-8');
    } catch {
      return undefined;
    }
  },
  write: (file, text) => {
    fs.mkdirSync(path.dirname(file), { recursive: true });
    writeFileAtomicSync(file, Buffer.from(text, 'utf-8'));
  },
  remove: (file) => fs.rmSync(file, { force: true }),
  list: (dir) => {
    try {
      return fs.readdirSync(dir);
    } catch {
      return [];
    }
  },
  alive: isProcessAlive, // EPERM counts as alive: running, but not ours to signal
  sleep: (ms) => new Promise((resolve) => setTimeout(resolve, ms)),
  now: () => Date.now(),
};

/** How long a press waits for the editor to answer. The plugin looks every 0.1 s while the dialog is open. */
const PRESS_TIMEOUT_MS = 5_000;
const PRESS_POLL_MS = 50;
/** How long a request the editor took at the deadline still has to be answered. */
const PRESS_GRACE_MS = 2_000;

/** Shown when no dialog is served: what the user can still do. */
export const NO_DIALOG_HINT =
  'A dialog shown before the UnrealMCP plugin loaded (or by a plugin older than this CLI) is not visible here: answer it in the editor.';

function dialogDir(projectDir: string): string {
  return path.join(projectDir, 'Saved', 'UnrealMCP', 'Dialogs');
}

/** Where editor `pid` of the project at `projectDir` serves its dialog files. */
export function dialogFile(projectDir: string, pid: number, suffix: '.json' | '.press.json' | '.result.json' = '.json'): string {
  return path.join(dialogDir(projectDir), `${pid}${suffix}`);
}

function isStringArray(value: unknown): value is string[] {
  return Array.isArray(value) && value.every((v) => typeof v === 'string');
}

/** A published dialog, validated; `undefined` for anything else (a half-written or foreign file). Exported for tests. */
export function parseDialog(text: string | undefined, pid: number): EditorDialog | undefined {
  if (text === undefined) return undefined;
  let value: unknown;
  try {
    value = JSON.parse(text);
  } catch {
    return undefined;
  }
  const d = value as Partial<EditorDialog> | null;
  if (!d || typeof d.id !== 'string' || !/^\d+$/.test(d.id) || typeof d.title !== 'string') return undefined;
  if (d.pid !== pid) return undefined; // another process's file under this name
  return {
    id: d.id,
    pid,
    title: d.title,
    message: typeof d.message === 'string' ? d.message : '',
    buttons: isStringArray(d.buttons) ? d.buttons : [],
  };
}

/** The modal dialogs the editors `pids` of the project at `projectDir` show (one per editor at most: the top one). */
export function listEditorDialogs(projectDir: string, pids: readonly number[], io: DialogFileIo = nodeDialogFileIo): EditorDialog[] {
  const dialogs: EditorDialog[] = [];
  for (const pid of pids) {
    const dialog = parseDialog(io.read(dialogFile(projectDir, pid)), pid);
    if (dialog) dialogs.push(dialog);
  }
  return dialogs;
}

/**
 * The dialogs published for the project at `projectDirOrFile` (its folder or its `.uproject`) by editors of it that
 * are still running. Cheap when none is published (a directory read): only a file of a live process costs the process
 * listing that confirms it is an editor of this project (a file a crashed editor left behind may name a reused pid).
 */
export function publishedDialogs(projectDirOrFile: string, deps: FindDialogsDeps = {}): EditorDialog[] {
  const io = deps.io ?? nodeDialogFileIo;
  const project = readUProject(path.resolve(projectDirOrFile));
  if (!project) return [];
  const candidates = io.list(dialogDir(project.projectDir))
    .map((name) => /^(\d+)\.json$/.exec(name)?.[1])
    .filter((pid): pid is string => pid !== undefined)
    .map(Number)
    .filter((pid) => io.alive(pid));
  if (candidates.length === 0) return [];
  const listed = selectEditorProcesses((deps.listProcessesImpl ?? listProcesses)(), project.projectDir, project.uprojectPath);
  const editors = new Set(listed.map((p) => p.pid));
  return listEditorDialogs(project.projectDir, candidates.filter((pid) => editors.has(pid)), io);
}

function sameLabel(a: string, b: string): boolean {
  return a.trim().toLowerCase() === b.trim().toLowerCase();
}

/**
 * Press the button labelled `button` (case-insensitive) on dialog `id`, or on the first open dialog that has such a
 * button when `id` is omitted. The label travels as JSON data in the request file, never as code.
 */
export async function pressEditorDialogButton(
  projectDir: string,
  pids: readonly number[],
  button: string,
  { id, io = nodeDialogFileIo, timeoutMs = PRESS_TIMEOUT_MS }: { id?: string; io?: DialogFileIo; timeoutMs?: number } = {},
): Promise<PressDialogResult> {
  const dialogs = listEditorDialogs(projectDir, pids, io);
  if (dialogs.length === 0) return { kind: 'not-found', message: `No modal dialog is open in the editor. ${NO_DIALOG_HINT}` };
  const open = dialogs.map(describeDialog).join('; ');
  const matches = id !== undefined ? dialogs.filter((d) => d.id === id) : dialogs.filter((d) => d.buttons.some((b) => sameLabel(b, button)));
  // Ids count per editor: with several editors of the project open, an id may name a dialog in each.
  if (id !== undefined && matches.length > 1) {
    return { kind: 'not-found', message: `Dialog ${id} is open in several editors (PID ${matches.map((d) => d.pid).join(', ')}): close all but one.` };
  }
  const target = matches[0];
  if (!target) {
    return {
      kind: 'not-found',
      message: id !== undefined ? `Dialog ${id} is not open (open: ${open}).` : `No open dialog has a "${button}" button (open: ${open}).`,
    };
  }

  const request = randomUUID();
  const pressFile = dialogFile(projectDir, target.pid, '.press.json');
  const resultFile = dialogFile(projectDir, target.pid, '.result.json');
  try {
    removeQuietly(io, resultFile);
    io.write(pressFile, JSON.stringify({ request, button, id: target.id }));
  } catch (err) {
    return { kind: 'no-answer', message: `Could not write ${pressFile}: ${err instanceof Error ? err.message : String(err)}` };
  }
  let deadline = io.now() + timeoutMs;
  let extended = false;
  for (;;) {
    const answer = parseAnswer(io.read(resultFile));
    if (answer?.request === request) {
      removeQuietly(io, resultFile);
      return answer.result === 'pressed'
        ? { kind: 'pressed', button: answer.button || button, dialog: target }
        : { kind: 'not-found', message: answer.message || `The dialog "${target.title}" has no "${button}" button.` };
    }
    if (io.now() >= deadline) {
      // Taken just in time: its answer is on the way, so wait a moment rather than misreport a press.
      if (extended || io.read(pressFile) !== undefined) break;
      extended = true;
      deadline = io.now() + PRESS_GRACE_MS;
    }
    await io.sleep(PRESS_POLL_MS);
  }
  removeQuietly(io, pressFile); // never pressed later, behind the caller's back
  return {
    kind: 'no-answer',
    message: `The editor (PID ${target.pid}) did not take the request within ${Math.round(timeoutMs / 1000)} s: the dialog "${target.title}" ` +
      'may have closed, or it is not one the plugin can answer. Answer it in the editor.',
  };
}

/** Remove a file, ignoring a failure (Windows refuses while the editor has it open): a library never throws. */
function removeQuietly(io: DialogFileIo, file: string): void {
  try {
    io.remove(file);
  } catch {
    // left for the next request, which replaces it
  }
}

function parseAnswer(text: string | undefined): { request?: string; result?: string; button?: string; message?: string } | undefined {
  if (text === undefined) return undefined;
  try {
    const value = JSON.parse(text) as unknown;
    return value && typeof value === 'object' ? (value as { request?: string }) : undefined;
  } catch {
    return undefined; // not complete yet
  }
}

/** `"Title": message [Yes | No]`, on one line. */
export function describeDialog(d: EditorDialog): string {
  const text = [d.title ? `"${d.title}"` : '', d.message.replace(/\s+/g, ' ').trim()].filter(Boolean).join(': ');
  return `${text}${text ? ' ' : ''}[${d.buttons.join(' | ')}]`;
}

/** The AWAITING_INPUT message for `dialogs`, with how to answer them. */
export function awaitingInputMessage(dialogs: readonly EditorDialog[]): string {
  const answer = dialogs.some((d) => d.buttons.length > 0) ? ', or with `unreal-mcp-cli dialog --press <button>`' : '';
  return `The Unreal Editor is blocked by a modal dialog: ${dialogs.map(describeDialog).join('; ')}. Answer it in the editor${answer}.`;
}

/** Test seams of {@link findProjectDialogs}. */
interface FindDialogsDeps {
  listProcessesImpl?: () => RunningProcess[];
  io?: DialogFileIo;
}

/**
 * The editors running the project at `projectDir` and the dialogs they show. `undefined` when there is no project
 * there. Never throws.
 */
export function findProjectDialogs(
  projectDir: string,
  deps: FindDialogsDeps = {},
): { project: UProjectInfo; pids: number[]; dialogs: EditorDialog[] } | undefined {
  const project = readUProject(path.resolve(projectDir));
  if (!project) return undefined;
  const editors = selectEditorProcesses((deps.listProcessesImpl ?? listProcesses)(), project.projectDir, project.uprojectPath);
  const pids = editors.map((p) => p.pid);
  return { project, pids, dialogs: listEditorDialogs(project.projectDir, pids, deps.io) };
}

/** Failures a blocking dialog can cause (a timeout, a busy or unreachable editor, a call ended by the modal watch). */
const DIALOG_SUSPECTS: ReadonlySet<string> = new Set(['TIMEOUT', 'BUSY', 'UNREACHABLE', 'AWAITING_INPUT']);

/** A command failure: what it exits with, its message, and the envelope data. */
export interface CommandFailure {
  kind: FailureKind;
  message: string;
  data: Record<string, unknown>;
}

/**
 * The failure a command reports, given the one it found: when a modal dialog can cause it and the project's editor
 * shows one, AWAITING_INPUT (exit 18) naming the dialogs, with them in `data.dialogs` (a call the modal watch already
 * ended keeps its own message, which names the dialog). Otherwise `failure` unchanged. `find`: test seam.
 */
export function withBlockingDialogs(
  projectDir: string,
  failure: CommandFailure,
  find: (projectDir: string) => EditorDialog[] = (dir) => publishedDialogs(dir),
): CommandFailure {
  if (!DIALOG_SUSPECTS.has(failure.kind.code)) return failure;
  const dialogs = find(projectDir);
  if (dialogs.length === 0) return failure;
  // Already AWAITING_INPUT (the tool error named the dialog): keep its kind, with the server's HTTP status, and message.
  const already = failure.kind.code === FAILURE.awaitingInput.code;
  const message = already ? failure.message : awaitingInputMessage(dialogs);
  return { kind: already ? failure.kind : FAILURE.awaitingInput, message, data: { ...failure.data, dialogs } };
}
