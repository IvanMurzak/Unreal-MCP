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
  it('builds a stdio entry launching unreal-mcp-server', () => {
    const e = buildServerEntry('stdio', 'http://x', undefined);
    expect(e).toMatchObject({ type: 'stdio', command: 'unreal-mcp-server' });
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
