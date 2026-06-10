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
  const envToken = nonEmpty(env['UNREAL_MCP_TOKEN']);

  if (opts.url && opts.url.trim().length > 0) {
    return { url: stripTrailingSlash(opts.url), token: opts.token ?? envToken, source: 'override' };
  }

  if (!opts.projectDir) {
    throw new Error('Either url or projectDir must be provided to resolve a connection.');
  }
  const projectDir = path.resolve(opts.projectDir);

  const fileEnv = readEnvFile(path.join(projectDir, '.env'));
  const fileToken = nonEmpty(fileEnv['UNREAL_MCP_TOKEN']);

  // Token precedence is layered independently of which layer supplied the
  // URL (§8: explicit override -> process env -> project `.env`). Resolving
  // it once here avoids the asymmetry of, e.g., a process-env HOST ignoring a
  // `.env` token or a `.env` HOST ignoring a process-env token.
  const token = opts.token ?? envToken ?? fileToken;

  // Process env beats the file (live override, never persisted — §8).
  const envHost = nonEmpty(env['UNREAL_MCP_HOST']);
  if (envHost) {
    return { url: stripTrailingSlash(envHost), token, source: 'process-env' };
  }

  const fileHost = nonEmpty(fileEnv['UNREAL_MCP_HOST']);
  if (fileHost) {
    return { url: stripTrailingSlash(fileHost), token, source: 'env-file' };
  }

  const port = generatePortFromDirectory(projectDir);
  return { url: `http://localhost:${port}`, token, source: 'deterministic-port' };
}

/** Trim a value and return `undefined` when it is absent or blank. */
function nonEmpty(value: string | undefined): string | undefined {
  if (value === undefined) return undefined;
  const trimmed = value.trim();
  return trimmed.length > 0 ? trimmed : undefined;
}

export function stripTrailingSlash(url: string): string {
  return url.trim().replace(/\/+$/, '');
}
