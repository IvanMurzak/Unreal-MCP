// `install-plugin` / `remove-plugin` — place (or junction) the UnrealMCP
// plugin under `<project>/Plugins/UnrealMCP`. Library-safe.
//
// Copy mode recursively copies the plugin source. Junction mode (Windows,
// dev) creates a directory junction so the project always tracks the live
// plugin source — the same pattern the infra testbed uses.

import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import { isUnrealProjectDir } from '../utils/project.js';
import { emitProgress } from './progress.js';
import type {
  InstallPluginOptions,
  InstallPluginResult,
  RemovePluginOptions,
  RemovePluginResult,
} from './types.js';

const PLUGIN_DIRNAME = 'UnrealMCP';
/** Subtree under `Binaries/` holding the bundled bridge (ARCHITECTURE §6.1). */
const BUNDLED_BRIDGE_REL = path.join('Binaries', 'ThirdParty');

/** True when `p` is a symlink/junction (vs a real directory). */
function isLink(p: string): boolean {
  try {
    return fs.lstatSync(p).isSymbolicLink();
  } catch {
    return false;
  }
}

export async function installPlugin(opts: InstallPluginOptions): Promise<InstallPluginResult> {
  const warnings: string[] = [];
  try {
    if (!opts?.projectDir) throw new Error('projectDir is required.');
    if (!opts?.pluginSourceDir) throw new Error('pluginSourceDir is required.');

    const projectDir = path.resolve(opts.projectDir);
    const pluginSourceDir = path.resolve(opts.pluginSourceDir);
    if (!fs.existsSync(projectDir)) throw new Error(`Project directory does not exist: ${projectDir}`);
    // Guard against a wrong-cwd run silently scaffolding Plugins/UnrealMCP in
    // an arbitrary directory (consistent with `close`/`status`, which key on
    // a `.uproject`). Warn rather than refuse — installing into a not-yet-
    // initialised project tree is a valid, if rarer, flow.
    if (!isUnrealProjectDir(projectDir)) {
      warnings.push(`No .uproject found in ${projectDir} — is this an Unreal project directory?`);
    }
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

    const useJunction = opts.junction === true && process.platform === 'win32';
    if (opts.junction === true && !useJunction) {
      warnings.push('Junction mode is Windows-only; falling back to copy.');
    }

    // Preserve the bundled sidecar bridge across a COPY re-install. A prior
    // (release-packaged) install ships `Binaries/ThirdParty/UnrealMcpBridge/`,
    // but the plugin SOURCE we copy from (a dev/source checkout) does not — so
    // a naive rm-then-copy would strip the bridge and leave the plugin unable
    // to spawn its sidecar (ARCHITECTURE §6). Stash the existing bridge subtree
    // to a temp dir before clearing, then restore it after the copy IF the new
    // source did not provide its own. Junction installs never go through this
    // path (the junction points at the live source — nothing to preserve).
    let stashedBridge: string | null = null;
    const priorBridge = path.join(installedPath, BUNDLED_BRIDGE_REL);
    const installedIsRealDir = fs.existsSync(installedPath) && !isLink(installedPath);
    if (!useJunction && installedIsRealDir && fs.existsSync(priorBridge)) {
      stashedBridge = fs.mkdtempSync(path.join(os.tmpdir(), 'unreal-mcp-bridge-stash-'));
      fs.cpSync(priorBridge, stashedBridge, { recursive: true });
    }

    // Clear any prior install (link or real dir) so the operation is
    // idempotent and never nests a copy inside a stale junction.
    if (fs.existsSync(installedPath) || isLink(installedPath)) {
      if (isLink(installedPath)) {
        fs.unlinkSync(installedPath);
      } else {
        fs.rmSync(installedPath, { recursive: true, force: true });
      }
    }

    if (useJunction) {
      fs.symlinkSync(pluginSourceDir, installedPath, 'junction');
      emitProgress(opts.onProgress, { phase: 'done', message: 'Plugin junctioned.' });
      return { kind: 'success', success: true, installedPath, mode: 'junction', warnings };
    }

    fs.cpSync(pluginSourceDir, installedPath, { recursive: true });

    // Restore the stashed bridge if the freshly-copied source did not ship one.
    if (stashedBridge) {
      try {
        if (!fs.existsSync(priorBridge)) {
          fs.mkdirSync(path.dirname(priorBridge), { recursive: true });
          fs.cpSync(stashedBridge, priorBridge, { recursive: true });
          warnings.push('Preserved the previously-bundled sidecar bridge across the re-install.');
        }
      } finally {
        fs.rmSync(stashedBridge, { recursive: true, force: true });
      }
    }

    emitProgress(opts.onProgress, { phase: 'done', message: 'Plugin copied.' });
    return { kind: 'success', success: true, installedPath, mode: 'copy', warnings };
  } catch (err: unknown) {
    return {
      kind: 'failure',
      success: false,
      warnings,
      error: err instanceof Error ? err : new Error(String(err)),
    };
  }
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

    const present = fs.existsSync(installedPath) || isLink(installedPath);
    if (!present) {
      emitProgress(opts.onProgress, { phase: 'done', message: 'Plugin was not installed.' });
      return { kind: 'success', success: true, removed: false, installedPath, warnings };
    }

    // unlink a junction (never recurse through it into the live source);
    // rm a real directory.
    if (isLink(installedPath)) {
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
      error: err instanceof Error ? err : new Error(String(err)),
    };
  }
}
