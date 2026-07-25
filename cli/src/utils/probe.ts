// HTTP readiness probe against a project's local MCP server. Used by
// `status` and `wait-for-ready`. The `ping` tool echoes back —
// a 2xx means the server (and the plugin behind it) is reachable.

// `ping` is a SYSTEM tool (docs/ARCHITECTURE.md §2.4, owner ruling 2026-07-25):
// it is host plumbing this CLI drives, not a capability an AI agent should see
// in `tools/list`, so it lives at `/api/system-tools/ping` and NO LONGER
// resolves at `/api/tools/ping`.
//
// The legacy endpoint is kept as a FALLBACK, not as the primary: a CLI is
// routinely newer than the plugin a given project has installed, and against a
// pre-§2.4 plugin `ping` is still a standard tool. Probing system-first and
// falling back on a non-2xx means both plugin generations report reachable, and
// neither costs a round trip in the steady state. (This mirrors the D17-style
// feature-detect the desktop app uses for the same transition.)

import { asError } from './error.js';
import { fetchWithTimeout, networkErrorCategory, type NetworkErrorCategory } from './http.js';

/** The system-tools route `ping` lives on since §2.4. Tried first. */
export const PING_ENDPOINT = '/api/system-tools/ping';

/** The pre-§2.4 standard-tool route. Tried only when {@link PING_ENDPOINT} answers non-2xx. */
export const LEGACY_PING_ENDPOINT = '/api/tools/ping';

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
 * POST `{}` to the server's `ping` endpoint — the system-tools route first, the
 * legacy standard-tool route as a fallback for a pre-§2.4 plugin. Never throws:
 * every failure mode (connection refused, timeout, non-2xx, malformed body) is
 * folded into a `{ ok: false }` result with a human-readable reason.
 *
 * A TRANSPORT failure (refused / timeout / DNS) is reported immediately without
 * trying the fallback — the server is unreachable, so a second identical round
 * trip would only double the wait for the same answer. Only a reachable server
 * that answered non-2xx justifies asking the other route.
 */
export async function probePing(baseUrl: string, opts: ProbeOptions = {}): Promise<ProbeResult> {
  const primary = await probeEndpoint(baseUrl, PING_ENDPOINT, opts);
  if (primary.ok || primary.transportFailed) return primary.result;

  const fallback = await probeEndpoint(baseUrl, LEGACY_PING_ENDPOINT, opts);
  // Surface the SYSTEM route's failure when neither answered: it is the route
  // this CLI expects, so its status is the actionable one.
  return fallback.ok ? fallback.result : primary.result;
}

interface EndpointProbe {
  ok: boolean;
  /** The request never reached the server (refused / timeout / DNS) — retrying another path is pointless. */
  transportFailed: boolean;
  result: ProbeResult;
}

async function probeEndpoint(
  baseUrl: string,
  route: string,
  opts: ProbeOptions,
): Promise<EndpointProbe> {
  const endpoint = `${baseUrl}${route}`;
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
      return {
        ok: true,
        transportFailed: false,
        result: { ok: true, baseUrl, httpStatus: response.status, data },
      };
    }
    return {
      ok: false,
      transportFailed: false,
      result: { ok: false, baseUrl, reason: `HTTP ${response.status}` },
    };
  } catch (err) {
    return {
      ok: false,
      transportFailed: true,
      result: { ok: false, baseUrl, reason: classifyError(err) },
    };
  }
}

/** Human-readable text for each shared network-failure category. */
const NETWORK_CATEGORY_TEXT: Record<NetworkErrorCategory, string> = {
  'connection-refused': 'connection refused',
  'connection-reset': 'connection reset',
  'network-error': 'host not found',
};

function classifyError(err: unknown): string {
  if (err instanceof Error && err.name === 'AbortError') return 'timed out';
  const category = networkErrorCategory(err);
  if (category) return NETWORK_CATEGORY_TEXT[category];
  return asError(err).message;
}
