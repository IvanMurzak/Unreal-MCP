// `run-tool` / `run-system-tool` with an editor session: call the tool where the project's editor answers; when no
// editor answers, start a headless one like `exec` (reusing any running editor, never closing the user's), call it
// there, and with `once` close the editor again if THIS call started it. Library-safe: never prints, never exits.

import {
  EditorSessionError,
  type EditorSession,
  type CloseOwnedResult,
  type EditorSessionErrorReason,
  type EnsureSource,
} from '@baizor/gamedev-cli-core';
import { liveOwnedEditorPid, openProjectSession, sessionFailureMessage, UNREAL_SESSION_CLOSE_TIMEOUT_MS, type UnrealSessionOptions } from './session.js';
import { classifyToolFailure } from './tool-outcome.js';
import { PreLaunchBuildError } from './open.js';
import { FAILURE } from '../utils/output.js';
import { isAbsoluteLoopbackHost } from '../utils/port.js';
import { isAuthFailure, LOCAL_FALLBACK_NOTE, mustStayInCloud, SIGNED_IN_BUT_UNUSABLE_MESSAGE, usesProjectCloudMode, type CredentialState } from './cloud-auth.js';
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
  /** Test seam: the machine's ai-game.dev credential store state (default: cli-core MachineCredentialStore.readState). */
  credentialState?: () => CredentialState;
}

export interface RunToolSessionResult {
  /** The tool call's result, when the tool was called. */
  result?: RunToolResult;
  /** How the editor was obtained when a session was used (`started` = this call launched it). */
  source?: EnsureSource;
  /** Cloud mode on a signed-in machine whose sign-in the server rejected: re-sign-in (never a local fallback). */
  signInRequired?: string;
  /** Set when no editor answered and none could be started. */
  sessionFailure?: {
    reason?: EditorSessionErrorReason;
    message: string;
    /** No project at the path, or a bad `--url`. */
    invalidInput?: boolean;
    /** The pre-launch C++ build failed: the message lists the errors and names the build log. */
    buildFailed?: boolean;
  };
  /** `once`: the close of the editor this call started. */
  closed?: CloseOwnedResult | { kind: 'error'; message: string };
}

/**
 * A failure that means "no editor / server answered there" (worth starting an editor for, then calling again). A
 * connection dropped after the call was sent (`connection-reset`, HTTP 502) is UNREACHABLE too, but the tool may have
 * run: it is reported, never called a second time.
 */
export function isUnreachableFailure(failure: RunToolFailure): boolean {
  if (failure.reason === 'connection-reset' || failure.httpStatus === 502) return false;
  return classifyToolFailure(failure).code === FAILURE.unreachable.code;
}

function isCloudEndpoint(endpoint: string): boolean {
  if (!endpoint) return false;
  try {
    return !isAbsoluteLoopbackHost(new URL(endpoint).origin);
  } catch {
    return false;
  }
}

export async function runToolWithSession(options: RunToolSessionOptions): Promise<RunToolSessionResult> {
  const { invoke, start, once, nullrhi, readyTimeoutMs, idleSeconds, engineRoot, build, onStatus, sessionOptions, credentialState, ...call } = options;
  // An explicit remote --url is not ours to start: only a project's local server can be served by a started editor.
  const canStart = start !== false && (call.url === undefined || isAbsoluteLoopbackHost(call.url));

  // A Cloud-mode project whose editor the CLI started (it serves the project's LOCAL server): go straight to it.
  const cliEditorLocal = canStart && usesProjectCloudMode(call) && liveOwnedEditorPid(call.projectDir!) !== undefined;
  let firstMessage = 'the CLI-started editor serves the project\'s local server';
  if (!cliEditorLocal) {
    const first = await invoke(call);
    if (!canStart || first.kind === 'success') return { result: first };
    firstMessage = first.message;
    if (usesProjectCloudMode(call) && isCloudEndpoint(first.endpoint)) {
      // Only a machine that NEVER signed in falls back to the project's local server: an existing sign-in is the
      // user's choice of the cloud, even when it cannot be used now (cloud-auth.ts).
      if (mustStayInCloud(call, credentialState)) {
        return isAuthFailure(first) ? { result: first, signInRequired: SIGNED_IN_BUT_UNUSABLE_MESSAGE } : { result: first };
      }
      if (!isUnreachableFailure(first) && !isAuthFailure(first)) return { result: first };
      onStatus?.(LOCAL_FALLBACK_NOTE);
    } else if (isCloudEndpoint(first.endpoint) && call.token !== undefined) {
      // An explicit --token against a remote server is used as is: never a fallback.
      return { result: first };
    } else if (!isUnreachableFailure(first)) {
      return { result: first };
    }
  }

  if (!cliEditorLocal) onStatus?.('No editor answered: starting a headless Unreal Editor...');
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
    // No project at the path, or a bad --url: nothing could be started.
    return { sessionFailure: { invalidInput: true, message: `${firstMessage}; could not start an editor: ${asError(err).message}` } };
  }
  const { connection, session } = opened;

  let ready;
  try {
    ready = await session.ensure({ mode: 'headless' });
  } catch (err) {
    const sessionFailure = {
      ...(err instanceof EditorSessionError ? { reason: err.reason } : {}),
      ...(err instanceof PreLaunchBuildError ? { buildFailed: true } : {}),
      message: sessionFailureMessage(err, connection.url, opened.projectDir),
    };
    // An editor this call launched may still be starting (e.g. --ready-timeout ran out): --once leaves nothing
    // behind. closeOwned acts only on the CLI lease, never on the user's editor.
    if (!once || !(err instanceof EditorSessionError && err.source === 'started')) return { sessionFailure };
    return { sessionFailure, closed: await closeOwned(session) };
  }

  onStatus?.('Running...');
  const result = await invoke({ ...call, url: ready.ready, ...(connection.token ? { token: connection.token } : {}) });
  if (!once || ready.source !== 'started') return { result, source: ready.source };

  onStatus?.('Closing the editor this call started...');
  return { result, source: ready.source, closed: await closeOwned(session) };
}

async function closeOwned(session: EditorSession<string>): Promise<NonNullable<RunToolSessionResult['closed']>> {
  try {
    return await session.closeOwned({ timeoutMs: UNREAL_SESSION_CLOSE_TIMEOUT_MS, force: true });
  } catch (err) {
    return { kind: 'error', message: asError(err).message };
  }
}
