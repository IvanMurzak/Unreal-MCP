import * as fs from 'node:fs';
import * as os from 'node:os';
import * as path from 'node:path';
import { randomUUID } from 'node:crypto';
import type { ExtensionDescriptor } from './extensions-catalog.js';

export const EXTENSIONS_INDEX_URL =
  'https://github.com/IvanMurzak/Unreal-MCP/releases/download/extensions-index/extensions-index.json';

export interface IndexedRelease {
  readonly repo: string;
  readonly tag: string;
  readonly sha256?: string;
  readonly minCoreVersion?: string | null;
  readonly enginePlugins?: readonly string[];
}

export interface IndexedExtension extends ExtensionDescriptor {
  readonly releases: Readonly<Record<string, IndexedRelease>>;
}

export interface ExtensionsIndex {
  readonly schemaVersion: 1;
  readonly extensions: readonly IndexedExtension[];
}

const VERSION = /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/;
const NAME = /^[A-Za-z][A-Za-z0-9]*$/;
const REPO = /^IvanMurzak\/(Unreal-MCP|Unreal-AI-[A-Za-z0-9-]+)$/;

/** Reject malformed indices before either installation or caching. No executable paths/URLs. */
export function parseExtensionsIndex(value: unknown): ExtensionsIndex {
  const index = value as ExtensionsIndex;
  if (!index || index.schemaVersion !== 1 || !Array.isArray(index.extensions)) {
    throw new Error('Unsupported extensions index schema.');
  }
  const ids = new Set<string>();
  const plugins = new Set<string>();
  for (const e of index.extensions as readonly IndexedExtension[]) {
    if (!e || typeof e.extensionId !== 'string' || !/^[a-z0-9]+(?:[.-][a-z0-9]+)+$/.test(e.extensionId)
      || typeof e.pluginName !== 'string' || !NAME.test(e.pluginName)
      || typeof e.name !== 'string' || typeof e.description !== 'string'
      || typeof e.version !== 'string' || !VERSION.test(e.version)
      || (e.minCoreVersion !== null && (typeof e.minCoreVersion !== 'string' || !VERSION.test(e.minCoreVersion)))
      || !Array.isArray(e.enginePlugins) || !e.enginePlugins.every(p => typeof p === 'string' && NAME.test(p))
      || !Array.isArray(e.tools) || !e.tools.every(t => t && typeof t.name === 'string' && typeof t.description === 'string')
      || !e.releases || typeof e.releases !== 'object' || Array.isArray(e.releases)) {
      throw new Error('Invalid extension descriptor in index.');
    }
    if (ids.has(e.extensionId) || plugins.has(e.pluginName.toLowerCase())) {
      throw new Error('Duplicate extension identity in index.');
    }
    ids.add(e.extensionId);
    plugins.add(e.pluginName.toLowerCase());
    for (const [version, release] of Object.entries(e.releases)) {
      if (!VERSION.test(version) || !release || typeof release.repo !== 'string' || !REPO.test(release.repo)
        || typeof release.tag !== 'string'
        || !(release.tag === `v${version}` || /^[a-z][a-z0-9-]*-v/.test(release.tag) && release.tag.endsWith(`-v${version}`))
        || !/^[A-Za-z0-9.-]+$/.test(release.tag)
        || (release.minCoreVersion !== undefined && release.minCoreVersion !== null && !VERSION.test(release.minCoreVersion))
        || (release.enginePlugins !== undefined && (!Array.isArray(release.enginePlugins) || !release.enginePlugins.every((p: unknown) => typeof p === 'string' && NAME.test(p))))
        || (release.sha256 !== undefined && !/^[a-f0-9]{64}$/.test(release.sha256))) {
        throw new Error('Invalid extension release in index.');
      }
    }
    if (!Object.prototype.hasOwnProperty.call(e.releases, e.version) || e.repo !== e.releases[e.version].repo) {
      throw new Error('Latest extension release is missing or inconsistent.');
    }
  }
  return index;
}

export async function loadExtensionsIndex(opts: {
  fetchImpl?: typeof fetch;
  cachePath?: string;
  timeoutMs?: number;
}): Promise<{ index: ExtensionsIndex | null; warning?: string }> {
  const cachePath = opts.cachePath ?? path.join(os.homedir(), '.cache', 'unreal-mcp-cli', 'extensions-index.json');
  try {
    const response = await (opts.fetchImpl ?? fetch)(EXTENSIONS_INDEX_URL, {
      signal: AbortSignal.timeout(opts.timeoutMs ?? 10_000),
    });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const text = await response.text();
    if (text.length > 2_000_000) throw new Error('Extensions index is too large.');
    const index = parseExtensionsIndex(JSON.parse(text));
    const temporary = `${cachePath}.${randomUUID()}.tmp`;
    try {
      fs.mkdirSync(path.dirname(cachePath), { recursive: true });
      fs.writeFileSync(temporary, JSON.stringify(index));
      fs.renameSync(temporary, cachePath);
    } catch {
      // A read-only cache must not prevent an online install.
    } finally {
      try { fs.rmSync(temporary, { force: true }); } catch { /* Best-effort private cache cleanup. */ }
    }
    return { index };
  } catch (error) {
    const reason = error instanceof Error ? error.message : String(error);
    try {
      const index = parseExtensionsIndex(JSON.parse(fs.readFileSync(cachePath, 'utf8')));
      return { index, warning: `Could not refresh extensions index (${reason}); using cached versions. Newer releases may exist.` };
    } catch {
      return { index: null, warning: `Could not load extensions index (${reason}); using the bundled legacy catalog.` };
    }
  }
}
