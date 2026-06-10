// `wait-for-ready` — poll the project's MCP server `ping` endpoint until it
// responds 2xx or the overall timeout elapses. Clock + sleep + fetch are
// injectable so the loop is fully unit-testable without real time or
// sockets. Library-safe.

import { resolveConnection } from '../utils/config.js';
import { probePing } from '../utils/probe.js';
import { emitProgress } from './progress.js';
import type { WaitForReadyOptions, WaitForReadyResult } from './types.js';

const DEFAULT_TIMEOUT_MS = 120_000;
const DEFAULT_INTERVAL_MS = 2_000;
const DEFAULT_PROBE_TIMEOUT_MS = 4_000;

const realSleep = (ms: number): Promise<void> => new Promise((r) => setTimeout(r, ms));

export async function waitForReady(opts: WaitForReadyOptions = {}): Promise<WaitForReadyResult> {
  const now = opts.nowImpl ?? Date.now;
  const sleep = opts.sleepImpl ?? realSleep;
  const timeoutMs = positive(opts.timeoutMs, DEFAULT_TIMEOUT_MS);
  const intervalMs = positive(opts.intervalMs, DEFAULT_INTERVAL_MS);
  const probeTimeoutMs = positive(opts.probeTimeoutMs, DEFAULT_PROBE_TIMEOUT_MS);

  let url = '';
  let token: string | undefined;
  try {
    const conn = resolveConnection({ projectDir: opts.projectDir, url: opts.url, token: opts.token });
    url = conn.url;
    token = conn.token;
  } catch (err) {
    const error = err instanceof Error ? err : new Error(String(err));
    return { kind: 'failure', success: false, url, elapsedMs: 0, attempts: 0, lastReason: error.message, error };
  }

  const start = now();
  const deadline = start + timeoutMs;
  let attempts = 0;
  let lastReason = 'no attempts made';

  emitProgress(opts.onProgress, { phase: 'start', message: `Waiting for ${url} to become ready` });

  while (now() < deadline) {
    attempts += 1;
    const probe = await probePing(url, { token, timeoutMs: probeTimeoutMs, fetchImpl: opts.fetchImpl });
    if (probe.ok) {
      const elapsedMs = now() - start;
      emitProgress(opts.onProgress, { phase: 'done', message: `Ready after ${elapsedMs}ms (${attempts} attempt(s))` });
      return { kind: 'success', success: true, url, elapsedMs, attempts };
    }
    lastReason = probe.reason;
    emitProgress(opts.onProgress, { phase: 'info', message: `Not ready (${probe.reason}) — retrying` });

    const remaining = deadline - now();
    if (remaining <= 0) break;
    await sleep(Math.min(intervalMs, remaining));
  }

  const elapsedMs = now() - start;
  return {
    kind: 'failure',
    success: false,
    url,
    elapsedMs,
    attempts,
    lastReason,
    error: new Error(`Timed out after ${elapsedMs}ms waiting for ${url} (last: ${lastReason}).`),
  };
}

function positive(v: number | undefined, fallback: number): number {
  return typeof v === 'number' && v > 0 ? v : fallback;
}
