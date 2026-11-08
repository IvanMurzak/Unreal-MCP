// `update` — re-sync the UnrealMCP plugin installed in a project against a
// plugin source, reporting the version delta. Reads `VersionName` from each
// side's `UnrealMCP.uplugin` and re-copies when they differ (or always with
// `force`). Library-safe.

import * as fs from 'fs';
import * as path from 'path';
import { listProcesses, selectEditorProcesses, type RunningProcess } from './close.js';
import { compareSemver, installExtension } from './install-extension.js';
import { installPlugin } from './install-plugin.js';
import { cleanPluginBuildCache } from './clean-plugin.js';
import { asError } from '../utils/error.js';
import { isSymlink } from '../utils/fs.js';
import { findUProjectFile } from '../utils/project.js';
import { emitProgress } from './progress.js';
import { PACKAGE_VERSION } from '../version.js';
import { defaultCorePluginSource, resolvePluginSource } from './plugin-source.js';
import { stripLeadingV } from '../utils/extension-source.js';
import { fetchLatestVersion } from '../utils/update-check.js';
import { EXTENSIONS_CATALOG } from '../utils/extensions-catalog.js';
import { loadExtensionsIndex } from '../utils/extensions-index.js';
import type { ProgressCallback } from './types.js';

export interface UpdateOptions {
  projectDir: string;
  /**
   * Optional plugin source to update from. When omitted, the library uses a
   * local repo checkout if present, otherwise the GitHub Release asset matching
   * the CLI package version.
   */
  pluginSourceDir?: string;
  /**
   * `--version` escape hatch: the plugin-source release version to download when no
   * local source is used. Defaults to the CLI's own `PACKAGE_VERSION`.
   */
  version?: string;
  /** Re-install even when versions match. Default `false`. */
  force?: boolean;
  /** Leave the installed extensions as they are. By default a plugin update brings them to their latest versions. */
  noExtensions?: boolean;
  /** Override the extensions index cache location, as `installExtension` does (isolated tests). */
  indexCachePath?: string;
  /** Only report what would change; write nothing. */
  dryRun?: boolean;
  /** Test seam: the running processes, to refuse replacing files under an open editor. */
  listProcessesImpl?: () => RunningProcess[];
  /**
   * Skip wiping the installed plugin's stale UE C++ build cache
   * (`Intermediate/` + C++ `Binaries/`) after a copy-mode update. Cleaning is
   * the DEFAULT (`noClean` omitted/false) so the user always gets a clean
   * recompile of the new code on next editor launch — see issue #58. The
   * bundled sidecar bridge under `Binaries/ThirdParty/` is preserved either
   * way. Junction (dev) installs are never cleaned regardless of this flag.
   */
  noClean?: boolean;
  /** Injectable fetch for tests (download path only). */
  fetchImpl?: typeof fetch;
  /**
   * Test/injection seam for the pinned publisher key the downloaded plugin
   * source's `.minisig` is verified against (see `ResolvePluginSourceOptions`).
   * Production callers leave this unset (the baked-in key is used).
   */
  publicKeyOverride?: string;
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
  /** Plugin names of the installed extensions this update moved to a new version. */
  extensionsUpdated: string[];
  /** The installed extensions checked, as found before any write. */
  extensions: ExtensionUpdateItem[];
  /**
   * `true` when the stale UE build cache was wiped after this update (copy
   * mode, `noClean` not set). `false` for no-op updates, junction installs,
   * or when `noClean` was passed.
   */
  cleaned: boolean;
  installedPath: string;
  warnings: string[];
}

export interface ExtensionUpdateItem {
  pluginName: string;
  extensionId: string;
  from: string | null;
  /** The index's newest version; null when only the bundled catalog knows the extension. */
  to: string | null;
  status: 'outdated' | 'current' | 'unknown';
}

export interface UpdateFailure {
  kind: 'failure';
  success: false;
  warnings: string[];
  error: Error;
}

export type UpdateResult = UpdateSuccess | UpdateFailure;

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
  let cleanupSource: (() => void) | undefined;
  try {
    if (!opts?.projectDir) throw new Error('projectDir is required.');
    const projectDir = path.resolve(opts.projectDir);

    const installedUplugin = path.join(projectDir, 'Plugins', 'UnrealMCP', 'UnrealMCP.uplugin');
    const fromVersion = readPluginVersion(installedUplugin);
    const installedPath = path.join(projectDir, 'Plugins', 'UnrealMCP');
    const localSource =
      (opts.pluginSourceDir ?? '').trim() !== ''
        ? path.resolve(opts.pluginSourceDir!)
        : opts.fetchImpl
          ? null
          : defaultCorePluginSource();
    const downloadVersion = (opts.version ?? '').trim() || PACKAGE_VERSION;
    const toVersion = localSource
      ? readPluginVersion(path.join(localSource, 'UnrealMCP.uplugin'))
      : downloadVersion;

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
    const needsUpdate =
      opts.force === true ||
      fromVersion === null ||
      stripLeadingV(fromVersion ?? '') !== stripLeadingV(toVersion ?? '');
    const extensions = opts.noExtensions ? [] : await planInstalledExtensions(projectDir, opts, warnings);
    if (opts.dryRun) {
      return { kind: 'success', success: true, fromVersion, toVersion, updated: false, extensionsUpdated: [], extensions, cleaned: false, installedPath, warnings };
    }

    // Replacing files under a running editor can leave it with half-loaded modules.
    if (needsUpdate || extensions.some((extension) => extension.status !== 'current')) {
      const running = runningEditors(projectDir, opts);
      if (running) throw new Error(`Close the Unreal Editor first (PID ${running}): it has this project open.`);
    }

    if (!needsUpdate) {
      const extensionsUpdated = await updateInstalledExtensions(projectDir, extensions, opts, warnings);
      emitProgress(opts.onProgress, { phase: 'done', message: 'Plugin already up to date.' });
      return { kind: 'success', success: true, fromVersion, toVersion, updated: false, extensionsUpdated, extensions, cleaned: false, installedPath, warnings };
    }

    const resolvedSource = await resolvePluginSource({
      pluginSourceDir: localSource ?? undefined,
      version: opts.version,
      fetchImpl: opts.fetchImpl,
      publicKeyOverride: opts.publicKeyOverride,
      onProgress: opts.onProgress,
    });
    cleanupSource = resolvedSource.cleanup;

    // Preserve the existing install mode: a dev junction install must stay a
    // junction across `update --force` rather than being silently replaced
    // with a copy (which would detach the project from the live source).
    const installedAsJunction = isSymlink(installedPath);
    const installResult = await installPlugin({
      projectDir,
      pluginSourceDir: resolvedSource.pluginSourceDir,
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

    const extensionsUpdated = await updateInstalledExtensions(projectDir, extensions, opts, warnings);

    emitProgress(opts.onProgress, { phase: 'done', message: 'Plugin updated.' });
    return { kind: 'success', success: true, fromVersion, toVersion, updated: true, extensionsUpdated, extensions, cleaned, installedPath, warnings };
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

function runningEditors(projectDir: string, opts: UpdateOptions): number | null {
  const uprojectPath = findUProjectFile(projectDir);
  if (uprojectPath === null) return null;
  const processes = (opts.listProcessesImpl ?? listProcesses)();
  return selectEditorProcesses(processes, projectDir, uprojectPath)[0]?.pid ?? null;
}

/**
 * The extensions under `Plugins/` and their latest versions. Only known extensions copied into the
 * project qualify: other plugins are not ours, and a symlink is a dev checkout. Best effort: this must
 * never fail a plugin update, so a problem is a warning.
 */
async function planInstalledExtensions(projectDir: string, opts: UpdateOptions, warnings: string[]): Promise<ExtensionUpdateItem[]> {
  try {
    const pluginsDir = path.join(projectDir, 'Plugins');
    if (!fs.existsSync(pluginsDir)) return [];
    const candidates = fs.readdirSync(pluginsDir).filter((name) => {
      const dir = path.join(pluginsDir, name);
      return name !== 'UnrealMCP' && !isSymlink(dir) && fs.existsSync(path.join(dir, `${name}.uplugin`));
    });
    if (candidates.length === 0) return [];

    const { index } = await loadExtensionsIndex({ fetchImpl: opts.fetchImpl, cachePath: opts.indexCachePath });
    const indexed = index?.extensions ?? [];

    return candidates.flatMap((pluginName): ExtensionUpdateItem[] => {
      const matches = (candidate: { pluginName: string }): boolean => candidate.pluginName.toLowerCase() === pluginName.toLowerCase();
      const online = indexed.find(matches);
      const extension = online ?? EXTENSIONS_CATALOG.find(matches);
      if (!extension) return [];

      const from = readPluginVersion(path.join(pluginsDir, pluginName, `${pluginName}.uplugin`));
      const to = online?.version ?? null;
      const status = to === null || from === null
        ? 'unknown'
        : compareSemver(stripLeadingV(from), stripLeadingV(to)) < 0 ? 'outdated' : 'current';
      return [{ pluginName, extensionId: extension.extensionId, from, to, status }];
    });
  } catch (err: unknown) {
    warnings.push(`Could not check the installed extensions: ${asError(err).message}`);
    return [];
  }
}

/** Apply the plan. One the user disabled stays disabled. The plugin update already succeeded, so a failure is a warning. */
async function updateInstalledExtensions(
  projectDir: string,
  extensions: readonly ExtensionUpdateItem[],
  opts: UpdateOptions,
  warnings: string[],
): Promise<string[]> {
  const updated: string[] = [];
  for (const extension of extensions.filter((candidate) => candidate.status !== 'current')) {
    const result = await installExtension({
      projectDir,
      extensionId: extension.extensionId,
      fetchImpl: opts.fetchImpl,
      indexCachePath: opts.indexCachePath,
      keepDisabled: true,
      onProgress: opts.onProgress,
    });
    if (result.kind === 'failure') {
      warnings.push(`Could not update extension ${extension.pluginName}: ${result.error.message}`);
      continue;
    }
    warnings.push(...result.warnings);
    if (result.outcome === 'updated') updated.push(extension.pluginName);
  }
  return updated;
}

/** The nearest folder at or above `start` holding a `.uproject`, or null. */
export function findUnrealProjectRoot(start: string): string | null {
  for (let dir = path.resolve(start); ; dir = path.dirname(dir)) {
    if (findUProjectFile(dir) !== null) return dir;
    if (dir === path.dirname(dir)) return null;
  }
}

export type ProjectUpdateStatus = 'outdated' | 'current' | 'unknown';

export interface ProjectUpdateItem {
  kind: 'plugin' | 'extension';
  name: string;
  from: string | null;
  to: string | null;
  status: ProjectUpdateStatus;
}

export interface UpdateProjectSuccess {
  kind: 'success';
  success: true;
  /** Everything checked, as found before any write. */
  items: ProjectUpdateItem[];
  /** The items this call moved to a new version (always empty with `dryRun`). */
  updated: ProjectUpdateItem[];
  warnings: string[];
  nextSteps: string[];
}

export type UpdateProjectResult = UpdateProjectSuccess | UpdateFailure;

/**
 * Bring an Unreal project's UnrealMCP plugin and its installed extensions to their latest versions. The
 * one function behind `unreal-mcp-cli update`. A project without the plugin under `Plugins/` (a Fab
 * install, which the Epic Games Launcher updates) is left alone unless `force`. Library-safe.
 */
export async function updateProject(opts: UpdateOptions): Promise<UpdateProjectResult> {
  const warnings: string[] = [];
  const projectDir = path.resolve(opts?.projectDir ?? '');
  const hasPlugin = readPluginVersion(path.join(projectDir, 'Plugins', 'UnrealMCP', 'UnrealMCP.uplugin')) !== null;
  if (!hasPlugin && !opts?.force) return { kind: 'success', success: true, items: [], updated: [], warnings, nextSteps: [] };

  let version = opts.version;
  if (!version && !opts.pluginSourceDir) {
    try {
      version = await fetchLatestVersion();
    } catch (err: unknown) {
      warnings.push(`Could not look up the latest plugin, using this CLI's: ${asError(err).message}`);
    }
  }

  const result = await update({ ...opts, version });
  if (result.kind === 'failure') return { ...result, warnings: [...warnings, ...result.warnings] };

  const { fromVersion: from, toVersion: to } = result;
  const pluginItem: ProjectUpdateItem = {
    kind: 'plugin',
    name: 'UnrealMCP plugin',
    from,
    to,
    status: from === null || to === null
      ? 'unknown'
      : compareSemver(stripLeadingV(from), stripLeadingV(to)) < 0 ? 'outdated' : 'current',
  };
  const extensionItems = result.extensions.map((extension): ProjectUpdateItem => ({
    kind: 'extension',
    name: extension.pluginName,
    from: extension.from,
    to: extension.to,
    status: extension.status,
  }));
  const updated = [
    ...(result.updated ? [pluginItem] : []),
    ...extensionItems.filter((item) => result.extensionsUpdated.includes(item.name)),
  ];

  return {
    kind: 'success',
    success: true,
    items: [pluginItem, ...extensionItems],
    updated,
    warnings: [...warnings, ...result.warnings],
    nextSteps: updated.length > 0 ? ['Open the project in the Unreal Editor: it recompiles the new plugin source.'] : [],
  };
}
