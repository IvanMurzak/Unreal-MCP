// Connection resolution for the HTTP commands (`run-tool`,
// `run-system-tool`, `status`, `wait-for-ready`).
//
// Precedence mirrors docs/ARCHITECTURE.md §8 (highest wins):
//   explicit override -> process env -> project `.env` -> deterministic
//   localhost port. Only the plugin owns the on-disk JSON config under
//   `Saved/Config/UnrealMcp/`, so the CLI never reads it — the `.env`
//   layer plus the deterministic port are the CLI's whole picture.

import * as path from 'path';
import { readEnvFile } from './env-file.js';
import { generatePortFromDirectory } from './port.js';

export interface ResolvedConnection {
  /** Base URL with no trailing slash. */
  url: string;
  /** Bearer token, when one is configured. */
  token: string | undefined;
  /** Where the URL came from — for diagnostics / `status`. */
  source: 'override' | 'process-env' | 'env-file' | 'deterministic-port';
}

export interface ResolveConnectionOptions {
  /** Absolute project dir — used for `.env` read + deterministic port. */
  projectDir?: string;
  /** Explicit URL override (wins over everything). */
  url?: string;
  /** Explicit token override (wins over everything). */
  token?: string;
  /** Injectable env source (defaults to `process.env`). */
  processEnv?: NodeJS.ProcessEnv;
}

/**
 * Resolve the MCP server URL + token to use for a project. Pure given an
 * injected `processEnv`; reads the project `.env` via the filesystem.
 *
 * Throws only when neither a `projectDir` nor a `url` override is supplied
 * — every other path degrades to the deterministic localhost port.
 */
export function resolveConnection(opts: ResolveConnectionOptions): ResolvedConnection {
  const env = opts.processEnv ?? process.env;

  if (opts.url && opts.url.trim().length > 0) {
    return { url: stripTrailingSlash(opts.url), token: opts.token, source: 'override' };
  }

  if (!opts.projectDir) {
    throw new Error('Either url or projectDir must be provided to resolve a connection.');
  }
  const projectDir = path.resolve(opts.projectDir);

  // Process env beats the file (live override, never persisted — §8).
  const envHost = env['UNREAL_MCP_HOST'];
  const envToken = env['UNREAL_MCP_TOKEN'];
  if (envHost && envHost.trim().length > 0) {
    return {
      url: stripTrailingSlash(envHost),
      token: opts.token ?? (envToken && envToken.length > 0 ? envToken : undefined),
      source: 'process-env',
    };
  }

  const fileEnv = readEnvFile(path.join(projectDir, '.env'));
  const fileHost = fileEnv['UNREAL_MCP_HOST'];
  const fileToken = fileEnv['UNREAL_MCP_TOKEN'];
  if (fileHost && fileHost.length > 0) {
    return {
      url: stripTrailingSlash(fileHost),
      token: opts.token ?? (fileToken && fileToken.length > 0 ? fileToken : undefined),
      source: 'env-file',
    };
  }

  const port = generatePortFromDirectory(projectDir);
  return {
    url: `http://localhost:${port}`,
    token:
      opts.token ??
      (envToken && envToken.length > 0
        ? envToken
        : fileToken && fileToken.length > 0
          ? fileToken
          : undefined),
    source: 'deterministic-port',
  };
}

export function stripTrailingSlash(url: string): string {
  return url.trim().replace(/\/+$/, '');
}
