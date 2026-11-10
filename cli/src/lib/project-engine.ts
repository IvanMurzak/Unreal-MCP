// The project + engine pair the standalone `compile` and `test` commands run against, and the check that no
// editor already holds the project. Library-safe: never prints, never exits.

import * as fs from 'fs';
import * as path from 'path';
import { platform } from 'os';
import { discoverEngine } from './engine.js';
import { listProcesses, selectEditorProcesses, type RunningProcess } from './close.js';
import { readUProject, type UProjectInfo } from '../utils/project.js';

export interface ProjectEngine {
  project: UProjectInfo;
  engineRoot: string;
  /** The GUI editor binary (`UnrealEditor.exe`). */
  editorPath: string;
}

/** Why the project or engine could not be resolved: `project` → no `.uproject`; `engine` → no engine install. */
export class ProjectEngineError extends Error {
  constructor(readonly kind: 'project' | 'engine', message: string) {
    super(message);
    this.name = 'ProjectEngineError';
  }
}

export interface ResolveProjectEngineOptions {
  /** Project root or `.uproject` path; defaults to `process.cwd()`. */
  projectDir?: string;
  engineRoot?: string;
  noCache?: boolean;
  os?: NodeJS.Platform;
}

/** Resolve the project and the engine that opens it. Throws {@link ProjectEngineError}. */
export function resolveProjectEngine(options: ResolveProjectEngineOptions): ProjectEngine {
  const input = path.resolve(options.projectDir ?? process.cwd());
  const project = readUProject(input);
  if (!project) throw new ProjectEngineError('project', `No .uproject found at ${input}`);
  const resolution = discoverEngine({
    engineAssociation: project.engineAssociation,
    engineRootOverride: options.engineRoot,
    os: options.os ?? (platform() as NodeJS.Platform),
    noCache: options.noCache,
  });
  if (resolution.kind === 'unresolved') throw new ProjectEngineError('engine', resolution.message);
  return { project, engineRoot: resolution.engineRoot, editorPath: resolution.editorPath };
}

/**
 * The console editor binary for unattended runs: `UnrealEditor-Cmd.exe` beside the editor on Windows (stdout and an
 * exit code, no window), else the editor itself.
 */
export function commandletEditorPath(
  editorPath: string,
  os: NodeJS.Platform = platform() as NodeJS.Platform,
  exists: (p: string) => boolean = fs.existsSync,
): string {
  if (os !== 'win32') return editorPath;
  const cmd = path.win32.join(path.win32.dirname(editorPath), 'UnrealEditor-Cmd.exe');
  return exists(cmd) ? cmd : editorPath;
}

/** Hint for a project that an editor holds. */
export const PROJECT_OPEN_HINT =
  'Close it first (`unreal-mcp-cli close --owned-only` for an editor the CLI started, `unreal-mcp-cli close` for any), ' +
  'or compile from the open editor (Live Coding: Ctrl+Alt+F11).';

export interface StandaloneRunFailure {
  /** `not-found`: no `.uproject`; `unreachable`: no engine install; `project-open`: an editor holds the project. */
  reason: 'not-found' | 'unreachable' | 'project-open';
  message: string;
  editorPids?: number[];
}

/**
 * The project and engine of a standalone run (`compile`, `test`) — which needs the project to itself, so a running
 * editor of it is a failure (`why` completes its message).
 */
export async function prepareStandaloneRun(
  options: ResolveProjectEngineOptions & {
    listProcessesImpl?: () => RunningProcess[];
    /**
     * Save and quit an editor the CLI itself started (e.g. a warm `exec` session), returning its pid; the user's editor
     * is never touched. Without it, any editor is a `project-open` failure.
     */
    closeOwnedEditor?: (projectDir: string) => Promise<number | undefined>;
    /** The pid of the editor the CLI started, when one runs: another editor makes closing ours pointless. */
    ownedEditorPid?: (projectDir: string) => number | undefined;
  },
  why: string,
): Promise<(ProjectEngine & { closedOwnedPid?: number }) | StandaloneRunFailure> {
  let resolved: ProjectEngine;
  try {
    resolved = resolveProjectEngine(options);
  } catch (err) {
    const reason = err instanceof ProjectEngineError && err.kind === 'project' ? 'not-found' : 'unreachable';
    return { reason, message: err instanceof Error ? err.message : String(err) };
  }
  let editorPids = runningProjectEditors(resolved.project, options.listProcessesImpl);
  let closedOwnedPid: number | undefined;
  const owned = editorPids.length > 0 ? options.ownedEditorPid?.(resolved.project.projectDir) : undefined;
  // Only when ours is the only one: with the user's editor open too, the run fails anyway.
  if (owned !== undefined && editorPids.every((pid) => pid === owned) && options.closeOwnedEditor) {
    closedOwnedPid = await options.closeOwnedEditor(resolved.project.projectDir);
    if (closedOwnedPid !== undefined) editorPids = runningProjectEditors(resolved.project, options.listProcessesImpl);
  }
  if (editorPids.length === 0) return closedOwnedPid === undefined ? resolved : { ...resolved, closedOwnedPid };
  return {
    reason: 'project-open',
    message: `An Unreal Editor has ${resolved.project.projectName} open (pid ${editorPids.join(', ')}): ${why}. ${PROJECT_OPEN_HINT}`,
    editorPids,
  };
}

/** The pids of running Unreal Editors that have `project` open (the user's GUI, a CLI session or a commandlet). */
export function runningProjectEditors(
  project: UProjectInfo,
  listProcessesImpl: () => RunningProcess[] = () => listProcesses(),
): number[] {
  return selectEditorProcesses(listProcessesImpl(), project.projectDir, project.uprojectPath).map((p) => p.pid);
}
