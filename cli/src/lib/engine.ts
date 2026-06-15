// Library-facing engine discovery: the full, cache-first resolution chain that
// turns a project's `EngineAssociation` into a concrete `UnrealEditor` binary.
//
// Chain order (fast → slow), each layer feeding the next + the cache:
//   cache → launcher manifest → registry (Win source builds) →
//   common-location scan (all 3 OSes) → resolve binary.
// (Consent-gated auto-install lives in `auto-install-engine.ts`; the `open`
// command invokes it as a separate, explicit step when resolution misses, so a
// silent multi-GB download can never happen inside discovery.)
//
// Thin orchestration over `utils/launcher` + `utils/engine` +
// `utils/engine-discovery` + `utils/engine-cache` + `utils/project`, so command
// logic never reaches into `utils/`. Pure given its injected surfaces — the
// whole chain is unit-testable without a real engine, registry, or host OS.

import * as fs from 'fs';
import { platform } from 'os';
import {
  getDefaultLauncherManifestPath,
  readLauncherManifest,
  type EngineInstallation,
} from '../utils/launcher.js';
import {
  resolveEngine,
  editorBinaryPath,
  type ResolveEngineResult,
} from '../utils/engine.js';
import {
  scanCommonLocationEngines,
  readRegistryEngineBuilds,
  matchRegistryBuild,
  type DiscoveryFs,
  type RegistryQueryImpl,
} from '../utils/engine-discovery.js';
import {
  readCachedEnginePath,
  writeCachedEnginePath,
  clearCachedEnginePath,
  type EngineCacheIo,
} from '../utils/engine-cache.js';
import { readUProject } from '../utils/project.js';
import { verbose } from '../utils/ui.js';

/** Read the installed engines from the (platform-default) launcher manifest. */
export function listInstalledEngines(manifestPath?: string, os?: NodeJS.Platform): {
  manifestPath: string | null;
  engines: EngineInstallation[];
} {
  const targetOs = os ?? (platform() as NodeJS.Platform);
  const resolvedPath = manifestPath ?? getDefaultLauncherManifestPath(targetOs);
  const engines = resolvedPath ? readLauncherManifest(resolvedPath) : [];
  return { manifestPath: resolvedPath, engines };
}

/**
 * Resolve the engine for a project on disk: read its `.uproject`
 * `EngineAssociation`, then match it against the installed engines.
 * Returns the resolution result plus the project info that fed it.
 *
 * NOTE: this is the launcher-manifest-only resolver kept for back-compat with
 * existing callers/tests. The full chained resolver is `discoverEngine`.
 */
export function resolveEngineForProject(opts: {
  projectDir: string;
  engineRootOverride?: string;
  manifestPath?: string;
  os?: NodeJS.Platform;
}): {
  uproject: ReturnType<typeof readUProject>;
  resolution: ResolveEngineResult;
} {
  const os = opts.os ?? (platform() as NodeJS.Platform);
  const uproject = readUProject(opts.projectDir);
  const { engines } = listInstalledEngines(opts.manifestPath, os);
  const resolution = resolveEngine({
    engineAssociation: uproject?.engineAssociation ?? '',
    engines,
    engineRootOverride: opts.engineRootOverride,
    os,
  });
  return { uproject, resolution };
}

// ---------------------------------------------------------------------------
// Full cache-first discovery chain
// ---------------------------------------------------------------------------

/** How a `discoverEngine` resolution was obtained (for logging / status). */
export type EngineDiscoverySource =
  | 'override'
  | 'cache'
  | 'launcher-manifest'
  | 'registry'
  | 'common-location';

export interface DiscoverEngineInput {
  /** Project `EngineAssociation` (`"5.7"`, a GUID `{...}`, or `""`). */
  engineAssociation: string;
  /** Explicit engine root override; skips the whole chain when present. */
  engineRootOverride?: string;
  /** Target platform; defaults to the host. */
  os?: NodeJS.Platform;
  /** Skip the persistent cache read/write (forces a fresh chain). */
  noCache?: boolean;

  // --- Injectable surfaces (defaults wire to the real system) -------------
  /** Launcher engines (defaults to reading the platform manifest). */
  enginesImpl?: () => EngineInstallation[];
  /** Filesystem surface for the common-location scan. */
  discoveryFs?: DiscoveryFs;
  /** Environment for common-location roots. */
  env?: NodeJS.ProcessEnv;
  /** Registry reader (defaults to `reg query`; no-op off Windows). */
  registryQueryImpl?: RegistryQueryImpl;
  /** Binary existence check (defaults to `fs.existsSync`). */
  existsImpl?: (p: string) => boolean;
  /** Cache I/O surface (defaults to `~/.unreal-mcp-cli-engine-cache.json`). */
  cacheIo?: EngineCacheIo;
}

export type DiscoverEngineResult =
  | {
      kind: 'resolved';
      engineRoot: string;
      editorPath: string;
      installation: EngineInstallation | null;
      source: EngineDiscoverySource;
    }
  | (Extract<ResolveEngineResult, { kind: 'unresolved' }> & { source: null });

/**
 * Resolve a project's engine through the full chain, cache-first. Verbose-logs
 * each layer it tries. On a cache hit the editor binary is returned immediately
 * (after a fresh existence check via the cache's stale-eviction); on a miss the
 * chain runs launcher → registry → common-location, the winning path is cached
 * under the association's slot, and the result is returned.
 *
 * An explicit `engineRootOverride` bypasses everything (and the cache) — it is
 * the source-build escape hatch and must always win deterministically.
 */
export function discoverEngine(input: DiscoverEngineInput): DiscoverEngineResult {
  const os = input.os ?? (platform() as NodeJS.Platform);
  const exists = input.existsImpl ?? ((p: string): boolean => fs.existsSync(p));
  const assoc = input.engineAssociation.trim();
  const cacheIo: EngineCacheIo = { logImpl: verbose, existsImpl: exists, ...(input.cacheIo ?? {}) };

  // 0. Explicit override — bypass the chain AND the cache (deterministic).
  if (input.engineRootOverride && input.engineRootOverride.trim().length > 0) {
    verbose(`discoverEngine: using explicit engine-root override`);
    const r = resolveEngine({
      engineAssociation: assoc,
      engines: [],
      engineRootOverride: input.engineRootOverride,
      os,
      existsImpl: exists,
    });
    return r.kind === 'resolved' ? { ...r, source: 'override' } : { ...r, source: null };
  }

  // 1. Cache — fast path. Stale entries self-evict inside readCachedEnginePath.
  if (!input.noCache) {
    const cached = readCachedEnginePath(assoc, cacheIo);
    if (cached) {
      verbose(`discoverEngine: cache hit for ${assoc || '__auto__'} -> ${cached}`);
      return {
        kind: 'resolved',
        engineRoot: engineRootFromEditorPath(cached, os),
        editorPath: cached,
        installation: null,
        source: 'cache',
      };
    }
  }

  // 2. Launcher manifest.
  const launcherEngines = input.enginesImpl
    ? input.enginesImpl()
    : (() => {
        const manifestPath = getDefaultLauncherManifestPath(os);
        return manifestPath ? readLauncherManifest(manifestPath) : [];
      })();
  verbose(`discoverEngine: launcher manifest yielded ${launcherEngines.length} engine(s)`);
  const fromLauncher = resolveEngine({ engineAssociation: assoc, engines: launcherEngines, os, existsImpl: exists });
  if (fromLauncher.kind === 'resolved') {
    if (!input.noCache) writeCachedEnginePath(assoc, fromLauncher.editorPath, cacheIo);
    return { ...fromLauncher, source: 'launcher-manifest' };
  }

  // 3. Windows registry (registered source/custom builds, GUID/label assoc).
  if (os === 'win32' && assoc.length > 0) {
    const builds = readRegistryEngineBuilds(os, input.registryQueryImpl);
    verbose(`discoverEngine: registry yielded ${builds.length} build(s)`);
    const match = matchRegistryBuild(assoc, builds);
    if (match) {
      const editorPath = editorBinaryPath(match.installLocation, os);
      if (exists(editorPath)) {
        verbose(`discoverEngine: registry resolved ${assoc} -> ${match.installLocation}`);
        if (!input.noCache) writeCachedEnginePath(assoc, editorPath, cacheIo);
        return {
          kind: 'resolved',
          engineRoot: match.installLocation,
          editorPath,
          installation: {
            appName: match.buildId,
            appVersion: assoc,
            installLocation: match.installLocation,
            engineAssociation: assoc,
          },
          source: 'registry',
        };
      }
      verbose(`discoverEngine: registry build ${assoc} install missing its editor binary (${editorPath})`);
    }
  }

  // 4. Common-location scan (all 3 OSes; the only Linux discovery layer).
  const scanned = scanCommonLocationEngines(os, input.env, input.discoveryFs);
  verbose(`discoverEngine: common-location scan yielded ${scanned.length} engine(s)`);
  // A GUID association is never matched by a version scan — fall back to the
  // highest scanned engine ONLY for a version/empty association.
  const scanAssoc = assoc.startsWith('{') ? '' : assoc;
  const fromScan = resolveEngine({ engineAssociation: scanAssoc, engines: scanned, os, existsImpl: exists });
  if (fromScan.kind === 'resolved') {
    verbose(`discoverEngine: common-location resolved -> ${fromScan.editorPath}`);
    // Cache under the ORIGINAL association key (incl. a GUID resolved via the
    // highest-installed fallback) so a warm run short-circuits identically.
    if (!input.noCache) writeCachedEnginePath(assoc, fromScan.editorPath, cacheIo);
    return { ...fromScan, source: 'common-location' };
  }

  // Exhausted — surface the most actionable unresolved reason. Prefer the
  // launcher's (it carries install/override guidance), falling back to scan's.
  verbose(`discoverEngine: all layers exhausted for ${assoc || '__auto__'}`);
  const unresolved = fromLauncher.kind === 'unresolved' ? fromLauncher : fromScan;
  return { ...unresolved, source: null };
}

/** Invalidate a cached engine path (called when the editor failed to spawn). */
export function invalidateCachedEngine(engineAssociation: string, cacheIo?: EngineCacheIo): void {
  clearCachedEnginePath(engineAssociation, { logImpl: verbose, ...(cacheIo ?? {}) });
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

/**
 * Recover the engine ROOT from a resolved editor binary path (the inverse of
 * `editorBinaryPath`), so a cache hit can still report a sensible engineRoot.
 * Strips the known `Engine/Binaries/<plat>/...` suffix; falls back to the
 * binary's grandparent when the shape is unexpected.
 */
export function engineRootFromEditorPath(editorPath: string, os: NodeJS.Platform): string {
  const sep = os === 'win32' ? '\\' : '/';
  const marker = `${sep}Engine${sep}Binaries${sep}`;
  const idx = editorPath.indexOf(marker);
  if (idx > 0) return editorPath.slice(0, idx);
  return editorPath;
}
