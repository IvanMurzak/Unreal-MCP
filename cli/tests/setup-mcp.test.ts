import { describe, it, expect, afterEach } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import { setupMcp, listAgentIds, buildServerEntry } from '../src/lib/setup-mcp.js';
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

describe('listAgentIds', () => {
  it('includes the known agents', () => {
    expect(listAgentIds()).toEqual(expect.arrayContaining(['claude-code', 'cursor', 'vscode']));
  });
});

describe('buildServerEntry', () => {
  it('builds an http entry with bearer header', () => {
    const e = buildServerEntry('http', 'http://localhost:5220', 'tok');
    expect(e).toMatchObject({ type: 'http', url: 'http://localhost:5220/mcp', headers: { Authorization: 'Bearer tok' } });
  });
  it('builds a stdio entry launching the absolute server binary with port + auth args', () => {
    const e = buildServerEntry('stdio', 'http://x', 'tok', {
      serverPath: '/abs/Intermediate/UnrealMCP/server/win-x64/gamedev-mcp-server.exe',
      port: 5220,
    });
    expect(e).toMatchObject({
      type: 'stdio',
      command: '/abs/Intermediate/UnrealMCP/server/win-x64/gamedev-mcp-server.exe',
    });
    expect(e.args).toEqual(
      expect.arrayContaining(['port=5220', 'client-transport=stdio', 'authorization=required', 'token=tok']),
    );
  });

  it('stdio uses authorization=none when there is no token', () => {
    const e = buildServerEntry('stdio', 'http://x', undefined, { serverPath: '/abs/srv', port: 1 });
    expect(e.args).toEqual(expect.arrayContaining(['authorization=none', 'token=']));
  });

  it('stdio without resolved server params throws (a bare PATH command would not launch)', () => {
    expect(() => buildServerEntry('stdio', 'http://x', undefined)).toThrow();
  });
});

describe('setupMcp', () => {
  it('writes .mcp.json for claude-code and merges with existing servers', async () => {
    const dir = tmp();
    fs.writeFileSync(path.join(dir, '.mcp.json'), JSON.stringify({ mcpServers: { other: { type: 'stdio' } } }), 'utf-8');
    const r = await setupMcp({ agentId: 'claude-code', projectDir: dir, transport: 'http', url: 'http://localhost:5220' });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    const written = JSON.parse(fs.readFileSync(r.configPath, 'utf-8'));
    expect(written.mcpServers.other).toBeDefined();
    expect(written.mcpServers['unreal-mcp'].url).toBe('http://localhost:5220/mcp');
  });

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
    expect(entry.type).toBe('stdio');
    expect(path.isAbsolute(entry.command)).toBe(true);
    expect(entry.command).toContain(path.join('Intermediate', 'UnrealMCP', 'server'));
    expect(entry.command).toMatch(/gamedev-mcp-server(\.exe)?$/);
    expect(entry.args.some((a: string) => /^port=\d+$/.test(a))).toBe(true);
  });

  it('stdio honors the UNREAL_MCP_SERVER_PATH override and skips the download', async () => {
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

  it('stdio degrades a failed download to a warning (config still written with the §6 path)', async () => {
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

  it('stdio dry-run does not download (side-effect-free)', async () => {
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
    expect(r.snippet).toContain('gamedev-mcp-server');
  });

  it('writes vscode config under the top-level `servers` key (not `mcpServers`)', async () => {
    const dir = tmp();
    const r = await setupMcp({ agentId: 'vscode', projectDir: dir, transport: 'http', url: 'http://localhost:5220' });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    const written = JSON.parse(fs.readFileSync(r.configPath, 'utf-8'));
    // VS Code's .vscode/mcp.json uses `servers`; `mcpServers` is ignored.
    expect(written.servers['unreal-mcp'].url).toBe('http://localhost:5220/mcp');
    expect(written.mcpServers).toBeUndefined();
  });

  it('dry-run returns the snippet without writing', async () => {
    const dir = tmp();
    const r = await setupMcp({ agentId: 'cursor', projectDir: dir, dryRun: true, url: 'http://h' });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(fs.existsSync(r.configPath)).toBe(false);
    expect(r.snippet).toContain('unreal-mcp');
  });

  it('fails for an unknown agent', async () => {
    const r = await setupMcp({ agentId: 'emacs', projectDir: tmp() });
    expect(r.kind).toBe('failure');
  });
});
