// `setup-mcp` — write an MCP client config snippet for a supported AI agent so
// it can reach the project's local/cloud Unreal MCP server. Library-safe.
//
// The per-agent knowledge (config path, body key, config format, and the
// stdio/http server-entry shapes) lives in the shared `utils/agents.ts`
// registry — the same design the Unity and Godot CLIs use, so all three stay
// consistent. This module is the orchestrator: it resolves the connection,
// acquires the local `gamedev-mcp-server` binary for stdio transport (via the
// §6 `download-server.ts` resolver), and merges the chosen agent's entry into
// its config file (JSON or TOML), preserving any existing servers.

import * as path from 'path';
import { platform } from 'os';
import { resolveConnection, appendMcp } from '../utils/config.js';
import { asError } from '../utils/error.js';
import { generatePortFromDirectory } from '../utils/port.js';
import {
  getAgentById,
  getAgentIds,
  writeJsonAgentConfig,
  writeTomlAgentConfig,
  MCP_SERVER_NAME,
} from '../utils/agents.js';
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

/** Valid agent ids accepted by `setupMcp`. Pure. */
export function listAgentIds(): string[] {
  return getAgentIds();
}

export async function setupMcp(opts: SetupMcpOptions): Promise<SetupMcpResult> {
  const warnings: string[] = [];
  const nextSteps: string[] = [];
  try {
    const agent = getAgentById(opts.agentId);
    if (!agent) {
      throw new Error(
        `Unknown agent "${opts.agentId}". Valid agents: ${getAgentIds().join(', ')}.`,
      );
    }
    const transport: McpTransport = opts.transport ?? 'http';
    const projectDir = path.resolve(opts.projectDir ?? process.cwd());

    const conn = resolveConnection({ projectDir, url: opts.url, token: opts.token });

    // Build the agent's server-entry props for the chosen transport.
    let props: Record<string, unknown>;
    let removeKeys: string[];

    if (transport === 'stdio') {
      // stdio needs a LOCAL server binary. Resolution: the UNREAL_MCP_SERVER_PATH
      // override wins (no download, no version check — §6 dev-override semantics,
      // mirroring UNREAL_MCP_BRIDGE_PATH); otherwise download/refresh the pinned
      // gamedev-mcp-server release into the §6 install path. A failed download
      // degrades to a warning (the config is still written and works once the
      // binary is provided), never a hard failure.
      const env = opts.env ?? process.env;
      const osPlatform = platform() as NodeJS.Platform;
      let serverPath: string | null = resolveServerOverride(env);
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
      const port = generatePortFromDirectory(projectDir);
      const auth = conn.token ? 'required' : 'none';
      props = agent.getStdioProps(serverPath, port, auth, conn.token ?? '');
      removeKeys = agent.stdioRemoveKeys;
    } else {
      // http — point the agent at the resolved `<host>/mcp` client URL. The
      // `/mcp` segment is appended ONCE here (idempotently, tolerating a URL
      // that already ends in `/mcp`) so the agent closures use `url` verbatim;
      // this avoids the historical `/mcp` double-append.
      const httpUrl = appendMcp(conn.url);
      const token = conn.token ?? '';
      props = agent.getHttpProps(httpUrl, token, token.length > 0);
      removeKeys = agent.httpRemoveKeys;
    }

    const configPath = agent.getConfigPath(projectDir);
    emitProgress(opts.onProgress, {
      phase: 'start',
      message: `Configuring ${agent.name} MCP for ${transport} transport`,
    });

    const snippet =
      agent.configFormat === 'toml'
        ? writeTomlAgentConfig(configPath, agent.bodyPath, MCP_SERVER_NAME, props, removeKeys, opts.dryRun)
        : writeJsonAgentConfig(configPath, agent.bodyPath, MCP_SERVER_NAME, props, removeKeys, opts.dryRun);

    if (opts.dryRun) {
      emitProgress(opts.onProgress, { phase: 'done', message: 'Dry run — config not written.' });
    } else {
      emitProgress(opts.onProgress, {
        phase: 'file-written',
        message: `Wrote ${configPath}`,
        filePath: configPath,
      });
    }

    nextSteps.push(`Restart ${agent.name} to pick up the new MCP server.`);
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
      error: asError(err),
    };
  }
}
