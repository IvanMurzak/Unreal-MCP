// `login` — OAuth 2.0 device authorization flow against ai-game.dev.
// Requests a device code, surfaces the verification URL + user code via
// `onProgress`, then polls the token endpoint until the user authorizes
// (or the flow times out). On success, optionally persists the token into
// the project's `.env`. fetch + sleep + clock are injectable for tests.
// Library-safe: never throws past the boundary.

import { writeEnvFile, ensureEnvGitignored } from '../utils/env-file.js';
import * as path from 'path';
import { emitProgress } from './progress.js';
import type { ProgressCallback } from './types.js';

const DEFAULT_BASE_URL = 'https://ai-game.dev';
const DEFAULT_DEVICE_PATH = '/api/auth/device/code';
const DEFAULT_TOKEN_PATH = '/api/auth/device/token';
/** Per-request deadline so a hung endpoint can't stall the flow forever. */
const PER_REQUEST_TIMEOUT_MS = 30_000;

export interface LoginOptions {
  baseUrl?: string;
  /** Persist the token into `<projectDir>/.env` on success. */
  projectDir?: string;
  /** Overall poll deadline. Default 300000 ms (5 min). */
  timeoutMs?: number;
  fetchImpl?: typeof fetch;
  sleepImpl?: (ms: number) => Promise<void>;
  nowImpl?: () => number;
  onProgress?: ProgressCallback;
}

export interface LoginSuccess {
  kind: 'success';
  success: true;
  token: string;
  /** `true` when the token was written to a project `.env`. */
  persisted: boolean;
  envPath?: string;
}

export interface LoginFailure {
  kind: 'failure';
  success: false;
  /** Coarse reason — `access_denied`, `expired_token`, `timeout`, ... */
  reason: string;
  error: Error;
}

export type LoginResult = LoginSuccess | LoginFailure;

interface DeviceCodeResponse {
  device_code: string;
  user_code: string;
  verification_uri: string;
  verification_uri_complete?: string;
  interval?: number;
  expires_in?: number;
}

const realSleep = (ms: number): Promise<void> => new Promise((r) => setTimeout(r, ms));

export async function login(opts: LoginOptions = {}): Promise<LoginResult> {
  const baseUrl = (opts.baseUrl ?? DEFAULT_BASE_URL).replace(/\/+$/, '');
  const fetchImpl = opts.fetchImpl ?? globalThis.fetch;
  const sleep = opts.sleepImpl ?? realSleep;
  const now = opts.nowImpl ?? Date.now;
  const timeoutMs = typeof opts.timeoutMs === 'number' && opts.timeoutMs > 0 ? opts.timeoutMs : 300_000;

  try {
    emitProgress(opts.onProgress, { phase: 'start', message: 'Requesting device authorization' });

    const codeResp = await fetchWithTimeout(fetchImpl, `${baseUrl}${DEFAULT_DEVICE_PATH}`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: '{}',
    });
    if (!codeResp.ok) {
      return fail('device-code-failed', `Device code request failed: HTTP ${codeResp.status}`);
    }
    const code = (await codeResp.json()) as DeviceCodeResponse;
    if (!code.device_code || !code.user_code || !code.verification_uri) {
      return fail('device-code-malformed', 'Device code response missing required fields.');
    }

    emitProgress(opts.onProgress, {
      phase: 'info',
      message:
        `To authorize, open ${code.verification_uri_complete ?? code.verification_uri} ` +
        `and enter code ${code.user_code}`,
    });

    let intervalMs = Math.max(1, code.interval ?? 5) * 1000;
    const start = now();
    const deadline = start + timeoutMs;

    while (now() < deadline) {
      await sleep(intervalMs);
      const tokenResp = await fetchWithTimeout(fetchImpl, `${baseUrl}${DEFAULT_TOKEN_PATH}`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ device_code: code.device_code }),
      });

      if (tokenResp.ok) {
        const body = (await tokenResp.json()) as { access_token?: string };
        if (!body.access_token) {
          return fail('token-malformed', 'Token response missing access_token.');
        }
        const persisted = persistToken(opts.projectDir, body.access_token);
        emitProgress(opts.onProgress, { phase: 'done', message: 'Login complete.' });
        return {
          kind: 'success',
          success: true,
          token: body.access_token,
          persisted: persisted !== null,
          envPath: persisted ?? undefined,
        };
      }

      // Pending / slow-down / terminal errors per the device-flow spec.
      const errBody = (await tokenResp.json().catch(() => ({}))) as { error?: string };
      const error = errBody.error ?? `http-${tokenResp.status}`;
      if (error === 'authorization_pending') continue;
      if (error === 'slow_down') {
        // RFC 8628 §3.5: on `slow_down` the poll interval MUST be increased by
        // 5 seconds for this and ALL subsequent requests — a persistent
        // back-off, not a one-shot extra sleep. The next loop iteration sleeps
        // the new interval at the top before polling again.
        intervalMs += 5000;
        continue;
      }
      // access_denied, expired_token, or anything else terminal.
      return fail(error, `Authorization failed: ${error}`);
    }

    return fail('timeout', `Login timed out after ${timeoutMs}ms.`);
  } catch (err) {
    const error = err instanceof Error ? err : new Error(String(err));
    // A per-request `fetchWithTimeout` deadline surfaces as an AbortError;
    // classify that as a timeout rather than a generic network error.
    const reason = error.name === 'AbortError' ? 'timeout' : 'network-error';
    return { kind: 'failure', success: false, reason, error };
  }
}

function persistToken(projectDir: string | undefined, token: string): string | null {
  if (!projectDir) return null;
  const dir = path.resolve(projectDir);
  const envPath = path.join(dir, '.env');
  // The token is a secret — make sure `.env` is gitignored BEFORE it lands on
  // disk (mirrors `configure`'s §8 guard so `login -p .` can't leave a
  // committable token file behind even if the process dies between the two
  // writes).
  ensureEnvGitignored(dir);
  writeEnvFile(envPath, { UNREAL_MCP_TOKEN: token, UNREAL_MCP_CONNECTION_MODE: 'Cloud' });
  return envPath;
}

/**
 * `fetch` with a per-request abort deadline so a hung endpoint cannot stall
 * the device flow indefinitely (the overall poll deadline only fires between
 * polls). Honors the injected `fetchImpl` for tests.
 */
async function fetchWithTimeout(
  fetchImpl: typeof fetch,
  url: string,
  init: RequestInit,
): Promise<Response> {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), PER_REQUEST_TIMEOUT_MS);
  try {
    return await fetchImpl(url, { ...init, signal: controller.signal });
  } finally {
    clearTimeout(timer);
  }
}

function fail(reason: string, message: string): LoginFailure {
  return { kind: 'failure', success: false, reason, error: new Error(message) };
}
