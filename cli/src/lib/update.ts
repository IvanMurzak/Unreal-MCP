// `update` — re-sync the UnrealMCP plugin installed in a project against a
// plugin source, reporting the version delta. Reads `VersionName` from each
// side's `UnrealMCP.uplugin` and re-copies when they differ (or always with
// `force`). Library-safe.

import * as fs from 'fs';
import * as path from 'path';
import { installPlugin } from './install-plugin.js';
import { cleanPluginBuildCache } from './clean-plugin.js';
import { emitProgress } from './progress.js';
import type { ProgressCallback } from './types.js';

export interface UpdateOptions {
  projectDir: string;
  /** Plugin source to update from. */
  pluginSourceDir: string;
  /** Re-install even when versions match. Default `false`. */
  force?: boolean;
  /**
   * Skip wiping the installed plugin's stale UE C++ build cache
   * (`Intermediate/` + C++ `Binaries/`) after a copy-mode update. Cleaning is
   * the DEFAULT (`noClean` omitted/false) so the user always gets a clean
   * recompile of the new code on next editor launch — see issue #58. The
   * bundled sidecar bridge under `Binaries/ThirdParty/` is preserved either
   * way. Junction (dev) installs are never cleaned regardless of this flag.
   */
  noClean?: boolean;
  onProgress?: ProgressCallback;
}

export interface UpdateSuccess {
  kind: 'success';
  success: true;
  /** Version before the update (null when not installed). */
  fromVersion: string | null;
  /** Version after the update. */
  toVersion: string | null;
  /** `true` when files were re-copied. */
  updated: boolean;
  /**
   * `true` when the stale UE build cache was wiped after this update (copy
   * mode, `noClean` not set). `false` for no-op updates, junction installs,
   * or when `noClean` was passed.
   */
  cleaned: boolean;
  installedPath: string;
  warnings: string[];
}

export interface UpdateFailure {
  kind: 'failure';
  success: false;
  warnings: string[];
  error: Error;
}

export type UpdateResult = UpdateSuccess | UpdateFailure;

/** True when `p` is an existing symlink/junction (vs a real directory). */
function isJunction(p: string): boolean {
  try {
    return fs.lstatSync(p).isSymbolicLink();
  } catch {
    return false;
  }
}

/** Read `VersionName` from a `UnrealMCP.uplugin`. Pure. Returns null on miss. */
export function readPluginVersion(upluginPath: string): string | null {
  if (!fs.existsSync(upluginPath)) return null;
  try {
    const parsed = JSON.parse(fs.readFileSync(upluginPath, 'utf-8')) as Record<string, unknown>;
    const v = parsed['VersionName'];
    return typeof v === 'string' ? v : null;
  } catch {
    return null;
  }
}

export async function update(opts: UpdateOptions): Promise<UpdateResult> {
  const warnings: string[] = [];
  try {
    if (!opts?.projectDir) throw new Error('projectDir is required.');
    if (!opts?.pluginSourceDir) throw new Error('pluginSourceDir is required.');
    const projectDir = path.resolve(opts.projectDir);
    const pluginSourceDir = path.resolve(opts.pluginSourceDir);

    const installedUplugin = path.join(projectDir, 'Plugins', 'UnrealMCP', 'UnrealMCP.uplugin');
    const sourceUplugin = path.join(pluginSourceDir, 'UnrealMCP.uplugin');
    const fromVersion = readPluginVersion(installedUplugin);
    const toVersion = readPluginVersion(sourceUplugin);
    const installedPath = path.join(projectDir, 'Plugins', 'UnrealMCP');

    emitProgress(opts.onProgress, {
      phase: 'start',
      message: `Updating plugin (installed=${fromVersion ?? 'none'}, source=${toVersion ?? 'unknown'})`,
    });

    // An unreadable source `.uplugin` (e.g. a typo'd `--plugin-source`) reads
    // back as `toVersion === null`; surface that rather than silently treating
    // it as a no-op.
    if (toVersion === null) {
      warnings.push(
        'Could not read VersionName from the plugin source UnrealMCP.uplugin; proceeding with install.',
      );
    }
    // A plugin that is NOT installed (`fromVersion === null`) must always be
    // installed: `null !== null` is `false`, so without the explicit
    // `fromVersion === null` clause an uninstalled plugin against an unreadable
    // source would short-circuit to "already up to date" and never install.
    const needsUpdate = opts.force === true || fromVersion === null || fromVersion !== toVersion;
    if (!needsUpdate) {
      emitProgress(opts.onProgress, { phase: 'done', message: 'Plugin already up to date.' });
      return { kind: 'success', success: true, fromVersion, toVersion, updated: false, cleaned: false, installedPath, warnings };
    }

    // Preserve the existing install mode: a dev junction install must stay a
    // junction across `update --force` rather than being silently replaced
    // with a copy (which would detach the project from the live source).
    const installedAsJunction = isJunction(installedPath);
    const installResult = await installPlugin({
      projectDir,
      pluginSourceDir,
      junction: installedAsJunction,
      onProgress: opts.onProgress,
    });
    if (installResult.kind === 'failure') {
      return { kind: 'failure', success: false, warnings, error: installResult.error };
    }
    warnings.push(...installResult.warnings);

    // Wipe the stale UE C++ build cache so the editor recompiles the new code
    // on next launch (issue #58). DEFAULT on copy-mode updates; opt out with
    // `noClean`. NEVER on junction installs — that would recurse through the
    // junction and destroy the live dev source's build outputs. `installPlugin`
    // already falls back to copy on non-Windows even when junction is requested,
    // but the post-install path is a real dir only in true copy mode, so gate
    // on the resolved install mode rather than the requested one.
    let cleaned = false;
    // `installResult` is guaranteed a success here — failure returned above.
    const resolvedMode = installResult.mode;
    if (!opts.noClean && resolvedMode === 'copy') {
      const cleanResult = await cleanPluginBuildCache({ installedPath, onProgress: opts.onProgress });
      warnings.push(...cleanResult.warnings);
      if (cleanResult.kind === 'failure') {
        // A failed clean must not fail the whole update — the source is already
        // copied. Surface it as a warning so the user can clean manually.
        warnings.push(`Could not clean stale build cache: ${cleanResult.error.message}`);
      } else {
        // `cleaned` means "a clean was ensured this update", not "files were
        // found to delete" — `installPlugin`'s copy-mode rm already strips the
        // old install (stale cache included; bridge stashed+restored), so the
        // explicit clean often finds nothing yet still guarantees a clean tree.
        cleaned = true;
      }
    } else if (!opts.noClean && resolvedMode === 'junction') {
      warnings.push(
        'Junction (dev) install detected — skipping build-cache clean to protect the live source outputs.',
      );
    }

    emitProgress(opts.onProgress, { phase: 'done', message: 'Plugin updated.' });
    return { kind: 'success', success: true, fromVersion, toVersion, updated: true, cleaned, installedPath, warnings };
  } catch (err: unknown) {
    return {
      kind: 'failure',
      success: false,
      warnings,
      error: err instanceof Error ? err : new Error(String(err)),
    };
  }
}
