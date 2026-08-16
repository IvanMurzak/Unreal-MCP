// `login` — OAuth 2.1 device authorization flow (RFC 8628) against ai-game.dev
// on the shared `@baizor/gamedev-cli-core` engine, adopted onto the
// unified-machine-auth model (design 03 F1/F7/F10, task e2):
//
//   • DEFAULT: an **agent-scope** (`mcp:agent`) mint committed through cli-core's
//     `commitAgentLogin` — the two-lock-hold sequence: agent family under the
//     first lock hold, RFC 8693 token exchange (lock released) deriving the
//     plugin family, plugin family + v1 mirror under the second hold. A failed
//     exchange leaves the agent family committed (`partial`) and is retried
//     here with backoff before surfacing.
//   • `--tools-only` (O10 ruling — this exact flag name): a **plugin-scope**
//     (`mcp:plugin`) mint committed via `commitToolsOnlyLogin` — the store then
//     holds NO agent family, so desktop-App pickup is impossible by design and
//     a CI runner appears as its own revocable device group (F10).
//   • `--path <dir>`: the per-project override — a plugin-scope mint written to
//     `<dir>/.env` (gitignored), NOT the machine store. Kept working this
//     release (W4 demotes the default sink in e4, not here).
//
// The D6/F7 account-switch guard runs inside the commit helpers: a mint whose
// `subject` differs from the store's requires explicit confirmation
// (`--yes`-gated on this CLI); decline revokes the just-minted family (best
// effort, RFC 7009) and aborts with the store untouched.
//
// This file NEVER re-implements store writes, locking, or refresh — that is
// cli-core's `login-commit.ts` / `MachineCredentialProvider` (the e2 spec
// deleted the CLI-local store copy). transport + exchange + delay + clock are
// injectable for tests. Library-safe: never throws past the boundary. Never
// logs token bytes.

import { writeEnvFile, ensureEnvGitignored } from '../utils/env-file.js';
import {
  commitAgentLogin,
  commitToolsOnlyLogin,
  derivePluginFamily,
  deviceLogin,
  effectiveFamilies,
  HttpDeviceAuthTransport,
  HttpTokenExchangeClient,
  MachineCredentialStore,
  unrealAdapter,
  DEFAULT_PLUGIN_SCOPE,
  MCP_AGENT_SCOPE,
  type CredentialCodec,
  type DeviceAuthTransport,
  type MachineCredentials as CoreMachineCredentials,
  type TokenExchangeClient,
} from '@baizor/gamedev-cli-core';
import * as path from 'path';
import { asError } from '../utils/error.js';
import { emitProgress } from './progress.js';
import type { ProgressCallback } from './types.js';

const DEFAULT_BASE_URL = 'https://ai-game.dev';

/** How many times the F1.4 plugin-family derivation is retried after a `partial` commit. */
const EXCHANGE_RETRY_ATTEMPTS = 2;
/** Base backoff between derivation retries (doubles per attempt). */
const EXCHANGE_RETRY_BACKOFF_MS = 2_000;

export interface LoginOptions {
  baseUrl?: string;
  /**
   * When set, persist the credential into `<projectDir>/.env` (gitignored) instead
   * of the shared machine store — the `--path` per-project override. Unset (the
   * default) persists into the shared machine credential store.
   */
  projectDir?: string;
  /**
   * `--tools-only` (O10): mint a plugin-plane credential ONLY — no agent family,
   * so the desktop App cannot pick the sign-in up (CI/automation runners).
   */
  toolsOnly?: boolean;
  /**
   * `--yes`: confirm replacing the machine's stored account when this sign-in
   * resolves to a DIFFERENT subject (D6/F7). Without it a subject mismatch is
   * declined fail-closed: the just-minted family is revoked (best effort) and
   * the store stays untouched.
   */
  assumeYes?: boolean;
  /** Test seam: override the machine credential store base dir (default `~/.ai-game-dev`). */
  storeBaseDir?: string;
  /** Test seam: override the store's at-rest codec (default: DPAPI on Windows). */
  storeCodec?: CredentialCodec;
  /**
   * Overall poll deadline (ms). Accepted for CLI/back-compat; the effective
   * deadline is the device-code `expires_in` the authorization server returns
   * (RFC 8628), which `deviceLogin` enforces.
   */
  timeoutMs?: number;
  fetchImpl?: typeof fetch;
  sleepImpl?: (ms: number) => Promise<void>;
  nowImpl?: () => number;
  /** Test seam: inject a device-authorization transport (bypasses the HTTP transport). */
  transport?: DeviceAuthTransport;
  /** Test seam: inject the RFC 8693 exchange client used to derive the plugin family. */
  exchangeClient?: TokenExchangeClient;
  /**
   * Test/programmatic seam for the D6/F7 confirmation. Overrides `assumeYes`
   * wiring; absent + no `assumeYes` ⇒ a mismatch is declined (fail closed).
   */
  confirmAccountSwitch?: (info: { storedSubject: string; newSubject: string }) => boolean | Promise<boolean>;
  onProgress?: ProgressCallback;
}

/** Where a successful login persisted its credential. */
export type PersistTarget = 'machine-store' | 'project-env';

export interface LoginSuccess {
  kind: 'success';
  success: true;
  token: string;
  /** `true` when the credential was persisted (always true on success). */
  persisted: boolean;
  /** Where the credential landed: the shared machine store (default) or a project `.env` (`--path`). */
  persistedTo: PersistTarget;
  /** Absolute path of the file the credential was written to. */
  credentialPath: string;
  /** Set only for the project-`.env` override path (back-compat convenience). */
  envPath?: string;
}

export interface LoginFailure {
  kind: 'failure';
  success: false;
  /**
   * Coarse reason — `denied`, `expired`, `cancelled`, `error` (cli-core
   * `deviceLogin` reasons), plus the commit-stage reasons
   * `account-switch-declined`, `exchange-failed`, and `commit-aborted`.
   */
  reason: string;
  error: Error;
}

export type LoginResult = LoginSuccess | LoginFailure;

export async function login(opts: LoginOptions = {}): Promise<LoginResult> {
  const baseUrl = (opts.baseUrl ?? DEFAULT_BASE_URL).replace(/\/+$/, '');
  const sleep = opts.sleepImpl ?? ((ms: number) => new Promise<void>((r) => setTimeout(r, ms)));

  try {
    emitProgress(opts.onProgress, { phase: 'start', message: 'Requesting device authorization' });

    // The plane selects the scope (03 F1/F10): the default sign-in mints the
    // agent plane and DERIVES the plugin family via token exchange; the
    // `--path` project-`.env` sink and `--tools-only` mint the plugin plane
    // directly — their token goes straight onto the MCP wire.
    const scope = opts.projectDir || opts.toolsOnly ? DEFAULT_PLUGIN_SCOPE : MCP_AGENT_SCOPE;

    // Build the RFC 8628 device-auth transport. `client_id` is the Unreal
    // product id from the shared engine adapter (`unreal-mcp-cli` — unchanged,
    // cli-core `engine-adapter.ts`). A test may inject its own transport.
    const transport =
      opts.transport ??
      new HttpDeviceAuthTransport({
        serverBaseUrl: baseUrl,
        clientId: unrealAdapter.clientId,
        scope,
        fetchImpl: opts.fetchImpl,
      });

    const result = await deviceLogin({
      serverBaseUrl: baseUrl,
      clientId: unrealAdapter.clientId,
      // Scope is carried by the transport above; `deviceLogin` reads its own
      // `scope` only to build a DEFAULT transport, which we always override.
      // Record the AS root on the credential (never a pinned hub URL) — b2 MED-2.
      serverTarget: baseUrl,
      transport,
      delay: opts.sleepImpl,
      now: opts.nowImpl,
      onUserCode: (userCode, verificationUri) => {
        emitProgress(opts.onProgress, {
          phase: 'info',
          message: `To authorize, open ${verificationUri} and enter code ${userCode}`,
        });
      },
      onPolling: () => {
        emitProgress(opts.onProgress, { phase: 'info', message: 'Waiting for authorization…' });
      },
    });

    if (!result.ok) {
      return { kind: 'failure', success: false, reason: result.reason, error: new Error(result.message) };
    }

    // --path: the per-project `.env` sink (unchanged this release — W4/e4).
    if (opts.projectDir) {
      const envPath = persistProjectEnv(opts.projectDir, result.credentials);
      emitProgress(opts.onProgress, { phase: 'done', message: 'Login complete.' });
      return {
        kind: 'success',
        success: true,
        token: result.credentials.accessToken ?? '',
        persisted: true,
        persistedTo: 'project-env',
        credentialPath: envPath,
        envPath,
      };
    }

    return await commitToMachineStore(opts, baseUrl, sleep, result.credentials);
  } catch (err) {
    return { kind: 'failure', success: false, reason: 'error', error: asError(err) };
  }
}

/**
 * Commit the mint into the shared machine store through cli-core's login-commit
 * helpers (never a hand-rolled write): `commitToolsOnlyLogin` for `--tools-only`,
 * else the F1 two-lock-hold `commitAgentLogin` + bounded derivation retries.
 */
async function commitToMachineStore(
  opts: LoginOptions,
  baseUrl: string,
  sleep: (ms: number) => Promise<void>,
  credentials: CoreMachineCredentials,
): Promise<LoginResult> {
  const store = new MachineCredentialStore(opts.storeBaseDir, opts.storeCodec);
  const warn = (message: string) => emitProgress(opts.onProgress, { phase: 'info', message });

  // The D6/F7 guard confirmation: `--yes` confirms an account switch; absent,
  // the commit helpers decline fail-closed (revoke just-minted, store untouched).
  const confirmAccountSwitch =
    opts.confirmAccountSwitch ?? (opts.assumeYes ? async () => true : undefined);

  if (opts.toolsOnly) {
    const outcome = await commitToolsOnlyLogin({
      store,
      clientId: unrealAdapter.clientId,
      credentials,
      confirmAccountSwitch,
      fetchImpl: opts.fetchImpl,
      onWarning: warn,
    });
    if (outcome.status === 'switch-declined') return switchDeclinedFailure(outcome);
    if (outcome.status === 'aborted') return abortedFailure(outcome.reason);
    emitProgress(opts.onProgress, { phase: 'done', message: 'Login complete (tools-only).' });
    return machineStoreSuccess(store, credentials.accessToken ?? '');
  }

  const exchangeClient =
    opts.exchangeClient ??
    new HttpTokenExchangeClient({ defaultServerBaseUrl: baseUrl, fetchImpl: opts.fetchImpl, now: opts.nowImpl });

  const outcome = await commitAgentLogin({
    store,
    exchangeClient,
    clientId: unrealAdapter.clientId,
    credentials,
    confirmAccountSwitch,
    fetchImpl: opts.fetchImpl,
    onWarning: warn,
  });

  if (outcome.status === 'switch-declined') return switchDeclinedFailure(outcome);
  if (outcome.status === 'aborted') return abortedFailure(outcome.reason, outcome.detail);

  let document = outcome.status === 'committed' ? outcome.document : undefined;

  if (outcome.status === 'partial') {
    // F1 failure path: the agent family IS committed; retry the derivation leg
    // alone with backoff before surfacing (the design's "partially authorized,
    // retrying").
    emitProgress(opts.onProgress, {
      phase: 'info',
      message: 'Partially authorized — retrying tool-credential derivation…',
    });
    let lastFailure = outcome.exchangeFailure;
    for (let attempt = 0; attempt < EXCHANGE_RETRY_ATTEMPTS && document === undefined; attempt += 1) {
      await sleep(EXCHANGE_RETRY_BACKOFF_MS * (attempt + 1));
      const retry = await derivePluginFamily({
        store,
        exchangeClient,
        clientId: unrealAdapter.clientId,
        agentAccessToken: credentials.accessToken ?? '',
        expectedSubject: credentials.subject,
        serverTarget: credentials.serverTarget ?? baseUrl,
        fetchImpl: opts.fetchImpl,
        onWarning: warn,
      });
      if (retry.status === 'derived') {
        document = retry.document;
      } else if (retry.status === 'aborted') {
        return abortedFailure(retry.reason, retry.detail);
      } else {
        lastFailure = retry.reason;
      }
    }
    if (document === undefined) {
      return {
        kind: 'failure',
        success: false,
        reason: 'exchange-failed',
        error: new Error(
          `Signed in, but deriving the tool credential failed (${lastFailure}). ` +
            'The account sign-in is committed — re-run `unreal-mcp-cli login` to finish.',
        ),
      };
    }
  }

  emitProgress(opts.onProgress, { phase: 'done', message: 'Login complete.' });
  // Surface the PLUGIN-plane token — the credential engine tools authenticate
  // with (the agent token never goes onto the MCP wire from this CLI).
  const pluginToken = document ? effectiveFamilies(document).plugin?.accessToken : undefined;
  return machineStoreSuccess(store, pluginToken ?? credentials.accessToken ?? '');
}

function machineStoreSuccess(store: MachineCredentialStore, token: string): LoginSuccess {
  return {
    kind: 'success',
    success: true,
    token,
    persisted: true,
    persistedTo: 'machine-store',
    credentialPath: store.credentialsPath,
  };
}

function switchDeclinedFailure(info: { storedSubject: string; newSubject: string }): LoginFailure {
  return {
    kind: 'failure',
    success: false,
    reason: 'account-switch-declined',
    error: new Error(
      `This machine is signed in as a different account (${info.storedSubject}); the new sign-in ` +
        `resolves to ${info.newSubject}. Re-run with --yes to switch the whole machine to the new ` +
        'account (the old account is signed out everywhere on this machine). Nothing was changed.',
    ),
  };
}

function abortedFailure(reason: string, detail?: string): LoginFailure {
  return {
    kind: 'failure',
    success: false,
    reason: 'commit-aborted',
    error: new Error(
      `Login aborted before writing (${reason}${detail ? `: ${detail}` : ''}) — the machine's stored ` +
        'credential changed while the sign-in was in flight. Re-run `unreal-mcp-cli login` to retry.',
    ),
  };
}

/**
 * The `--path` sink: write the plugin-scope token into `<projectDir>/.env`
 * (gitignored BEFORE the secret lands on disk — mirrors `configure`'s §8
 * guard). Never touches the machine store.
 */
function persistProjectEnv(projectDir: string, credentials: CoreMachineCredentials): string {
  const dir = path.resolve(projectDir);
  const envPath = path.join(dir, '.env');
  ensureEnvGitignored(dir);
  writeEnvFile(envPath, {
    UNREAL_MCP_TOKEN: credentials.accessToken ?? '',
    UNREAL_MCP_CONNECTION_MODE: 'Cloud',
  });
  return envPath;
}
