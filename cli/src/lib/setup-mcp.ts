// `setup-mcp` — write an MCP client config snippet for a supported agent
// so it can reach the project's local Unreal MCP server. Library-safe.
//
// Each agent has a project-relative config file and a merge strategy that
// preserves any existing server entries under the agent's top-level key
// (`mcpServers` for Claude Code / Cursor, `servers` for VS Code). The
// snippet shape follows the MCP client convention shared by the Unity/Godot
// CLIs.

import * as fs from 'fs';
import * as path from 'path';
import { platform } from 'os';
import { resolveConnection } from '../utils/config.js';
import { generatePortFromDirectory } from '../utils/port.js';
import {
  downloadServer,
  resolveServerBinaryPath,
  resolveServerOverride,
  SERVER_PATH_ENV_VAR,
} from './download-server.js';
import { emitProgress } from './progress.js';
import type { McpTransport, SetupMcpOptions, SetupMcpResult } from './types.js';

// Re-exported for compatibility: the §6 install-path resolver lives in
// `download-server.ts` (single owner of the server-binary layout).
export { resolveServerBinaryPath } from './download-server.js';

const SERVER_KEY = 'unreal-mcp';

/** Stdio launch parameters for the local `gamedev-mcp-server` binary (§6). */
interface StdioServer {
  /** Absolute path to the published server binary. */
  serverPath: string;
  /** Deterministic localhost port for the project's editor (§1.1). */
  port: number;
}

interface AgentDef {
  id: string;
  /** Config file path relative to the project dir. */
  relConfigPath: string;
  /** Display label. */
  label: string;
  /**
   * Top-level key the agent nests its MCP servers under. Claude Code and
   * Cursor use `mcpServers`; VS Code's `.vscode/mcp.json` uses `servers`
   * (writing `mcpServers` there is silently ignored by VS Code).
   */
  bodyKey: 'mcpServers' | 'servers';
}

const AGENTS: AgentDef[] = [
  { id: 'claude-code', relConfigPath: '.mcp.json', label: 'Claude Code', bodyKey: 'mcpServers' },
  { id: 'cursor', relConfigPath: path.join('.cursor', 'mcp.json'), label: 'Cursor', bodyKey: 'mcpServers' },
  { id: 'vscode', relConfigPath: path.join('.vscode', 'mcp.json'), label: 'VS Code', bodyKey: 'servers' },
];

/** Valid agent ids accepted by `setupMcp`. Pure. */
export function listAgentIds(): string[] {
  return AGENTS.map((a) => a.id);
}

/**
 * Build the `mcpServers` entry for the given transport. HTTP points the
 * client at the running server; stdio launches the local `gamedev-mcp-server`
 * binary (the §6 install path) on demand, passing the deterministic port,
 * auth mode, and token as args (the shared GameDev-MCP-Server host's CLI
 * contract, shared with Unity/Godot). Pure — exported for tests.
 */
export function buildServerEntry(
  transport: McpTransport,
  url: string,
  token: string | undefined,
  stdio?: StdioServer,
): Record<string, unknown> {
  if (transport === 'http') {
    const entry: Record<string, unknown> = { type: 'http', url: `${url}/mcp` };
    if (token) entry['headers'] = { Authorization: `Bearer ${token}` };
    return entry;
  }
  // stdio — a bare binary name is not on PATH (the server lands under the
  // project's Intermediate/ per §6), so an absolute command + port/token
  // args are required for the entry to actually launch a working server.
  if (!stdio) {
    throw new Error('stdio transport requires a resolved server path + port.');
  }
  return {
    type: 'stdio',
    command: stdio.serverPath,
    args: [
      `port=${stdio.port}`,
      'client-transport=stdio',
      `authorization=${token ? 'required' : 'none'}`,
      `token=${token ?? ''}`,
    ],
  };
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

    // stdio needs a LOCAL server binary. Resolution: the UNREAL_MCP_SERVER_PATH
    // override wins (no download, no version check — §6 dev-override semantics,
    // mirroring UNREAL_MCP_BRIDGE_PATH); otherwise download/refresh the pinned
    // gamedev-mcp-server release into the §6 install path. A failed download
    // degrades to a warning (the config is still written and works once the
    // binary is provided), never a hard failure.
    let stdio: StdioServer | undefined;
    if (transport === 'stdio') {
      const env = opts.env ?? process.env;
      const osPlatform = platform() as NodeJS.Platform;
      let serverPath = resolveServerOverride(env);
      if (!serverPath && opts.dryRun) {
        // Dry run must stay side-effect-free: report the would-be path.
        serverPath = resolveServerBinaryPath(projectDir, osPlatform);
      } else if (!serverPath) {
        const download = opts.downloadServerImpl ?? downloadServer;
        const dl = await download({ projectDir, env, onProgress: opts.onProgress });
        if (dl.kind === 'success') {
          warnings.push(...dl.warnings);
          serverPath = dl.serverPath;
        } else {
          serverPath = resolveServerBinaryPath(projectDir, osPlatform);
          warnings.push(`Could not download the gamedev-mcp-server binary: ${dl.error.message}`);
          nextSteps.push(
            `Provide the server binary manually: set ${SERVER_PATH_ENV_VAR} to a local gamedev-mcp-server build, or re-run this command once the download can succeed.`,
          );
        }
      }
      stdio = { serverPath, port: generatePortFromDirectory(projectDir) };
    }
    const serverEntry = buildServerEntry(transport, conn.url, conn.token, stdio);

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
    const servers = (config[agent.bodyKey] && typeof config[agent.bodyKey] === 'object'
      ? (config[agent.bodyKey] as Record<string, unknown>)
      : {});
    servers[SERVER_KEY] = serverEntry;
    config[agent.bodyKey] = servers;

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
      nextSteps.push('Ensure the Unreal Editor (and its MCP server) is running — see `unreal-mcp-cli status`.');
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
