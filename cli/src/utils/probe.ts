// HTTP readiness probe against a project's local MCP server. Used by
// `status` and `wait-for-ready`. The `ping` MCP tool echoes back —
// a 2xx means the server (and the plugin behind it) is reachable.

// NOTE (doc/code reconciliation): The Unreal sidecar does NOT implement the
// `ping` system-tool handler (`/api/system-tools/ping` returns null → HTTP 500),
// so we route through `/api/tools/ping` (the MCP tool route). This was
// verified manually: system-tools/ping → 500 (null response),
// /api/tools/ping → 200 {"result":"pong"}. ARCHITECTURE.md §e2e runbook
// already cited /api/tools/ping — this endpoint now matches it.

import { fetchWithTimeout } from './http.js';

export const PING_ENDPOINT = '/api/tools/ping';

export interface ProbeSuccess {
  ok: true;
  baseUrl: string;
  httpStatus: number;
  data: unknown;
}

export interface ProbeFailure {
  ok: false;
  baseUrl: string;
  reason: string;
}

export type ProbeResult = ProbeSuccess | ProbeFailure;

export interface ProbeOptions {
  token?: string;
  timeoutMs?: number;
  /** Injectable fetch (defaults to global `fetch`). */
  fetchImpl?: typeof fetch;
}

/**
 * POST `{}` to the server's `ping` endpoint. Never throws — every failure
 * mode (connection refused, timeout, non-2xx, malformed body) is folded
 * into a `{ ok: false }` result with a human-readable reason.
 */
export async function probePing(baseUrl: string, opts: ProbeOptions = {}): Promise<ProbeResult> {
  const endpoint = `${baseUrl}${PING_ENDPOINT}`;
  const timeoutMs = typeof opts.timeoutMs === 'number' && opts.timeoutMs > 0 ? opts.timeoutMs : 5000;
  const fetchImpl = opts.fetchImpl ?? globalThis.fetch;

  const headers: Record<string, string> = { 'Content-Type': 'application/json' };
  if (opts.token) headers['Authorization'] = `Bearer ${opts.token}`;

  try {
    const response = await fetchWithTimeout(
      fetchImpl,
      endpoint,
      { method: 'POST', headers, body: '{}' },
      { timeoutMs },
    );
    const text = await response.text().catch(() => '');
    if (response.ok) {
      let data: unknown;
      try {
        data = text.length > 0 ? JSON.parse(text) : undefined;
      } catch {
        data = text;
      }
      return { ok: true, baseUrl, httpStatus: response.status, data };
    }
    return { ok: false, baseUrl, reason: `HTTP ${response.status}` };
  } catch (err) {
    return { ok: false, baseUrl, reason: classifyError(err) };
  }
}

function classifyError(err: unknown): string {
  if (err instanceof Error && err.name === 'AbortError') return 'timed out';
  const cause =
    err instanceof Error && 'cause' in err ? (err.cause as { code?: string } | undefined) : undefined;
  switch (cause?.code) {
    case 'ECONNREFUSED':
      return 'connection refused';
    case 'ECONNRESET':
      return 'connection reset';
    case 'ENOTFOUND':
    case 'EAI_AGAIN':
      return 'host not found';
    default:
      return err instanceof Error ? err.message : String(err);
  }
}
