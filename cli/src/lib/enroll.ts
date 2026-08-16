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

import { upsertServerTarget } from '../utils/project-marker.js';
// B5 FIX (auth-fixes design 02 T3): the routing pin is derived with cli-core's
// v2 ProjectIdentity (`derivePinV2`), which normalizes `\`→`/` before hashing.
// `path.resolve` (below) yields a Windows backslash root; the legacy v1
// `deriveProjectPin` hashed those backslashes verbatim, so the enroll pin never
// prefix-matched the plugin's forward-slash `projectPathHash` and pinned routing
// silently failed on Windows. v2 collapses `C:\a\b` and `C:/a/b` to one hash.
//
// e2: the credential persist goes through cli-core's `commitToolsOnlyLogin`
// (enroll IS the browser-less tools-only mint path — 03 F10): ONE lock hold
// writing the plugin family (+ v1 mirror) into the SHARED v2 machine store,
// with the D6/F7 account-switch guard applied. The CLI-local store copy is
// deleted (it dropped unknown fields and wrote non-atomically).
import {
  commitToolsOnlyLogin,
  derivePinV2,
  MachineCredentialStore,
  normalizeRedeemResponse,
  unrealAdapter,
  type CredentialCodec,
} from '@baizor/gamedev-cli-core';
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
  /** Override the store's at-rest codec (default: DPAPI on Windows). Test injection. */
  storeCodec?: CredentialCodec;
  /**
   * D6/F7 account-switch confirmation (`--yes`-gated on the CLI): called when
   * the redeemed credential's `sub` differs from the store's subject. ABSENT ⇒
   * a mismatch is DECLINED fail-closed (the just-redeemed family is revoked
   * best-effort and NOTHING is written).
   */
  confirmAccountSwitch?: (info: { storedSubject: string; newSubject: string }) => boolean | Promise<boolean>;
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
  /** O5/a6: the account (`sub`) the credential resolves to — feeds the D6/F7 guard. */
  sub?: string;
  /** O5/a6: the OAuth client id the credential was minted under — stored verbatim. */
  client_id?: string;
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
      // A 400 (or an explicit `invalid_grant`) is the AS's uniform spent/invalid/
      // expired-code signal (no oracle). Any OTHER status (e.g. a transient 5xx) is
      // a server-side failure, not a bad code — surfacing INVALID_CODE_MESSAGE there
      // would wrongly tell the user to reissue a code that is actually fine.
      if (resp.status === 400 || body.error === 'invalid_grant') {
        return fail(reason, INVALID_CODE_MESSAGE);
      }
      return fail(
        reason,
        `The enrollment server returned HTTP ${resp.status}${resp.statusText ? ` ${resp.statusText}` : ''}. ` +
          `This is a server-side error, not a bad code — try again shortly.`,
      );
    }

    const body = (await resp.json()) as RedeemResponse;
    if (!body.access_token) return fail('token-malformed', 'Redeem response missing access_token.');
    // cli-core's normalizer maps the AS response defensively (snake_case AND
    // camelCase; `expires_in` seconds → absolute ISO `expiresAt`) and carries
    // the O5/a6 fields: `sub` (the account the credential resolves to — feeds
    // the D6/F7 guard) and `client_id` (the mint client, stamped verbatim onto
    // the stored family so refresh presents the RIGHT id — 04 §3 rule 2).
    const redeemed = normalizeRedeemResponse(body as unknown as Record<string, unknown>, now);
    const serverTarget = (redeemed.serverTarget ?? DEFAULT_BASE_URL).replace(/\/+$/, '');

    // 1. Persist the plugin credential into the SHARED machine store (D12)
    //    through cli-core's tools-only commit (F10): plugin family + v1 mirror
    //    under ONE lock hold, D6/F7 guard included. Pre-a6 servers omit
    //    `client_id`; this CLI's own id is the transitional stamp then (the
    //    provider would present it as the component default anyway).
    const store = new MachineCredentialStore(opts.storeBaseDir, opts.storeCodec);
    const commit = await commitToolsOnlyLogin({
      store,
      clientId: redeemed.clientId ?? unrealAdapter.clientId,
      credentials: {
        accessToken: redeemed.accessToken,
        refreshToken: redeemed.refreshToken,
        expiresAt: redeemed.expiresAt,
        serverTarget,
        subject: redeemed.subject,
      },
      confirmAccountSwitch: opts.confirmAccountSwitch,
      fetchImpl,
      onWarning: (message) => warnings.push(message),
    });
    if (commit.status === 'switch-declined') {
      return fail(
        'account-switch-declined',
        `This machine is signed in as a different account (${commit.storedSubject}); the enrollment ` +
          `code resolves to ${commit.newSubject}. Nothing was written — re-run with --yes to switch ` +
          'the whole machine to the new account.',
      );
    }
    if (commit.status === 'aborted') {
      return fail(
        'commit-aborted',
        'Enrollment aborted before writing: the machine\'s stored credential changed while the ' +
          'redeem was in flight. Retry the enrollment.',
      );
    }
    emitProgress(opts.onProgress, {
      phase: 'info',
      message: `Plugin credential saved to the shared machine store (${store.credentialsPath}).`,
    });

    // 2. Record the enrolled server target in the committable project marker.
    const markerPath = upsertServerTarget(projectDir, serverTarget) ?? '';

    // 3. Upsert the D14 routing pin (cli-core v2 identity — B5 fix) into any
    //    existing project-local agent config.
    const pin = derivePinV2(projectDir);
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
