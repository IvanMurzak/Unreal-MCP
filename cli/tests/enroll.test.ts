import { describe, it, expect, afterEach } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import { enrollPlugin } from '../src/lib/enroll.js';
import { readProjectMarker } from '../src/utils/project-marker.js';
import {
  MachineCredentialStore,
  derivePinV2,
  effectiveFamilies,
  identityCredentialCodec,
} from '@baizor/gamedev-cli-core';
import { makeTempDir, rmTempDir, fakeResponse } from './helpers.js';

/** Open the machine store with the test codec (no DPAPI/PowerShell in tests). */
function openStore(baseDir: string): MachineCredentialStore {
  return new MachineCredentialStore(baseDir, identityCredentialCodec);
}

const dirs: string[] = [];
afterEach(() => {
  while (dirs.length) rmTempDir(dirs.pop()!);
});
function tmp(): string {
  const d = makeTempDir();
  dirs.push(d);
  return d;
}

interface RedeemBody {
  access_token: string;
  refresh_token?: string;
  expires_in?: number;
  scope?: string;
  server_url?: string;
  /** O5/a6 fields. */
  sub?: string;
  client_id?: string;
}

/** A fetch that records the redeem request and returns a fixed response. */
function redeemFetch(
  response: { ok: boolean; status: number; body: string },
  captured: { url?: string; method?: string; body?: unknown } = {},
): typeof fetch {
  return (async (url: string, init: RequestInit) => {
    captured.url = String(url);
    captured.method = init?.method;
    captured.body = init?.body ? JSON.parse(String(init.body)) : undefined;
    return fakeResponse(response);
  }) as unknown as typeof fetch;
}

function ok(body: RedeemBody): { ok: true; status: 200; body: string } {
  return { ok: true, status: 200, body: JSON.stringify(body) };
}

describe('enrollPlugin', () => {
  it('redeems and persists credential to the shared machine store + writes the marker + pin', async () => {
    const store = tmp();
    const project = tmp();
    // A pre-existing manually-added Claude Code config (workflow 1A step 1).
    fs.writeFileSync(
      path.join(project, '.mcp.json'),
      JSON.stringify({ mcpServers: { 'ai-game-developer': { type: 'http', url: 'https://ai-game.dev/mcp' } } }, null, 2),
    );

    const captured: { url?: string; method?: string; body?: unknown } = {};
    const r = await enrollPlugin({
      enrollCode: 'ABCD-1234',
      projectDir: project,
      storeBaseDir: store,
      storeCodec: identityCredentialCodec,
      nowImpl: () => 1_000_000,
      fetchImpl: redeemFetch(
        ok({
          access_token: 'plugin-jwt',
          refresh_token: 'refresh-1',
          expires_in: 3600,
          scope: 'mcp:plugin',
          server_url: 'https://ai-game.dev',
          sub: 'user-1',
          client_id: 'enroll-client-1',
        }),
        captured,
      ),
    });

    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;

    // Request shape: POST /api/auth/enroll/redeem with { enroll_code }.
    expect(captured.url).toBe('https://ai-game.dev/api/auth/enroll/redeem');
    expect(captured.method).toBe('POST');
    expect(captured.body).toEqual({ enroll_code: 'ABCD-1234' });

    // Credential landed in the machine store (NOT a project .env) as the v2
    // PLUGIN family — enroll is the tools-only mint path (F10) — with the v1
    // compat mirror at top level for old readers.
    const stored = openStore(store).read();
    expect(stored?.accessToken).toBe('plugin-jwt');
    expect(stored?.refreshToken).toBe('refresh-1');
    expect(stored?.serverTarget).toBe('https://ai-game.dev');
    expect(stored?.expiresAt).toBe(new Date(1_000_000 + 3600 * 1000).toISOString());
    expect(stored?.subject).toBe('user-1');
    const families = effectiveFamilies(stored!);
    expect(families.plugin?.accessToken).toBe('plugin-jwt');
    // O5/a6: the redeem response's mint client id is stored VERBATIM, so refresh
    // presents the RIGHT id (04 §3 rule 2).
    expect(families.plugin?.clientId).toBe('enroll-client-1');
    expect(families.agent).toBeUndefined();
    expect(fs.existsSync(path.join(project, '.env'))).toBe(false);

    // Marker recorded the server target.
    expect(readProjectMarker(project)).toEqual({ serverTarget: 'https://ai-game.dev' });

    // Pin upserted into the existing project-local config (cli-core v2 identity — B5 fix).
    const pin = derivePinV2(project);
    expect(r.pin).toBe(pin);
    expect(r.pinnedConfigFiles).toContain(path.join(project, '.mcp.json'));
    const parsed = JSON.parse(fs.readFileSync(path.join(project, '.mcp.json'), 'utf-8'));
    expect(parsed.mcpServers['ai-game-developer'].url).toBe(`https://ai-game.dev/mcp/p/${pin}`);
  });

  it('records a LOCAL server target when the code was minted for a localhost RS', async () => {
    const store = tmp();
    const project = tmp();
    const r = await enrollPlugin({
      enrollCode: 'code',
      projectDir: project,
      storeBaseDir: store,
      storeCodec: identityCredentialCodec,
      fetchImpl: redeemFetch(ok({ access_token: 't', server_url: 'http://localhost:24567' })),
    });
    expect(r.kind).toBe('success');
    expect(readProjectMarker(project)).toEqual({ serverTarget: 'http://localhost:24567' });
  });

  it('surfaces a clean, uniform error for a spent/invalid code (no oracle)', async () => {
    const store = tmp();
    const project = tmp();
    const r = await enrollPlugin({
      enrollCode: 'spent',
      projectDir: project,
      storeBaseDir: store,
      storeCodec: identityCredentialCodec,
      fetchImpl: redeemFetch({
        ok: false,
        status: 400,
        body: JSON.stringify({ error: 'invalid_grant', error_description: 'invalid code' }),
      }),
    });
    expect(r.kind).toBe('failure');
    if (r.kind === 'failure') {
      expect(r.reason).toBe('invalid_code');
      expect(r.error.message).toMatch(/single-use|invalid|expired/i);
    }
    // Nothing was persisted on failure.
    expect(openStore(store).exists).toBe(false);
    expect(readProjectMarker(project)).toBeNull();
  });

  it('declines a subject-mismatch redeem fail-closed (D6/F7): store + marker untouched', async () => {
    const store = tmp();
    const project = tmp();
    // Machine already authorized as user-A.
    openStore(store).write({
      version: 2,
      serverTarget: 'https://ai-game.dev',
      subject: 'user-A',
      families: { plugin: { accessToken: 'old-plugin', clientId: 'unreal-mcp-cli', scope: 'mcp:plugin' } },
    });
    const r = await enrollPlugin({
      enrollCode: 'code-for-user-B',
      projectDir: project,
      storeBaseDir: store,
      storeCodec: identityCredentialCodec,
      // No confirmAccountSwitch (no --yes) ⇒ the mismatch must be declined.
      fetchImpl: redeemFetch(ok({ access_token: 'new-plugin', server_url: 'https://ai-game.dev', sub: 'user-B' })),
    });
    expect(r.kind).toBe('failure');
    if (r.kind === 'failure') expect(r.reason).toBe('account-switch-declined');
    // Store still user-A's; the marker + pin were never written (B1).
    expect(openStore(store).read()?.subject).toBe('user-A');
    expect(readProjectMarker(project)).toBeNull();
  });

  it('surfaces a distinct signing-unavailable error on 503', async () => {
    const r = await enrollPlugin({
      enrollCode: 'code',
      projectDir: tmp(),
      storeBaseDir: tmp(),
      fetchImpl: redeemFetch({ ok: false, status: 503, body: '{}' }),
    });
    expect(r.kind).toBe('failure');
    if (r.kind === 'failure') expect(r.reason).toBe('signing_unavailable');
  });

  it('rejects a blank enrollment code without hitting the network', async () => {
    let called = false;
    const r = await enrollPlugin({
      enrollCode: '   ',
      projectDir: tmp(),
      storeBaseDir: tmp(),
      fetchImpl: (async () => {
        called = true;
        return fakeResponse({ ok: true, status: 200, body: '{}' });
      }) as unknown as typeof fetch,
    });
    expect(r.kind).toBe('failure');
    expect(called).toBe(false);
  });
});
