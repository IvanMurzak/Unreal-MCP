// `run-tool` / `run-system-tool` — POST a tool invocation to a project's
// local MCP server over HTTP. URL/token resolution: explicit override ->
// process env -> project `.env` -> deterministic localhost port.
// Library-safe: errors are returned, never thrown.

import { resolveConnection } from '../utils/config.js';
import { asError } from '../utils/error.js';
import type {
  RunToolFailure,
  RunToolFailureReason,
  RunToolOptions,
  RunToolResult,
} from './types.js';

const DEFAULT_TIMEOUT_MS = 60_000;

export async function runTool(opts: RunToolOptions): Promise<RunToolResult> {
  return invokeTool('/api/tools', opts);
}

export async function runSystemTool(opts: RunToolOptions): Promise<RunToolResult> {
  return invokeTool('/api/system-tools', opts);
}

async function invokeTool(routePrefix: string, opts: RunToolOptions): Promise<RunToolResult> {
  const invalid = validateOptions(opts);
  if (invalid) return invalid;

  let url: string;
  let token: string | undefined;
  try {
    const resolved = resolveConnection({
      projectDir: opts.projectDir,
      url: opts.url,
      token: opts.token,
    });
    url = resolved.url;
    token = resolved.token;
  } catch (err) {
    return makeFailure({
      endpoint: '',
      reason: 'invalid-input',
      message: asError(err).message,
      error: err instanceof Error ? err : undefined,
    });
  }

  const body = serializeInput(opts.input);
  if ('error' in body) {
    return makeFailure({ endpoint: '', reason: 'invalid-input', message: body.error.message, error: body.error });
  }

  const endpoint = `${url}${routePrefix}/${encodeURIComponent(opts.toolName)}`;
  const fetchImpl = opts.fetchImpl ?? globalThis.fetch;
  const timeoutMs = typeof opts.timeoutMs === 'number' && opts.timeoutMs > 0 ? opts.timeoutMs : DEFAULT_TIMEOUT_MS;

  const controller = new AbortController();
  let timedOut = false;
  const timer = setTimeout(() => {
    timedOut = true;
    controller.abort();
  }, timeoutMs);
  const externalAbort = (): void => controller.abort();
  if (opts.signal) {
    if (opts.signal.aborted) controller.abort();
    else opts.signal.addEventListener('abort', externalAbort, { once: true });
  }

  const headers: Record<string, string> = { 'Content-Type': 'application/json' };
  if (token) headers['Authorization'] = `Bearer ${token}`;

  try {
    const response = await fetchImpl(endpoint, {
      method: 'POST',
      headers,
      body: body.json,
      signal: controller.signal,
    });
    const text = await safeReadText(response);
    const data = parseJsonOrText(text);
    if (!response.ok) {
      return makeFailure({
        endpoint,
        reason: 'http-error',
        httpStatus: response.status,
        data,
        message: response.statusText || `HTTP ${response.status}`,
      });
    }
    return { kind: 'success', success: true, endpoint, httpStatus: response.status, data };
  } catch (err) {
    return classifyFetchError(err, endpoint, timeoutMs, timedOut);
  } finally {
    clearTimeout(timer);
    opts.signal?.removeEventListener('abort', externalAbort);
  }
}

function validateOptions(opts: RunToolOptions): RunToolFailure | null {
  if (!opts || typeof opts !== 'object') {
    return makeFailure({ endpoint: '', reason: 'invalid-input', message: 'options object is required.' });
  }
  if (typeof opts.toolName !== 'string' || opts.toolName.trim().length === 0) {
    return makeFailure({ endpoint: '', reason: 'invalid-input', message: 'toolName is required and must be a non-empty string.' });
  }
  const hasUrl = typeof opts.url === 'string' && opts.url.length > 0;
  const hasProjectDir = typeof opts.projectDir === 'string' && opts.projectDir.trim().length > 0;
  if (!hasUrl && !hasProjectDir) {
    return makeFailure({ endpoint: '', reason: 'invalid-input', message: 'Either projectDir or url must be provided.' });
  }
  return null;
}

function serializeInput(input: unknown): { json: string } | { error: Error } {
  if (input === undefined || input === null) return { json: '{}' };
  if (typeof input === 'string') {
    try {
      JSON.parse(input);
      return { json: input };
    } catch (err) {
      return { error: new Error(`input string is not valid JSON: ${asError(err).message}`) };
    }
  }
  if (typeof input !== 'object') {
    return { error: new Error('input must be a plain object, JSON string, undefined, or null.') };
  }
  try {
    return { json: JSON.stringify(input) };
  } catch (err) {
    return { error: new Error(`input could not be serialized to JSON: ${asError(err).message}`) };
  }
}

async function safeReadText(response: Response): Promise<string> {
  try {
    return await response.text();
  } catch {
    return '';
  }
}

function parseJsonOrText(text: string): unknown {
  if (text.length === 0) return undefined;
  try {
    return JSON.parse(text);
  } catch {
    return text;
  }
}

function classifyFetchError(err: unknown, endpoint: string, timeoutMs: number, timedOut: boolean): RunToolFailure {
  if (err instanceof Error && err.name === 'AbortError') {
    // An AbortError from our own timer is a timeout; one from a caller-
    // supplied signal is a deliberate cancellation — don't conflate them.
    return timedOut
      ? makeFailure({ endpoint, reason: 'timeout', message: `Tool call timed out after ${timeoutMs}ms.`, error: err })
      : makeFailure({ endpoint, reason: 'aborted', message: 'Tool call was aborted by the caller.', error: err });
  }
  const error = err instanceof Error ? err : new Error(String(err));
  const cause = err instanceof Error && 'cause' in err ? (err.cause as { code?: string } | undefined) : undefined;
  let reason: RunToolFailureReason = 'unknown';
  if (cause?.code === 'ECONNREFUSED') reason = 'connection-refused';
  else if (cause?.code === 'ECONNRESET') reason = 'connection-reset';
  else if (cause?.code === 'ENOTFOUND' || cause?.code === 'EAI_AGAIN') reason = 'network-error';
  return makeFailure({ endpoint, reason, message: error.message, error });
}

function makeFailure(fields: Omit<RunToolFailure, 'kind' | 'success'>): RunToolFailure {
  return { kind: 'failure', success: false, ...fields };
}
