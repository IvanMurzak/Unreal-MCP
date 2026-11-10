// `run-tool` / `run-system-tool` with an editor session: call the tool where the project's editor answers; when no
// editor answers, start a headless one like `exec` (reusing any running editor, never closing the user's), call it
// there, and with `once` close the editor again if THIS call started it. Library-safe: never prints, never exits.

import {
  EditorSessionError,
  type CloseOwnedResult,
  type EditorSessionErrorReason,
  type EnsureSource,
} from '@baizor/gamedev-cli-core';
import { openProjectSession, sessionFailureMessage, UNREAL_SESSION_CLOSE_TIMEOUT_MS, type UnrealSessionOptions } from './session.js';
import { isAbsoluteLoopbackHost } from '../utils/port.js';
import { asError } from '../utils/error.js';
import type { RunToolFailure, RunToolOptions, RunToolResult } from './types.js';

export interface RunToolSessionOptions extends RunToolOptions {
  invoke: (options: RunToolOptions) => Promise<RunToolResult>;
  /** Start a headless editor when none answers. Default `true`. */
  start?: boolean;
  /** Close the editor after the call when this call started it. */
  once?: boolean;
  nullrhi?: boolean;
  readyTimeoutMs?: number;
  idleSeconds?: number;
  engineRoot?: string;
  build?: boolean;
  onStatus?: (message: string) => void;
  /** Test seam: session hooks. */
  sessionOptions?: Partial<UnrealSessionOptions>;
}

export interface RunToolSessionResult {
  /** The tool call's result, when the tool was called. */
  result?: RunToolResult;
  /** How the editor was obtained when a session was used (`started` = this call launched it). */
  source?: EnsureSource;
  /** Set when no editor answered and none could be started. */
  sessionFailure?: { reason?: EditorSessionErrorReason; message: string };
  /** `once`: the close of the editor this call started. */
  closed?: CloseOwnedResult | { kind: 'error'; message: string };
}

/** A failure that means "no editor / server answered there" (worth starting an editor for). */
export function isUnreachableFailure(failure: RunToolFailure): boolean {
  if (failure.reason === 'connection-refused' || failure.reason === 'connection-reset' || failure.reason === 'network-error') return true;
  // The local server is up but no editor (sidecar) is connected to it.
  return failure.reason === 'http-error' && /no connected clients/i.test(failure.message);
}

export async function runToolWithSession(options: RunToolSessionOptions): Promise<RunToolSessionResult> {
  const { invoke, start, once, nullrhi, readyTimeoutMs, idleSeconds, engineRoot, build, onStatus, sessionOptions, ...call } = options;
  // An explicit remote --url is not ours to start: only a project's local server can be served by a started editor.
  const canStart = start !== false && (call.url === undefined || isAbsoluteLoopbackHost(call.url));

  const first = await invoke(call);
  if (!canStart || first.kind === 'success' || !isUnreachableFailure(first)) return { result: first };

  onStatus?.('No editor answered: starting a headless Unreal Editor...');
  let opened;
  try {
    opened = await openProjectSession({
      projectDir: call.projectDir,
      url: call.url,
      token: call.token,
      readyTimeoutMs,
      idleSeconds,
      engineRoot,
      build,
      nullrhi,
      onStatus,
      ...sessionOptions,
    });
  } catch (err) {
    return { sessionFailure: { message: `${first.message}; could not start an editor: ${asError(err).message}` } };
  }
  const { connection, session } = opened;

  let ready;
  try {
    ready = await session.ensure({ mode: 'headless' });
  } catch (err) {
    return {
      sessionFailure: {
        ...(err instanceof EditorSessionError ? { reason: err.reason } : {}),
        message: sessionFailureMessage(err, connection.url),
      },
    };
  }

  onStatus?.('Running...');
  const result = await invoke({ ...call, url: ready.ready, ...(connection.token ? { token: connection.token } : {}) });
  if (!once || ready.source !== 'started') return { result, source: ready.source };

  onStatus?.('Closing the editor this call started...');
  let closed: RunToolSessionResult['closed'];
  try {
    closed = await session.closeOwned({ timeoutMs: UNREAL_SESSION_CLOSE_TIMEOUT_MS, force: true });
  } catch (err) {
    closed = { kind: 'error', message: asError(err).message };
  }
  return { result, source: ready.source, closed };
}
