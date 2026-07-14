// `enroll` — redeem a D13 agent-driven enrollment code for a plugin credential
// with NO browser hop (mcp-authorize design 06/09, D13). The agent's
// `enroll_engine_plugin` tool hands the user a `install-plugin --enroll <code>`
// command; this redeems `<code>` against the cloud AS
// `POST /api/auth/enroll/redeem` and, on success:
//
//   1. persists the returned mcp:plugin credential into the SHARED machine
//      credential store (`~/.ai-game-dev/credentials.json`, D12) — the same
//      once-per-machine store `login` writes, NOT a project `.env`;
//   2. records the redeemed **server target** URL in the committable project
//      marker `<project>/.ai-game-dev/project.json` (so the plugin boots against
//      the right hub — hosted vs local);
//   3. **upserts the D14 routing pin** into any existing project-local agent
//      config entry pointed at that server (so a hosted server added manually in
//      workflow-1A step 1 becomes pinned).
//
// The code is **burned server-side on the first redeem attempt** (single-use, 5
// min TTL) — a spent/invalid/expired code yields the SAME uniform error (no
// oracle), surfaced here as a clean, actionable message. Never mint or log a
// real token. fetch + clock + store base dir are injectable for tests.
// Library-safe: never throws past the boundary.

import { MachineCredentialStore, type MachineCredentials, CREDENTIALS_VERSION } from '../utils/machine-credentials.js';
import { upsertServerTarget } from '../utils/project-marker.js';
import { deriveProjectPin } from '../utils/port.js';
import { upsertProjectPin } from '../utils/pin-upsert.js';
import { asError } from '../utils/error.js';
import { fetchWithTimeout } from '../utils/http.js';
import { emitProgress } from './progress.js';
import type { ProgressCallback } from './types.js';
import * as path from 'path';

const DEFAULT_BASE_URL = 'https://ai-game.dev';
const REDEEM_PATH = '/api/auth/enroll/redeem';
/** Per-request deadline so a hung endpoint can't stall the flow forever. */
const PER_REQUEST_TIMEOUT_MS = 30_000;

export interface EnrollOptions {
  /** The one-time enrollment code from the agent's `enroll_engine_plugin` tool. */
  enrollCode: string;
  /** The project to record the marker + pin into (the enrolled project). */
  projectDir: string;
  /** Auth base URL (defaults to `https://ai-game.dev`). Test injection. */
  baseUrl?: string;
  /** Override the machine credential store base dir (default `~/.ai-game-dev`). Test injection. */
  storeBaseDir?: string;
  fetchImpl?: typeof fetch;
  nowImpl?: () => number;
  onProgress?: ProgressCallback;
}

export interface EnrollSuccess {
  kind: 'success';
  success: true;
  /** The redeemed mcp:plugin access token. */
  token: string;
  /** The server-target URL the code was minted for (hosted vs local hub). */
  serverTarget: string;
  /** Absolute path of the machine-store credential file the token was written to. */
  credentialPath: string;
  /** Absolute path of the project marker written (`.ai-game-dev/project.json`). */
  markerPath: string;
  /** The D14 routing pin derived for this project. */
  pin: string;
  /** Project-local agent config files whose server URL was pinned this run. */
  pinnedConfigFiles: string[];
  warnings: string[];
}

export interface EnrollFailure {
  kind: 'failure';
  success: false;
  /** Coarse reason — `invalid_code`, `signing_unavailable`, `network-error`, `timeout`, … */
  reason: string;
  error: Error;
}

export type EnrollResult = EnrollSuccess | EnrollFailure;

interface RedeemResponse {
  access_token?: string;
  token_type?: string;
  expires_in?: number;
  refresh_token?: string;
  scope?: string;
  server_url?: string;
}

/** The single actionable message for a spent/invalid/expired code (server uniform error). */
const INVALID_CODE_MESSAGE =
  'Enrollment code is invalid, expired, or already used. Codes are single-use and burn on the first ' +
  'attempt (5 min TTL) — ask the agent to run `enroll_engine_plugin` again for a fresh code.';

export async function enrollPlugin(opts: EnrollOptions): Promise<EnrollResult> {
  const warnings: string[] = [];
  const baseUrl = (opts.baseUrl ?? DEFAULT_BASE_URL).replace(/\/+$/, '');
  const fetchImpl = opts.fetchImpl ?? globalThis.fetch;
  const now = opts.nowImpl ?? Date.now;

  try {
    const enrollCode = (opts.enrollCode ?? '').trim();
    if (enrollCode.length === 0) return fail('missing_code', 'An enrollment code is required.');
    if (!opts.projectDir || opts.projectDir.trim().length === 0) {
      return fail('missing_project', 'A project directory is required to record the enrollment marker.');
    }
    const projectDir = path.resolve(opts.projectDir);

    emitProgress(opts.onProgress, { phase: 'start', message: 'Redeeming enrollment code' });

    const resp = await fetchWithTimeout(
      fetchImpl,
      `${baseUrl}${REDEEM_PATH}`,
      {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ enroll_code: enrollCode }),
      },
      { timeoutMs: PER_REQUEST_TIMEOUT_MS },
    );

    if (!resp.ok) {
      // The AS returns a uniform `{ error, error_description }` for a spent /
      // invalid / expired code (400), and 503 when signing is unavailable.
      if (resp.status === 503) {
        return fail('signing_unavailable', 'The server could not sign a plugin credential right now (503). Try again shortly.');
      }
      const body = (await resp.json().catch(() => ({}))) as { error?: string; error_description?: string };
      const reason = body.error === 'invalid_grant' ? 'invalid_code' : body.error ?? `http-${resp.status}`;
      return fail(reason, INVALID_CODE_MESSAGE);
    }

    const body = (await resp.json()) as RedeemResponse;
    if (!body.access_token) return fail('token-malformed', 'Redeem response missing access_token.');
    const serverTarget = (body.server_url ?? DEFAULT_BASE_URL).replace(/\/+$/, '');

    // 1. Persist the plugin credential into the SHARED machine store (D12).
    const store = new MachineCredentialStore(opts.storeBaseDir);
    const credentials: MachineCredentials = {
      version: CREDENTIALS_VERSION,
      accessToken: body.access_token,
      refreshToken: body.refresh_token ?? undefined,
      expiresAt:
        typeof body.expires_in === 'number' && body.expires_in > 0
          ? new Date(now() + body.expires_in * 1000).toISOString()
          : undefined,
      serverTarget,
    };
    await store.write(credentials);
    emitProgress(opts.onProgress, {
      phase: 'info',
      message: `Plugin credential saved to the shared machine store (${store.credentialsPath}).`,
    });

    // 2. Record the enrolled server target in the committable project marker.
    const markerPath = upsertServerTarget(projectDir, serverTarget) ?? '';

    // 3. Upsert the D14 routing pin into any existing project-local agent config.
    const pin = deriveProjectPin(projectDir);
    const { updatedFiles } = upsertProjectPin(projectDir, pin, { serverTarget });
    if (updatedFiles.length > 0) {
      emitProgress(opts.onProgress, {
        phase: 'info',
        message: `Pinned ${updatedFiles.length} existing agent config(s) to /p/${pin}.`,
      });
    }

    emitProgress(opts.onProgress, { phase: 'done', message: 'Enrollment complete.' });
    return {
      kind: 'success',
      success: true,
      token: body.access_token,
      serverTarget,
      credentialPath: store.credentialsPath,
      markerPath,
      pin,
      pinnedConfigFiles: updatedFiles,
      warnings,
    };
  } catch (err) {
    const error = asError(err);
    const reason = error.name === 'AbortError' ? 'timeout' : 'network-error';
    return { kind: 'failure', success: false, reason, error };
  }
}

function fail(reason: string, message: string): EnrollFailure {
  return { kind: 'failure', success: false, reason, error: new Error(message) };
}
