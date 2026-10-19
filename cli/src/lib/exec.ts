// `exec` — run Python in the project's Unreal Editor through `python-execute`, starting a warm headless editor
// (`-RenderOffscreen`) when none is running. Library-safe: never prints, never exits.

import {
  EditorSessionError,
  type EditorSessionErrorReason,
  type EnsureSource,
  type WaitedEditorSource,
} from '@baizor/gamedev-cli-core';
import { runTool } from './run-tool.js';
import { isToolSuccess, openProjectSession, sessionFailureMessage, type UnrealSessionOptions } from './session.js';
import { asError } from '../utils/error.js';
import type { RunToolFailure, RunToolOptions, RunToolResult } from './types.js';

export const PYTHON_EXECUTE_TOOL = 'python-execute';
/** The plugin's answer while PythonScriptPlugin is not initialized (UnrealMcpPythonTools.cpp). */
const PYTHON_NOT_AVAILABLE_PREFIX = 'NotAvailable: Python is not available';
/** How long a fresh editor (started or joined by this call) gets for Python to come up after MCP answers. */
export const PYTHON_STARTUP_GRACE_MS = 30_000;
const PYTHON_STARTUP_RETRY_MS = 1_000;
/**
 * Client-side tool call timeout: a Python snippet may build or save assets. Stays above the server's wait for the
 * editor's default 300 s tool timeout (`UNREAL_MCP_TOOL_TIMEOUT` + 10 s, passed as `tool-timeout`) so the editor's
 * or the server's own timeout error arrives first.
 */
export const DEFAULT_EXEC_TIMEOUT_MS = 320_000;

export interface ExecPythonOptions {
  /** Project root or `.uproject` path; defaults to `process.cwd()`. */
  projectDir?: string;
  code: string;
  /** Helper `.py` files / folders, relative to `<Project>/.ai-game-dev/snippets` or absolute. */
  include?: string[];
  /** Start the editor with a window instead of headless (only when none is running). */
  window?: boolean;
  /** A started editor quits after this many seconds without tool calls. Default 300; `0` = never. */
  idleSeconds?: number;
  readyTimeoutMs?: number;
  /** Tool call timeout. Default 320000. */
  timeoutMs?: number;
  /** Local server URL / token overrides. */
  url?: string;
  token?: string;
  engineRoot?: string;
  build?: boolean;
  onStatus?: (message: string) => void;
  /** Test seams: session hooks and the tool call. */
  sessionOptions?: Partial<UnrealSessionOptions>;
  invoke?: (options: RunToolOptions) => Promise<RunToolResult>;
  sleep?: (ms: number) => Promise<void>;
}

export interface ExecPythonSuccess {
  kind: 'success';
  success: true;
  /** The server that ran the code. */
  baseUrl: string;
  /** How the editor was obtained (`started` = this call launched it). */
  source: EnsureSource;
  /** The raw `python-execute` response. */
  data: unknown;
}

export interface ExecPythonFailure {
  kind: 'failure';
  success: false;
  /** `invalid-input`: bad project / URL; `session`: no ready editor (see `sessionReason`); `tool-failed`: the call failed. */
  reason: 'invalid-input' | 'session' | 'tool-failed';
  sessionReason?: EditorSessionErrorReason;
  /** Which editor the session waited for (`foreign` = one the user opened). */
  sessionSource?: WaitedEditorSource;
  toolFailure?: RunToolFailure;
  message: string;
  error?: Error;
}

export type ExecPythonResult = ExecPythonSuccess | ExecPythonFailure;

/**
 * Run Python in the project's editor. A running editor (the user's GUI or a CLI session) is reused and never closed;
 * otherwise one is started headless (or with a window) that saves and quits after `idleSeconds` without tool calls.
 */
export async function execPython(options: ExecPythonOptions): Promise<ExecPythonResult> {
  let opened;
  try {
    opened = await openProjectSession({
      projectDir: options.projectDir,
      url: options.url,
      token: options.token,
      idleSeconds: options.idleSeconds,
      readyTimeoutMs: options.readyTimeoutMs,
      engineRoot: options.engineRoot,
      build: options.build,
      onStatus: options.onStatus,
      ...options.sessionOptions,
    });
  } catch (err) {
    const error = asError(err);
    return { kind: 'failure', success: false, reason: 'invalid-input', message: error.message, error };
  }
  const { connection, session } = opened;

  let ready: { ready: string; source: EnsureSource };
  try {
    ready = await session.ensure({ mode: options.window ? 'window' : 'headless' });
  } catch (err) {
    const sessionError = err instanceof EditorSessionError ? err : undefined;
    return {
      kind: 'failure',
      success: false,
      reason: 'session',
      ...(sessionError ? { sessionReason: sessionError.reason, sessionSource: sessionError.source } : {}),
      message: sessionFailureMessage(err, connection.url),
      error: asError(err),
    };
  }

  options.onStatus?.('Running...');
  const invoke = options.invoke ?? runTool;
  const call = (): Promise<RunToolResult> => invoke({
    toolName: PYTHON_EXECUTE_TOOL,
    url: ready.ready,
    ...(connection.token ? { token: connection.token } : {}),
    input: { code: options.code, ...(options.include && options.include.length > 0 ? { include: options.include } : {}) },
    timeoutMs: options.timeoutMs ?? DEFAULT_EXEC_TIMEOUT_MS,
  });
  let result = await call();
  // A just-started editor serves MCP before PythonScriptPlugin is up: give a fresh editor a short grace period.
  if (ready.source === 'started' || ready.source === 'owned') {
    const sleep = options.sleep ?? ((ms: number) => new Promise<void>((resolve) => setTimeout(resolve, ms)));
    const deadline = Date.now() + PYTHON_STARTUP_GRACE_MS;
    while (pythonNotYetAvailable(result) && Date.now() < deadline) {
      options.onStatus?.('Waiting for Python in the editor...');
      await sleep(PYTHON_STARTUP_RETRY_MS);
      result = await call();
    }
  }
  if (result.kind === 'failure') {
    return { kind: 'failure', success: false, reason: 'tool-failed', toolFailure: result, message: toolErrorText(result), error: result.error };
  }
  return { kind: 'success', success: true, baseUrl: ready.ready, source: ready.source, data: result.data };
}

/** Whether `python-execute` answered that the editor's Python interpreter is not (yet) initialized. */
export function pythonNotYetAvailable(result: RunToolResult): boolean {
  const text = result.kind === 'failure' ? toolErrorText(result) : readPythonResponse(result.data).value;
  return text.startsWith(PYTHON_NOT_AVAILABLE_PREFIX);
}

/** A failed call's own error (the server answers a tool error as HTTP 500 `{ "error": "<text>" }`), else the HTTP reason. */
export function toolErrorText(failure: RunToolFailure): string {
  const error = (failure.data as { error?: unknown } | null | undefined)?.error;
  return typeof error === 'string' && error.length > 0 ? error : failure.message;
}

export interface PythonLogLine {
  level: string;
  message: string;
}

/**
 * The parts of a `python-execute` answer a terminal shows. Success is answered as `{ status, structured }` where
 * `structured` is the Code-Exec response (`value`, `logs`, ...); an error as `{ status: "error", content: [text] }`.
 */
export function readPythonResponse(data: unknown): { ok: boolean; value: string; logs: PythonLogLine[] } {
  if (typeof data === 'string') return { ok: true, value: data, logs: [] };
  const response = data as {
    status?: string;
    content?: Array<{ type?: string; text?: string }>;
    structured?: { success?: boolean; value?: unknown; logs?: Array<{ level?: unknown; message?: unknown }> } | null;
  } | null;
  const structured = response?.structured;
  if (structured && typeof structured === 'object') {
    const logs = (Array.isArray(structured.logs) ? structured.logs : [])
      .filter((log) => typeof log?.message === 'string')
      .map((log) => ({ level: typeof log.level === 'string' ? log.level : 'info', message: log.message as string }));
    const value = structured.value === undefined ? 'null' : typeof structured.value === 'string' ? structured.value : JSON.stringify(structured.value);
    return { ok: isToolSuccess(data) && structured.success !== false, value, logs };
  }
  const texts = (response?.content ?? []).filter((c) => c?.type === 'text' && typeof c.text === 'string').map((c) => c.text!);
  return {
    ok: isToolSuccess(data),
    value: texts.length > 0 ? texts.join('\n') : data === undefined ? '' : JSON.stringify(data),
    logs: [],
  };
}
