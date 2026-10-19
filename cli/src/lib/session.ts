// Editor sessions for `exec`, `open --headless` and `close --owned-only` (Code-Exec plan §4.6): cli-core's
// `EditorSession` plus the Unreal pieces — the editor binary (engine discovery + the pre-launch build), the local
// connection a started editor serves, the `editor-quit` close request a windowless editor needs, and the scan for an
// editor the user opened. Library-safe: never prints, never exits.

import * as fs from 'fs';
import * as path from 'path';
import { platform } from 'os';
import {
  EditorSession,
  EditorSessionError,
  defaultSessionProcessOps,
  mcpReadinessProbe,
  parseSessionLease,
  unrealAdapter,
  SESSION_DIR_NAME,
  SESSION_LEASE_FILE_NAME,
  type CloseOwnedResult,
  type EditorLaunchSpec,
  type EditorSessionErrorReason,
  type EditorSessionMode,
  type EngineAdapter,
  type EnsureSource,
  type PrepareWindowResult,
  type SessionLease,
  type SessionProcessOps,
} from '@baizor/gamedev-cli-core';
import { discoverEngine } from './engine.js';
import { listProcesses, selectEditorProcesses, type RunningProcess } from './close.js';
import { buildOpenEnv, maybeBuildBeforeOpen } from './open.js';
import { runSystemTool } from './run-tool.js';
import { resolveConnection, stripTrailingSlash } from '../utils/config.js';
import { asError } from '../utils/error.js';
import { isAbsoluteLoopbackHost, resolveLocalBindPort } from '../utils/port.js';
import { readProjectMarker } from '../utils/project-marker.js';
import { readUProject, type UProjectInfo } from '../utils/project.js';
import type { RunToolOptions, RunToolResult } from './types.js';

/** The plugin's system tool that saves and quits an editor a CLI session started. */
export const EDITOR_QUIT_TOOL = 'editor-quit';
/** A cold start compiles shaders on a fresh project: wait longer than cli-core's 5-minute default. */
export const UNREAL_SESSION_READY_TIMEOUT_MS = 600_000;
/** How long `close --owned-only` waits for the editor to save and exit. */
export const UNREAL_SESSION_CLOSE_TIMEOUT_MS = 60_000;

/** The project's LOCAL MCP server: the one a CLI-started editor serves and the only one readiness may probe. */
export interface SessionConnection {
  url: string;
  /** Bearer for that server (never a cloud token). */
  token?: string;
}

/**
 * Resolve the local server of a project. A loopback URL from the usual chain (`--url`, env, `.env`, plugin config)
 * is used as is; otherwise (Cloud mode, a remote host) the project's deterministic local port. A cloud relay answers
 * whether or not this editor runs, so it can never stand in for readiness. Throws on a non-loopback `--url`.
 */
export async function resolveSessionConnection(
  projectDir: string,
  overrides: { url?: string; token?: string } = {},
): Promise<SessionConnection> {
  if (overrides.url !== undefined && overrides.url.trim().length > 0) {
    if (!isAbsoluteLoopbackHost(overrides.url)) {
      throw new Error(`--url must be a local (loopback) server for an editor session: ${overrides.url}`);
    }
    const resolved = await resolveConnection({ projectDir, url: overrides.url, token: overrides.token });
    return { url: resolved.url, token: resolved.token };
  }
  const resolved = await resolveConnection({ projectDir, token: overrides.token });
  if (isAbsoluteLoopbackHost(resolved.url)) return { url: resolved.url, token: resolved.token };
  const { port } = resolveLocalBindPort({ projectDir, markerPortOverride: readProjectMarker(projectDir)?.portOverride });
  return { url: `http://localhost:${port}`, token: overrides.token };
}

/**
 * The connection a started editor gets: Custom mode on the local server, which its plugin starts itself
 * (`UNREAL_MCP_START_SERVER`) and stops on exit. An explicit token is passed on; a stored one is already in the
 * project config the plugin reads.
 */
export function sessionEditorEnv(connection: SessionConnection, explicitToken?: string): Record<string, string> {
  // A host infers Custom mode; buildOpenEnv returns undefined only under `noConnect`.
  return buildOpenEnv({
    host: stripTrailingSlash(connection.url),
    transport: 'http',
    startServer: true,
    keepConnected: true,
    ...(explicitToken ? { token: explicitToken } : {}),
  })!;
}

export interface UnrealSessionOptions {
  /** The project root (the directory holding the `.uproject`). */
  projectDir: string;
  connection: SessionConnection;
  /** An explicit token (`--token`): handed to a started editor as UNREAL_MCP_TOKEN. */
  token?: string;
  /** Extra environment for a started editor (on top of {@link sessionEditorEnv}). */
  env?: Record<string, string>;
  /** Idle seconds before a started editor quits; `0` = never. Default 300. */
  idleSeconds?: number;
  readyTimeoutMs?: number;
  /** Engine root override (source builds). */
  engineRoot?: string;
  /** Compile native modules before a launch when needed. Default `true`. */
  build?: boolean;
  noCache?: boolean;
  onStatus?: (message: string) => void;
  // --- Test seams (defaults wire to the real system) ---
  probeReady?: () => Promise<string | undefined>;
  editorBinary?: string | (() => string | Promise<string>);
  processOps?: Partial<SessionProcessOps>;
  listProcessesImpl?: () => RunningProcess[];
  invokeSystemTool?: (options: RunToolOptions) => Promise<RunToolResult>;
}

/** The project's lease as stored (not validated); undefined when absent or unreadable. */
export function readSessionLease(projectDir: string): SessionLease | undefined {
  try {
    return parseSessionLease(fs.readFileSync(path.join(projectDir, SESSION_DIR_NAME, SESSION_LEASE_FILE_NAME), 'utf-8'));
  } catch {
    return undefined;
  }
}

/** Whether a direct tool response reports success (`{ status: "success" | "error", ... }`). */
export function isToolSuccess(data: unknown): boolean {
  const status = (data as { status?: unknown } | null)?.status;
  return status === undefined || status === 'success';
}

/** A cli-core session for an Unreal project: the generic lease / lock / reuse policy with the Unreal hooks. */
export function createUnrealSession(options: UnrealSessionOptions): EditorSession<string> {
  const projectRoot = path.resolve(options.projectDir);
  const ops: SessionProcessOps = { ...defaultSessionProcessOps, ...options.processOps };
  const invoke = options.invokeSystemTool ?? runSystemTool;
  const { connection } = options;

  // One cold `ensure` asks "is an editor running?" up to three times within seconds (binary resolver + two locked
  // rounds); each real listing spawns PowerShell, so share one for a few seconds.
  const listProcessesImpl = options.listProcessesImpl ?? (() => listProcesses());
  let listed: { at: number; processes: RunningProcess[] } | undefined;
  const listEditors = (): RunningProcess[] => {
    if (listed === undefined || Date.now() - listed.at > 5_000) listed = { at: Date.now(), processes: listProcessesImpl() };
    return listed.processes;
  };

  const findRunningEditor = async (root: string): Promise<boolean> => {
    const project = readUProject(root);
    return project !== null && selectEditorProcesses(listEditors(), project.projectDir, project.uprojectPath).length > 0;
  };

  // A windowless editor never sees WM_CLOSE: ask our plugin to save and quit. The lease nonce proves to the plugin
  // that the caller owns the session. Fall back to the OS polite quit (enough for a window).
  const requestClose = async (pid: number): Promise<boolean> => {
    const lease = session.readLease();
    if (lease?.pid === pid) {
      const result = await invoke({
        toolName: EDITOR_QUIT_TOOL,
        url: connection.url,
        ...(connection.token ? { token: connection.token } : {}),
        input: { nonce: lease.nonce },
        timeoutMs: 15_000,
      });
      if (result.kind === 'success' && isToolSuccess(result.data)) return true;
    }
    return ops.requestClose(pid);
  };

  const editor: EditorLaunchSpec = { ...unrealAdapter.editor!, requestClose, findRunningEditor };
  const adapter: EngineAdapter = { ...unrealAdapter, editor };

  // EditorSession resolves the binary as soon as the warm probe fails, even when it then joins an editor that is
  // still starting; never build next to a running editor (its modules are loaded and Live Coding refuses).
  // A lease counts only while it is provably live (a crashed editor's pid may be reused by another process).
  const editorStarting = async (): Promise<boolean> =>
    (await session.ownedSession()) !== undefined || (await findRunningEditor(projectRoot));

  const session: EditorSession<string> = new EditorSession<string>({
    adapter,
    projectRoot,
    probeReady: options.probeReady ?? mcpReadinessProbe({ urls: [connection.url], token: connection.token }),
    editorBinary: options.editorBinary ?? (() => resolveEditorBinary(options, projectRoot, editorStarting)),
    env: { ...sessionEditorEnv(connection, options.token), ...options.env },
    ...(options.idleSeconds !== undefined ? { idleSeconds: options.idleSeconds } : {}),
    readyTimeoutMs: options.readyTimeoutMs ?? UNREAL_SESSION_READY_TIMEOUT_MS,
    ...(options.onStatus ? { onStatus: options.onStatus } : {}),
    ...(options.processOps ? { processOps: options.processOps } : {}),
  });
  return session;
}

async function resolveEditorBinary(
  options: UnrealSessionOptions,
  projectRoot: string,
  editorStarting: () => Promise<boolean>,
): Promise<string> {
  const project = readUProject(projectRoot);
  if (!project) throw new Error(`No .uproject found at ${projectRoot}`);
  const resolution = discoverEngine({
    engineAssociation: project.engineAssociation,
    engineRootOverride: options.engineRoot,
    os: platform() as NodeJS.Platform,
    noCache: options.noCache,
  });
  if (resolution.kind === 'unresolved') throw new Error(resolution.message);

  if (options.build !== false && !(await editorStarting())) {
    const warnings: string[] = [];
    await maybeBuildBeforeOpen(
      {
        projectDir: project.projectDir,
        projectName: project.projectName,
        uprojectPath: project.uprojectPath,
        engineRoot: resolution.engineRoot,
        onProgress: (event) => options.onStatus?.(event.message),
      },
      warnings,
    );
    for (const warning of warnings) options.onStatus?.(warning);
  }
  return resolution.editorPath;
}

export interface SessionCommandOptions extends Partial<Omit<UnrealSessionOptions, 'projectDir' | 'connection'>> {
  /** Project root or `.uproject` path; defaults to `process.cwd()`. */
  projectDir?: string;
  /** Local server URL override (loopback only). */
  url?: string;
}

export type SessionCommandResult<T> =
  | ({ kind: 'success'; success: true; projectDir: string } & T)
  | { kind: 'failure'; success: false; message: string; reason?: EditorSessionErrorReason; error: Error };

/** The project (root or `.uproject` path, default cwd); throws naming the path when there is none. */
export function resolveProject(projectDir: string | undefined): UProjectInfo {
  const input = path.resolve(projectDir ?? process.cwd());
  const project = readUProject(input);
  if (!project) throw new Error(`No .uproject found at ${input}`);
  return project;
}

/** Resolve the project and its local connection, and build its session. Throws on a bad project or `--url`. */
export async function openProjectSession(
  options: SessionCommandOptions,
): Promise<{ projectDir: string; connection: SessionConnection; session: EditorSession<string> }> {
  const { projectDir } = resolveProject(options.projectDir);
  const connection = await resolveSessionConnection(projectDir, { url: options.url, token: options.token });
  return { projectDir, connection, session: createUnrealSession({ ...options, projectDir, connection }) };
}

async function withSession<T>(
  options: SessionCommandOptions,
  run: (session: EditorSession<string>) => Promise<T>,
): Promise<SessionCommandResult<T>> {
  try {
    const { projectDir, session } = await openProjectSession(options);
    return { kind: 'success', success: true, projectDir, ...(await run(session)) };
  } catch (err) {
    const error = asError(err);
    return {
      kind: 'failure',
      success: false,
      message: error.message,
      ...(err instanceof EditorSessionError ? { reason: err.reason } : {}),
      error,
    };
  }
}

/** `open --headless` / `open --idle`: reuse a running editor or start a CLI-owned one, and wait until it is ready. */
export function openEditorSession(
  options: SessionCommandOptions & { mode: EditorSessionMode },
): Promise<SessionCommandResult<{ url: string; source: EnsureSource; pid?: number }>> {
  return withSession(options, async (session) => {
    const ensured = await session.ensure({ mode: options.mode });
    return { url: ensured.ready, source: ensured.source, ...(ensured.lease ? { pid: ensured.lease.pid } : {}) };
  });
}

/** `close --owned-only`: save and quit the editor only when this project's CLI lease names it. */
export function closeOwnedSession(
  options: SessionCommandOptions & { timeoutMs?: number; force?: boolean },
): Promise<SessionCommandResult<{ result: CloseOwnedResult }>> {
  return withSession(options, async (session) => ({
    result: await session.closeOwned({ timeoutMs: options.timeoutMs ?? UNREAL_SESSION_CLOSE_TIMEOUT_MS, force: options.force }),
  }));
}

/** A GUI `open`: close our headless editor first, if one holds the project. No lease → nothing to do. */
export async function prepareGuiOpen(
  options: SessionCommandOptions & { timeoutMs?: number },
): Promise<SessionCommandResult<{ result: PrepareWindowResult }>> {
  const input = path.resolve(options.projectDir ?? process.cwd());
  const projectDir = readUProject(input)?.projectDir ?? input;
  // Before any session exists: no lease means nothing of ours to stop (and no connection to resolve).
  if (readSessionLease(projectDir) === undefined) {
    return { kind: 'success', success: true, projectDir, result: { kind: 'proceed' } };
  }
  return withSession(options, async (session) => ({
    result: await session.prepareWindowOpen({ timeoutMs: options.timeoutMs ?? UNREAL_SESSION_CLOSE_TIMEOUT_MS }),
  }));
}
