import { describe, it, expect, afterEach } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import { setupMcp, listAgentIds, shouldWriteAuthHeader } from '../src/lib/setup-mcp.js';
import { agentRegistry, getAgentById, getAgentIds, MCP_SERVER_NAME } from '../src/utils/agents.js';
import { derivePinV2 } from '@baizor/gamedev-cli-core';
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

// The full pinned roster (parity with unity/godot). Order-independent.
const EXPECTED_IDS = [
  'claude-code',
  'claude-desktop',
  'cursor',
  'vscode-copilot',
  'vs-copilot',
  'rider-junie',
  'github-copilot-cli',
  'gemini',
  'antigravity',
  'cline',
  'open-code',
  'codex',
  'kilo-code',
  'custom',
];

describe('agent roster', () => {
  it('exposes exactly the 14 pinned agents', () => {
    expect(getAgentIds()).toHaveLength(14);
    expect([...getAgentIds()].sort()).toEqual([...EXPECTED_IDS].sort());
  });

  it('listAgentIds() mirrors the registry', () => {
    expect([...listAgentIds()].sort()).toEqual([...getAgentIds()].sort());
  });

  it('uses id `vscode-copilot` (not `vscode`), includes `custom`, omits `unity-ai`', () => {
    expect(getAgentIds()).toContain('vscode-copilot');
    expect(getAgentIds()).not.toContain('vscode');
    expect(getAgentIds()).toContain('custom');
    expect(getAgentIds()).not.toContain('unity-ai');
  });

  it('every agent carries the full closure surface', () => {
    for (const a of agentRegistry) {
      expect(typeof a.getConfigPath).toBe('function');
      expect(typeof a.getStdioProps).toBe('function');
      expect(typeof a.getHttpProps).toBe('function');
      expect(['json', 'toml']).toContain(a.configFormat);
      expect(typeof a.bodyPath).toBe('string');
      expect(a.bodyPath.length).toBeGreaterThan(0);
      expect(typeof a.supportsOAuth).toBe('boolean');
    }
  });

  it('every agent is OAuth-capable (supportsOAuth true — b6 / D11 default)', () => {
    for (const a of agentRegistry) {
      expect(a.supportsOAuth).toBe(true);
    }
  });

  it('the server entry key is `unreal-mcp`', () => {
    expect(MCP_SERVER_NAME).toBe('unreal-mcp');
  });

  it('exactly one agent (codex) uses the TOML format', () => {
    const toml = agentRegistry.filter((a) => a.configFormat === 'toml').map((a) => a.id);
    expect(toml).toEqual(['codex']);
  });
});

describe('per-agent server-entry shapes (pure closures)', () => {
  it('antigravity uses serverUrl + disabled (no url/type)', () => {
    const e = getAgentById('antigravity')!.getHttpProps('http://h/mcp', '', false);
    expect(e).toMatchObject({ disabled: false, serverUrl: 'http://h/mcp' });
    expect(e['url']).toBeUndefined();
    expect(e['type']).toBeUndefined();
  });

  it('cline uses type=streamableHttp', () => {
    expect(getAgentById('cline')!.getHttpProps('http://h/mcp', '', false)).toMatchObject({
      type: 'streamableHttp',
      url: 'http://h/mcp',
    });
  });

  it('kilo-code uses type=streamable-http + disabled', () => {
    expect(getAgentById('kilo-code')!.getHttpProps('http://h/mcp', '', false)).toMatchObject({
      type: 'streamable-http',
      disabled: false,
      url: 'http://h/mcp',
    });
  });

  it('github-copilot-cli always advertises tools:["*"]', () => {
    const e = getAgentById('github-copilot-cli')!.getHttpProps('http://h/mcp', 'tok', true);
    expect(e).toMatchObject({ type: 'http', url: 'http://h/mcp', tools: ['*'] });
    expect(e['headers']).toEqual({ Authorization: 'Bearer tok' });
  });

  it('rider-junie carries enabled:true on both transports', () => {
    expect(getAgentById('rider-junie')!.getHttpProps('http://h/mcp', '', false)).toMatchObject({ enabled: true });
    expect(getAgentById('rider-junie')!.getStdioProps('/srv', 5, 'none', '')).toMatchObject({ enabled: true });
  });

  it('open-code stdio uses an array `command`', () => {
    const e = getAgentById('open-code')!.getStdioProps('/srv', 5, 'none', '');
    expect(e).toMatchObject({ type: 'local', enabled: true });
    expect(e['command']).toEqual([
      '/srv',
      'port=5',
      'client-transport=stdio',
      'authorization=none',
      'token=',
    ]);
  });

  it('codex stdio omits the bearer token from the arg vector', () => {
    const e = getAgentById('codex')!.getStdioProps('/srv', 5, 'required', 'secret');
    expect(e['command']).toBe('/srv');
    expect(e['args']).toEqual(['port=5', 'client-transport=stdio', 'authorization=required']);
    expect((e['args'] as string[]).some((a) => a.startsWith('token='))).toBe(false);
  });
});

describe('setupMcp — http transport', () => {
  it('writes .mcp.json for claude-code and merges with existing servers', async () => {
    const dir = tmp();
    fs.writeFileSync(path.join(dir, '.mcp.json'), JSON.stringify({ mcpServers: { other: { type: 'stdio' } } }), 'utf-8');
    const r = await setupMcp({ agentId: 'claude-code', projectDir: dir, transport: 'http', url: 'http://localhost:5220' });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    const written = JSON.parse(fs.readFileSync(r.configPath, 'utf-8'));
    expect(written.mcpServers.other).toBeDefined();
    // Pinned by default (T4): `<base>/mcp/p/<pin-v2>`.
    expect(written.mcpServers['unreal-mcp'].url).toBe(`http://localhost:5220/mcp/p/${derivePinV2(dir)}`);
    expect(written.mcpServers['unreal-mcp'].type).toBe('http');
  });

  it('adds a bearer header when a token is EXPLICITLY supplied (PAT opt-in / Flow C)', async () => {
    const dir = tmp();
    const r = await setupMcp({ agentId: 'claude-code', projectDir: dir, transport: 'http', url: 'http://h', token: 'tok' });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    const written = JSON.parse(fs.readFileSync(r.configPath, 'utf-8'));
    expect(written.mcpServers['unreal-mcp'].url).toBe(`http://h/mcp/p/${derivePinV2(dir)}`);
    expect(written.mcpServers['unreal-mcp'].headers).toEqual({ Authorization: 'Bearer tok' });
  });

  it('appends `/mcp` exactly once (no double-append on a URL that already ends in /mcp)', async () => {
    const dir = tmp();
    const r = await setupMcp({ agentId: 'claude-code', projectDir: dir, transport: 'http', url: 'http://h/mcp' });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    const written = JSON.parse(fs.readFileSync(r.configPath, 'utf-8'));
    // Still no `/mcp/mcp` — the pin segment is appended once after the single `/mcp`.
    expect(written.mcpServers['unreal-mcp'].url).toBe(`http://h/mcp/p/${derivePinV2(dir)}`);
  });

  it('--no-pin writes an UNPINNED <base>/mcp URL (T4 escape hatch)', async () => {
    const dir = tmp();
    const r = await setupMcp({ agentId: 'claude-code', projectDir: dir, transport: 'http', url: 'http://localhost:5220', noPin: true });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    const written = JSON.parse(fs.readFileSync(r.configPath, 'utf-8'));
    expect(written.mcpServers['unreal-mcp'].url).toBe('http://localhost:5220/mcp');
  });

  it('writes vscode-copilot config under the top-level `servers` key (not `mcpServers`)', async () => {
    const dir = tmp();
    const r = await setupMcp({ agentId: 'vscode-copilot', projectDir: dir, transport: 'http', url: 'http://localhost:5220' });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    const written = JSON.parse(fs.readFileSync(r.configPath, 'utf-8'));
    expect(written.servers['unreal-mcp'].url).toBe(`http://localhost:5220/mcp/p/${derivePinV2(dir)}`);
    expect(written.mcpServers).toBeUndefined();
  });

  it('writes codex config as TOML under [mcp_servers.unreal-mcp]', async () => {
    const dir = tmp();
    const r = await setupMcp({ agentId: 'codex', projectDir: dir, transport: 'http', url: 'http://localhost:5220' });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.configPath).toMatch(/[\\/]\.codex[\\/]config\.toml$/);
    const content = fs.readFileSync(r.configPath, 'utf-8');
    expect(content).toContain('[mcp_servers.unreal-mcp]');
    expect(content).toContain(`url = "http://localhost:5220/mcp/p/${derivePinV2(dir)}"`);
    expect(content).toContain('tool_timeout_sec = 300');
  });

  it('dry-run returns the snippet without writing', async () => {
    const dir = tmp();
    const r = await setupMcp({ agentId: 'cursor', projectDir: dir, dryRun: true, url: 'http://h' });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(fs.existsSync(r.configPath)).toBe(false);
    expect(r.snippet).toContain('unreal-mcp');
    expect(r.snippet).toContain('http://h/mcp');
  });
});

describe('setupMcp — D11 credential-free OAuth config (http)', () => {
  // The flagship fix: OAuth-capable interactive clients must get a URL-only
  // `{type,url}` config with NO Authorization header, so the client performs its
  // own native RFC 9728 OAuth. A static Bearer header both 401s against the hosted
  // AS and suppresses the client's OAuth handshake.
  for (const agentId of ['claude-code', 'cursor', 'vscode-copilot']) {
    it(`writes URL-only ${agentId} config (no Authorization header) with no token`, async () => {
      const dir = tmp();
      const r = await setupMcp({ agentId, projectDir: dir, transport: 'http', url: 'https://ai-game.dev' });
      expect(r.kind).toBe('success');
      if (r.kind !== 'success') return;
      const written = JSON.parse(fs.readFileSync(r.configPath, 'utf-8'));
      const body = getAgentById(agentId)!.bodyPath;
      const entry = written[body]['unreal-mcp'];
      expect(entry.type).toBe('http');
      expect(entry.url).toBe(`https://ai-game.dev/mcp/p/${derivePinV2(dir)}`);
      expect(entry.headers).toBeUndefined();
    });
  }

  it('writes URL-only codex (TOML) config with no Authorization', async () => {
    const dir = tmp();
    const r = await setupMcp({ agentId: 'codex', projectDir: dir, transport: 'http', url: 'https://ai-game.dev' });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    const content = fs.readFileSync(r.configPath, 'utf-8');
    expect(content).toContain(`url = "https://ai-game.dev/mcp/p/${derivePinV2(dir)}"`);
    expect(content.toLowerCase()).not.toContain('authorization');
  });

  it('an AMBIENT token (project .env) does NOT inject a header for an OAuth client (the bug)', async () => {
    const dir = tmp();
    // Simulate a project whose .env carries a token — the exact pre-fix condition
    // that used to inject a static Bearer header and break the client's OAuth.
    fs.writeFileSync(
      path.join(dir, '.env'),
      'UNREAL_MCP_HOST=https://ai-game.dev\nUNREAL_MCP_TOKEN=ambient-pat\n',
      'utf-8',
    );
    const r = await setupMcp({ agentId: 'claude-code', projectDir: dir, transport: 'http' });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    const entry = JSON.parse(fs.readFileSync(r.configPath, 'utf-8')).mcpServers['unreal-mcp'];
    expect(entry.url).toBe(`https://ai-game.dev/mcp/p/${derivePinV2(dir)}`);
    expect(entry.headers).toBeUndefined();
  });

  it('a re-run over a config that still has a stale Authorization header strips it', async () => {
    const dir = tmp();
    // Pre-fix state left on disk by an older buggy run: a lingering Bearer header.
    fs.writeFileSync(
      path.join(dir, '.mcp.json'),
      JSON.stringify({
        mcpServers: { 'unreal-mcp': { type: 'http', url: 'https://ai-game.dev/mcp', headers: { Authorization: 'Bearer stale' } } },
      }),
      'utf-8',
    );
    const r = await setupMcp({ agentId: 'claude-code', projectDir: dir, transport: 'http', url: 'https://ai-game.dev' });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    const entry = JSON.parse(fs.readFileSync(r.configPath, 'utf-8')).mcpServers['unreal-mcp'];
    expect(entry.url).toBe(`https://ai-game.dev/mcp/p/${derivePinV2(dir)}`);
    expect(entry.headers).toBeUndefined();
  });
});

describe('shouldWriteAuthHeader (pure decision)', () => {
  it('OAuth-capable client with an ambient token → no header (URL-only)', () => {
    expect(shouldWriteAuthHeader({ token: 'ambient', supportsOAuth: true, explicitPatOptIn: false })).toBe(false);
  });
  it('OAuth-capable client with an EXPLICIT PAT opt-in → header (Flow C)', () => {
    expect(shouldWriteAuthHeader({ token: 'pat', supportsOAuth: true, explicitPatOptIn: true })).toBe(true);
  });
  it('non-OAuth client with a token → header (fallback), even without explicit opt-in', () => {
    expect(shouldWriteAuthHeader({ token: 'tok', supportsOAuth: false, explicitPatOptIn: false })).toBe(true);
  });
  it('no token → never a header, regardless of the other flags', () => {
    expect(shouldWriteAuthHeader({ token: '', supportsOAuth: false, explicitPatOptIn: true })).toBe(false);
    expect(shouldWriteAuthHeader({ token: '', supportsOAuth: true, explicitPatOptIn: false })).toBe(false);
  });
});

describe('setupMcp — stdio transport', () => {
  it('writes a stdio entry with an absolute §6 server path and a port arg (download injected)', async () => {
    const dir = tmp();
    const serverPath = path.join(dir, 'Intermediate', 'UnrealMCP', 'server', 'win-x64', 'gamedev-mcp-server.exe');
    const downloads: string[] = [];
    const r = await setupMcp({
      agentId: 'claude-code',
      projectDir: dir,
      transport: 'stdio',
      env: {},
      downloadServerImpl: async (o) => {
        downloads.push(o.projectDir);
        return { kind: 'success', success: true, serverPath, source: 'download', version: '8.0.0', warnings: [] };
      },
    });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(downloads).toEqual([dir]);
    const written = JSON.parse(fs.readFileSync(r.configPath, 'utf-8'));
    const entry = written.mcpServers['unreal-mcp'];
    expect(path.isAbsolute(entry.command)).toBe(true);
    expect(entry.command).toContain(path.join('Intermediate', 'UnrealMCP', 'server'));
    expect(entry.command).toMatch(/gamedev-mcp-server(\.exe)?$/);
    expect(entry.args).toContain('client-transport=stdio');
    expect(entry.args.some((a: string) => /^port=\d+$/.test(a))).toBe(true);
  });

  it('re-running stdio over a stale http entry drops the http-only keys', async () => {
    const dir = tmp();
    fs.writeFileSync(
      path.join(dir, '.mcp.json'),
      JSON.stringify({ mcpServers: { 'unreal-mcp': { type: 'http', url: 'http://old/mcp' } } }),
      'utf-8',
    );
    const serverPath = path.join(dir, 'Intermediate', 'UnrealMCP', 'server', 'win-x64', 'gamedev-mcp-server.exe');
    const r = await setupMcp({
      agentId: 'claude-code',
      projectDir: dir,
      transport: 'stdio',
      env: {},
      downloadServerImpl: async () => ({
        kind: 'success',
        success: true,
        serverPath,
        source: 'download',
        version: '8.0.0',
        warnings: [],
      }),
    });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    const entry = JSON.parse(fs.readFileSync(r.configPath, 'utf-8')).mcpServers['unreal-mcp'];
    expect(entry.command).toBeDefined();
    expect(entry.url).toBeUndefined();
    expect(entry.type).toBeUndefined();
  });

  it('honors the UNREAL_MCP_SERVER_PATH override and skips the download', async () => {
    const dir = tmp();
    const override = path.join(dir, 'local-gamedev-mcp-server.exe');
    fs.writeFileSync(override, 'x');
    let downloadCalled = false;
    const r = await setupMcp({
      agentId: 'claude-code',
      projectDir: dir,
      transport: 'stdio',
      env: { UNREAL_MCP_SERVER_PATH: override },
      downloadServerImpl: async () => {
        downloadCalled = true;
        throw new Error('must not be called');
      },
    });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(downloadCalled).toBe(false);
    const written = JSON.parse(fs.readFileSync(r.configPath, 'utf-8'));
    expect(written.mcpServers['unreal-mcp'].command).toBe(override);
  });

  it('degrades a failed download to a warning (config still written with the §6 path)', async () => {
    const dir = tmp();
    const r = await setupMcp({
      agentId: 'claude-code',
      projectDir: dir,
      transport: 'stdio',
      env: {},
      downloadServerImpl: async () => ({
        kind: 'failure',
        success: false,
        warnings: [],
        error: new Error('HTTP 404'),
      }),
    });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.warnings.some((w) => w.includes('HTTP 404'))).toBe(true);
    expect(r.nextSteps.some((s) => s.includes('UNREAL_MCP_SERVER_PATH'))).toBe(true);
    const written = JSON.parse(fs.readFileSync(r.configPath, 'utf-8'));
    expect(written.mcpServers['unreal-mcp'].command).toMatch(/gamedev-mcp-server(\.exe)?$/);
  });

  it('dry-run does not download (side-effect-free)', async () => {
    const dir = tmp();
    let downloadCalled = false;
    const r = await setupMcp({
      agentId: 'claude-code',
      projectDir: dir,
      transport: 'stdio',
      dryRun: true,
      env: {},
      downloadServerImpl: async () => {
        downloadCalled = true;
        throw new Error('must not be called');
      },
    });
    expect(r.kind).toBe('success');
    expect(downloadCalled).toBe(false);
    if (r.kind !== 'success') return;
    expect(fs.existsSync(r.configPath)).toBe(false);
    expect(r.snippet).toContain('gamedev-mcp-server');
  });
});

describe('setupMcp — errors', () => {
  it('fails for an unknown agent', async () => {
    const r = await setupMcp({ agentId: 'emacs', projectDir: tmp() });
    expect(r.kind).toBe('failure');
  });
});
