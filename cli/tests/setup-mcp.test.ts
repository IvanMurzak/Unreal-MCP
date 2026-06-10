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
      serverPath: '/abs/Intermediate/UnrealMCP/server/win-x64/unreal-mcp-server.exe',
      port: 5220,
    });
    expect(e).toMatchObject({
      type: 'stdio',
      command: '/abs/Intermediate/UnrealMCP/server/win-x64/unreal-mcp-server.exe',
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

  it('writes a stdio entry with an absolute §6 server path and a port arg', async () => {
    const dir = tmp();
    const r = await setupMcp({ agentId: 'claude-code', projectDir: dir, transport: 'stdio' });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    const written = JSON.parse(fs.readFileSync(r.configPath, 'utf-8'));
    const entry = written.mcpServers['unreal-mcp'];
    expect(entry.type).toBe('stdio');
    expect(path.isAbsolute(entry.command)).toBe(true);
    expect(entry.command).toContain(path.join('Intermediate', 'UnrealMCP', 'server'));
    expect(entry.command).toMatch(/unreal-mcp-server(\.exe)?$/);
    expect(entry.args.some((a: string) => /^port=\d+$/.test(a))).toBe(true);
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
