// Parity between unreal-mcp-cli and the shared `@baizor/gamedev-cli-core`
// (auth-fixes design T7 / g1 DoD). These assertions prove the CLI's migrated
// surfaces derive identical values to the golden-vector-gated core — the same
// contract the C# LIB + Editor Configure follow — so a config the CLI writes and
// a config the Editor writes route to the same engine instance.

import { describe, it, expect, afterEach } from 'vitest';
import * as fs from 'fs';
import { setupMcp } from '../src/lib/setup-mcp.js';
import { MCP_SERVER_NAME } from '../src/utils/agents.js';
import { deriveProjectPin, generatePortFromDirectory } from '../src/utils/port.js';
import { resolveServerBinaryPath } from '../src/lib/download-server.js';
import {
  resolveSetupMcpPlan,
  getAgentById as coreGetAgentById,
  unrealAdapter,
  derivePinV2,
  derivePortV2,
  derivePin,
  derivePort,
  pinUrl,
} from '@baizor/gamedev-cli-core';
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

describe('engine adapter parity (unrealAdapter)', () => {
  it('server-entry name matches the CLI registry and is `unreal-mcp`', () => {
    expect(MCP_SERVER_NAME).toBe(unrealAdapter.serverName);
    expect(unrealAdapter.serverName).toBe('unreal-mcp');
  });

  it('OAuth client id is `unreal-mcp-cli`', () => {
    expect(unrealAdapter.clientId).toBe('unreal-mcp-cli');
  });

  it('stdio transport is ON for Unreal', () => {
    expect(unrealAdapter.stdioSupported).toBe(true);
  });
});

describe('project-identity parity (local v1 == cli-core v1 — golden-vector gated)', () => {
  const roots = ['/home/dev/proj', 'C:/Users/dev/My Project', '/a/b/c/', 'C:\\Users\\dev\\proj'];
  it('local deriveProjectPin == core derivePin', () => {
    for (const r of roots) expect(deriveProjectPin(r)).toBe(derivePin(r));
  });
  it('local generatePortFromDirectory == core derivePort', () => {
    for (const r of roots) expect(generatePortFromDirectory(r)).toBe(derivePort(r));
  });
});

describe('server install-layout parity (§6 == cli-core unrealAdapter)', () => {
  it('resolves the same binary path per RID', () => {
    const project = tmp();
    for (const [platform, arch] of [
      ['win32', 'x64'],
      ['linux', 'x64'],
      ['darwin', 'arm64'],
    ] as [NodeJS.Platform, string][]) {
      expect(resolveServerBinaryPath(project, platform, arch)).toBe(
        unrealAdapter.serverBinaryPath(project, platform, arch),
      );
    }
  });
});

describe('pinned setup-mcp parity vs the shared Configure policy (T4 DoD)', () => {
  it('the http URL the CLI writes equals cli-core resolveSetupMcpPlan().resolvedUrl', async () => {
    const dir = tmp();
    const base = 'https://ai-game.dev';
    const r = await setupMcp({ agentId: 'claude-code', projectDir: dir, transport: 'http', url: base });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    const writtenUrl = JSON.parse(fs.readFileSync(r.configPath, 'utf-8')).mcpServers['unreal-mcp'].url;

    const pin = derivePinV2(dir);

    // (a) The URL uses cli-core's exact routing helper + v2 pin.
    expect(writtenUrl).toBe(pinUrl('https://ai-game.dev/mcp', pin));

    // (b) The URL matches cli-core's shared setup-mcp policy — the SAME plan the
    //     Editor Configure / sibling CLIs run (golden-vector-gated vs the C# LIB).
    const plan = resolveSetupMcpPlan({
      adapter: unrealAdapter,
      agent: coreGetAgentById('claude-code')!,
      transport: 'http',
      projectRoot: dir,
      pin,
      port: derivePortV2(dir),
      timeoutMs: 10000,
      authorization: 'none',
      noPin: false,
      url: 'https://ai-game.dev/mcp',
    });
    expect(writtenUrl).toBe(plan.resolvedUrl);
  });
});
