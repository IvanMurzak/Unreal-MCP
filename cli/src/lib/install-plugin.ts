// `install-plugin` / `remove-plugin` — place (or junction) the UnrealMCP
// plugin under `<project>/Plugins/UnrealMCP`. Library-safe.
//
// Copy mode recursively copies the plugin source. Junction mode (Windows,
// dev) creates a directory junction so the project always tracks the live
// plugin source — the same pattern the infra testbed uses.

import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
// T5/B1: recognise the project root via cli-core's shared marker probe using the
// Unreal engine adapter (any `*.uproject` in the root) — one policy across the
// three CLIs. Warn-not-refuse: install-plugin's `requireMarker` is effectively
// false (a not-yet-initialised tree is a valid, rarer flow).
import { probeProjectMarkers, unrealAdapter } from '@baizor/gamedev-cli-core';
import { asError } from '../utils/error.js';
import { isSymlink } from '../utils/fs.js';
import { emitProgress } from './progress.js';
import { resolvePluginSource } from './plugin-source.js';
import { downloadServer } from './download-server.js';
import { enrollPlugin } from './enroll.js';
import type {
  InstallPluginOptions,
  InstallPluginResult,
  InstallPluginSuccess,
  RemovePluginOptions,
  RemovePluginResult,
} from './types.js';

const PLUGIN_DIRNAME = 'UnrealMCP';
/** Subtree under `Binaries/` holding the bundled bridge (ARCHITECTURE §6.1). */
const BUNDLED_BRIDGE_REL = path.join('Binaries', 'ThirdParty');
/** Bridge root under `Binaries/ThirdParty/` after a release/source-asset install. */
const BUNDLED_BRIDGE_ROOT_REL = path.join(BUNDLED_BRIDGE_REL, 'UnrealMcpBridge');
/** Fab-surviving source-side bridge root shipped by the dedicated source asset. */
const SOURCE_BUNDLED_BRIDGE_ROOT_REL = path.join('Source', 'ThirdParty', 'UnrealMcpBridge');
/** `Binaries/ThirdParty` with forward slashes, for the copy filter's keep-check. */
const BUNDLED_BRIDGE_REL_POSIX = BUNDLED_BRIDGE_REL.split(path.sep).join('/');

function hasBundledBridgePayload(root: string): boolean {
  if (!fs.existsSync(root)) return false;
  const queue: string[] = [root];
  while (queue.length > 0) {
    const dir = queue.shift()!;
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
      const next = path.join(dir, entry.name);
      if (entry.isDirectory()) {
        queue.push(next);
        continue;
      }
      if (entry.isFile() && /^unreal-mcp-bridge(?:\.exe)?$/i.test(entry.name)) {
        return true;
      }
    }
  }
  return false;
}

/**
 * Decide whether `srcAbs` (an absolute path inside `pluginSourceDir`) should be
 * copied into the target project. Excludes the plugin SOURCE checkout's local
 * dev build cache so we never ship stale/mismatched compiled modules (issue
 * #73 — the inverse of #60's update-time auto-clean):
 *   - `Intermediate/` and everything under it (UBT build artifacts).
 *   - compiled-module dirs directly under `Binaries/` (e.g. `Binaries/Win64`),
 *     i.e. anything under `Binaries/` that is NOT the bundled-bridge subtree.
 * Keeps everything else — `Source/`, `Resources/`, `Config/`, `*.uplugin`, the
 * plugin root, `Binaries/` itself (needed to reach ThirdParty), and the bundled
 * sidecar under `Binaries/ThirdParty/**` (ARCHITECTURE §6.1).
 */
function copyFilter(srcAbs: string, pluginSourceDir: string): boolean {
  const rel = path.relative(pluginSourceDir, srcAbs).split(path.sep).join('/');
  if (rel === '' || rel === '.') return true; // the plugin root itself
  // Skip Intermediate and its subtree.
  if (rel === 'Intermediate' || rel.startsWith('Intermediate/')) return false;
  // Under Binaries/: allow `Binaries` itself and the ThirdParty subtree; skip
  // every other child (compiled-module platform dirs like Binaries/Win64).
  if (rel === 'Binaries') return true;
  if (rel.startsWith('Binaries/')) {
    return rel === BUNDLED_BRIDGE_REL_POSIX || rel.startsWith(BUNDLED_BRIDGE_REL_POSIX + '/');
  }
  return true;
}

export async function installPlugin(opts: InstallPluginOptions): Promise<InstallPluginResult> {
  const warnings: string[] = [];
  let cleanupSource: (() => void) | undefined;
  try {
    if (!opts?.projectDir) throw new Error('projectDir is required.');

    const projectDir = path.resolve(opts.projectDir);
    if (!fs.existsSync(projectDir)) throw new Error(`Project directory does not exist: ${projectDir}`);
    // Guard against a wrong-cwd run silently scaffolding Plugins/UnrealMCP in
    // an arbitrary directory (consistent with `close`/`status`, which key on
    // a `.uproject`). Warn rather than refuse — installing into a not-yet-
    // initialised project tree is a valid, if rarer, flow. The marker set comes
    // from the shared cli-core Unreal adapter (any `*.uproject`).
    if (!probeProjectMarkers(projectDir, unrealAdapter.markers).found) {
      warnings.push(`No .uproject found in ${projectDir} — is this an Unreal project directory?`);
    }
    const resolvedSource = await resolvePluginSource({
      pluginSourceDir: opts.pluginSourceDir,
      fetchImpl: opts.fetchImpl,
      onProgress: opts.onProgress,
    });
    cleanupSource = resolvedSource.cleanup;
    const pluginSourceDir = path.resolve(resolvedSource.pluginSourceDir);
    if (!fs.existsSync(pluginSourceDir))
      throw new Error(`Plugin source directory does not exist: ${pluginSourceDir}`);
    if (!fs.existsSync(path.join(pluginSourceDir, 'UnrealMCP.uplugin'))) {
      warnings.push(`No UnrealMCP.uplugin found in ${pluginSourceDir} — installing contents anyway.`);
    }

    const pluginsDir = path.join(projectDir, 'Plugins');
    const installedPath = path.join(pluginsDir, PLUGIN_DIRNAME);
    fs.mkdirSync(pluginsDir, { recursive: true });

    emitProgress(opts.onProgress, {
      phase: 'start',
      message: `Installing UnrealMCP plugin into ${installedPath}`,
    });

    let useJunction = opts.junction === true && process.platform === 'win32';
    if (opts.junction === true && process.platform !== 'win32') {
      warnings.push('Junction mode is Windows-only; falling back to copy.');
    }
    if (opts.junction === true && resolvedSource.sourceKind !== 'local') {
      warnings.push(
        'Junction mode requires a stable local plugin source; falling back to copy for the downloaded GitHub source asset.',
      );
      useJunction = false;
    }

    // Preserve or refresh the bundled sidecar bridge across a COPY re-install.
    // A prior release install ships `Binaries/ThirdParty/UnrealMcpBridge/`, but
    // a dev/source checkout does not; the dedicated GitHub source asset instead
    // ships the RID payload under `Source/ThirdParty/UnrealMcpBridge/`. Stash the
    // existing `Binaries/ThirdParty/` subtree before clearing, then after the copy
    // either materialize a fresh `Binaries/ThirdParty/UnrealMcpBridge/` from the
    // source-side payload, or restore the prior bundled bridge when the new source
    // has no bridge at all. Junction installs never go through this path.
    let stashedBridge: string | null = null;
    const priorBridge = path.join(installedPath, BUNDLED_BRIDGE_REL);
    const installedBridgeRoot = path.join(installedPath, BUNDLED_BRIDGE_ROOT_REL);
    const sourceBridgeRoot = path.join(pluginSourceDir, SOURCE_BUNDLED_BRIDGE_ROOT_REL);
    const installedIsRealDir = fs.existsSync(installedPath) && !isSymlink(installedPath);
    if (!useJunction && installedIsRealDir && fs.existsSync(priorBridge)) {
      stashedBridge = fs.mkdtempSync(path.join(os.tmpdir(), 'unreal-mcp-bridge-stash-'));
      fs.cpSync(priorBridge, stashedBridge, { recursive: true });
    }

    // Clear any prior install (link or real dir) so the operation is
    // idempotent and never nests a copy inside a stale junction.
    if (fs.existsSync(installedPath) || isSymlink(installedPath)) {
      if (isSymlink(installedPath)) {
        fs.unlinkSync(installedPath);
      } else {
        fs.rmSync(installedPath, { recursive: true, force: true });
      }
    }

    if (useJunction) {
      fs.symlinkSync(pluginSourceDir, installedPath, 'junction');
      emitProgress(opts.onProgress, { phase: 'done', message: 'Plugin junctioned.' });
      return await postInstall(
        { kind: 'success', success: true, installedPath, mode: 'junction', warnings },
        opts,
        projectDir,
      );
    }

    // If the copy THROWS after the old install was rm'd and the bridge stashed,
    // restore the stash before rethrowing — otherwise the only copy of the
    // (unrecoverable-from-source) bundled bridge is abandoned in os.tmpdir().
    try {
      fs.cpSync(pluginSourceDir, installedPath, {
        recursive: true,
        filter: (src) => copyFilter(src, pluginSourceDir),
      });
    } catch (copyErr: unknown) {
      if (stashedBridge) {
        try {
          fs.mkdirSync(path.dirname(priorBridge), { recursive: true });
          fs.cpSync(stashedBridge, priorBridge, { recursive: true });
          fs.rmSync(stashedBridge, { recursive: true, force: true });
          stashedBridge = null;
        } catch {
          // Best-effort restore failed — surface the stash path on the error so
          // the user can recover the bridge manually instead of losing it.
          const e = asError(copyErr);
          e.message += ` (the bundled sidecar bridge was stashed at ${stashedBridge} — recover it manually)`;
          throw e;
        }
      }
      throw copyErr;
    }

    // The dedicated source asset ships the bridge payload under Source/ThirdParty;
    // materialize it into Binaries/ThirdParty so the first editor open has the
    // staged runtime path even before a local recompile runs.
    if (!fs.existsSync(installedBridgeRoot) && hasBundledBridgePayload(sourceBridgeRoot)) {
      fs.mkdirSync(path.dirname(installedBridgeRoot), { recursive: true });
      fs.cpSync(sourceBridgeRoot, installedBridgeRoot, { recursive: true });
    }

    // Restore the stashed bridge if the freshly-copied source still did not ship one.
    if (stashedBridge) {
      try {
        if (!fs.existsSync(installedBridgeRoot)) {
          fs.mkdirSync(path.dirname(priorBridge), { recursive: true });
          fs.cpSync(stashedBridge, priorBridge, { recursive: true });
          warnings.push('Preserved the previously-bundled sidecar bridge across the re-install.');
        }
      } finally {
        fs.rmSync(stashedBridge, { recursive: true, force: true });
      }
    }

    emitProgress(opts.onProgress, { phase: 'done', message: 'Plugin copied.' });
    return await postInstall(
      { kind: 'success', success: true, installedPath, mode: 'copy', warnings },
      opts,
      projectDir,
    );
  } catch (err: unknown) {
    return {
      kind: 'failure',
      success: false,
      warnings,
      error: asError(err),
    };
  } finally {
    cleanupSource?.();
  }
}

/**
 * Run the post-plugin-install add-ons the `install-plugin` flags request, in
 * order: `--with-server` (best-effort — a download failure degrades to a warning,
 * the plugin is already in), then `--enroll` (a failure IS surfaced — the user
 * explicitly asked to redeem a credential, and a bad/spent code must not read as
 * success). `base` is the already-successful plugin install; its `warnings` array
 * is mutated in place. Never throws.
 */
async function postInstall(
  base: InstallPluginSuccess,
  opts: InstallPluginOptions,
  projectDir: string,
): Promise<InstallPluginResult> {
  // --with-server: acquire the RID-matched gamedev-mcp-server into the managed
  // dir (SHA256SUMS-verified). Best-effort: a failure warns but keeps the install.
  if (opts.withServer) {
    const download = opts.downloadServerImpl ?? downloadServer;
    const dl = await download({
      projectDir,
      version: opts.serverVersion,
      source: opts.serverSource,
      fetchImpl: opts.fetchImpl,
      onProgress: opts.onProgress,
    });
    if (dl.kind === 'success') {
      base.warnings.push(...dl.warnings);
      base.serverPath = dl.serverPath;
      base.serverVersion = dl.version;
    } else {
      base.warnings.push(
        `--with-server: could not download the gamedev-mcp-server binary: ${dl.error.message}. ` +
          `The plugin is installed; re-run once the download can succeed or use --server-source.`,
      );
    }
  }

  // --enroll: redeem the code → machine store + marker + pin. A failure is
  // surfaced as an overall failure (the plugin/server are still installed).
  if (opts.enrollCode !== undefined) {
    const enroll = opts.enrollImpl ?? enrollPlugin;
    const er = await enroll({
      enrollCode: opts.enrollCode,
      projectDir,
      baseUrl: opts.baseUrl,
      storeBaseDir: opts.storeBaseDir,
      fetchImpl: opts.fetchImpl,
      nowImpl: opts.nowImpl,
      onProgress: opts.onProgress,
    });
    if (er.kind === 'success') {
      base.enrolled = true;
      base.serverTarget = er.serverTarget;
      base.pin = er.pin;
      base.pinnedConfigFiles = er.pinnedConfigFiles;
      base.warnings.push(...er.warnings);
    } else {
      return {
        kind: 'failure',
        success: false,
        warnings: base.warnings,
        error: new Error(
          `Plugin installed at ${base.installedPath}, but enrollment failed (${er.reason}): ${er.error.message}`,
        ),
      };
    }
  }

  return base;
}

export async function removePlugin(opts: RemovePluginOptions): Promise<RemovePluginResult> {
  const warnings: string[] = [];
  try {
    if (!opts?.projectDir) throw new Error('projectDir is required.');
    const projectDir = path.resolve(opts.projectDir);
    const installedPath = path.join(projectDir, 'Plugins', PLUGIN_DIRNAME);

    emitProgress(opts.onProgress, {
      phase: 'start',
      message: `Removing UnrealMCP plugin from ${installedPath}`,
    });

    const present = fs.existsSync(installedPath) || isSymlink(installedPath);
    if (!present) {
      emitProgress(opts.onProgress, { phase: 'done', message: 'Plugin was not installed.' });
      return { kind: 'success', success: true, removed: false, installedPath, warnings };
    }

    // unlink a junction (never recurse through it into the live source);
    // rm a real directory.
    if (isSymlink(installedPath)) {
      fs.unlinkSync(installedPath);
    } else {
      fs.rmSync(installedPath, { recursive: true, force: true });
    }

    emitProgress(opts.onProgress, { phase: 'done', message: 'Plugin removed.' });
    return { kind: 'success', success: true, removed: true, installedPath, warnings };
  } catch (err: unknown) {
    return {
      kind: 'failure',
      success: false,
      warnings,
      error: asError(err),
    };
  }
}
