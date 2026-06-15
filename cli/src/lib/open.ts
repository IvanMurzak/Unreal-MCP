// `open` — launch the Unreal Editor for a project, wiring the MCP
// connection env vars. Library-safe: never prints, never exits, never
// throws past the boundary.

import * as fs from 'fs';
import * as path from 'path';
import { spawn } from 'child_process';
import { platform } from 'os';
import { discoverEngine, invalidateCachedEngine } from './engine.js';
import { readUProject } from '../utils/project.js';
import { emitProgress } from './progress.js';
import type {
  AuthOption,
  McpTransport,
  OpenProjectOptions,
  OpenProjectResult,
} from './types.js';

function isValidAuth(v: unknown): v is AuthOption {
  return v === 'none' || v === 'required';
}
function isValidTransport(v: unknown): v is McpTransport {
  return v === 'stdio' || v === 'http';
}

/**
 * Build the `UNREAL_MCP_*` env-var map propagated to the editor process.
 * Returns `undefined` when `noConnect` is set or nothing maps. Pure;
 * throws on invalid enum values (caught at the `openProject` boundary).
 * Exported for tests.
 */
export function buildOpenEnv(opts: OpenProjectOptions): Record<string, string> | undefined {
  if (opts.noConnect === true) return undefined;
  const env: Record<string, string> = {};
  if (opts.host !== undefined) env['UNREAL_MCP_HOST'] = opts.host;
  if (opts.token !== undefined) env['UNREAL_MCP_TOKEN'] = opts.token;
  if (opts.keepConnected) env['UNREAL_MCP_KEEP_CONNECTED'] = 'true';
  if (opts.tools !== undefined) env['UNREAL_MCP_TOOLS'] = opts.tools;
  if (opts.auth !== undefined) {
    if (!isValidAuth(opts.auth)) throw new Error('auth must be "none" or "required"');
    env['UNREAL_MCP_AUTH_OPTION'] = opts.auth;
  }
  if (opts.transport !== undefined) {
    if (!isValidTransport(opts.transport)) throw new Error('transport must be "stdio" or "http"');
    env['UNREAL_MCP_TRANSPORT'] = opts.transport;
  }
  if (opts.startServer !== undefined) env['UNREAL_MCP_START_SERVER'] = opts.startServer ? 'true' : 'false';
  return Object.keys(env).length > 0 ? env : undefined;
}

export async function openProject(opts: OpenProjectOptions): Promise<OpenProjectResult> {
  const warnings: string[] = [];
  let projectDir: string | undefined;
  try {
    const os = platform() as NodeJS.Platform;
    // Accept either a project directory or a `.uproject` file path (the doc
    // on OpenProjectOptions.projectDir promises both); `readUProject`
    // resolves both shapes.
    const inputPath = path.resolve(opts.projectDir ?? process.cwd());
    projectDir = inputPath;

    if (!fs.existsSync(inputPath)) {
      throw new Error(`Project path does not exist: ${inputPath}`);
    }
    const uproject = readUProject(inputPath);
    if (!uproject) {
      throw new Error(`No .uproject found at ${inputPath}`);
    }
    projectDir = uproject.projectDir;

    emitProgress(opts.onProgress, { phase: 'start', message: `Opening ${uproject.projectName}` });

    // Validate enum options up-front (before engine discovery I/O).
    const env = buildOpenEnv(opts);

    // `buildOpenEnv` returns early under `--no-connect`, so connection-related
    // flags are silently dropped (and their enum values never validated). Tell
    // the caller their flags had no effect rather than letting e.g.
    // `open --no-connect --auth bogus` look accepted.
    if (opts.noConnect === true) {
      const ignored: string[] = [];
      if (opts.host !== undefined) ignored.push('host');
      if (opts.token !== undefined) ignored.push('token');
      if (opts.auth !== undefined) ignored.push('auth');
      if (opts.transport !== undefined) ignored.push('transport');
      if (opts.keepConnected) ignored.push('keepConnected');
      if (opts.tools !== undefined) ignored.push('tools');
      if (opts.startServer !== undefined) ignored.push('startServer');
      if (ignored.length > 0) {
        warnings.push(`Connection options are ignored under --no-connect: ${ignored.join(', ')}.`);
      }
    }

    // Full cache-first discovery chain:
    //   cache → launcher manifest → registry → common-location scan.
    // `enginesImpl` (when supplied) still injects the LAUNCHER layer's engines,
    // preserving existing test/embed behaviour; the registry + common-location
    // layers are skipped under an injected `enginesImpl` only if the launcher
    // layer already resolves (otherwise they run, with their own defaults).
    const resolution = discoverEngine({
      engineAssociation: uproject.engineAssociation,
      engineRootOverride: opts.engineRoot,
      os,
      noCache: opts.noCache,
      enginesImpl: opts.enginesImpl,
      // Thread the discovery surfaces so a library embedder / test can keep
      // resolution hermetic — never touching the real home cache file, host
      // registry, or real engine-install directories. Unset in normal CLI use,
      // where the defaults wire to the real system.
      cacheIo: opts.cacheIo,
      discoveryFs: opts.discoveryFs,
      registryQueryImpl: opts.registryQueryImpl,
      existsImpl: opts.existsImpl,
    });
    if (resolution.kind === 'unresolved') {
      throw new Error(resolution.message);
    }

    emitProgress(opts.onProgress, {
      phase: 'engine-resolved',
      message: `Resolved engine at ${resolution.engineRoot}`,
      editorPath: resolution.editorPath,
      engineRoot: resolution.engineRoot,
    });

    emitProgress(opts.onProgress, {
      phase: 'launching',
      message: 'Launching Unreal Editor',
      editorPath: resolution.editorPath,
      projectDir,
    });

    const args = [uproject.uprojectPath];
    const childEnv: NodeJS.ProcessEnv = { ...process.env, ...(env ?? {}) };
    // Invalidate-on-spawn-failure: when the editor fails to launch from a path
    // we resolved (most importantly a CACHED path pointing at a moved/removed
    // engine), drop that cache slot so the next `open` re-runs the full chain.
    // Only relevant for the real detached spawn — an injected `spawnImpl`
    // (tests) owns its own error semantics.
    const onSpawnError = (): void => {
      if (!opts.noCache) invalidateCachedEngine(uproject.engineAssociation);
    };
    const child = opts.spawnImpl
      ? opts.spawnImpl(resolution.editorPath, args, childEnv)
      : spawnDetached(resolution.editorPath, args, childEnv, onSpawnError);

    emitProgress(opts.onProgress, {
      phase: 'launched',
      message: `Launched (PID: ${child.pid ?? 'unknown'})`,
      pid: child.pid,
    });
    emitProgress(opts.onProgress, { phase: 'done', message: 'Editor launched.' });

    return {
      kind: 'success',
      success: true,
      editorPath: resolution.editorPath,
      engineRoot: resolution.engineRoot,
      projectDir,
      editorPid: child.pid,
      envVars: env ?? {},
      warnings,
    };
  } catch (err: unknown) {
    const error = err instanceof Error ? err : new Error(String(err));
    return {
      kind: 'failure',
      success: false,
      projectDir,
      warnings,
      errorMessage: error.message,
      error,
    };
  }
}

function spawnDetached(
  editorPath: string,
  args: string[],
  env: NodeJS.ProcessEnv,
  onError?: () => void,
): { pid?: number } {
  const child = spawn(editorPath, args, { detached: true, stdio: 'ignore', env });
  // A detached spawn can fail asynchronously (EACCES, ENOENT) AFTER we have
  // already returned success; without an 'error' listener that emits an
  // unhandled 'error' event which crashes the CLI. Swallow it — the editor
  // simply did not launch, and `status` will report it unreachable — but first
  // invalidate any cached engine path so a stale cache self-heals.
  child.on('error', () => {
    try {
      onError?.();
    } catch {
      /* never let cache cleanup crash the CLI */
    }
  });
  child.unref();
  return { pid: child.pid };
}
