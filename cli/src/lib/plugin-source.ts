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
import {
  MINISIGN_PUBLIC_KEY,
  PLUGIN_SOURCE_SIGNATURE_ASSET_SUFFIX,
  verifyMinisign,
  signatureFailureReason,
} from './plugin-signature.js';
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
  /**
   * `--version` escape hatch: the plugin-source release version to download.
   * Defaults to this CLI's own `PACKAGE_VERSION` (the version-locked default —
   * plugin/bridge/cli share one semver and release together). Ignored when a local
   * `pluginSourceDir` is used.
   */
  version?: string;
  /** Injectable fetch for tests. Defaults to global `fetch`. */
  fetchImpl?: typeof fetch;
  /**
   * Test/injection seam for the pinned publisher key the downloaded source zip's
   * `.minisig` is verified against. Defaults to the baked-in `MINISIGN_PUBLIC_KEY`.
   * Production callers leave this unset; tests inject an ephemeral keypair's public
   * key so the verify-before-extract gate can be exercised without the real key.
   */
  publicKeyOverride?: string;
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

/** `unreal-mcp-plugin-source-<version>.zip.minisig` — the detached signature sibling asset. */
export function corePluginSourceSignatureAssetName(version: string = PACKAGE_VERSION): string {
  return `${corePluginSourceAssetName(version)}${PLUGIN_SOURCE_SIGNATURE_ASSET_SUFFIX}`;
}

/**
 * Release-asset URL of the detached minisign signature for the source zip — the
 * SIBLING of `corePluginSourceDownloadUrl` under the SAME `v<version>` release
 * tag. Verified against the pinned publisher key BEFORE extraction (fail-closed).
 */
export function corePluginSourceSignatureUrl(version: string = PACKAGE_VERSION): string {
  return `${corePluginSourceDownloadUrl(version)}${PLUGIN_SOURCE_SIGNATURE_ASSET_SUFFIX}`;
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

/** Attempts (1 initial + retries) for the `.minisig` signature fetch before fail-closed. */
const SIGNATURE_FETCH_ATTEMPTS = 3;

/**
 * Fetch the detached `.minisig` signature text with a bounded transient-retry
 * (a just-published release can briefly 404 a sibling asset), reusing the
 * injectable `fetchImpl`. Returns the signature body, or `null` when every
 * attempt failed — the fail-closed signal an unverified zip is never extracted.
 * Never throws.
 */
async function fetchSignatureText(url: string, fetchImpl: typeof fetch): Promise<string | null> {
  for (let attempt = 1; attempt <= SIGNATURE_FETCH_ATTEMPTS; attempt++) {
    try {
      const response = await fetchImpl(url);
      if (response.ok) return await response.text();
    } catch {
      // transient — retried below; a persistent failure fails closed (null).
    }
  }
  return null;
}

/**
 * Resolve the core plugin source for install/update. Falls back to the dedicated
 * GitHub Release SOURCE asset that matches THIS CLI's version when no local
 * source exists.
 *
 * The downloaded zip is SIGNATURE-VERIFIED against the pinned publisher key BEFORE
 * it is extracted (D12): the `.minisig` sibling asset is fetched and checked with
 * `verifyMinisign` — a tampered/unsigned/wrong-key zip is rejected fail-closed and
 * NEVER installed. A local `pluginSourceDir` (explicit `--plugin-source` or the
 * in-repo checkout) is a trusted source and skips verification.
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

  const version = (opts.version ?? '').trim() || PACKAGE_VERSION;
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

  // FAIL-CLOSED SIGNATURE GATE (verify-before-extract, D12). The zip bytes are in
  // hand but UNTRUSTED. Fetch the detached `.minisig` sibling asset and verify it
  // against the pinned publisher key BEFORE unzipSync/extract. A same-origin
  // SHA256 would be integrity-only (a release-asset attacker replaces the zip AND
  // its checksum); a signature over an offline-held key cannot be forged. On ANY
  // non-`verified` verdict — missing/malformed signature, wrong key, tampered zip,
  // or an un-provisioned pinned key — we throw WITHOUT extracting: an unverified
  // plugin source is NEVER installed. `--plugin-source <dir>` (a trusted local
  // source) is the offline/dev escape hatch and does not reach this branch.
  const publicKey = opts.publicKeyOverride ?? MINISIGN_PUBLIC_KEY;
  const signatureUrl = corePluginSourceSignatureUrl(version);
  assertTrustedDownloadUrl(signatureUrl);
  const signatureText = await fetchSignatureText(signatureUrl, fetchImpl);
  const signatureAssetName = corePluginSourceSignatureAssetName(version);
  if (signatureText === null) {
    throw new Error(
      `Refusing to install UnrealMCP plugin ${version}: could not download its signature ` +
        `(${signatureAssetName}) from ${signatureUrl} after ${SIGNATURE_FETCH_ATTEMPTS} attempt(s). ` +
        `The download was NOT verified and will not be extracted (fail-closed). ` +
        `Pass --plugin-source <dir> for an offline/dev install.`,
    );
  }
  const verdict = verifyMinisign(publicKey, signatureText, zipBytes);
  if (verdict !== 'verified') {
    throw new Error(
      `Refusing to install UnrealMCP plugin ${version}: ${signatureFailureReason(verdict, signatureAssetName)}. ` +
        `The plugin source will not be extracted or installed (fail-closed).`,
    );
  }
  emitProgress(opts.onProgress, {
    phase: 'info',
    message: `Verified ${signatureAssetName} against the pinned publisher key.`,
  });

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
