// Resolve/materialize the CORE UnrealMCP plugin source for `install-plugin` /
// `update`. The npm package does NOT bundle the C++ plugin, so the default path
// is:
//   1. explicit local `pluginSourceDir`
//   2. local repo checkout (`UnrealMCP/`)
//   3. GitHub Release SOURCE asset that matches THIS CLI's package version
//
// The version coupling is intentional: Unreal-MCP plugin / bridge / CLI share
// one semver and are released together, so the published CLI can safely fetch
// `unreal-mcp-plugin-source-<PACKAGE_VERSION>.zip`.

import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import { fileURLToPath } from 'url';
import { unzipSync } from 'fflate';
import { emitProgress } from './progress.js';
import { PACKAGE_VERSION } from '../version.js';
import { defaultPluginSource } from '../utils/repo.js';
import {
  TRUSTED_DOWNLOAD_HOST,
  assertTrustedDownloadUrl,
  releaseTag,
  stripLeadingV,
} from '../utils/extension-source.js';
import type { ProgressCallback } from './types.js';

const HERE = path.dirname(fileURLToPath(import.meta.url));

/** GitHub repo releasing the core UnrealMCP source-install asset. */
export const CORE_PLUGIN_SOURCE_RELEASE_REPO = 'IvanMurzak/Unreal-MCP';
/** Release-asset basename for the source-install core plugin asset. */
export const CORE_PLUGIN_SOURCE_ZIP_BASENAME = 'unreal-mcp-plugin-source';

export interface ResolvePluginSourceOptions {
  /**
   * Explicit local plugin source. Accepts either the `UnrealMCP/` dir itself, or
   * any parent dir that contains a `.uplugin` beneath it.
   */
  pluginSourceDir?: string;
  /** Injectable fetch for tests. Defaults to global `fetch`. */
  fetchImpl?: typeof fetch;
  onProgress?: ProgressCallback;
}

export interface ResolvedPluginSource {
  /** Absolute path to the `UnrealMCP/` plugin dir to install from. */
  pluginSourceDir: string;
  /** Where the source came from. */
  sourceKind: 'local' | 'github-release';
  /** Best-effort cleanup hook (no-op for local sources). */
  cleanup: () => void;
}

/** `unreal-mcp-plugin-source-<version>.zip` (asset uses the bare version, not `v`). */
export function corePluginSourceAssetName(version: string = PACKAGE_VERSION): string {
  return `${CORE_PLUGIN_SOURCE_ZIP_BASENAME}-${stripLeadingV(version)}.zip`;
}

/**
 * Release-asset URL for the dedicated core-plugin SOURCE asset, version-locked
 * to the CLI unless the caller overrides it for tests.
 */
export function corePluginSourceDownloadUrl(version: string = PACKAGE_VERSION): string {
  return `https://${TRUSTED_DOWNLOAD_HOST}/${CORE_PLUGIN_SOURCE_RELEASE_REPO}/releases/download/${releaseTag(version)}/${corePluginSourceAssetName(version)}`;
}

/** Resolve the nearest repo checkout's `UnrealMCP/` dir from this package, if any. */
export function defaultCorePluginSource(): string | null {
  return defaultPluginSource(HERE);
}

/**
 * Resolve a local source path for the core plugin. Preserve the current
 * install/update semantics for explicit local dirs: if the dir exists we accept
 * it even when it lacks `UnrealMCP.uplugin`, so the caller can still install the
 * contents and surface a warning later. When the path is a repo/workspace parent
 * that *does* contain the plugin dir, prefer that nested `UnrealMCP/`.
 */
export function resolveLocalPluginRoot(source: string): string {
  const resolved = path.resolve(source);
  if (!fs.existsSync(resolved)) {
    throw new Error(`Plugin source directory does not exist: ${resolved}`);
  }
  if (!fs.statSync(resolved).isDirectory()) {
    throw new Error(`Plugin source is not a directory: ${resolved}`);
  }
  if (fs.existsSync(path.join(resolved, 'UnrealMCP.uplugin'))) return resolved;
  const nestedCore = path.join(resolved, 'UnrealMCP');
  if (fs.existsSync(path.join(nestedCore, 'UnrealMCP.uplugin'))) return nestedCore;
  const found = findUPluginFile(resolved);
  return found ? path.dirname(found) : resolved;
}

/** Find the shallowest `*.uplugin` beneath `root`, breadth-first. */
export function findUPluginFile(root: string): string | null {
  const queue: string[] = [path.resolve(root)];
  while (queue.length > 0) {
    const dir = queue.shift()!;
    let entries: fs.Dirent[];
    try {
      entries = fs.readdirSync(dir, { withFileTypes: true });
    } catch {
      continue;
    }
    const file = entries
      .filter((entry) => entry.isFile() && entry.name.toLowerCase().endsWith('.uplugin'))
      .map((entry) => path.join(dir, entry.name))
      .sort()[0];
    if (file) return file;
    for (const entry of entries.filter((item) => item.isDirectory()).sort((a, b) => a.name.localeCompare(b.name))) {
      queue.push(path.join(dir, entry.name));
    }
  }
  return null;
}

/**
 * Resolve the core plugin source for install/update. Falls back to the dedicated
 * GitHub Release SOURCE asset that matches THIS CLI's version when no local
 * source exists.
 */
export async function resolvePluginSource(
  opts: ResolvePluginSourceOptions = {},
): Promise<ResolvedPluginSource> {
  const explicit = (opts.pluginSourceDir ?? '').trim();
  if (explicit !== '') {
    return {
      pluginSourceDir: resolveLocalPluginRoot(explicit),
      sourceKind: 'local',
      cleanup: () => {},
    };
  }

  // `fetchImpl` is a test seam; when it is injected we intentionally suppress
  // the repo fallback so tests can force the download path from inside this repo.
  const repoSource = opts.fetchImpl ? null : defaultCorePluginSource();
  if (repoSource) {
    return {
      pluginSourceDir: repoSource,
      sourceKind: 'local',
      cleanup: () => {},
    };
  }

  const version = PACKAGE_VERSION;
  const url = corePluginSourceDownloadUrl(version);
  assertTrustedDownloadUrl(url);
  emitProgress(opts.onProgress, {
    phase: 'info',
    message: `Downloading UnrealMCP plugin ${version} from ${url}`,
  });

  const fetchImpl = opts.fetchImpl ?? fetch;
  const response = await fetchImpl(url);
  if (!response.ok) {
    throw new Error(
      `Failed to download UnrealMCP plugin ${version}: HTTP ${response.status} ${response.statusText} from ${url}. ` +
        `Verify the ${releaseTag(version)} release exists, or pass --plugin-source <dir> for an offline/dev install.`,
    );
  }
  const zipBytes = new Uint8Array(await response.arrayBuffer());
  emitProgress(opts.onProgress, {
    phase: 'info',
    message: `Extracting ${corePluginSourceAssetName(version)}`,
  });

  const stagingDir = fs.mkdtempSync(path.join(os.tmpdir(), 'unreal-mcp-plugin-'));
  try {
    const entries = unzipSync(zipBytes);
    for (const [entryName, data] of Object.entries(entries)) {
      if (entryName.endsWith('/')) continue;
      const target = path.join(stagingDir, entryName);
      if (!path.resolve(target).startsWith(path.resolve(stagingDir) + path.sep)) {
        throw new Error(`Refusing to extract suspicious zip entry outside the staging dir: ${entryName}`);
      }
      fs.mkdirSync(path.dirname(target), { recursive: true });
      fs.writeFileSync(target, data);
    }

    const pluginFile = findUPluginFile(stagingDir);
    if (!pluginFile) {
      throw new Error(
        `Downloaded plugin archive ${corePluginSourceAssetName(version)} did not contain an UnrealMCP.uplugin descriptor.`,
      );
    }
    let descriptor: Record<string, unknown>;
    try {
      descriptor = JSON.parse(fs.readFileSync(pluginFile, 'utf-8')) as Record<string, unknown>;
    } catch (err: unknown) {
      const message = err instanceof Error ? err.message : String(err);
      throw new Error(
        `Downloaded plugin archive ${corePluginSourceAssetName(version)} contained an unreadable UnrealMCP.uplugin descriptor: ${message}`,
      );
    }
    const engineVersion = typeof descriptor['EngineVersion'] === 'string' ? descriptor['EngineVersion'].trim() : '';
    if (engineVersion !== '') {
      throw new Error(
        `Downloaded plugin asset ${corePluginSourceAssetName(version)} is not a source install bundle: UnrealMCP.uplugin pins EngineVersion '${engineVersion}'. ` +
          `Verify the ${releaseTag(version)} release published ${corePluginSourceAssetName(version)}, or pass --plugin-source <dir> for an offline/dev install.`,
      );
    }

    return {
      pluginSourceDir: path.dirname(pluginFile),
      sourceKind: 'github-release',
      cleanup: () => {
        fs.rmSync(stagingDir, { recursive: true, force: true });
      },
    };
  } catch (err: unknown) {
    fs.rmSync(stagingDir, { recursive: true, force: true });
    throw err;
  }
}
