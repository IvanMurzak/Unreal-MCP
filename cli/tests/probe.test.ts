import { describe, expect, it } from 'vitest';

import {
  LEGACY_PING_ENDPOINT,
  PING_ENDPOINT,
  probePing,
} from '../src/utils/probe.js';

function response(status: number, body = '{}'): Response {
  return {
    ok: status >= 200 && status < 300,
    status,
    text: async () => body,
  } as unknown as Response;
}

/** Records every URL the probe asks for, answering each from `answers` (default: 404). */
function recordingFetch(answers: Record<string, Response>) {
  const calls: string[] = [];
  const impl = (async (url: string) => {
    calls.push(url);
    return answers[url] ?? response(404);
  }) as unknown as typeof fetch;
  return { calls, impl };
}

describe('probePing endpoint routing (§2.4 system-tool surface)', () => {
  const base = 'http://localhost:5220';

  it('exposes the system-tools route as the primary endpoint', () => {
    // `ping` became a SYSTEM tool (owner ruling 2026-07-25); this constant is what
    // moved, and a regression here means the CLI probes a route that no longer resolves.
    expect(PING_ENDPOINT).toBe('/api/system-tools/ping');
    expect(LEGACY_PING_ENDPOINT).toBe('/api/tools/ping');
  });

  it('probes the system-tools route first and stops there on success', async () => {
    const { calls, impl } = recordingFetch({
      [`${base}${PING_ENDPOINT}`]: response(200, '{"result":"pong"}'),
    });

    const result = await probePing(base, { fetchImpl: impl });

    expect(result.ok).toBe(true);
    expect(calls).toEqual([`${base}${PING_ENDPOINT}`]);
  });

  it('falls back to the legacy route for a pre-§2.4 plugin', async () => {
    // A newer CLI against an older plugin: `ping` is still a standard tool there.
    const { calls, impl } = recordingFetch({
      [`${base}${PING_ENDPOINT}`]: response(500),
      [`${base}${LEGACY_PING_ENDPOINT}`]: response(200, '{"result":"pong"}'),
    });

    const result = await probePing(base, { fetchImpl: impl });

    expect(result.ok).toBe(true);
    expect(calls).toEqual([`${base}${PING_ENDPOINT}`, `${base}${LEGACY_PING_ENDPOINT}`]);
  });

  it('reports the system route status when neither route answers', async () => {
    const { calls, impl } = recordingFetch({
      [`${base}${PING_ENDPOINT}`]: response(503),
      [`${base}${LEGACY_PING_ENDPOINT}`]: response(404),
    });

    const result = await probePing(base, { fetchImpl: impl });

    expect(result.ok).toBe(false);
    expect(result.ok === false && result.reason).toBe('HTTP 503');
    expect(calls).toHaveLength(2);
  });

  it('does not retry the fallback when the server is unreachable', async () => {
    // A refused connection is the server being down, not a route mismatch — a second
    // identical round trip would only double the wait for the same answer.
    const calls: string[] = [];
    const impl = (async (url: string) => {
      calls.push(url);
      const err = new Error('connect ECONNREFUSED') as NodeJS.ErrnoException;
      err.code = 'ECONNREFUSED';
      throw err;
    }) as unknown as typeof fetch;

    const result = await probePing(base, { fetchImpl: impl });

    expect(result.ok).toBe(false);
    expect(calls).toEqual([`${base}${PING_ENDPOINT}`]);
  });

  it('sends the bearer token and a JSON body on the probed route', async () => {
    let seen: RequestInit | undefined;
    const impl = (async (_url: string, init: RequestInit) => {
      seen = init;
      return response(200, '{"result":"pong"}');
    }) as unknown as typeof fetch;

    await probePing(base, { token: 'abc123', fetchImpl: impl });

    const headers = seen?.headers as Record<string, string>;
    expect(headers['Authorization']).toBe('Bearer abc123');
    expect(headers['Content-Type']).toBe('application/json');
    expect(seen?.method).toBe('POST');
    expect(seen?.body).toBe('{}');
  });
});
