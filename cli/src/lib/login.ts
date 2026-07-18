// `login` — OAuth 2.1 device authorization flow (RFC 8628) against ai-game.dev,
// now on the shared `@baizor/gamedev-cli-core` `deviceLogin` engine (auth-fixes
// design 02 T1). It POSTs `client_id` (`unreal-mcp-cli`) + scope (`mcp:plugin`)
// to `{base}/oauth/device_authorization`, surfaces the verification URL + user
// code via `onProgress`, then redeems the grant at `{base}/oauth/token` — an
// ES256 hub JWT plus a rotating refresh token. This REPLACES the legacy JSON
// device-authorization endpoints and **never mints a PAT** (personal access
// tokens stay a manual, human-only tool).
//
// On success the FULL credential set (accessToken, refreshToken, expiresAt,
// serverTarget, subject) is persisted into the SHARED machine credential store
// (`~/.ai-game-dev/credentials.json`, mcp-authorize design 06/09 D12) so sign-in
// happens once per machine — never into a committable project file on the default
// path. The `--path` override still keeps a project-local `.env` (gitignored) for
// per-project accounts. transport + delay + clock are injectable for tests.
// Library-safe: never throws past the boundary.

import { writeEnvFile, ensureEnvGitignored } from '../utils/env-file.js';
import {
  MachineCredentialStore,
  type MachineCredentials,
  CREDENTIALS_VERSION,
} from '../utils/machine-credentials.js';
import {
  deviceLogin,
  HttpDeviceAuthTransport,
  unrealAdapter,
  DEFAULT_PLUGIN_SCOPE,
  type DeviceAuthTransport,
  type MachineCredentials as CoreMachineCredentials,
} from '@baizor/gamedev-cli-core';
import * as path from 'path';
import { asError } from '../utils/error.js';
import { emitProgress } from './progress.js';
import type { ProgressCallback } from './types.js';

const DEFAULT_BASE_URL = 'https://ai-game.dev';

export interface LoginOptions {
  baseUrl?: string;
  /**
   * When set, persist the credential into `<projectDir>/.env` (gitignored) instead
   * of the shared machine store — the `--path` per-project override. Unset (the
   * default) persists into the shared machine credential store.
   */
  projectDir?: string;
  /** Test seam: override the machine credential store base dir (default `~/.ai-game-dev`). */
  storeBaseDir?: string;
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
  /** Coarse reason — `denied`, `expired`, `cancelled`, `error`, ... (cli-core `deviceLogin` reasons). */
  reason: string;
  error: Error;
}

export type LoginResult = LoginSuccess | LoginFailure;

export async function login(opts: LoginOptions = {}): Promise<LoginResult> {
  const baseUrl = (opts.baseUrl ?? DEFAULT_BASE_URL).replace(/\/+$/, '');

  try {
    emitProgress(opts.onProgress, { phase: 'start', message: 'Requesting device authorization' });

    // Build the RFC 8628 device-auth transport. `client_id` is the Unreal product
    // id from the shared engine adapter; scope selects the mcp:plugin JWT +
    // refresh-token response. A test may inject its own transport directly.
    const transport =
      opts.transport ??
      new HttpDeviceAuthTransport({
        serverBaseUrl: baseUrl,
        clientId: unrealAdapter.clientId,
        scope: DEFAULT_PLUGIN_SCOPE,
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

    const persisted = await persistCredential(opts, result.credentials);
    emitProgress(opts.onProgress, { phase: 'done', message: 'Login complete.' });
    return {
      kind: 'success',
      success: true,
      token: result.credentials.accessToken ?? '',
      persisted: true,
      persistedTo: persisted.persistedTo,
      credentialPath: persisted.credentialPath,
      envPath: persisted.envPath,
    };
  } catch (err) {
    return { kind: 'failure', success: false, reason: 'error', error: asError(err) };
  }
}

interface PersistOutcome {
  persistedTo: PersistTarget;
  credentialPath: string;
  envPath?: string;
}

/**
 * Persist the freshly-authorized credential. The default path writes the shared
 * machine store (`~/.ai-game-dev/credentials.json`, DPAPI/0600) so the token
 * NEVER lands in a committable project file; the `--path` override keeps the
 * project-local `.env` (gitignored) for per-project accounts. The store is
 * byte-compatible with the C# / cli-core store, so the returned cli-core
 * credential set is written through verbatim.
 */
async function persistCredential(
  opts: LoginOptions,
  credentials: CoreMachineCredentials,
): Promise<PersistOutcome> {
  if (opts.projectDir) {
    const dir = path.resolve(opts.projectDir);
    const envPath = path.join(dir, '.env');
    // The token is a secret — make sure `.env` is gitignored BEFORE it lands on
    // disk (mirrors `configure`'s §8 guard so `login -p .` can't leave a
    // committable token file behind even if the process dies between the two
    // writes).
    ensureEnvGitignored(dir);
    writeEnvFile(envPath, { UNREAL_MCP_TOKEN: credentials.accessToken ?? '', UNREAL_MCP_CONNECTION_MODE: 'Cloud' });
    return { persistedTo: 'project-env', credentialPath: envPath, envPath };
  }

  const store = new MachineCredentialStore(opts.storeBaseDir);
  const localCreds: MachineCredentials = {
    version: CREDENTIALS_VERSION,
    accessToken: credentials.accessToken,
    refreshToken: credentials.refreshToken ?? undefined,
    expiresAt: credentials.expiresAt ?? undefined,
    serverTarget: credentials.serverTarget ?? undefined,
    subject: credentials.subject,
  };
  await store.write(localCreds);
  return { persistedTo: 'machine-store', credentialPath: store.credentialsPath };
}
