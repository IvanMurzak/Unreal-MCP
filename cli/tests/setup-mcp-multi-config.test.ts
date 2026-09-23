import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import { setupMcp } from '../src/lib/setup-mcp.js';
import { getAgentById, getAgentConfigPaths } from '../src/utils/agents.js';
import { derivePinV2, ProjectKeyStore, type ProjectKeyResolver } from '@baizor/gamedev-cli-core';
import { makeTempDir, rmTempDir } from './helpers.js';

const SERVER_NAME = 'unreal-mcp';
// Global agent configs resolve under os.homedir() / APPDATA — point them at a temp dir so these tests
// never touch the developer's real ~/.gemini.
const HOME_KEYS = ['HOME', 'USERPROFILE', 'APPDATA', 'XDG_CONFIG_HOME'];

let home: string;
let project: string;
const saved: Record<string, string | undefined> = {};

beforeEach(() => {
  home = makeTempDir('unreal-multi-home-');
  project = makeTempDir('unreal-multi-proj-');
  for (const k of HOME_KEYS) {
    saved[k] = process.env[k];
    process.env[k] = home;
  }
});
afterEach(() => {
  for (const k of HOME_KEYS) {
    if (saved[k] === undefined) delete process.env[k];
    else process.env[k] = saved[k];
  }
  rmTempDir(home);
  rmTempDir(project);
});

/** setup-mcp against the hosted Cloud for `project`. */
const cloud = { transport: 'http' as const, url: 'https://ai-game.dev' };

const agA = () => path.join(home, '.gemini', 'config', 'mcp_config.json');
const agB = () => path.join(home, '.gemini', 'antigravity', 'mcp_config.json');
const pinned = () => `https://ai-game.dev/mcp/p/${derivePinV2(path.resolve(project))}`;
const readJson = (p: string) => JSON.parse(fs.readFileSync(p, 'utf-8'));
const writeJson = (p: string, v: unknown) => {
  fs.mkdirSync(path.dirname(p), { recursive: true });
  fs.writeFileSync(p, JSON.stringify(v, null, 2));
};

function keyResolver(key: string, revokePrevious?: () => Promise<string | undefined>): ProjectKeyResolver {
  return async (request) => ({ kind: 'ok', key, keyId: 'kid-new', pin: request.pin, source: 'minted', warnings: [], revokePrevious });
}

describe('Antigravity — two config file locations', () => {
  it('resolves both candidate files under the user profile, and displays both', () => {
    const agent = getAgentById('antigravity')!;
    expect(getAgentConfigPaths(agent, project)).toEqual([agA(), agB()]);
    expect(agent.getConfigPath(project)).toBe(agA());
    expect(agent.configPathDisplay).toContain('~/.gemini/config/mcp_config.json');
    expect(agent.configPathDisplay).toContain('~/.gemini/antigravity/mcp_config.json');
  });

  it('writes the entry into BOTH files (creating them) and reports both paths', async () => {
    const r = await setupMcp({ agentId: 'antigravity', projectDir: project, ...cloud, projectKeyResolver: keyResolver('agd_pk_k1') });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.configPaths).toEqual([agA(), agB()]);
    expect(r.configPath).toBe(agA());
    for (const p of [agA(), agB()]) {
      expect(readJson(p).mcpServers[SERVER_NAME], p).toEqual({
        disabled: false,
        serverUrl: pinned(),
        headers: { Authorization: 'Bearer agd_pk_k1' },
      });
    }
  });

  it('preserves every other entry and field in each file', async () => {
    writeJson(agA(), { theme: 'dark', mcpServers: { other: { command: 'x' } } });
    writeJson(agB(), { mcpServers: { another: { serverUrl: 'https://z' }, [SERVER_NAME]: { serverUrl: 'https://stale', custom: 1 } } });
    const r = await setupMcp({ agentId: 'antigravity', projectDir: project, ...cloud, oauth: true });
    expect(r.kind).toBe('success');
    const a = readJson(agA());
    expect(a.theme).toBe('dark');
    expect(a.mcpServers.other).toEqual({ command: 'x' });
    expect(a.mcpServers[SERVER_NAME].serverUrl).toBe(pinned());
    const b = readJson(agB());
    expect(b.mcpServers.another).toEqual({ serverUrl: 'https://z' });
    expect(b.mcpServers[SERVER_NAME]).toEqual({ serverUrl: pinned(), custom: 1, disabled: false });
  });

  it('a failure writing one file is reported by path and fails the run; the other file is still written', async () => {
    // A directory where file B belongs makes its write fail (EISDIR) on every platform.
    fs.mkdirSync(agB(), { recursive: true });
    const r = await setupMcp({ agentId: 'antigravity', projectDir: project, ...cloud, oauth: true });
    expect(r.kind).toBe('failure');
    if (r.kind !== 'failure') return;
    expect(r.error.message).toContain(agB());
    expect(r.error.message).toContain(`Written: ${agA()}`);
    expect(readJson(agA()).mcpServers[SERVER_NAME].serverUrl).toBe(pinned());
  });

  it('a stdio config is written into both files too', async () => {
    const r = await setupMcp({
      agentId: 'antigravity', projectDir: project, transport: 'stdio', env: {},
      downloadServerImpl: async () => ({ kind: 'success', success: true, serverPath: path.join(project, 'server'), source: 'download', version: '8.0.0', warnings: [] }),
    });
    expect(r.kind).toBe('success');
    for (const p of [agA(), agB()]) {
      expect(readJson(p).mcpServers[SERVER_NAME], p).toMatchObject({ disabled: false, command: path.join(project, 'server') });
    }
  });

  it('a dry run returns a snippet per file and writes neither', async () => {
    const r = await setupMcp({ agentId: 'antigravity', projectDir: project, ...cloud, oauth: true, dryRun: true });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.snippets.map((s) => s.path)).toEqual([agA(), agB()]);
    for (const { content } of r.snippets) expect(JSON.parse(content).mcpServers[SERVER_NAME].serverUrl).toBe(pinned());
    expect(fs.existsSync(agA())).toBe(false);
    expect(fs.existsSync(agB())).toBe(false);
  });

  it('other agents still write exactly one file', async () => {
    const r = await setupMcp({ agentId: 'cursor', projectDir: project, ...cloud, oauth: true });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.configPaths).toEqual([path.join(project, '.cursor', 'mcp.json')]);
  });
});

describe('--regenerate-key carries the new key into the project\'s other agent configs', () => {
  const OLD = 'agd_pk_old';
  const NEW = 'agd_pk_new';

  function seedOtherConfigs() {
    const cursor = path.join(project, '.cursor', 'mcp.json');
    writeJson(cursor, { mcpServers: { keep: { url: 'u' }, [SERVER_NAME]: { type: 'http', url: pinned(), headers: { Authorization: `Bearer ${OLD}`, 'X-Other': 'v' } } } });
    writeJson(agA(), { mcpServers: { [SERVER_NAME]: { disabled: false, serverUrl: pinned(), headers: { Authorization: `Bearer ${OLD}` } } } });
    writeJson(agB(), { mcpServers: { [SERVER_NAME]: { disabled: false, serverUrl: pinned(), headers: { Authorization: `Bearer ${OLD}` } } } });
    const codex = path.join(project, '.codex', 'config.toml');
    fs.mkdirSync(path.dirname(codex), { recursive: true });
    fs.writeFileSync(
      codex,
      `[other]\nx = 1\n\n[mcp_servers.${SERVER_NAME}]\nenabled = true\nurl = "${pinned()}"\nhttp_headers = { Authorization = "Bearer ${OLD}" }\n`,
    );
    // Holds the old key on the UNPINNED url (a `--no-pin` config) — revoking would break it too: rewritten.
    const vscode = path.join(project, '.vscode', 'mcp.json');
    writeJson(vscode, { servers: { [SERVER_NAME]: { type: 'http', url: 'https://ai-game.dev/mcp', headers: { Authorization: `Bearer ${OLD}` } } } });
    // This project's pinned entry with a DIFFERENT key and no trace of the old one: untouched.
    const gemini = path.join(project, '.gemini', 'settings.json');
    writeJson(gemini, { mcpServers: { other: { url: 'https://z' }, [SERVER_NAME]: { type: 'http', url: pinned(), headers: { Authorization: 'Bearer agd_pk_other' } } } });
    return { cursor, codex, vscode, gemini };
  }

  it('rewrites every other config holding the old key (pinned or unpinned URL), BEFORE revoking', async () => {
    const { cursor, codex, vscode, gemini } = seedOtherConfigs();
    const geminiBefore = fs.readFileSync(gemini, 'utf-8');
    let revoked = false;
    const atRevoke: string[] = [];
    const lookups: [string, string][] = [];
    const r = await setupMcp({
      agentId: 'claude-code',
      projectDir: project, ...cloud,
      regenerateKey: true,
      previousProjectKey: (issuer, pin) => {
        lookups.push([issuer, pin]);
        return OLD;
      },
      projectKeyResolver: keyResolver(NEW, async () => {
        revoked = true;
        for (const p of [cursor, agA(), agB(), codex, vscode]) {
          if (fs.readFileSync(p, 'utf-8').includes(OLD)) atRevoke.push(p);
        }
        return undefined;
      }),
    });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(lookups).toEqual([['https://ai-game.dev', derivePinV2(path.resolve(project))]]);
    expect(revoked).toBe(true); // the revoke RAN — so the check below is not satisfied by a skipped revoke
    expect(atRevoke).toEqual([]); // nothing still held the old key when it was revoked
    expect([...r.rewrittenConfigPaths].sort()).toEqual([cursor, agA(), agB(), codex, vscode].map((p) => path.resolve(p)).sort());
    const c = readJson(cursor).mcpServers;
    expect(c[SERVER_NAME].headers).toEqual({ Authorization: `Bearer ${NEW}`, 'X-Other': 'v' });
    expect(c.keep).toEqual({ url: 'u' });
    expect(readJson(agA()).mcpServers[SERVER_NAME].headers.Authorization).toBe(`Bearer ${NEW}`);
    expect(readJson(agB()).mcpServers[SERVER_NAME].headers.Authorization).toBe(`Bearer ${NEW}`);
    const toml = fs.readFileSync(codex, 'utf-8');
    expect(toml).toContain(`http_headers = { Authorization = "Bearer ${NEW}" }`);
    expect(toml).toContain('[other]\nx = 1');
    expect(readJson(vscode).servers[SERVER_NAME]).toEqual({ type: 'http', url: 'https://ai-game.dev/mcp', headers: { Authorization: `Bearer ${NEW}` } });
    expect(fs.readFileSync(gemini, 'utf-8')).toBe(geminiBefore);
    expect(readJson(path.join(project, '.mcp.json')).mcpServers[SERVER_NAME].headers.Authorization).toBe(`Bearer ${NEW}`);
  });

  it('skips the revoke and names the config when one holding the old key cannot be rewritten', async () => {
    seedOtherConfigs();
    const broken = path.join(project, '.kilocode', 'mcp.json');
    fs.mkdirSync(path.dirname(broken), { recursive: true });
    fs.writeFileSync(broken, `{ "mcpServers": { "${SERVER_NAME}": { "url": "${pinned()}", "headers": { "Authorization": "Bearer ${OLD}" } } `); // truncated JSON
    let revoked = false;
    const r = await setupMcp({
      agentId: 'claude-code',
      projectDir: project, ...cloud,
      regenerateKey: true,
      previousProjectKey: () => OLD,
      projectKeyResolver: keyResolver(NEW, async () => {
        revoked = true;
        return undefined;
      }),
    });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(revoked).toBe(false);
    expect(r.warnings.join('\n')).toContain(path.resolve(broken));
    expect(r.warnings.join('\n')).toContain('NOT revoked');
    // The configs that could be rewritten still were.
    expect(readJson(agA()).mcpServers[SERVER_NAME].headers.Authorization).toBe(`Bearer ${NEW}`);
  });

  it('withholds the revoke when a config holds the old key outside its unreal-mcp header', async () => {
    seedOtherConfigs();
    // A renamed / hand-copied entry still using the old key would break on revoke — it is not ours to rewrite.
    const foreign = path.join(project, '.gemini', 'settings.json');
    writeJson(foreign, { mcpServers: { 'unreal-copy': { url: pinned(), headers: { Authorization: `Bearer ${OLD}` } } } });
    const before = fs.readFileSync(foreign, 'utf-8');
    let revoked = false;
    const r = await setupMcp({
      agentId: 'claude-code',
      projectDir: project, ...cloud,
      regenerateKey: true,
      previousProjectKey: () => OLD,
      projectKeyResolver: keyResolver(NEW, async () => {
        revoked = true;
        return undefined;
      }),
    });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(revoked).toBe(false);
    expect(r.warnings.join('\n')).toContain(path.resolve(foreign));
    expect(fs.readFileSync(foreign, 'utf-8')).toBe(before);
    // The configs that could be rewritten still were.
    expect(readJson(agA()).mcpServers[SERVER_NAME].headers.Authorization).toBe(`Bearer ${NEW}`);
  });

  it('writes a new key containing `$` replacement patterns into a TOML config verbatim', async () => {
    const { codex } = seedOtherConfigs();
    const dollarKey = "agd_pk_a$&b$'c$$d";
    const r = await setupMcp({
      agentId: 'claude-code',
      projectDir: project, ...cloud,
      regenerateKey: true,
      previousProjectKey: () => OLD,
      projectKeyResolver: keyResolver(dollarKey, async () => undefined),
    });
    expect(r.kind).toBe('success');
    expect(fs.readFileSync(codex, 'utf-8')).toContain(`http_headers = { Authorization = "Bearer ${dollarKey}" }`);
  });

  it('rewrites a config that starts with a UTF-8 BOM', async () => {
    const vs = path.join(project, '.vs', 'mcp.json');
    fs.mkdirSync(path.dirname(vs), { recursive: true });
    fs.writeFileSync(vs, '\uFEFF' + JSON.stringify({ servers: { [SERVER_NAME]: { type: 'http', url: pinned(), headers: { Authorization: `Bearer ${OLD}` } } } }));
    let revoked = false;
    const r = await setupMcp({
      agentId: 'claude-code',
      projectDir: project, ...cloud,
      regenerateKey: true,
      previousProjectKey: () => OLD,
      projectKeyResolver: keyResolver(NEW, async () => {
        revoked = true;
        return undefined;
      }),
    });
    expect(r.kind).toBe('success');
    expect(revoked).toBe(true);
    expect(readJson(vs).servers[SERVER_NAME].headers.Authorization).toBe(`Bearer ${NEW}`);
  });

  it('by default reads the key being replaced from the machine project-key cache', async () => {
    const { cursor } = seedOtherConfigs();
    const pin = derivePinV2(path.resolve(project));
    // HOME/USERPROFILE point at the temp home, so this is the cache the default lookup reads.
    new ProjectKeyStore(path.join(home, '.ai-game-dev')).put({
      key: OLD, keyId: 'kid-old', pin, issuer: 'https://ai-game.dev', sub: 'u1', engine: 'unreal', createdAt: '2026-09-23T00:00:00Z',
    });
    const r = await setupMcp({
      agentId: 'claude-code',
      projectDir: project, ...cloud,
      regenerateKey: true,
      projectKeyResolver: keyResolver(NEW, async () => undefined),
    });
    expect(r.kind).toBe('success');
    expect(readJson(cursor).mcpServers[SERVER_NAME].headers.Authorization).toBe(`Bearer ${NEW}`);
  });

  it('touches nothing else when the regenerate does not revoke a previous key', async () => {
    const { cursor } = seedOtherConfigs();
    const r = await setupMcp({
      agentId: 'claude-code',
      projectDir: project, ...cloud,
      regenerateKey: true,
      previousProjectKey: () => OLD,
      projectKeyResolver: keyResolver(NEW), // no revokePrevious: nothing is revoked, so nothing breaks
    });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.rewrittenConfigPaths).toEqual([]);
    expect(readJson(cursor).mcpServers[SERVER_NAME].headers.Authorization).toBe(`Bearer ${OLD}`);
  });
});
