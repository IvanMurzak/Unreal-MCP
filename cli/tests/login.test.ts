import { describe, it, expect, afterEach } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import { login } from '../src/lib/login.js';
import { readEnvFile } from '../src/utils/env-file.js';
import { MachineCredentialStore } from '../src/utils/machine-credentials.js';
import type {
  DeviceAuthTransport,
  DeviceAuthorizeResponse,
  DeviceTokenResponse,
} from '@baizor/gamedev-cli-core';
import { makeTempDir, rmTempDir } from './helpers.js';

const dirs: string[] = [];
afterEach(() => {
  while (dirs.length) rmTempDir(dirs.pop()!);
});
function tmp(): string {
  const d = makeTempDir();
  dirs.push(d);
  return d;
}

interface SuccessBody {
  access_token: string;
  refresh_token?: string;
  expires_in?: number;
}

/**
 * A fake cli-core device-auth transport: `pendingCount` `authorization_pending`
 * polls, then the success token. Bypasses the HTTP transport entirely, so the
 * test exercises the CLI's adapter (delegation + persistence), not core's RFC
 * 8628 state machine (which cli-core unit-tests itself).
 */
function deviceFlowTransport(
  pendingCount: number,
  success: SuccessBody = { access_token: 'final-token' },
): DeviceAuthTransport {
  let polls = 0;
  return {
    async requestDeviceCode(): Promise<DeviceAuthorizeResponse> {
      return {
        device_code: 'dev123',
        user_code: 'WXYZ-1234',
        verification_uri: 'https://ai-game.dev/activate',
        expires_in: 900,
        interval: 0,
      };
    },
    async pollToken(): Promise<DeviceTokenResponse> {
      polls += 1;
      if (polls <= pendingCount) return { error: 'authorization_pending' };
      return {
        access_token: success.access_token,
        refresh_token: success.refresh_token,
        expires_in: success.expires_in,
        token_type: 'Bearer',
      };
    },
  };
}

/** A transport that returns a terminal OAuth error on the first poll. */
function terminalTransport(error: string): DeviceAuthTransport {
  return {
    async requestDeviceCode(): Promise<DeviceAuthorizeResponse> {
      return { device_code: 'd', user_code: 'u', verification_uri: 'https://ai-game.dev/activate', expires_in: 900 };
    },
    async pollToken(): Promise<DeviceTokenResponse> {
      return { error };
    },
  };
}

describe('login (OAuth 2.1 device flow → cli-core)', () => {
  it('polls past authorization_pending and returns the token', async () => {
    const r = await login({
      storeBaseDir: tmp(),
      transport: deviceFlowTransport(2),
      sleepImpl: async () => {},
      nowImpl: () => 0,
    });
    expect(r.kind).toBe('success');
    if (r.kind === 'success') expect(r.token).toBe('final-token');
  });

  it('persists the FULL credential set to the shared machine store by default (not a project .env)', async () => {
    const store = tmp();
    const project = tmp();
    const r = await login({
      storeBaseDir: store,
      transport: deviceFlowTransport(0, { access_token: 'jwt-1', refresh_token: 'refresh-1', expires_in: 3600 }),
      sleepImpl: async () => {},
      nowImpl: () => 1_000_000, // fixed clock for a deterministic expiresAt
    });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;

    expect(r.persisted).toBe(true);
    expect(r.persistedTo).toBe('machine-store');

    const creds = await new MachineCredentialStore(store).read();
    expect(creds).not.toBeNull();
    expect(creds!.version).toBe(1);
    expect(creds!.accessToken).toBe('jwt-1');
    expect(creds!.refreshToken).toBe('refresh-1');
    expect(creds!.serverTarget).toBe('https://ai-game.dev');
    // expiresAt = now (1_000_000ms) + expires_in (3600s).
    expect(creds!.expiresAt).toBe(new Date(1_000_000 + 3600 * 1000).toISOString());

    // The token must NOT have leaked into any project file: no `.env` was written.
    expect(fs.existsSync(path.join(project, '.env'))).toBe(false);
  });

  it('persists the token into a project .env when --path (projectDir) is given', async () => {
    const dir = tmp();
    const r = await login({
      projectDir: dir,
      transport: deviceFlowTransport(0),
      sleepImpl: async () => {},
      nowImpl: () => 0,
    });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.persisted).toBe(true);
    expect(r.persistedTo).toBe('project-env');
    const env = readEnvFile(path.join(dir, '.env'));
    expect(env['UNREAL_MCP_TOKEN']).toBe('final-token');
    expect(env['UNREAL_MCP_CONNECTION_MODE']).toBe('Cloud');
    // The persisted token must be gitignored (§8) so it can't be committed.
    expect(fs.readFileSync(path.join(dir, '.gitignore'), 'utf-8')).toContain('.env');
  });

  it('fails cleanly on access_denied', async () => {
    const r = await login({ transport: terminalTransport('access_denied'), sleepImpl: async () => {}, nowImpl: () => 0 });
    expect(r.kind).toBe('failure');
    if (r.kind === 'failure') expect(r.reason).toBe('denied');
  });

  it('fails cleanly on expired_token', async () => {
    const r = await login({ transport: terminalTransport('expired_token'), sleepImpl: async () => {}, nowImpl: () => 0 });
    expect(r.kind).toBe('failure');
    if (r.kind === 'failure') expect(r.reason).toBe('expired');
  });
});
