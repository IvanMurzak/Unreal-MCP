// Editor sessions for `exec`, `open --headless` and `close --owned-only` (Code-Exec plan §4.6): cli-core's
// `EditorSession` plus the Unreal pieces — the editor binary (engine discovery), the pre-launch UBT build (run under
// the session's startup lock), the local connection a started editor serves, the `editor-quit` close request a
// windowless editor needs, and the scan for an editor the user opened. Library-safe: never prints, never exits.

import * as path from 'path';
import { platform } from 'os';
import {
  EditorSession,
  EditorSessionError,
  defaultSessionProcessOps,
  mcpReadinessProbe,
  readSessionLease,
  unrealAdapter,
  type CloseOwnedResult,
  type EditorLaunchSpec,
  type EditorSessionErrorReason,
  type EditorSessionMode,
  type EngineAdapter,
  type EnsureSource,
  type PrepareWindowResult,
  type SessionLease,
  type SessionProcessOps,
  type WaitedEditorSource,
} from '@baizor/gamedev-cli-core';
import { discoverEngine } from './engine.js';
import { listProcesses, selectEditorProcesses, type RunningProcess } from './close.js';
import { buildOpenEnv, maybeBuildBeforeOpen } from './open.js';
import { runSystemTool } from './run-tool.js';
import { resolveConnection, resolveCustomHostUrl, stripTrailingSlash } from '../utils/config.js';
import { asError } from '../utils/error.js';
import { isAbsoluteLoopbackHost, resolveLocalBindPort, tryGetExplicitPort } from '../utils/port.js';
import { readProjectMarker } from '../utils/project-marker.js';
import { readUProject, type UProjectInfo } from '../utils/project.js';
import type { RunToolOptions, RunToolResult } from './types.js';

export { readSessionLease };

/** The plugin's system tool that saves and quits an editor a CLI session started. */
export const EDITOR_QUIT_TOOL = 'editor-quit';
/** A cold start compiles shaders on a fresh project: wait longer than cli-core's 5-minute default. */
export const UNREAL_SESSION_READY_TIMEOUT_MS = 600_000;
/**
 * How long to wait for an editor the user opened (not started by the CLI) to answer on the local server. One in
 * Cloud mode, or connected elsewhere, never does: fail fast instead of waiting out the cold-start timeout.
 */
export const UNREAL_SESSION_FOREIGN_READY_TIMEOUT_MS = 60_000;
/** How long `close --owned-only` waits for the editor to save and exit. */
export const UNREAL_SESSION_CLOSE_TIMEOUT_MS = 60_000;
/** Connection mode of a session: a CLI-started editor always serves the project's local server. */
export const SESSION_CONNECTION_MODE = 'Custom';

/** The project's LOCAL MCP server: the one a CLI-started editor serves and the only one readiness may probe. */
export interface SessionConnection {
  url: string;
  /** Bearer for that server (never a cloud token). */
  token?: string;
  /** The plugin connection mode a started editor gets (always {@link SESSION_CONNECTION_MODE}). */
  connectionMode: typeof SESSION_CONNECTION_MODE;
}

/**
 * Resolve the local server of a project, on the port its sidecar binds (marker `portOverride` → port typed into the
 * host → the deterministic port). A loopback URL from the usual chain (`--url`, env, `.env`, plugin config) keeps its
 * host; otherwise (Cloud mode, a remote host) the project's local port is used: a cloud relay answers whether or not
 * this editor runs, so it can never stand in for readiness. Throws on a non-loopback `--url`, and on a `--url` whose
 * port the project's `portOverride` overrules (the editor would serve another port than the one probed).
 */
export async function resolveSessionConnection(
  projectDir: string,
  overrides: { url?: string; token?: string } = {},
): Promise<SessionConnection> {
  const url = overrides.url?.trim();
  if (url) {
    if (!isAbsoluteLoopbackHost(url)) {
      throw new Error(`--url must be a local (loopback) server for an editor session: ${url}`);
    }
    const bound = resolveCustomHostUrl(url, projectDir);
    const typed = tryGetExplicitPort(url);
    const pinned = tryGetExplicitPort(bound);
    if (typed !== undefined && typed !== pinned) {
      throw new Error(
        `--url ${url} names port ${typed}, but the project pins port ${pinned} ` +
          '(portOverride in .ai-game-dev/project.json), which the editor serves: drop --url or change the portOverride.',
      );
    }
    const resolved = await resolveConnection({ projectDir, url: bound, token: overrides.token });
    return { url: resolved.url, token: resolved.token, connectionMode: SESSION_CONNECTION_MODE };
  }
  const resolved = await resolveConnection({ projectDir, token: overrides.token });
  if (isAbsoluteLoopbackHost(resolved.url)) {
    return { url: resolveCustomHostUrl(resolved.url, projectDir), token: resolved.token, connectionMode: SESSION_CONNECTION_MODE };
  }
  const { port } = resolveLocalBindPort({ projectDir, markerPortOverride: readProjectMarker(projectDir)?.portOverride });
  return { url: `http://localhost:${port}`, token: overrides.token, connectionMode: SESSION_CONNECTION_MODE };
}

/**
 * The connection a started editor gets: Custom mode on the local server, which its plugin starts itself
 * (`UNREAL_MCP_START_SERVER`) and stops on exit. An explicit token is passed on; a stored one is already in the
 * project config the plugin reads.
 */
export function sessionEditorEnv(connection: SessionConnection, explicitToken?: string): Record<string, string> {
  // buildOpenEnv returns undefined only under `noConnect`.
  return buildOpenEnv({
    host: stripTrailingSlash(connection.url),
    connectionMode: connection.connectionMode,
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
  /** Wait for an editor the user opened. Default {@link UNREAL_SESSION_FOREIGN_READY_TIMEOUT_MS} (capped by `readyTimeoutMs`). */
  foreignReadyTimeoutMs?: number;
  /** Engine root override (source builds). */
  engineRoot?: string;
  /** Compile native modules before a launch when needed. Default `true`. */
  build?: boolean;
  noCache?: boolean;
  onStatus?: (message: string) => void;
  // --- Test seams (defaults wire to the real system) ---
  probeReady?: () => Promise<string | undefined>;
  editorBinary?: string | (() => string | Promise<string>);
  /** Replaces the pre-launch build (runs under the startup lock, only when this call launches). */
  beforeLaunch?: (projectRoot: string) => Promise<void>;
  processOps?: Partial<SessionProcessOps>;
  listProcessesImpl?: () => RunningProcess[];
  invokeSystemTool?: (options: RunToolOptions) => Promise<RunToolResult>;
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

  // A cold `ensure` asks "is an editor running?" in each locked round (before and after resolving the binary); each
  // real listing spawns PowerShell, so share one for a few seconds.
  const listProcessesImpl = options.listProcessesImpl ?? (() => listProcesses());
  let listed: { at: number; processes: RunningProcess[] } | undefined;
  const listEditors = (): RunningProcess[] => {
    if (listed === undefined || Date.now() - listed.at > 5_000) listed = { at: Date.now(), processes: listProcessesImpl() };
    return listed.processes;
  };

  // The pid lets the session notice cheaply when that editor quits while it waits for it.
  const findRunningEditor = async (root: string): Promise<number | false> => {
    const project = readUProject(root);
    if (project === null) return false;
    return selectEditorProcesses(listEditors(), project.projectDir, project.uprojectPath)[0]?.pid ?? false;
  };

  // A windowless editor never sees WM_CLOSE: ask our plugin, on the server it was started against, to save and quit.
  // The lease nonce proves to the plugin that the caller owns the session. Fall back to the OS polite quit (enough
  // for a window).
  const requestClose = async (pid: number, _root: string, lease?: SessionLease): Promise<boolean> => {
    if (lease?.pid === pid) {
      const result = await invoke({
        toolName: EDITOR_QUIT_TOOL,
        url: lease.serverUrl ?? connection.url,
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

  let engine: Promise<UnrealEngine> | undefined;
  const resolveEngine = (): Promise<UnrealEngine> => (engine ??= resolveUnrealEngine(options, projectRoot));
  const beforeLaunch =
    options.build === false
      ? undefined
      : (options.beforeLaunch ?? (async () => buildBeforeLaunch(await resolveEngine(), options.onStatus)));

  // An editor started earlier (e.g. with another --url) still answers on the server its lease names.
  const leaseUrl = readSessionLease(projectRoot)?.serverUrl;
  const probeUrls = leaseUrl !== undefined && leaseUrl !== connection.url ? [connection.url, leaseUrl] : [connection.url];
  const readyTimeoutMs = options.readyTimeoutMs ?? UNREAL_SESSION_READY_TIMEOUT_MS;

  return new EditorSession<string>({
    adapter,
    projectRoot,
    probeReady: options.probeReady ?? mcpReadinessProbe({ urls: probeUrls, token: connection.token }),
    editorBinary: options.editorBinary ?? (async () => (await resolveEngine()).editorPath),
    ...(beforeLaunch ? { beforeLaunch } : {}),
    env: { ...sessionEditorEnv(connection, options.token), ...options.env },
    serverUrl: connection.url,
    connectionMode: connection.connectionMode,
    ...(options.idleSeconds !== undefined ? { idleSeconds: options.idleSeconds } : {}),
    readyTimeoutMs,
    foreignReadyTimeoutMs: Math.min(options.foreignReadyTimeoutMs ?? UNREAL_SESSION_FOREIGN_READY_TIMEOUT_MS, readyTimeoutMs),
    ...(options.onStatus ? { onStatus: options.onStatus } : {}),
    ...(options.processOps ? { processOps: options.processOps } : {}),
  });
}

/** The project and the engine that opens it (resolved once a launch is decided). */
interface UnrealEngine {
  project: UProjectInfo;
  engineRoot: string;
  editorPath: string;
}

async function resolveUnrealEngine(options: UnrealSessionOptions, projectRoot: string): Promise<UnrealEngine> {
  const project = readUProject(projectRoot);
  if (!project) throw new Error(`No .uproject found at ${projectRoot}`);
  const resolution = discoverEngine({
    engineAssociation: project.engineAssociation,
    engineRootOverride: options.engineRoot,
    os: platform() as NodeJS.Platform,
    noCache: options.noCache,
  });
  if (resolution.kind === 'unresolved') throw new Error(resolution.message);
  return { project, engineRoot: resolution.engineRoot, editorPath: resolution.editorPath };
}

/** The UBT build of the editor modules, when needed. Runs under the startup lock: parallel callers never build twice. */
async function buildBeforeLaunch({ project, engineRoot }: UnrealEngine, onStatus?: (message: string) => void): Promise<void> {
  const warnings: string[] = [];
  await maybeBuildBeforeOpen(
    {
      projectDir: project.projectDir,
      projectName: project.projectName,
      uprojectPath: project.uprojectPath,
      engineRoot,
      onProgress: (event) => onStatus?.(event.message),
    },
    warnings,
  );
  for (const warning of warnings) onStatus?.(warning);
}

export interface SessionCommandOptions extends Partial<Omit<UnrealSessionOptions, 'projectDir' | 'connection'>> {
  /** Project root or `.uproject` path; defaults to `process.cwd()`. */
  projectDir?: string;
  /** Local server URL override (loopback only). */
  url?: string;
}

/** Options of a command that reaches the CLI-owned editor through its lease (no `--url`). */
export type LeaseSessionOptions = Omit<SessionCommandOptions, 'url'>;

export type SessionCommandResult<T> =
  | ({ kind: 'success'; success: true; projectDir: string } & T)
  | {
      kind: 'failure';
      success: false;
      message: string;
      reason?: EditorSessionErrorReason;
      /** Which editor the session waited for (`foreign` = one the user opened). */
      source?: WaitedEditorSource;
      error: Error;
    };

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

/** Unreal hint for an editor the user opened that never answers on the local server. */
const FOREIGN_NOT_READY_HINT =
  'An Unreal Editor in Cloud mode never answers there: switch its AI Game Developer connection to Custom (local).';

/**
 * The message for a session that could not provide a ready editor: cli-core's message plus the local URL that was
 * probed, and for an editor the user opened that never answered, the Unreal Cloud-mode hint.
 */
export function sessionFailureMessage(err: unknown, url: string): string {
  const { message } = asError(err);
  if (!(err instanceof EditorSessionError)) return message;
  const foreign = err.reason === 'not-ready' && err.source === 'foreign';
  return `${message} (local MCP server: ${url})${foreign ? `. ${FOREIGN_NOT_READY_HINT}` : ''}`;
}

async function withSession<T>(
  options: SessionCommandOptions,
  run: (session: EditorSession<string>) => Promise<T>,
): Promise<SessionCommandResult<T>> {
  let url: string | undefined;
  try {
    const { projectDir, connection, session } = await openProjectSession(options);
    url = connection.url;
    return { kind: 'success', success: true, projectDir, ...(await run(session)) };
  } catch (err) {
    return {
      kind: 'failure',
      success: false,
      message: url === undefined ? asError(err).message : sessionFailureMessage(err, url),
      ...(err instanceof EditorSessionError ? { reason: err.reason, source: err.source } : {}),
      error: asError(err),
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

/** `close --owned-only`: save and quit the editor only when this project's CLI lease names it (on the lease's server). */
export function closeOwnedSession(
  options: LeaseSessionOptions & { timeoutMs?: number; force?: boolean },
): Promise<SessionCommandResult<{ result: CloseOwnedResult }>> {
  return withSession(options, async (session) => ({
    result: await session.closeOwned({ timeoutMs: options.timeoutMs ?? UNREAL_SESSION_CLOSE_TIMEOUT_MS, force: options.force }),
  }));
}

/** A GUI `open`: close our headless editor first, if one holds the project. No lease → nothing to do. */
export async function prepareGuiOpen(
  options: LeaseSessionOptions & { timeoutMs?: number },
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
