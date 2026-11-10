// `test` — run Unreal Automation tests headless in a fresh `UnrealEditor-Cmd` (-nullrhi) and read the verdict from
// the exported report (`<dir>/index.json`), never from the editor's exit code. Library-safe: never prints, never exits.

import * as fs from 'fs';
import * as path from 'path';
import { spawn, execFile, type ChildProcess } from 'child_process';
import { platform } from 'os';
import { maybeBuildBeforeOpen } from './open.js';
import { commandletEditorPath, prepareStandaloneRun } from './project-engine.js';
import type { RunningProcess } from './close.js';
import type { UbtBuildStep } from './types.js';
import { parseAutomationReport, type AutomationSummary } from '../utils/automation-report.js';
import { asError } from '../utils/error.js';
import { forEachLine } from '../utils/unreal-build.js';

export const DEFAULT_TEST_FILTER = 'Project';
export const DEFAULT_TEST_TIMEOUT_SECONDS = 3600;
/** How long an editor that wrote its report gets to quit on its own before its tree is stopped. */
const QUIT_GRACE_MS = 30_000;
const POLL_MS = 1_000;
const LOG_TAIL_LINES = 20;
/** What UE logs when the filter selects no test (FAutomationExecCmd / the automation controller). */
const NO_TESTS_MATCHED = /No automation tests matched|Found 0 automation tests/i;

export interface RunTestsOptions {
  /** Project root or `.uproject` path; defaults to `process.cwd()`. */
  projectDir?: string;
  engineRoot?: string;
  /** `Automation RunTests` filter. Default {@link DEFAULT_TEST_FILTER}. */
  filter?: string;
  /** Report directory. Default `<project>/Saved/Automation/UnrealMcpCli/<timestamp>`. */
  outputDir?: string;
  /** The whole run (build excluded) is stopped after this many seconds. */
  timeoutSeconds?: number;
  /** Build the editor target first when needed. Default `true`. */
  build?: boolean;
  onStatus?: (message: string) => void;
  /** Every line the editor prints (`-stdout`). */
  onLog?: (line: string) => void;
  // --- Test seams ---
  os?: NodeJS.Platform;
  listProcessesImpl?: () => RunningProcess[];
  buildImpl?: (step: UbtBuildStep) => Promise<void>;
  spawnImpl?: (command: string, args: string[], env: NodeJS.ProcessEnv) => ChildProcess;
  killTreeImpl?: (pid: number) => Promise<void>;
  editorBinary?: string;
  sleep?: (ms: number) => Promise<void>;
  pollMs?: number;
  quitGraceMs?: number;
}

export interface RunTestsRun {
  filter: string;
  reportDir: string;
  logPath: string;
  editorPid?: number;
}

export type RunTestsResult =
  | ({ kind: 'completed'; summary: AutomationSummary; warnings: string[] } & RunTestsRun)
  | ({
      kind: 'failure';
      /**
       * `not-found`: no `.uproject`; `unreachable`: no engine / editor; `project-open`: an editor holds the project;
       * `build-failed`: the editor target did not compile; `timeout`: stopped after `timeoutSeconds`;
       * `no-verdict`: the editor ended without a readable report (crash, startup error).
       */
      reason: 'not-found' | 'unreachable' | 'project-open' | 'build-failed' | 'timeout' | 'no-verdict';
      message: string;
      warnings: string[];
      logTail?: string[];
    } & Partial<RunTestsRun>);

/** The editor command line of a test run. */
export function automationArgs(uprojectPath: string, filter: string, reportDir: string, logPath: string): string[] {
  return [
    uprojectPath,
    '-nullrhi',
    '-unattended',
    '-nosplash',
    '-NoSound',
    '-stdout',
    '-ddc=NoZenLocal',
    `-ExecCmds=Automation RunTests ${filter};Quit`,
    `-ReportExportPath=${reportDir}`,
    `-abslog=${logPath}`,
  ];
}

/**
 * The environment of a test editor: no inherited `UNREAL_MCP_*` credentials or endpoints, the plugin in Custom mode
 * without a connection (as `scripts/extension_suite.py` runs the suite).
 */
export function automationEnv(base: NodeJS.ProcessEnv = process.env): NodeJS.ProcessEnv {
  const env: NodeJS.ProcessEnv = {};
  for (const [key, value] of Object.entries(base)) if (!key.toUpperCase().startsWith('UNREAL_MCP_')) env[key] = value;
  return { ...env, UNREAL_MCP_CONNECTION_MODE: 'Custom', UNREAL_MCP_KEEP_CONNECTED: 'false' };
}

function timestamp(date = new Date()): string {
  const p = (n: number): string => String(n).padStart(2, '0');
  return `${date.getFullYear()}${p(date.getMonth() + 1)}${p(date.getDate())}-${p(date.getHours())}${p(date.getMinutes())}${p(date.getSeconds())}`;
}

/** Read and parse `<dir>/index.json`; `undefined` while it is missing or still being written. */
export function readReport(reportDir: string): AutomationSummary | undefined {
  try {
    return parseAutomationReport(fs.readFileSync(path.join(reportDir, 'index.json'), 'utf-8'));
  } catch {
    return undefined;
  }
}

/** Stop a process and everything it started (ShaderCompileWorker, zenserver, ...). */
export function killProcessTree(pid: number, os: NodeJS.Platform = platform() as NodeJS.Platform): Promise<void> {
  return new Promise((resolve) => {
    if (os === 'win32') {
      execFile('taskkill', ['/PID', String(pid), '/T', '/F'], { windowsHide: true }, () => resolve());
      return;
    }
    try {
      process.kill(-pid, 'SIGKILL'); // the process group: spawned detached
    } catch {
      try { process.kill(pid, 'SIGKILL'); } catch { /* already gone */ }
    }
    resolve();
  });
}

export async function runAutomationTests(options: RunTestsOptions = {}): Promise<RunTestsResult> {
  const os = options.os ?? (platform() as NodeJS.Platform);
  const filter = options.filter?.trim() || DEFAULT_TEST_FILTER;
  const warnings: string[] = [];
  const sleep = options.sleep ?? ((ms: number) => new Promise<void>((r) => setTimeout(r, ms)));

  const resolved = prepareStandaloneRun(
    { projectDir: options.projectDir, engineRoot: options.engineRoot, os, listProcessesImpl: options.listProcessesImpl },
    'a test run needs the project to itself (to test in the open editor, call its tools with `unreal-mcp-cli run-tool`)',
  );
  if ('reason' in resolved) return { kind: 'failure', reason: resolved.reason, message: resolved.message, warnings };
  const { project, engineRoot, editorPath } = resolved;

  if (options.build !== false) {
    try {
      await maybeBuildBeforeOpen(
        {
          projectDir: project.projectDir,
          projectName: project.projectName,
          uprojectPath: project.uprojectPath,
          engineRoot,
          ...(options.buildImpl ? { buildImpl: options.buildImpl } : {}),
          onProgress: (event) => options.onStatus?.(event.message),
        },
        warnings,
      );
    } catch (err) {
      return { kind: 'failure', reason: 'build-failed', message: asError(err).message, warnings };
    }
  }

  const reportDir = path.resolve(options.outputDir ?? path.join(project.projectDir, 'Saved', 'Automation', 'UnrealMcpCli', timestamp()));
  const logPath = path.join(reportDir, 'editor.log');
  try {
    fs.mkdirSync(reportDir, { recursive: true });
    fs.rmSync(path.join(reportDir, 'index.json'), { force: true }); // a stale report is no verdict
  } catch (err) {
    return { kind: 'failure', reason: 'no-verdict', message: `Cannot prepare the report directory ${reportDir}: ${asError(err).message}`, warnings };
  }
  const run: RunTestsRun = { filter, reportDir, logPath };

  const binary = options.editorBinary ?? commandletEditorPath(editorPath, os);
  const args = automationArgs(project.uprojectPath, filter, reportDir, logPath);
  const env = automationEnv();
  const spawnImpl = options.spawnImpl
    ?? ((command: string, argv: string[], childEnv: NodeJS.ProcessEnv) =>
      spawn(command, argv, { cwd: project.projectDir, env: childEnv, stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true, detached: os !== 'win32' }));
  const killTree = options.killTreeImpl ?? ((pid: number) => killProcessTree(pid, os));

  options.onStatus?.(`Running Automation tests "${filter}" in ${path.basename(binary)} (report: ${reportDir})`);
  let child: ChildProcess;
  try {
    child = spawnImpl(binary, args, env);
  } catch (err) {
    return { kind: 'failure', reason: 'unreachable', message: `Could not start ${binary}: ${asError(err).message}`, warnings, ...run };
  }
  run.editorPid = child.pid;

  const tail: string[] = [];
  let noTestsMatched = false;
  const onLine = (line: string): void => {
    if (!line.trim()) return;
    if (NO_TESTS_MATCHED.test(line)) noTestsMatched = true;
    tail.push(line);
    if (tail.length > LOG_TAIL_LINES) tail.shift();
    options.onLog?.(line);
  };
  forEachLine(child.stdout, onLine);
  forEachLine(child.stderr, onLine);
  let exited = false;
  let spawnError: Error | undefined;
  child.on('exit', () => { exited = true; });
  child.on('error', (err) => { spawnError = err; exited = true; });

  const pollMs = options.pollMs ?? POLL_MS;
  const deadline = Date.now() + (options.timeoutSeconds ?? DEFAULT_TEST_TIMEOUT_SECONDS) * 1000;
  let summary: AutomationSummary | undefined;
  while (!exited && Date.now() < deadline) {
    summary = readReport(reportDir);
    if (summary) break;
    await sleep(pollMs);
  }

  if (summary && !exited) {
    // The report is final: give the editor its `Quit`, then stop what is left of it.
    const graceEnd = Math.min(deadline, Date.now() + (options.quitGraceMs ?? QUIT_GRACE_MS));
    while (!exited && Date.now() < graceEnd) await sleep(pollMs);
  }
  if (!exited) {
    if (child.pid !== undefined) await killTree(child.pid);
    if (!summary) {
      return {
        kind: 'failure',
        reason: 'timeout',
        message: `The test run did not finish within ${options.timeoutSeconds ?? DEFAULT_TEST_TIMEOUT_SECONDS} s; the editor was stopped. Log: ${logPath}`,
        warnings,
        logTail: tail,
        ...run,
      };
    }
  }
  summary ??= readReport(reportDir);

  if (spawnError) {
    return { kind: 'failure', reason: 'unreachable', message: `Could not start ${binary}: ${spawnError.message}`, warnings, ...run };
  }
  if (!summary && noTestsMatched) {
    summary = { total: 0, passed: 0, failed: 0, notRun: 0, durationSeconds: 0, tests: [] };
  }
  if (!summary) {
    return {
      kind: 'failure',
      reason: 'no-verdict',
      message: `The editor exited without an Automation report (crash or startup error). Log: ${logPath}`,
      warnings,
      logTail: tail,
      ...run,
    };
  }
  if (summary.total === 0) warnings.push(`No Automation tests matched the filter "${filter}".`);
  return { kind: 'completed', summary, warnings, ...run };
}
