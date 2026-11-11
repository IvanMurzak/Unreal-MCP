// Map a tool call's outcome to the shared exit-code contract (utils/exit-codes.ts). Pure.

import type { EditorSessionErrorReason } from '@baizor/gamedev-cli-core';
import { modalDialogInToolError } from './editor-modal.js';
import { FAILURE, type FailureKind } from '../utils/output.js';
import { isToolSuccess } from './session.js';
import type { RunToolFailure } from './types.js';
import type { RunToolSessionResult } from './run-tool-session.js';

export type { FailureKind };

/** A failed call's own error (the server answers a tool error as HTTP 500 `{ "error": "<text>" }`), else the HTTP reason. */
export function toolErrorText(failure: RunToolFailure): string {
  const error = (failure.data as { error?: unknown } | null | undefined)?.error;
  return typeof error === 'string' && error.length > 0 ? error : failure.message;
}

/** Leads the message of a call whose editor could not start because the project's C++ does not compile. */
export const NOT_STARTED_BUILD =
  'The project\'s C++ code does not compile, so no editor was started (fix it; `unreal-mcp-cli compile` rebuilds without an editor):';

const NO_EDITOR_CONNECTED = /no connected clients|no game engine is connected/i;

/** Exit / error code of a failed tool call. */
export function classifyToolFailure(failure: RunToolFailure): FailureKind {
  switch (failure.reason) {
    case 'invalid-input':
      return FAILURE.usage;
    case 'connection-refused':
    case 'connection-reset':
    case 'network-error':
      return FAILURE.unreachable;
    case 'timeout':
      return FAILURE.timeout;
    case 'http-error':
      // The server is up but no editor is connected to it (local: "No connected clients"; cloud: "No game engine is
      // connected for your account", HTTP 503): unreachable, not busy.
      if (NO_EDITOR_CONNECTED.test(toolErrorText(failure))) return FAILURE.unreachable;
      if (failure.httpStatus === 503) return FAILURE.busy;
      if (failure.httpStatus === 404) return FAILURE.notFound; // no such tool
      if (failure.httpStatus === 504) return FAILURE.timeout; // the server gave up waiting for the editor
      if (failure.httpStatus === 502) return FAILURE.unreachable; // the server could not reach the editor
      if (modalDialogInToolError(toolErrorText(failure)) !== undefined) return FAILURE.awaitingInput;
      return FAILURE.toolFailed;
    default:
      return FAILURE.error;
  }
}

/**
 * The error text of a tool response that reports `status: "error"` (the call itself succeeded over HTTP), else
 * `undefined`.
 */
export function toolResponseError(data: unknown): string | undefined {
  if (isToolSuccess(data)) return undefined;
  const { texts } = textBlocks(data);
  if (texts.length > 0) return texts.join('\n');
  const error = (data as { error?: unknown } | null)?.error;
  return typeof error === 'string' && error ? error : 'The tool reported an error.';
}

/**
 * The text of a tool answer whose content is all text blocks, joined; `undefined` for any other answer (an image, no
 * content), which keeps its JSON.
 */
export function toolResponseText(data: unknown): string | undefined {
  const { texts, blocks } = textBlocks(data);
  return texts.length > 0 && texts.length === blocks ? texts.join('\n') : undefined;
}

/**
 * A tool answer for the text-mode JSON print: each image block's base64 `data` (a screenshot is megabytes) is replaced
 * by a note naming its size; everything else is kept. `--format json` keeps the data. Pure.
 */
export function withoutImageData(data: unknown): unknown {
  const content = (data as { content?: unknown } | null | undefined)?.content;
  if (!Array.isArray(content)) return data;
  return {
    ...(data as object),
    content: content.map((block: { type?: string; data?: unknown } | null) =>
      block?.type === 'image' && typeof block.data === 'string'
        ? { ...block, data: `<${block.data.length} base64 chars omitted: pass savePath for a file, or --format json for the data>` }
        : block,
    ),
  };
}

/** The text blocks of a tool answer's `content`, and how many blocks it has in all. */
function textBlocks(data: unknown): { texts: string[]; blocks: number } {
  const content = (data as { content?: unknown } | null | undefined)?.content;
  const blocks = Array.isArray(content) ? (content as Array<{ type?: string; text?: unknown } | null>) : [];
  const texts = blocks.filter((c) => c?.type === 'text' && typeof c.text === 'string').map((c) => c!.text as string);
  return { texts, blocks: blocks.length };
}

/** Exit / error code of a tool response with `status: "error"`. */
export function classifyToolResponseError(text: string): FailureKind {
  return modalDialogInToolError(text) !== undefined ? FAILURE.awaitingInput : FAILURE.toolFailed;
}

/**
 * The failure of a session-aware call that never reached a tool (no editor could be started, or the cloud sign-in was
 * rejected), with its message; `undefined` when the tool was called. `subject` names what was called.
 */
export function sessionOutcomeFailure(
  outcome: Pick<RunToolSessionResult, 'sessionFailure' | 'signInRequired'>,
  subject: string,
): { kind: FailureKind; message: string } | undefined {
  const failure = outcome.sessionFailure;
  if (failure) {
    return {
      kind: classifySessionFailure(failure.reason, failure.invalidInput, failure.buildFailed),
      message: failure.buildFailed ? `${NOT_STARTED_BUILD} ${failure.message}` : `No ready Unreal Editor for ${subject}: ${failure.message}`,
    };
  }
  if (outcome.signInRequired) return { kind: FAILURE.notAuthenticated, message: outcome.signInRequired };
  return undefined;
}

/** A warning when `--once` could not close the editor the call started. */
export function editorCloseWarning(outcome: Pick<RunToolSessionResult, 'closed'>): string | undefined {
  const closed = outcome.closed;
  if (!closed || closed.kind === 'closed' || closed.kind === 'not-owned') return undefined;
  return `The editor this call started did not close (${closed.kind}${'message' in closed ? `: ${closed.message}` : ''}).`;
}

/** Exit / error code of a session that could not provide a ready editor. */
export function classifySessionFailure(reason: EditorSessionErrorReason | undefined, invalidInput = false, buildFailed = false): FailureKind {
  if (invalidInput) return FAILURE.projectNotFound; // no project at the path / a bad --url: nothing to start
  if (buildFailed) return FAILURE.compilationFailed; // the C++ build before the launch failed: fix the code
  return reason === 'lock-busy' ? FAILURE.busy : FAILURE.unreachable;
}
