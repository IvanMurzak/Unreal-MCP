// `configure` — write `UNREAL_MCP_*` values into `<project>/.env` and
// guard the token-commit hazard by ensuring `.env` is gitignored
// (docs/ARCHITECTURE.md §8). Library-safe: never prints, never exits,
// never throws past the boundary.

import * as fs from 'fs';
import * as path from 'path';
import { writeEnvFile, ensureEnvGitignored, type KnownEnvKey } from '../utils/env-file.js';
import { asError } from '../utils/error.js';
import { emitProgress } from './progress.js';
import type { ConfigureOptions, ConfigureResult } from './types.js';

/**
 * Configure a project's `.env` for Unreal-MCP. Writes only the keys whose
 * option was supplied (so a second call with one field updates just that
 * field, preserving the rest of the file). Appends `.env` to `.gitignore`
 * unless `ensureGitignore === false`.
 */
export async function configure(opts: ConfigureOptions): Promise<ConfigureResult> {
  const warnings: string[] = [];
  try {
    if (!opts || typeof opts.projectDir !== 'string' || opts.projectDir.trim().length === 0) {
      return {
        kind: 'failure',
        success: false,
        warnings,
        error: new Error('projectDir is required.'),
      };
    }
    const projectDir = path.resolve(opts.projectDir);
    if (!fs.existsSync(projectDir)) {
      return {
        kind: 'failure',
        success: false,
        warnings,
        error: new Error(`Project directory does not exist: ${projectDir}`),
      };
    }

    emitProgress(opts.onProgress, {
      phase: 'start',
      message: `Configuring Unreal-MCP env for ${projectDir}`,
    });

    const updates: Partial<Record<KnownEnvKey, string>> = {};
    if (opts.connectionMode !== undefined) updates['UNREAL_MCP_CONNECTION_MODE'] = opts.connectionMode;
    if (opts.host !== undefined) updates['UNREAL_MCP_HOST'] = opts.host;
    if (opts.cloudUrl !== undefined) updates['UNREAL_MCP_CLOUD_URL'] = opts.cloudUrl;
    if (opts.token !== undefined) updates['UNREAL_MCP_TOKEN'] = opts.token;
    if (opts.authOption !== undefined) updates['UNREAL_MCP_AUTH_OPTION'] = opts.authOption;
    if (opts.keepConnected !== undefined)
      updates['UNREAL_MCP_KEEP_CONNECTED'] = opts.keepConnected ? 'true' : 'false';
    if (opts.tools !== undefined) updates['UNREAL_MCP_TOOLS'] = opts.tools;
    if (opts.startServer !== undefined)
      updates['UNREAL_MCP_START_SERVER'] = opts.startServer ? 'true' : 'false';
    if (opts.transport !== undefined) updates['UNREAL_MCP_TRANSPORT'] = opts.transport;
    if (opts.logLevel !== undefined) updates['UNREAL_MCP_LOG_LEVEL'] = opts.logLevel;

    const envPath = path.join(projectDir, '.env');
    const hasUpdates = Object.keys(updates).length > 0;
    // Only touch `.env` when there is something to write — otherwise the
    // "left unchanged" warning would contradict an actual (no-op) rewrite.
    const { added, updated } = hasUpdates
      ? writeEnvFile(envPath, updates)
      : { added: [], updated: [] };
    const keysWritten = [...added, ...updated];

    if (!hasUpdates) {
      warnings.push('No configuration values supplied — .env left unchanged.');
    } else {
      emitProgress(opts.onProgress, {
        phase: 'file-written',
        message: `Wrote ${keysWritten.length} key(s) to ${envPath}`,
        filePath: envPath,
      });
    }

    // Token-commit hazard mitigation (§8): ensure `.env` is gitignored.
    let gitignorePath: string | undefined;
    let gitignoreAction: 'created' | 'appended' | 'already-ignored' | 'skipped' = 'skipped';
    if (opts.ensureGitignore !== false) {
      const result = ensureEnvGitignored(projectDir);
      gitignorePath = result.gitignorePath;
      gitignoreAction = result.action;
      emitProgress(opts.onProgress, {
        phase: 'info',
        message:
          result.action === 'already-ignored'
            ? '.env already gitignored.'
            : `.env ${result.action} in ${result.gitignorePath}`,
      });
    }

    emitProgress(opts.onProgress, { phase: 'done', message: 'Configure complete.' });

    return {
      kind: 'success',
      success: true,
      envPath,
      keysWritten,
      gitignorePath,
      gitignoreAction,
      warnings,
    };
  } catch (err: unknown) {
    return {
      kind: 'failure',
      success: false,
      warnings,
      error: asError(err),
    };
  }
}
