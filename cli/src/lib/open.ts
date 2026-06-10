// `open` — launch the Unreal Editor for a project, wiring the MCP
// connection env vars. Library-safe: never prints, never exits, never
// throws past the boundary.

import * as fs from 'fs';
import * as path from 'path';
import { spawn } from 'child_process';
import { platform } from 'os';
import { getDefaultLauncherManifestPath, readLauncherManifest } from '../utils/launcher.js';
import { resolveEngine } from '../utils/engine.js';
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

    const engines = opts.enginesImpl
      ? opts.enginesImpl()
      : (() => {
          const manifestPath = getDefaultLauncherManifestPath(os);
          return manifestPath ? readLauncherManifest(manifestPath) : [];
        })();

    const resolution = resolveEngine({
      engineAssociation: uproject.engineAssociation,
      engines,
      engineRootOverride: opts.engineRoot,
      os,
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
    const child = opts.spawnImpl
      ? opts.spawnImpl(resolution.editorPath, args, childEnv)
      : spawnDetached(resolution.editorPath, args, childEnv);

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
): { pid?: number } {
  const child = spawn(editorPath, args, { detached: true, stdio: 'ignore', env });
  child.unref();
  return { pid: child.pid };
}
