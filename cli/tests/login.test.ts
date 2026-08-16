import { describe, it, expect, afterEach } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import { login } from '../src/lib/login.js';
import { readEnvFile } from '../src/utils/env-file.js';
import {
  MachineCredentialStore,
  effectiveFamilies,
  identityCredentialCodec,
  type DeviceAuthTransport,
  type DeviceAuthorizeResponse,
  type DeviceTokenResponse,
  type TokenExchangeClient,
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

/** Open the machine store with the test codec (no DPAPI/PowerShell in tests). */
function openStore(baseDir: string): MachineCredentialStore {
  return new MachineCredentialStore(baseDir, identityCredentialCodec);
}

/** An UNSIGNED JWT-shaped token whose payload carries `sub` (decodeJwtSubject reads it). */
function jwtWithSub(sub: string, marker = 'tok'): string {
  const payload = Buffer.from(JSON.stringify({ sub, marker }), 'utf-8').toString('base64url');
  return `eyJhbGciOiJFUzI1NiJ9.${payload}.sig`;
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

/** A fake RFC 8693 exchange client deriving a deterministic plugin family. */
function fakeExchange(
  result: { accessToken: string; refreshToken?: string; expiresAt?: string } = { accessToken: 'plugin-token' },
  calls: Array<{ subjectToken: string; clientId: string }> = [],
): TokenExchangeClient {
  return {
    async exchange(request) {
      calls.push({ subjectToken: request.subjectToken, clientId: request.clientId });
      return { ok: true, scope: 'mcp:plugin', ...result };
    },
  };
}

/** An exchange client that fails `times` times, then succeeds. */
function flakyExchange(times: number, accessToken = 'plugin-token-late'): TokenExchangeClient {
  let calls = 0;
  return {
    async exchange() {
      calls += 1;
      if (calls <= times) return { ok: false, reason: 'server_error' };
      return { ok: true, accessToken, scope: 'mcp:plugin' };
    },
  };
}

/**
 * A full fake authorization server behind `fetchImpl`: device authorization,
 * device-code token grant, RFC 8693 token exchange, and RFC 7009 revocation —
 * so the CLI's DEFAULT transports/clients are exercised and the wire scope of
 * each mint is observable.
 */
function fakeAs(opts: {
  agentToken?: string;
  pluginToken?: string;
}): {
  fetchImpl: typeof fetch;
  seen: { deviceScopes: string[]; grantTypes: string[]; revoked: string[]; exchangeScopes: string[] };
} {
  const seen = {
    deviceScopes: [] as string[],
    grantTypes: [] as string[],
    revoked: [] as string[],
    exchangeScopes: [] as string[],
  };
  const agentToken = opts.agentToken ?? 'agent-token';
  const pluginToken = opts.pluginToken ?? 'plugin-token';
  const json = (body: unknown): Response =>
    ({
      ok: true,
      status: 200,
      statusText: 'OK',
      text: async () => JSON.stringify(body),
      json: async () => body,
    }) as unknown as Response;

  const fetchImpl = (async (url: string, init?: RequestInit) => {
    const target = String(url);
    const form = new URLSearchParams(String(init?.body ?? ''));
    if (target.endsWith('/oauth/device_authorization')) {
      seen.deviceScopes.push(form.get('scope') ?? '');
      return json({
        device_code: 'dev123',
        user_code: 'WXYZ-1234',
        verification_uri: 'https://ai-game.dev/activate',
        expires_in: 900,
        interval: 0,
      });
    }
    if (target.endsWith('/oauth/token')) {
      const grant = form.get('grant_type') ?? '';
      seen.grantTypes.push(grant);
      if (grant === 'urn:ietf:params:oauth:grant-type:token-exchange') {
        seen.exchangeScopes.push(form.get('scope') ?? '');
        return json({
          access_token: pluginToken,
          issued_token_type: 'urn:ietf:params:oauth:token-type:access_token',
          token_type: 'Bearer',
          expires_in: 3600,
          refresh_token: 'plugin-refresh',
          scope: 'mcp:plugin',
        });
      }
      return json({ access_token: agentToken, refresh_token: 'agent-refresh', expires_in: 3600, token_type: 'Bearer' });
    }
    if (target.endsWith('/oauth/revoke')) {
      seen.revoked.push(form.get('token') ?? '');
      return json({});
    }
    throw new Error(`fake AS: unexpected request to ${target}`);
  }) as unknown as typeof fetch;

  return { fetchImpl, seen };
}

describe('login (OAuth 2.1 device flow → cli-core commit)', () => {
  it('polls past authorization_pending and returns the derived plugin token', async () => {
    const r = await login({
      storeBaseDir: tmp(),
      storeCodec: identityCredentialCodec,
      transport: deviceFlowTransport(2),
      exchangeClient: fakeExchange({ accessToken: 'plugin-token' }),
      sleepImpl: async () => {},
      nowImpl: () => 0,
    });
    expect(r.kind).toBe('success');
    // The surfaced token is the PLUGIN-plane credential (what engine tools send).
    if (r.kind === 'success') expect(r.token).toBe('plugin-token');
  });

  it('commits BOTH families (F1): agent from the mint, plugin from the exchange, v1 mirror = plugin', async () => {
    const storeDir = tmp();
    const project = tmp();
    const exchangeCalls: Array<{ subjectToken: string; clientId: string }> = [];
    const r = await login({
      storeBaseDir: storeDir,
      storeCodec: identityCredentialCodec,
      transport: deviceFlowTransport(0, { access_token: 'jwt-1', refresh_token: 'refresh-1', expires_in: 3600 }),
      exchangeClient: fakeExchange(
        { accessToken: 'plugin-jwt', refreshToken: 'plugin-refresh', expiresAt: new Date(7_200_000).toISOString() },
        exchangeCalls,
      ),
      sleepImpl: async () => {},
      nowImpl: () => 1_000_000, // fixed clock for a deterministic expiresAt
    });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;

    expect(r.persisted).toBe(true);
    expect(r.persistedTo).toBe('machine-store');

    // The exchange presented the fresh AGENT token and this CLI's own client id.
    expect(exchangeCalls).toEqual([{ subjectToken: 'jwt-1', clientId: 'unreal-mcp-cli' }]);

    const doc = openStore(storeDir).read();
    expect(doc).not.toBeNull();
    expect(doc!.version).toBe(2);
    const families = effectiveFamilies(doc!);
    // Agent family: the device mint, stamped with this CLI's id + agent scope.
    expect(families.agent?.accessToken).toBe('jwt-1');
    expect(families.agent?.refreshToken).toBe('refresh-1');
    expect(families.agent?.expiresAt).toBe(new Date(1_000_000 + 3600 * 1000).toISOString());
    expect(families.agent?.clientId).toBe('unreal-mcp-cli');
    expect(families.agent?.scope).toBe('mcp:agent');
    // Plugin family: the exchange derivation.
    expect(families.plugin?.accessToken).toBe('plugin-jwt');
    expect(families.plugin?.refreshToken).toBe('plugin-refresh');
    expect(families.plugin?.clientId).toBe('unreal-mcp-cli');
    expect(families.plugin?.scope).toBe('mcp:plugin');
    // v1 compat mirror = the PLUGIN family's triple (old readers keep working).
    expect(doc!.accessToken).toBe('plugin-jwt');
    expect(doc!.refreshToken).toBe('plugin-refresh');
    expect(doc!.serverTarget).toBe('https://ai-game.dev');

    // The token must NOT have leaked into any project file: no `.env` was written.
    expect(fs.existsSync(path.join(project, '.env'))).toBe(false);
  });

  it('the DEFAULT sign-in mints the AGENT scope on the wire; the exchange derives mcp:plugin (F1)', async () => {
    const { fetchImpl, seen } = fakeAs({});
    const r = await login({
      storeBaseDir: tmp(),
      storeCodec: identityCredentialCodec,
      fetchImpl,
      sleepImpl: async () => {},
      nowImpl: () => 0,
    });
    expect(r.kind).toBe('success');
    expect(seen.deviceScopes).toEqual(['mcp:agent']);
    expect(seen.grantTypes).toContain('urn:ietf:params:oauth:grant-type:token-exchange');
    expect(seen.exchangeScopes).toEqual(['mcp:plugin']);
  });

  it('--tools-only mints ONLY the plugin plane: plugin scope on the wire, NO agent family in the store (O10/F10)', async () => {
    const storeDir = tmp();
    const { fetchImpl, seen } = fakeAs({ pluginToken: 'unused' });
    const r = await login({
      storeBaseDir: storeDir,
      storeCodec: identityCredentialCodec,
      toolsOnly: true,
      fetchImpl,
      sleepImpl: async () => {},
      nowImpl: () => 0,
    });
    expect(r.kind).toBe('success');

    // The mint itself asked for the plugin scope, and NO token exchange ran.
    expect(seen.deviceScopes).toEqual(['mcp:plugin']);
    expect(seen.grantTypes).not.toContain('urn:ietf:params:oauth:grant-type:token-exchange');

    const doc = openStore(storeDir).read();
    expect(doc).not.toBeNull();
    const families = effectiveFamilies(doc!);
    // Plugin family only — App pickup impossible by design (F10).
    expect(families.plugin?.accessToken).toBe('agent-token'); // the device grant's token IS the plugin credential here
    expect(families.plugin?.scope).toBe('mcp:plugin');
    expect(families.agent).toBeUndefined();
    expect(families.legacy).toBeUndefined();
    // v1 mirror follows the plugin family.
    expect(doc!.accessToken).toBe('agent-token');
  });

  it('retries a failed exchange (partial commit) and completes on a later attempt', async () => {
    const storeDir = tmp();
    const r = await login({
      storeBaseDir: storeDir,
      storeCodec: identityCredentialCodec,
      transport: deviceFlowTransport(0, { access_token: 'agent-1' }),
      exchangeClient: flakyExchange(1, 'plugin-late'),
      sleepImpl: async () => {},
      nowImpl: () => 0,
    });
    expect(r.kind).toBe('success');
    if (r.kind === 'success') expect(r.token).toBe('plugin-late');
    const families = effectiveFamilies(openStore(storeDir).read()!);
    expect(families.agent?.accessToken).toBe('agent-1');
    expect(families.plugin?.accessToken).toBe('plugin-late');
  });

  it('surfaces exchange-failed with the AGENT family committed when every derivation attempt fails (F1 failure path)', async () => {
    const storeDir = tmp();
    const r = await login({
      storeBaseDir: storeDir,
      storeCodec: identityCredentialCodec,
      transport: deviceFlowTransport(0, { access_token: 'agent-1' }),
      exchangeClient: flakyExchange(Number.MAX_SAFE_INTEGER),
      sleepImpl: async () => {},
      nowImpl: () => 0,
    });
    expect(r.kind).toBe('failure');
    if (r.kind === 'failure') expect(r.reason).toBe('exchange-failed');
    // The two-hold design's whole point: the agent family survives the failed exchange.
    const families = effectiveFamilies(openStore(storeDir).read()!);
    expect(families.agent?.accessToken).toBe('agent-1');
    expect(families.plugin).toBeUndefined();
  });

  describe('account-switch guard (D6/F7 — --yes-gated)', () => {
    function seedStoreAs(storeDir: string, subject: string): void {
      openStore(storeDir).write({
        version: 2,
        serverTarget: 'https://ai-game.dev',
        subject,
        families: {
          plugin: { accessToken: 'old-plugin', refreshToken: 'old-refresh', clientId: 'unreal-mcp-cli', scope: 'mcp:plugin' },
        },
      });
    }

    it('WITHOUT --yes a subject mismatch is DECLINED: failure, store untouched, minted family revoked', async () => {
      const storeDir = tmp();
      seedStoreAs(storeDir, 'user-A');
      const { fetchImpl, seen } = fakeAs({ agentToken: jwtWithSub('user-B') });
      const r = await login({
        storeBaseDir: storeDir,
        storeCodec: identityCredentialCodec,
        fetchImpl,
        sleepImpl: async () => {},
        nowImpl: () => 0,
      });
      expect(r.kind).toBe('failure');
      if (r.kind === 'failure') {
        expect(r.reason).toBe('account-switch-declined');
        expect(r.error.message).toContain('--yes');
      }
      // Store untouched — still user-A's credential.
      const doc = openStore(storeDir).read();
      expect(doc?.subject).toBe('user-A');
      expect(effectiveFamilies(doc!).plugin?.accessToken).toBe('old-plugin');
      // The just-minted family was revoked best-effort (no orphan device row).
      expect(seen.revoked.length).toBeGreaterThan(0);
    });

    it('WITH --yes (assumeYes) the switch is confirmed: store REPLACED with the new account', async () => {
      const storeDir = tmp();
      seedStoreAs(storeDir, 'user-A');
      const { fetchImpl } = fakeAs({ agentToken: jwtWithSub('user-B') });
      const r = await login({
        storeBaseDir: storeDir,
        storeCodec: identityCredentialCodec,
        assumeYes: true,
        fetchImpl,
        sleepImpl: async () => {},
        nowImpl: () => 0,
      });
      expect(r.kind).toBe('success');
      const doc = openStore(storeDir).read();
      expect(doc?.subject).toBe('user-B');
      const families = effectiveFamilies(doc!);
      expect(families.agent?.accessToken).toBe(jwtWithSub('user-B'));
      // The old account's family is gone (single-account store, D6).
      expect(families.plugin?.accessToken).not.toBe('old-plugin');
    });

    it('a SAME-subject re-login needs no confirmation and no --yes', async () => {
      const storeDir = tmp();
      seedStoreAs(storeDir, 'user-A');
      const { fetchImpl } = fakeAs({ agentToken: jwtWithSub('user-A', 'fresh') });
      const r = await login({
        storeBaseDir: storeDir,
        storeCodec: identityCredentialCodec,
        fetchImpl,
        sleepImpl: async () => {},
        nowImpl: () => 0,
      });
      expect(r.kind).toBe('success');
      expect(openStore(storeDir).read()?.subject).toBe('user-A');
    });
  });

  it('persists the token into a project .env when --path (projectDir) is given (unchanged this release)', async () => {
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

  it('--path mints the PLUGIN scope (the .env token goes straight onto the MCP wire)', async () => {
    const dir = tmp();
    const { fetchImpl, seen } = fakeAs({});
    const r = await login({ projectDir: dir, fetchImpl, sleepImpl: async () => {}, nowImpl: () => 0 });
    expect(r.kind).toBe('success');
    expect(seen.deviceScopes).toEqual(['mcp:plugin']);
    expect(seen.grantTypes).not.toContain('urn:ietf:params:oauth:grant-type:token-exchange');
  });

  it('fails cleanly on access_denied', async () => {
    const r = await login({
      storeBaseDir: tmp(),
      transport: terminalTransport('access_denied'),
      sleepImpl: async () => {},
      nowImpl: () => 0,
    });
    expect(r.kind).toBe('failure');
    if (r.kind === 'failure') expect(r.reason).toBe('denied');
  });

  it('fails cleanly on expired_token', async () => {
    const r = await login({
      storeBaseDir: tmp(),
      transport: terminalTransport('expired_token'),
      sleepImpl: async () => {},
      nowImpl: () => 0,
    });
    expect(r.kind).toBe('failure');
    if (r.kind === 'failure') expect(r.reason).toBe('expired');
  });
});
