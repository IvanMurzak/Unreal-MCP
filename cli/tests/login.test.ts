import { describe, it, expect, afterEach } from 'vitest';
import * as path from 'path';
import { login } from '../src/lib/login.js';
import { readEnvFile } from '../src/utils/env-file.js';
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

function deviceFlowFetch(pendingCount: number): typeof fetch {
  let tokenCalls = 0;
  return (async (url: string) => {
    if (String(url).endsWith('/code')) {
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
    return fakeResponse({ ok: true, status: 200, body: JSON.stringify({ access_token: 'final-token' }) });
  }) as unknown as typeof fetch;
}

describe('login (device flow)', () => {
  it('polls past authorization_pending and returns the token', async () => {
    const r = await login({
      fetchImpl: deviceFlowFetch(2),
      sleepImpl: async () => {},
      nowImpl: () => 0,
      timeoutMs: 100000,
    });
    expect(r.kind).toBe('success');
    if (r.kind === 'success') expect(r.token).toBe('final-token');
  });

  it('persists the token into a project .env when projectDir is given', async () => {
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
    const env = readEnvFile(path.join(dir, '.env'));
    expect(env['UNREAL_MCP_TOKEN']).toBe('final-token');
    expect(env['UNREAL_MCP_CONNECTION_MODE']).toBe('Cloud');
  });

  it('fails on access_denied', async () => {
    const fetchImpl = (async (url: string) => {
      if (String(url).endsWith('/code')) {
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
      if (String(url).endsWith('/code')) {
        return fakeResponse({ ok: true, status: 200, body: JSON.stringify({ device_code: 'd', user_code: 'u', verification_uri: 'v', interval: 1 }) });
      }
      return fakeResponse({ ok: false, status: 400, body: JSON.stringify({ error: 'authorization_pending' }) });
    }) as unknown as typeof fetch;
    const r = await login({ fetchImpl, sleepImpl: async (ms) => { clock += ms; }, nowImpl: () => clock, timeoutMs: 3000 });
    expect(r.kind).toBe('failure');
    if (r.kind === 'failure') expect(r.reason).toBe('timeout');
  });
});
