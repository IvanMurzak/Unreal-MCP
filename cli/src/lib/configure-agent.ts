// `configure --agent <id>` — proxy the terminal to the managed
// `gamedev-mcp-server configure` subcommand (mcp-authorize design 06/09, D12).
//
// The shared C# agent-configurator registry (16 clients) lives inside the
// `gamedev-mcp-server` binary; exposing it from the terminal means agent-first
// users write any client config with ONE command and never touch the editor UI.
// This module is the thin proxy: it resolves the managed server binary (the same
// §6 `download-server` resolver `setup-mcp` uses — the binary lives in the CLI's
// managed dir under `Intermediate/UnrealMCP/server/<rid>/`, not on PATH), then
// spawns `<binary> configure --agent <id> [--url <url>] --transport <t>
// --project <projectDir>`, streaming its output and forwarding its exit code.
//
// Transport defaults to `stdio` — the offline-local installer flow this surface
// serves (design 09 workflow 1B: `configure --agent <id>` writes the stdio
// config with the derived `port=`/`project=` pin, `none` mode). Pass
// `transport: 'http'` for the hosted/local-oauth shape. Credentials are never
// written by the server's URL-only default (D11).
//
// Library-safe: never throws past the boundary. `downloadServerImpl` + `spawnImpl`
// are injectable so a test asserts the resolved binary + arg vector without a real
// server download or child process.

import * as path from 'path';
import { spawn } from 'child_process';
import {
  downloadServer,
  resolveServerOverride,
  SERVER_PATH_ENV_VAR,
} from './download-server.js';
import { asError } from '../utils/error.js';
import { emitProgress } from './progress.js';
import type { DownloadServerOptions, DownloadServerResult, McpTransport, ProgressCallback } from './types.js';

export interface ConfigureAgentOptions {
  /** Agent id to configure (e.g. `claude-code`). Forwarded to the server's `--agent`. */
  agentId: string;
  /** Project dir the config is pinned to (defaults to `process.cwd()`). */
  projectDir?: string;
  /** Explicit server URL (`--url`). Omitted → the server defaults to the local host. */
  url?: string;
  /** `stdio` (default — offline-local 1B flow) or `http` (hosted/local-oauth). */
  transport?: McpTransport;
  /** Server version to acquire when downloading the managed binary. Defaults to the pin. */
  serverVersion?: string;
  /** Offline/CI escape hatch forwarded to the download resolver (`--server-source`). */
  serverSource?: string;
  /** Env source for the `UNREAL_MCP_SERVER_PATH` override (test injection). */
  env?: NodeJS.ProcessEnv;
  /** Inject the server-binary acquisition (defaults to `downloadServer`). Test injection. */
  downloadServerImpl?: (opts: DownloadServerOptions) => Promise<DownloadServerResult>;
  /** Inject the child-process runner (defaults to spawning the server, stdio inherited). Test injection. */
  spawnImpl?: (bin: string, args: string[]) => Promise<{ exitCode: number }>;
  onProgress?: ProgressCallback;
}

export interface ConfigureAgentSuccess {
  kind: 'success';
  success: true;
  agentId: string;
  /** Absolute path of the managed server binary the config was written through. */
  serverPath: string;
  /** The full argument vector passed to the server binary. */
  args: string[];
  /** The server subcommand's exit code (0 on success). */
  exitCode: number;
  warnings: string[];
}

export interface ConfigureAgentFailure {
  kind: 'failure';
  success: false;
  warnings: string[];
  error: Error;
}

export type ConfigureAgentResult = ConfigureAgentSuccess | ConfigureAgentFailure;

export async function configureAgent(opts: ConfigureAgentOptions): Promise<ConfigureAgentResult> {
  const warnings: string[] = [];
  try {
    const agentId = (opts.agentId ?? '').trim();
    if (agentId.length === 0) throw new Error('An --agent id is required.');

    const projectDir = path.resolve(opts.projectDir ?? process.cwd());
    const transport: McpTransport = opts.transport ?? 'stdio';
    const env = opts.env ?? process.env;

    // Resolve the managed server binary: the UNREAL_MCP_SERVER_PATH override wins
    // (no download); otherwise download/refresh the pinned gamedev-mcp-server.
    let serverPath = resolveServerOverride(env);
    if (!serverPath) {
      const download = opts.downloadServerImpl ?? downloadServer;
      const dl = await download({
        projectDir,
        version: opts.serverVersion,
        source: opts.serverSource,
        env,
        onProgress: opts.onProgress,
      });
      if (dl.kind !== 'success') {
        throw new Error(
          `Could not acquire the gamedev-mcp-server binary to proxy configure: ${dl.error.message}. ` +
            `Provide one with ${SERVER_PATH_ENV_VAR}, --server-source, or once the download can succeed.`,
        );
      }
      warnings.push(...dl.warnings);
      serverPath = dl.serverPath;
    }

    const args = ['configure', '--agent', agentId];
    if (opts.url && opts.url.trim().length > 0) args.push('--url', opts.url.trim());
    args.push('--transport', transport);
    args.push('--project', projectDir);

    emitProgress(opts.onProgress, {
      phase: 'start',
      message: `Proxying to gamedev-mcp-server configure --agent ${agentId} (${transport})`,
    });

    const runner = opts.spawnImpl ?? defaultSpawn;
    const { exitCode } = await runner(serverPath, args);

    if (exitCode !== 0) {
      throw new Error(`gamedev-mcp-server configure exited with code ${exitCode}.`);
    }

    emitProgress(opts.onProgress, { phase: 'done', message: `Configured ${agentId}.` });
    return { kind: 'success', success: true, agentId, serverPath, args, exitCode, warnings };
  } catch (err: unknown) {
    return { kind: 'failure', success: false, warnings, error: asError(err) };
  }
}

/** Spawn the server binary with stdio inherited so its config summary reaches the user. */
function defaultSpawn(bin: string, args: string[]): Promise<{ exitCode: number }> {
  return new Promise((resolve, reject) => {
    const child = spawn(bin, args, { stdio: 'inherit' });
    child.on('error', (err) => reject(new Error(`Failed to launch the server configurator: ${err.message}`)));
    child.on('close', (code) => resolve({ exitCode: code ?? 0 }));
  });
}
