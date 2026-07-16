import { describe, it, expect } from 'vitest';
import * as path from 'path';
import { configureAgent } from '../src/lib/configure-agent.js';
import { SERVER_PATH_ENV_VAR } from '../src/lib/download-server.js';
import type { DownloadServerResult } from '../src/lib/types.js';

function okDownload(serverPath: string): () => Promise<DownloadServerResult> {
  return async () =>
    ({ kind: 'success', success: true, serverPath, source: 'download', version: '9.1.0', warnings: [] }) as DownloadServerResult;
}

describe('configureAgent (proxy to gamedev-mcp-server configure)', () => {
  it('spawns the managed server binary with the right arg vector (stdio default)', async () => {
    const projectDir = path.resolve('/tmp/my-unreal-proj');
    let spawned: { bin?: string; args?: string[] } = {};
    const r = await configureAgent({
      agentId: 'claude-code',
      projectDir,
      env: {},
      downloadServerImpl: okDownload('/managed/gamedev-mcp-server.exe'),
      spawnImpl: async (bin, args) => {
        spawned = { bin, args };
        return { exitCode: 0 };
      },
    });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(spawned.bin).toBe('/managed/gamedev-mcp-server.exe');
    expect(spawned.args).toEqual([
      'configure',
      '--agent',
      'claude-code',
      '--transport',
      'stdio',
      '--project',
      projectDir,
    ]);
    expect(r.serverPath).toBe('/managed/gamedev-mcp-server.exe');
  });

  it('forwards --url and an explicit --transport http', async () => {
    let args: string[] = [];
    await configureAgent({
      agentId: 'cursor',
      projectDir: path.resolve('/tmp/p'),
      url: 'https://ai-game.dev',
      transport: 'http',
      env: {},
      downloadServerImpl: okDownload('/managed/bin'),
      spawnImpl: async (_bin, a) => {
        args = a;
        return { exitCode: 0 };
      },
    });
    expect(args).toContain('--url');
    expect(args[args.indexOf('--url') + 1]).toBe('https://ai-game.dev');
    expect(args[args.indexOf('--transport') + 1]).toBe('http');
  });

  it('prefers the UNREAL_MCP_SERVER_PATH override and does NOT download', async () => {
    const overrideBin = makeExistingBinary();
    let downloaded = false;
    let usedBin = '';
    const r = await configureAgent({
      agentId: 'claude-code',
      projectDir: path.resolve('/tmp/p'),
      env: { [SERVER_PATH_ENV_VAR]: overrideBin },
      downloadServerImpl: async () => {
        downloaded = true;
        return okDownload('/should/not/be/used')();
      },
      spawnImpl: async (bin) => {
        usedBin = bin;
        return { exitCode: 0 };
      },
    });
    expect(r.kind).toBe('success');
    expect(downloaded).toBe(false);
    expect(usedBin).toBe(overrideBin);
  });

  it('fails when the server binary cannot be acquired', async () => {
    const r = await configureAgent({
      agentId: 'claude-code',
      projectDir: path.resolve('/tmp/p'),
      env: {},
      downloadServerImpl: async () =>
        ({ kind: 'failure', success: false, warnings: [], error: new Error('offline') }) as DownloadServerResult,
      spawnImpl: async () => ({ exitCode: 0 }),
    });
    expect(r.kind).toBe('failure');
    if (r.kind === 'failure') expect(r.error.message).toMatch(/could not acquire/i);
  });

  it('fails when the server configure subcommand exits non-zero', async () => {
    const r = await configureAgent({
      agentId: 'unknown-agent',
      projectDir: path.resolve('/tmp/p'),
      env: {},
      downloadServerImpl: okDownload('/managed/bin'),
      spawnImpl: async () => ({ exitCode: 2 }),
    });
    expect(r.kind).toBe('failure');
    if (r.kind === 'failure') expect(r.error.message).toMatch(/exited with code 2/i);
  });

  it('rejects a blank agent id', async () => {
    const r = await configureAgent({
      agentId: '   ',
      env: {},
      downloadServerImpl: okDownload('/managed/bin'),
      spawnImpl: async () => ({ exitCode: 0 }),
    });
    expect(r.kind).toBe('failure');
  });
});

// A real existing file so `resolveServerOverride` accepts the override path.
import * as fs from 'fs';
import * as os from 'os';
function makeExistingBinary(): string {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'unreal-mcp-cfgagent-'));
  const p = path.join(dir, 'gamedev-mcp-server.exe');
  fs.writeFileSync(p, 'x');
  return p;
}
