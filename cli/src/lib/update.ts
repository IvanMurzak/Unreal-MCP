// `update` — re-sync the UnrealMCP plugin installed in a project against a
// plugin source, reporting the version delta. Reads `VersionName` from each
// side's `UnrealMCP.uplugin` and re-copies when they differ (or always with
// `force`). Library-safe.

import * as fs from 'fs';
import * as path from 'path';
import { installPlugin } from './install-plugin.js';
import { emitProgress } from './progress.js';
import type { ProgressCallback } from './types.js';

export interface UpdateOptions {
  projectDir: string;
  /** Plugin source to update from. */
  pluginSourceDir: string;
  /** Re-install even when versions match. Default `false`. */
  force?: boolean;
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
      return { kind: 'success', success: true, fromVersion, toVersion, updated: false, installedPath, warnings };
    }

    // Preserve the existing install mode: a dev junction install must stay a
    // junction across `update --force` rather than being silently replaced
    // with a copy (which would detach the project from the live source).
    const installResult = await installPlugin({
      projectDir,
      pluginSourceDir,
      junction: isJunction(installedPath),
      onProgress: opts.onProgress,
    });
    if (installResult.kind === 'failure') {
      return { kind: 'failure', success: false, warnings, error: installResult.error };
    }
    warnings.push(...installResult.warnings);

    emitProgress(opts.onProgress, { phase: 'done', message: 'Plugin updated.' });
    return { kind: 'success', success: true, fromVersion, toVersion, updated: true, installedPath, warnings };
  } catch (err: unknown) {
    return {
      kind: 'failure',
      success: false,
      warnings,
      error: err instanceof Error ? err : new Error(String(err)),
    };
  }
}
