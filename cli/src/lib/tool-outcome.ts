// Map a tool call's outcome to the shared exit-code contract (utils/exit-codes.ts). Pure.

import type { EditorSessionErrorReason } from '@baizor/gamedev-cli-core';
import { modalDialogInToolError } from './editor-modal.js';
import { toolErrorText } from './exec.js';
import { FAILURE, type FailureKind } from '../utils/output.js';
import { isToolSuccess } from './session.js';
import type { RunToolFailure } from './types.js';

export type { FailureKind };

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
      if (failure.httpStatus === 503) return FAILURE.busy;
      if (failure.httpStatus === 404) return FAILURE.notFound; // no such tool
      if (failure.httpStatus === 504) return FAILURE.timeout; // the server gave up waiting for the editor
      if (failure.httpStatus === 502) return FAILURE.unreachable; // the server could not reach the editor
      if (modalDialogInToolError(toolErrorText(failure)) !== undefined) return FAILURE.awaitingInput;
      if (/no connected clients/i.test(failure.message)) return FAILURE.unreachable;
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
  const response = data as { content?: Array<{ type?: string; text?: unknown }>; error?: unknown } | null;
  const texts = (Array.isArray(response?.content) ? response.content : [])
    .filter((c) => c?.type === 'text' && typeof c.text === 'string')
    .map((c) => c.text as string);
  if (texts.length > 0) return texts.join('\n');
  return typeof response?.error === 'string' && response.error ? response.error : 'The tool reported an error.';
}

/** Exit / error code of a tool response with `status: "error"`. */
export function classifyToolResponseError(text: string): FailureKind {
  return modalDialogInToolError(text) !== undefined ? FAILURE.awaitingInput : FAILURE.toolFailed;
}

/** Exit / error code of a session that could not provide a ready editor. */
export function classifySessionFailure(reason: EditorSessionErrorReason | undefined, invalidInput = false): FailureKind {
  if (invalidInput) return FAILURE.projectNotFound; // no project at the path / a bad --url: nothing to start
  return reason === 'lock-busy' ? FAILURE.busy : FAILURE.unreachable;
}
