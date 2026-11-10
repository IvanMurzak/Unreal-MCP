// `compile` — build the project's editor target with UnrealBuildTool, standalone (no editor launch), and return
// structured diagnostics. Library-safe: never prints, never exits.

import * as fs from 'fs';
import { platform } from 'os';
import { resolveProjectEngine, runningProjectEditors, ProjectEngineError } from './project-engine.js';
import type { RunningProcess } from './close.js';
import type { UbtBuildStep } from './types.js';
import { DiagnosticCollector, type CompilerDiagnostic } from '../utils/compiler-diagnostics.js';
import { isDesktopBuildPlatform, resolveDesktopBuildStep, spawnUnrealBuild, type UnrealBuildExit } from '../utils/unreal-build.js';
import { asError } from '../utils/error.js';

export interface CompileOptions {
  /** Project root or `.uproject` path; defaults to `process.cwd()`. */
  projectDir?: string;
  engineRoot?: string;
  /** Warnings fail the build too. */
  strict?: boolean;
  /** Every UBT output line, as it comes. */
  onLine?: (line: string) => void;
  /** Every new diagnostic, as it comes. */
  onDiagnostic?: (diagnostic: CompilerDiagnostic) => void;
  // --- Test seams ---
  os?: NodeJS.Platform;
  listProcessesImpl?: () => RunningProcess[];
  existsImpl?: (p: string) => boolean;
  buildImpl?: (step: UbtBuildStep, onLine: (line: string) => void) => Promise<UnrealBuildExit>;
}

export interface CompileReport {
  target?: string;
  projectDir?: string;
  /** UBT's exit code, when it ran. */
  exitCode?: number | null;
  errors: CompilerDiagnostic[];
  warnings: CompilerDiagnostic[];
  durationMs: number;
}

export type CompileFailureReason =
  /** No `.uproject` at the path. */
  | 'not-found'
  /** No engine install, or no UnrealBuildTool in it, or the host cannot build. */
  | 'unreachable'
  /** An editor has the project open: UBT would fight its Live Coding / hot reload. */
  | 'project-open'
  /** UBT failed, or reported errors (or warnings under `strict`). */
  | 'compilation-failed';

export type CompileResult =
  | ({ kind: 'success'; success: true } & CompileReport)
  | ({ kind: 'failure'; success: false; reason: CompileFailureReason; message: string; editorPids?: number[] } & CompileReport);

/** Hint for a project that an editor holds. */
export const PROJECT_OPEN_HINT =
  'Close it first (`unreal-mcp-cli close --owned-only` for an editor the CLI started, `unreal-mcp-cli close` for any), ' +
  'or compile from the open editor (Live Coding: Ctrl+Alt+F11).';

export async function compileProject(options: CompileOptions = {}): Promise<CompileResult> {
  const started = Date.now();
  const empty = (): CompileReport => ({ errors: [], warnings: [], durationMs: Date.now() - started });
  const os = options.os ?? (platform() as NodeJS.Platform);

  let resolved;
  try {
    resolved = resolveProjectEngine({ projectDir: options.projectDir, engineRoot: options.engineRoot, os });
  } catch (err) {
    const reason = err instanceof ProjectEngineError && err.kind === 'project' ? 'not-found' : 'unreachable';
    return { kind: 'failure', success: false, reason, message: asError(err).message, ...empty() };
  }
  const { project, engineRoot } = resolved;
  const base = { projectDir: project.projectDir };

  if (!isDesktopBuildPlatform(os)) {
    return { kind: 'failure', success: false, reason: 'unreachable', message: `Building is not supported on ${os}.`, ...base, ...empty() };
  }
  const step = resolveDesktopBuildStep({ platform: os, engineRoot, projectName: project.projectName, uprojectPath: project.uprojectPath });
  const report = { ...base, target: step.editorTarget };
  if (!(options.existsImpl ?? fs.existsSync)(step.ubtPath)) {
    return { kind: 'failure', success: false, reason: 'unreachable', message: `UnrealBuildTool not found: ${step.ubtPath}`, ...report, ...empty() };
  }

  const editorPids = runningProjectEditors(project, options.listProcessesImpl);
  if (editorPids.length > 0) {
    return {
      kind: 'failure',
      success: false,
      reason: 'project-open',
      message: `An Unreal Editor has ${project.projectName} open (pid ${editorPids.join(', ')}): building now would fight its Live Coding / hot reload. ${PROJECT_OPEN_HINT}`,
      editorPids,
      ...report,
      ...empty(),
    };
  }

  const collector = new DiagnosticCollector();
  let exit: UnrealBuildExit;
  try {
    exit = await (options.buildImpl ?? spawnUnrealBuild)(step, (line) => {
      options.onLine?.(line);
      const diagnostic = collector.line(line);
      if (diagnostic) options.onDiagnostic?.(diagnostic);
    });
  } catch (err) {
    return { kind: 'failure', success: false, reason: 'unreachable', message: `Could not start UnrealBuildTool: ${asError(err).message}`, ...report, ...empty() };
  }

  const result: CompileReport = {
    ...report,
    exitCode: exit.code,
    errors: collector.errors,
    warnings: collector.warnings,
    durationMs: Date.now() - started,
  };
  if (exit.code !== 0 || result.errors.length > 0) {
    const count = result.errors.length;
    const message = count > 0
      ? `${step.editorTarget}: ${count} error${count === 1 ? '' : 's'} (UnrealBuildTool exit code ${exit.code}).`
      : `${step.editorTarget}: UnrealBuildTool failed with exit code ${exit.code}.\n${exit.tail.trim().slice(-2048)}`;
    return { kind: 'failure', success: false, reason: 'compilation-failed', message, ...result };
  }
  if (options.strict && result.warnings.length > 0) {
    const count = result.warnings.length;
    return {
      kind: 'failure',
      success: false,
      reason: 'compilation-failed',
      message: `${step.editorTarget}: ${count} warning${count === 1 ? '' : 's'} (--strict).`,
      ...result,
    };
  }
  return { kind: 'success', success: true, ...result };
}
