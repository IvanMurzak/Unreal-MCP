// `open` — launch the Unreal Editor for a project, wiring the MCP
// connection env vars. Library-safe: never prints, never exits, never
// throws past the boundary.

import * as fs from 'fs';
import * as path from 'path';
import { spawn, type ChildProcess } from 'child_process';
import { platform } from 'os';
import { discoverEngine, invalidateCachedEngine } from './engine.js';
import { readUProject } from '../utils/project.js';
import { asError } from '../utils/error.js';
import { emitProgress, pidSuffix } from './progress.js';
import {
  tryDismissUnrealStartupDialog,
  type DismissOutcome,
  type DismissPlatform,
  LINUX_WAYLAND_UNSUPPORTED_PREFIX,
  LINUX_XDOTOOL_MISSING_PREFIX,
  UNSUPPORTED_PLATFORM_PREFIX,
} from '../utils/startup-dialog-dismiss.js';
import {
  corePluginNeedsBuild,
  isDesktopBuildPlatform,
  projectHasNativeCode,
  resolveDesktopBuildStep,
  runUnrealBuildStep,
} from '../utils/unreal-build.js';
import type {
  AuthOption,
  EditorSpawnImpl,
  McpTransport,
  OpenProjectOptions,
  OpenProjectResult,
  UbtBuildStep,
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
 *
 * Connection-mode resolution (`UNREAL_MCP_CONNECTION_MODE`): an explicit
 * `connectionMode` always wins; otherwise an explicit `host` infers `Custom`
 * (a host implies a non-Cloud target, and the plugin defaults to Cloud — so a
 * host with no mode would silently dial the cloud). With neither set the var is
 * omitted and the plugin default applies. Mirrors godot-cli's host-presence
 * mode default. `noConnect` drops every var (including the mode).
 *
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
  // Connection mode: an explicit value always wins; otherwise infer `Custom`
  // from the presence of an explicit host (the plugin defaults to Cloud, where
  // a bare host is ignored). Neither set => omit (plugin default applies).
  if (opts.connectionMode !== undefined) {
    env['UNREAL_MCP_CONNECTION_MODE'] = opts.connectionMode;
  } else if (opts.host !== undefined) {
    env['UNREAL_MCP_CONNECTION_MODE'] = 'Custom';
  }
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
      if (opts.connectionMode !== undefined) ignored.push('connectionMode');
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

    await maybeBuildBeforeOpen(
      {
        projectDir,
        projectName: uproject.projectName,
        uprojectPath: uproject.uprojectPath,
        engineRoot: resolution.engineRoot,
        build: opts.build,
        buildImpl: opts.buildImpl,
        onProgress: opts.onProgress,
      },
      warnings,
    );

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
    // The same for an injected `spawnImpl` that throws or rejects (which also fails the open).
    const onSpawnError = (): void => {
      if (!opts.noCache) invalidateCachedEngine(uproject.engineAssociation, opts.cacheIo);
    };
    let pid: number | undefined;
    if (opts.spawnImpl) {
      // The embedder's pid may come later (a promise): it is waited for, and logged once known.
      try {
        pid = await spawnedEditorPid(opts.spawnImpl(resolution.editorPath, args, childEnv));
      } catch (err: unknown) {
        onSpawnError();
        throw err;
      }
    } else {
      const child = spawnDetached(resolution.editorPath, args, childEnv, onSpawnError);
      // Node sets `pid` at once; a spawn whose pid is reported later (a `spawn` event) is waited for briefly.
      pid = child.pid ?? (await waitForSpawn(child));
    }

    emitProgress(opts.onProgress, {
      phase: 'launched',
      message: `Launched${pidSuffix(pid)}`,
      pid,
    });

    if (isDesktopBuildPlatform(os) && opts.autoDismissStartupDialogs !== false) {
      await pollAndDismissStartupDialogs({
        timeoutMs: opts.startupDismissTimeoutMs ?? 12000,
        intervalMs: opts.startupDismissPollIntervalMs ?? 1000,
        onProgress: opts.onProgress,
        warnings,
        probe: opts.dismissStartupDialogImpl ?? tryDismissUnrealStartupDialog,
        platform: os,
      });
    }

    emitProgress(opts.onProgress, { phase: 'done', message: 'Editor launched.' });

    return {
      kind: 'success',
      success: true,
      editorPath: resolution.editorPath,
      engineRoot: resolution.engineRoot,
      projectDir,
      editorPid: pid,
      envVars: env ?? {},
      warnings,
    };
  } catch (err: unknown) {
    const error = asError(err);
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

export interface PreLaunchBuildOptions {
  projectDir: string;
  projectName: string;
  uprojectPath: string;
  engineRoot: string;
  build?: boolean;
  buildImpl?: (step: UbtBuildStep) => Promise<void>;
  onProgress?: OpenProjectOptions['onProgress'];
}

/** Compile native modules before a launch when the project or the core plugin needs it (desktop only). */
export async function maybeBuildBeforeOpen(
  opts: PreLaunchBuildOptions,
  warnings: string[],
): Promise<void> {
  if (opts.build === false) return;
  const hostPlatform = platform();
  if (!isDesktopBuildPlatform(hostPlatform)) {
    if (needsPreLaunchBuild(opts.projectDir)) {
      warnings.push(
        `Native modules may require compilation before launch, but pre-launch build is unsupported on ${hostPlatform}.`,
      );
    }
    return;
  }
  if (!needsPreLaunchBuild(opts.projectDir)) return;
  const step: UbtBuildStep = resolveDesktopBuildStep({
    platform: hostPlatform,
    engineRoot: opts.engineRoot,
    projectName: opts.projectName,
    uprojectPath: opts.uprojectPath,
  });

  emitProgress(opts.onProgress, {
    phase: 'info',
    message: `Compiling ${step.editorTarget} before launch`,
  });
  const buildImpl = opts.buildImpl ?? runUnrealBuildStep;
  try {
    await buildImpl(step);
    emitProgress(opts.onProgress, {
      phase: 'info',
      message: `Build succeeded for ${step.editorTarget}`,
    });
  } catch (err: unknown) {
    throw new Error(`Pre-launch build failed: ${asError(err).message}`);
  }
}

function needsPreLaunchBuild(projectDir: string): boolean {
  const hostPlatform = platform();
  return isDesktopBuildPlatform(hostPlatform)
    ? projectHasNativeCode(projectDir) || corePluginNeedsBuild(projectDir, hostPlatform)
    : projectHasNativeCode(projectDir);
}

interface PollAndDismissStartupDialogsOptions {
  timeoutMs: number;
  intervalMs: number;
  onProgress: OpenProjectOptions['onProgress'];
  warnings: string[];
  platform: DismissPlatform;
  probe: (platform: DismissPlatform) => Promise<DismissOutcome>;
  noDialogGraceMs?: number;
  settleAfterDismissMs?: number;
}

export async function _pollAndDismissStartupDialogsForTests(
  opts: PollAndDismissStartupDialogsOptions,
): Promise<void> {
  return pollAndDismissStartupDialogs(opts);
}

async function pollAndDismissStartupDialogs(
  opts: PollAndDismissStartupDialogsOptions,
): Promise<void> {
  const deadline = Date.now() + Math.max(0, opts.timeoutMs);
  const interval = Math.max(50, opts.intervalMs);
  const graceMs = Math.max(0, opts.noDialogGraceMs ?? 5000);
  const settleMs = Math.max(0, opts.settleAfterDismissMs ?? 3000);
  const start = Date.now();
  const seenErrorMessages = new Set<string>();
  let lastDismissedAt: number | undefined;

  while (Date.now() < deadline) {
    const outcome = await opts.probe(opts.platform);
    const now = Date.now();
    if (outcome.kind === 'dismissed') {
      lastDismissedAt = now;
      emitProgress(opts.onProgress, {
        phase: 'startup-dialog-dismissed',
        message: `[open] dismissed Unreal startup dialog (dialog=${outcome.dialog}, button=${outcome.button}, platform=${opts.platform})`,
        dialog: outcome.dialog,
        button: outcome.button,
        platform: opts.platform,
      });
    } else if (outcome.kind === 'error') {
      if (!seenErrorMessages.has(outcome.message)) {
        seenErrorMessages.add(outcome.message);
        opts.warnings.push(`startup-dialog auto-dismiss: ${outcome.message}`);
      }
      if (
        outcome.message.includes(UNSUPPORTED_PLATFORM_PREFIX) ||
        outcome.message.includes(LINUX_XDOTOOL_MISSING_PREFIX) ||
        outcome.message.includes(LINUX_WAYLAND_UNSUPPORTED_PREFIX)
      ) {
        return;
      }
    } else if (lastDismissedAt === undefined && now - start >= graceMs) {
      return;
    } else if (lastDismissedAt !== undefined && now - lastDismissedAt >= settleMs) {
      return;
    }

    const remaining = deadline - Date.now();
    if (remaining <= 0) break;
    await sleep(Math.min(interval, remaining));
  }

  if (seenErrorMessages.size > 0 && lastDismissedAt === undefined) {
    opts.warnings.push(
      `startup-dialog auto-dismiss timed out after ${opts.timeoutMs}ms — continuing without further startup-dialog polling`,
    );
  }
}

function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => {
    setTimeout(resolve, ms);
  });
}

/** The editor pid an `openProject` `spawnImpl` reported: the started process's `pid`, or its promise's value. */
async function spawnedEditorPid(started: ReturnType<EditorSpawnImpl>): Promise<number | undefined> {
  const value = await started;
  return value !== null && typeof value === 'object' ? value.pid : (value ?? undefined);
}

/**
 * Resolve to the child's pid once it emits `spawn`, or to `undefined` when it emits `error` first or
 * `timeoutMs` elapses. Never throws, never keeps the process alive (the child is detached and unref'd).
 */
function waitForSpawn(child: ChildProcess, timeoutMs = 2000): Promise<number | undefined> {
  return new Promise((resolve) => {
    let settled = false;
    const finish = (pid: number | undefined): void => {
      if (settled) return;
      settled = true;
      resolve(pid);
    };
    child.once('spawn', () => finish(child.pid ?? undefined));
    child.once('error', () => finish(undefined));
    setTimeout(() => finish(child.pid ?? undefined), timeoutMs).unref();
  });
}

function spawnDetached(
  editorPath: string,
  args: string[],
  env: NodeJS.ProcessEnv,
  onError?: () => void,
): ChildProcess {
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
  return child;
}
