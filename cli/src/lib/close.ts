// `close` — terminate the Unreal Editor process running a given project.
// Process discovery + kill are injectable so the matching logic is unit-
// testable without touching real processes. Library-safe.

import * as path from 'path';
import { execFileSync } from 'child_process';
import { platform } from 'os';
import { findUProjectFile } from '../utils/project.js';
import { asError } from '../utils/error.js';
import { emitProgress } from './progress.js';
import type { ProgressCallback } from './types.js';

export interface RunningProcess {
  pid: number;
  commandLine: string;
}

export interface CloseOptions {
  projectDir?: string;
  os?: NodeJS.Platform;
  /** Injectable process list (defaults to a platform-specific query). */
  listProcessesImpl?: () => RunningProcess[];
  /** Injectable kill (defaults to `process.kill`). */
  killImpl?: (pid: number) => void;
  onProgress?: ProgressCallback;
}

export interface CloseSuccess {
  kind: 'success';
  success: true;
  /** PIDs that were signalled. */
  terminated: number[];
  /** `true` when no matching editor was running. */
  wasRunning: boolean;
  warnings: string[];
}

export interface CloseFailure {
  kind: 'failure';
  success: false;
  warnings: string[];
  error: Error;
}

export type CloseResult = CloseSuccess | CloseFailure;

/**
 * Select the editor processes whose command line references both the
 * `.uproject` (or its directory) AND an `UnrealEditor` binary. Pure —
 * exported for tests.
 */
export function selectEditorProcesses(
  processes: RunningProcess[],
  projectDir: string,
  uprojectPath: string | null,
): RunningProcess[] {
  // Normalize separators so a backslash project path matches a command line
  // written with forward slashes (and vice versa) — UE / shells emit either.
  const norm = (s: string): string => s.toLowerCase().replace(/\\/g, '/');
  const dirNeedle = norm(projectDir);
  const fileNeedle = uprojectPath ? norm(uprojectPath) : null;
  return processes.filter((p) => {
    const cmd = norm(p.commandLine);
    if (!cmd.includes('unrealeditor')) return false;
    if (fileNeedle && cmd.includes(fileNeedle)) return true;
    return containsDirBounded(cmd, dirNeedle);
  });
}

/**
 * True when `cmd` references `dirNeedle` at a path-segment boundary — i.e.
 * the needle is followed by a `/` or the end of the string. Prevents a bare
 * substring match where `/work/MyGame` would also match `/work/MyGame2`.
 * Both inputs are already normalized (lowercased, forward slashes).
 */
function containsDirBounded(cmd: string, dirNeedle: string): boolean {
  let from = 0;
  for (;;) {
    const idx = cmd.indexOf(dirNeedle, from);
    if (idx < 0) return false;
    const after = cmd.charAt(idx + dirNeedle.length);
    if (after === '' || after === '/') return true;
    from = idx + 1;
  }
}

export async function close(opts: CloseOptions = {}): Promise<CloseResult> {
  const warnings: string[] = [];
  try {
    const os = opts.os ?? (platform() as NodeJS.Platform);
    const projectDir = path.resolve(opts.projectDir ?? process.cwd());
    const uprojectPath = findUProjectFile(projectDir);
    if (!uprojectPath) {
      // Without a `.uproject` we'd be forced to match editors by directory
      // alone — and running from an ancestor dir would then terminate every
      // editor under it. Refuse instead of guessing.
      throw new Error(
        `No .uproject found in ${projectDir}; refusing to match editors by directory alone.`,
      );
    }

    emitProgress(opts.onProgress, { phase: 'start', message: `Closing Unreal Editor for ${projectDir}` });

    const processes = opts.listProcessesImpl ? opts.listProcessesImpl() : listProcesses(os);
    const matches = selectEditorProcesses(processes, projectDir, uprojectPath);

    if (matches.length === 0) {
      emitProgress(opts.onProgress, { phase: 'done', message: 'No running editor matched this project.' });
      return { kind: 'success', success: true, terminated: [], wasRunning: false, warnings };
    }

    const kill = opts.killImpl ?? ((pid: number) => process.kill(pid));
    const terminated: number[] = [];
    for (const m of matches) {
      try {
        kill(m.pid);
        terminated.push(m.pid);
      } catch (err) {
        warnings.push(`Failed to terminate PID ${m.pid}: ${asError(err).message}`);
      }
    }

    emitProgress(opts.onProgress, { phase: 'done', message: `Terminated ${terminated.length} process(es).` });
    return { kind: 'success', success: true, terminated, wasRunning: true, warnings };
  } catch (err: unknown) {
    return {
      kind: 'failure',
      success: false,
      warnings,
      error: asError(err),
    };
  }
}

/** Best-effort platform process listing. Never throws — `[]` on failure. */
function listProcesses(os: NodeJS.Platform): RunningProcess[] {
  try {
    if (os === 'win32') {
      // CIM CommandLine via PowerShell — CSV pid|cmdline.
      const out = execFileSync(
        'powershell',
        [
          '-NoProfile',
          '-Command',
          "Get-CimInstance Win32_Process | Where-Object { $_.Name -like 'UnrealEditor*' } | ForEach-Object { \"$($_.ProcessId)|$($_.CommandLine)\" }",
        ],
        { encoding: 'utf-8', timeout: 30000 },
      );
      return parseProcLines(out, '|');
    }
    const out = execFileSync('ps', ['-axo', 'pid=,command='], { encoding: 'utf-8', timeout: 30000 });
    return out
      .split(/\r?\n/)
      .map((line) => line.trim())
      .filter((line) => line.length > 0)
      .map((line) => {
        const sp = line.indexOf(' ');
        const pid = parseInt(line.slice(0, sp), 10);
        return { pid, commandLine: line.slice(sp + 1) };
      })
      .filter((p) => !Number.isNaN(p.pid));
  } catch {
    return [];
  }
}

function parseProcLines(out: string, sep: string): RunningProcess[] {
  return out
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter((line) => line.length > 0)
    .map((line) => {
      const idx = line.indexOf(sep);
      const pid = parseInt(idx >= 0 ? line.slice(0, idx) : line, 10);
      return { pid, commandLine: idx >= 0 ? line.slice(idx + 1) : '' };
    })
    .filter((p) => !Number.isNaN(p.pid));
}
