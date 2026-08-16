// e2 DoD: the Cloud-mode machine-store token fallback.
//
// Cloud mode used to read ONLY the plugin-config `cloudToken`, so a
// machine-store credential (a `login`-once machine) was INVISIBLE to
// `run-tool`/`status`. These tests pin the new resolution order
//
//   explicit override (flag/env/`.env`) > plugin-config `cloudToken` > machine
//   store via cli-core's `MachineCredentialProvider`
//
// and prove the provider path end-to-end: a REAL on-disk store (cli-core
// codec-injected), and a refresh THROUGH EXPIRY against a fake authorization
// server reached only via the injected `fetchImpl`.

import { describe, it, expect, afterEach } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import {
  MachineCredentialStore,
  effectiveFamilies,
  identityCredentialCodec,
} from '@baizor/gamedev-cli-core';
import { resolveConnection, PLUGIN_CONFIG_RELATIVE_PATH, type MachineAuthOptions } from '../src/utils/config.js';
import { runTool } from '../src/lib/run-tool.js';
import { getStatus } from '../src/lib/status.js';
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

/** A project dir holding a plugin JSON config. */
function makeProject(config: object): string {
  const dir = tmp();
  const configPath = path.join(dir, PLUGIN_CONFIG_RELATIVE_PATH);
  fs.mkdirSync(path.dirname(configPath), { recursive: true });
  fs.writeFileSync(configPath, JSON.stringify(config), 'utf-8');
  return dir;
}

const CLOUD_CONFIG = { connectionMode: 'Cloud', cloudUrl: 'https://ai-game.dev' };

/** Write a v2 machine store with a plugin family into a temp dir; returns machineAuth options. */
function seedMachineStore(family: {
  accessToken: string;
  refreshToken?: string;
  expiresAt?: string;
  clientId?: string;
  serverTarget?: string;
}): { auth: MachineAuthOptions; storeDir: string } {
  const storeDir = tmp();
  new MachineCredentialStore(storeDir, identityCredentialCodec).write({
    version: 2,
    serverTarget: family.serverTarget ?? 'https://ai-game.dev',
    families: {
      plugin: {
        accessToken: family.accessToken,
        ...(family.refreshToken !== undefined ? { refreshToken: family.refreshToken } : {}),
        ...(family.expiresAt !== undefined ? { expiresAt: family.expiresAt } : {}),
        clientId: family.clientId ?? 'unreal-mcp-cli',
        scope: 'mcp:plugin',
      },
    },
  });
  return { auth: { storeBaseDir: storeDir, codec: identityCredentialCodec }, storeDir };
}

describe('Cloud-mode token resolution ORDER (e2 — the order itself is the contract)', () => {
  it('plugin-config cloudToken WINS over a present machine-store credential (until W4 demotes it)', async () => {
    const dir = makeProject({ ...CLOUD_CONFIG, cloudToken: 'config-token' });
    const { auth } = seedMachineStore({ accessToken: 'store-token' });
    const r = await resolveConnection({ projectDir: dir, processEnv: {}, machineAuth: auth });
    expect(r.token).toBe('config-token');
    expect(r.source).toBe('plugin-config');
  });

  it('an explicit override token WINS over both cloudToken and the machine store', async () => {
    const dir = makeProject({ ...CLOUD_CONFIG, cloudToken: 'config-token' });
    const { auth } = seedMachineStore({ accessToken: 'store-token' });
    const r = await resolveConnection({ projectDir: dir, processEnv: {}, token: 'explicit', machineAuth: auth });
    expect(r.token).toBe('explicit');
  });

  it('with NO cloudToken the machine store supplies the token (the fallback this task adds)', async () => {
    const dir = makeProject(CLOUD_CONFIG);
    const { auth } = seedMachineStore({ accessToken: 'store-token' });
    const r = await resolveConnection({ projectDir: dir, processEnv: {}, machineAuth: auth });
    expect(r.url).toBe('https://ai-game.dev/mcp');
    expect(r.token).toBe('store-token');
    expect(r.source).toBe('plugin-config');
  });

  it('Custom mode NEVER consults the machine store (its own token/authOption gate rules)', async () => {
    const dir = makeProject({ connectionMode: 'Custom', host: 'http://localhost:9100', authOption: 'None' });
    let consulted = false;
    const r = await resolveConnection({
      projectDir: dir,
      processEnv: {},
      machineAuth: {
        readToken: async () => {
          consulted = true;
          return 'store-token';
        },
      },
    });
    expect(r.token).toBeUndefined();
    expect(consulted).toBe(false);
  });

  it('a signed-out machine degrades to NO token (never a throw)', async () => {
    const dir = makeProject(CLOUD_CONFIG);
    const emptyStore = tmp();
    const r = await resolveConnection({
      projectDir: dir,
      processEnv: {},
      machineAuth: { storeBaseDir: emptyStore, codec: identityCredentialCodec },
    });
    expect(r.token).toBeUndefined();
    expect(r.url).toBe('https://ai-game.dev/mcp');
  });
});

describe('run-tool / status from a machine-store-ONLY machine (e2 DoD)', () => {
  it('runTool sends the machine-store Bearer token when the plugin config has no cloudToken', async () => {
    const dir = makeProject(CLOUD_CONFIG);
    const { auth } = seedMachineStore({ accessToken: 'store-jwt-1' });

    let authHeader: string | undefined;
    const fetchImpl = (async (_url: string, init: RequestInit) => {
      authHeader = (init.headers as Record<string, string>)['Authorization'];
      return fakeResponse({ ok: true, status: 200, body: '{"status":"success"}' });
    }) as unknown as typeof fetch;

    const r = await runTool({ toolName: 'ping', projectDir: dir, fetchImpl, machineAuth: auth });
    expect(r.kind).toBe('success');
    expect(authHeader).toBe('Bearer store-jwt-1');
  });

  it('getStatus reports hasToken=true from the machine store alone', async () => {
    const dir = makeProject(CLOUD_CONFIG);
    const { auth } = seedMachineStore({ accessToken: 'store-jwt-2' });
    const report = await getStatus({ projectDir: dir, noProbe: true, machineAuth: auth });
    expect(report.connection.url).toBe('https://ai-game.dev/mcp');
    expect(report.connection.hasToken).toBe(true);
    expect(report.connection.source).toBe('plugin-config');
  });
});

describe('provider refresh THROUGH EXPIRY against a fake AS (e2 DoD)', () => {
  /** A fake AS answering only the refresh-token grant at `/oauth/token`. */
  function fakeRefreshAs(freshToken: string): {
    fetchImpl: typeof fetch;
    seen: Array<{ url: string; grantType: string | null; clientId: string | null; refreshToken: string | null; scope: string | null }>;
  } {
    const seen: Array<{ url: string; grantType: string | null; clientId: string | null; refreshToken: string | null; scope: string | null }> = [];
    const fetchImpl = (async (url: string, init?: RequestInit) => {
      const form = new URLSearchParams(String(init?.body ?? ''));
      seen.push({
        url: String(url),
        grantType: form.get('grant_type'),
        clientId: form.get('client_id'),
        refreshToken: form.get('refresh_token'),
        scope: form.get('scope'),
      });
      return fakeResponse({
        ok: true,
        status: 200,
        body: JSON.stringify({
          access_token: freshToken,
          refresh_token: 'rotated-refresh',
          expires_in: 3600,
          token_type: 'Bearer',
        }),
      });
    }) as unknown as typeof fetch;
    return { fetchImpl, seen };
  }

  it('an EXPIRED stored token is refreshed via the fake AS; the rotation is persisted', async () => {
    const dir = makeProject(CLOUD_CONFIG);
    const now = 1_000_000_000_000;
    const { auth, storeDir } = seedMachineStore({
      accessToken: 'expired-token',
      refreshToken: 'refresh-1',
      expiresAt: new Date(now - 60_000).toISOString(), // already expired
      clientId: 'unreal-mcp-cli',
      serverTarget: 'https://fake-as.test',
    });
    const { fetchImpl, seen } = fakeRefreshAs('fresh-token');

    const r = await resolveConnection({
      projectDir: dir,
      processEnv: {},
      machineAuth: { ...auth, fetchImpl, clock: () => now },
    });

    // The token handed to the connection is the FRESH one, never the expired one.
    expect(r.token).toBe('fresh-token');

    // The refresh hit the credential's serverTarget with the 04 §3 wire shape:
    // grant_type + refresh_token + the family's STORED clientId, scope omitted.
    expect(seen).toHaveLength(1);
    expect(seen[0].url).toBe('https://fake-as.test/oauth/token');
    expect(seen[0].grantType).toBe('refresh_token');
    expect(seen[0].refreshToken).toBe('refresh-1');
    expect(seen[0].clientId).toBe('unreal-mcp-cli');
    expect(seen[0].scope).toBeNull();

    // The rotation was written back to the store (peers adopt it, no re-refresh).
    const doc = new MachineCredentialStore(storeDir, identityCredentialCodec).read();
    const plugin = effectiveFamilies(doc!).plugin;
    expect(plugin?.accessToken).toBe('fresh-token');
    expect(plugin?.refreshToken).toBe('rotated-refresh');
  });

  it('a still-valid token is served WITHOUT a network round-trip', async () => {
    const dir = makeProject(CLOUD_CONFIG);
    const now = 1_000_000_000_000;
    const { auth } = seedMachineStore({
      accessToken: 'valid-token',
      refreshToken: 'refresh-1',
      expiresAt: new Date(now + 3_600_000).toISOString(),
    });
    const { fetchImpl, seen } = fakeRefreshAs('never-used');
    const r = await resolveConnection({
      projectDir: dir,
      processEnv: {},
      machineAuth: { ...auth, fetchImpl, clock: () => now },
    });
    expect(r.token).toBe('valid-token');
    expect(seen).toHaveLength(0);
  });
});
