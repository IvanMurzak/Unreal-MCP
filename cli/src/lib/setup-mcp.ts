// `setup-mcp` — write an MCP client config snippet for a supported agent
// so it can reach the project's local Unreal MCP server. Library-safe.
//
// Each agent has a project-relative config file and a merge strategy that
// preserves any existing `mcpServers` entries. The snippet shape follows
// the MCP client convention shared by the Unity/Godot CLIs.

import * as fs from 'fs';
import * as path from 'path';
import { resolveConnection } from '../utils/config.js';
import { emitProgress } from './progress.js';
import type { McpTransport, SetupMcpOptions, SetupMcpResult } from './types.js';

const SERVER_KEY = 'unreal-mcp';

interface AgentDef {
  id: string;
  /** Config file path relative to the project dir. */
  relConfigPath: string;
  /** Display label. */
  label: string;
}

const AGENTS: AgentDef[] = [
  { id: 'claude-code', relConfigPath: '.mcp.json', label: 'Claude Code' },
  { id: 'cursor', relConfigPath: path.join('.cursor', 'mcp.json'), label: 'Cursor' },
  { id: 'vscode', relConfigPath: path.join('.vscode', 'mcp.json'), label: 'VS Code' },
];

/** Valid agent ids accepted by `setupMcp`. Pure. */
export function listAgentIds(): string[] {
  return AGENTS.map((a) => a.id);
}

/**
 * Build the `mcpServers` entry for the given transport. HTTP points the
 * client at the running server; stdio launches the local `unreal-mcp-server`
 * binary on demand. Pure — exported for tests.
 */
export function buildServerEntry(
  transport: McpTransport,
  url: string,
  token: string | undefined,
): Record<string, unknown> {
  if (transport === 'http') {
    const entry: Record<string, unknown> = { type: 'http', url: `${url}/mcp` };
    if (token) entry['headers'] = { Authorization: `Bearer ${token}` };
    return entry;
  }
  // stdio
  const entry: Record<string, unknown> = {
    type: 'stdio',
    command: 'unreal-mcp-server',
    args: [],
  };
  if (token) entry['env'] = { UNREAL_MCP_TOKEN: token };
  return entry;
}

export async function setupMcp(opts: SetupMcpOptions): Promise<SetupMcpResult> {
  const warnings: string[] = [];
  const nextSteps: string[] = [];
  try {
    const agent = AGENTS.find((a) => a.id === opts.agentId);
    if (!agent) {
      throw new Error(
        `Unknown agent "${opts.agentId}". Valid agents: ${listAgentIds().join(', ')}.`,
      );
    }
    const transport: McpTransport = opts.transport ?? 'http';
    const projectDir = path.resolve(opts.projectDir ?? process.cwd());

    const conn = resolveConnection({ projectDir, url: opts.url, token: opts.token });
    const serverEntry = buildServerEntry(transport, conn.url, conn.token);

    const configPath = path.join(projectDir, agent.relConfigPath);
    emitProgress(opts.onProgress, {
      phase: 'start',
      message: `Configuring ${agent.label} MCP for ${transport} transport`,
    });

    // Merge into any existing config, preserving other servers.
    let config: Record<string, unknown> = {};
    if (fs.existsSync(configPath)) {
      try {
        const parsed = JSON.parse(fs.readFileSync(configPath, 'utf-8'));
        if (parsed && typeof parsed === 'object') config = parsed as Record<string, unknown>;
      } catch {
        warnings.push(`Existing ${configPath} is not valid JSON — it will be replaced.`);
      }
    }
    const servers = (config['mcpServers'] && typeof config['mcpServers'] === 'object'
      ? (config['mcpServers'] as Record<string, unknown>)
      : {});
    servers[SERVER_KEY] = serverEntry;
    config['mcpServers'] = servers;

    const snippet = JSON.stringify(config, null, 2) + '\n';

    if (opts.dryRun) {
      emitProgress(opts.onProgress, { phase: 'done', message: 'Dry run — config not written.' });
    } else {
      fs.mkdirSync(path.dirname(configPath), { recursive: true });
      fs.writeFileSync(configPath, snippet, 'utf-8');
      emitProgress(opts.onProgress, {
        phase: 'file-written',
        message: `Wrote ${configPath}`,
        filePath: configPath,
      });
    }

    nextSteps.push(`Restart ${agent.label} to pick up the new MCP server.`);
    if (transport === 'http') {
      nextSteps.push('Ensure the Unreal Editor (and its MCP server) is running — see `unreal-cli status`.');
    }

    return {
      kind: 'success',
      success: true,
      agentId: agent.id,
      configPath,
      transport,
      snippet,
      warnings,
      nextSteps,
    };
  } catch (err: unknown) {
    return {
      kind: 'failure',
      success: false,
      warnings,
      nextSteps,
      error: err instanceof Error ? err : new Error(String(err)),
    };
  }
}
