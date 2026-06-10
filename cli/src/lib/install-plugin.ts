// `install-plugin` / `remove-plugin` — place (or junction) the UnrealMCP
// plugin under `<project>/Plugins/UnrealMCP`. Library-safe.
//
// Copy mode recursively copies the plugin source. Junction mode (Windows,
// dev) creates a directory junction so the project always tracks the live
// plugin source — the same pattern the infra testbed uses.

import * as fs from 'fs';
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

    // Clear any prior install (link or real dir) so the operation is
    // idempotent and never nests a copy inside a stale junction.
    if (fs.existsSync(installedPath) || isLink(installedPath)) {
      if (isLink(installedPath)) {
        fs.unlinkSync(installedPath);
      } else {
        fs.rmSync(installedPath, { recursive: true, force: true });
      }
    }

    const useJunction = opts.junction === true;
    if (useJunction) {
      if (process.platform !== 'win32') {
        warnings.push('Junction mode is Windows-only; falling back to copy.');
      } else {
        fs.symlinkSync(pluginSourceDir, installedPath, 'junction');
        emitProgress(opts.onProgress, { phase: 'done', message: 'Plugin junctioned.' });
        return { kind: 'success', success: true, installedPath, mode: 'junction', warnings };
      }
    }

    fs.cpSync(pluginSourceDir, installedPath, { recursive: true });
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
