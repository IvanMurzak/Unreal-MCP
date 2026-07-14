import { describe, it, expect, afterEach } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import { login } from '../src/lib/login.js';
import { readEnvFile } from '../src/utils/env-file.js';
import { MachineCredentialStore } from '../src/utils/machine-credentials.js';
import { makeTempDir, rmTempDir, fakeResponse } from './helpers.js';

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

function deviceFlowFetch(pendingCount: number, success: SuccessBody = { access_token: 'final-token' }): typeof fetch {
  let tokenCalls = 0;
  return (async (url: string) => {
    if (String(url).endsWith('/authorize')) {
      return fakeResponse({
        ok: true,
        status: 200,
        body: JSON.stringify({
          device_code: 'dev123',
          user_code: 'WXYZ-1234',
          verification_uri: 'https://ai-game.dev/activate',
          interval: 1,
        }),
      });
    }
    // token endpoint
    tokenCalls += 1;
    if (tokenCalls <= pendingCount) {
      return fakeResponse({ ok: false, status: 400, body: JSON.stringify({ error: 'authorization_pending' }) });
    }
    return fakeResponse({ ok: true, status: 200, body: JSON.stringify(success) });
  }) as unknown as typeof fetch;
}

describe('login (device flow)', () => {
  it('polls past authorization_pending and returns the token', async () => {
    const r = await login({
      storeBaseDir: tmp(),
      fetchImpl: deviceFlowFetch(2),
      sleepImpl: async () => {},
      nowImpl: () => 0,
      timeoutMs: 100000,
    });
    expect(r.kind).toBe('success');
    if (r.kind === 'success') expect(r.token).toBe('final-token');
  });

  it('persists to the shared machine store by default (not a project .env)', async () => {
    const store = tmp();
    const project = tmp();
    const r = await login({
      storeBaseDir: store,
      fetchImpl: deviceFlowFetch(0, { access_token: 'jwt-1', refresh_token: 'refresh-1', expires_in: 3600 }),
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
      fetchImpl: deviceFlowFetch(0),
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

  it('increases the poll interval persistently after slow_down (RFC 8628)', async () => {
    let tokenCalls = 0;
    const fetchImpl = (async (url: string) => {
      if (String(url).endsWith('/authorize')) {
        return fakeResponse({ ok: true, status: 200, body: JSON.stringify({ device_code: 'd', user_code: 'u', verification_uri: 'v', interval: 1 }) });
      }
      tokenCalls += 1;
      if (tokenCalls === 1) {
        return fakeResponse({ ok: false, status: 400, body: JSON.stringify({ error: 'slow_down' }) });
      }
      return fakeResponse({ ok: true, status: 200, body: JSON.stringify({ access_token: 'final-token' }) });
    }) as unknown as typeof fetch;
    const sleeps: number[] = [];
    const r = await login({ storeBaseDir: tmp(), fetchImpl, sleepImpl: async (ms) => { sleeps.push(ms); }, nowImpl: () => 0, timeoutMs: 100000 });
    expect(r.kind).toBe('success');
    // One sleep per poll (at the top of the loop): 1000ms before the first
    // (slow_down) poll, then a PERSISTENTLY increased 6000ms before the retry —
    // not a one-shot extra sleep at the old 1000ms cadence.
    expect(sleeps).toEqual([1000, 6000]);
  });

  it('fails on access_denied', async () => {
    const fetchImpl = (async (url: string) => {
      if (String(url).endsWith('/authorize')) {
        return fakeResponse({ ok: true, status: 200, body: JSON.stringify({ device_code: 'd', user_code: 'u', verification_uri: 'v', interval: 1 }) });
      }
      return fakeResponse({ ok: false, status: 400, body: JSON.stringify({ error: 'access_denied' }) });
    }) as unknown as typeof fetch;
    const r = await login({ fetchImpl, sleepImpl: async () => {}, nowImpl: () => 0 });
    expect(r.kind).toBe('failure');
    if (r.kind === 'failure') expect(r.reason).toBe('access_denied');
  });

  it('times out when the user never authorizes', async () => {
    let clock = 0;
    const fetchImpl = (async (url: string) => {
      if (String(url).endsWith('/authorize')) {
        return fakeResponse({ ok: true, status: 200, body: JSON.stringify({ device_code: 'd', user_code: 'u', verification_uri: 'v', interval: 1 }) });
      }
      return fakeResponse({ ok: false, status: 400, body: JSON.stringify({ error: 'authorization_pending' }) });
    }) as unknown as typeof fetch;
    const r = await login({ fetchImpl, sleepImpl: async (ms) => { clock += ms; }, nowImpl: () => clock, timeoutMs: 3000 });
    expect(r.kind).toBe('failure');
    if (r.kind === 'failure') expect(r.reason).toBe('timeout');
  });
});
