// `install-engine` — detect installed Unreal engines from the Epic launcher
// manifest and, for a not-yet-installed version, hand back a launcher deep
// link + a clear user-facing message. The CLI NEVER performs the multi-GB
// engine install itself — that is the Epic launcher's job. Library-safe.

import { platform } from 'os';
import {
  getDefaultLauncherManifestPath,
  readLauncherManifest,
  matchEngineForAssociation,
  type EngineInstallation,
} from '../utils/launcher.js';
import { scanCommonLocationEngines } from '../utils/engine-discovery.js';
import type {
  DetectEnginesOptions,
  DetectEnginesResult,
  PlanEngineInstallOptions,
  PlanEngineInstallResult,
} from './types.js';

/**
 * Gather every detectable engine: the Epic launcher manifest UNION the
 * common-location scan (the popular `…/Epic Games/UE_<ver>` install roots).
 * The launcher manifest can lag — a freshly-installed engine is sometimes not
 * written there for a while (and source/custom builds never are) — yet it is on
 * disk at the standard path. The same `discoverEngine` chain that `open` /
 * `create-project` use already scans those roots, so `install-engine` must too;
 * otherwise it falsely reports an installed engine as "not installed".
 * Deduped by install location (case-insensitive on Windows); the manifest entry
 * wins on overlap (richer metadata). Pure given the injected scan.
 */
function gatherInstalledEngines(
  os: NodeJS.Platform,
  manifestPath: string | null,
  scanImpl: (os: NodeJS.Platform) => EngineInstallation[],
): EngineInstallation[] {
  const manifestEngines = manifestPath ? readLauncherManifest(manifestPath) : [];
  const scanned = scanImpl(os);
  const keyOf = (e: EngineInstallation): string =>
    os === 'win32' ? e.installLocation.toLowerCase() : e.installLocation;
  const byKey = new Map<string, EngineInstallation>();
  // Manifest first so it wins on overlap with a scanned dir.
  for (const e of [...manifestEngines, ...scanned]) {
    const k = keyOf(e);
    if (!byKey.has(k)) byKey.set(k, e);
  }
  return [...byKey.values()];
}

/**
 * Build the `com.epicgames.launcher://` deep link that opens the Epic Games
 * Launcher on its Unreal Engine page so the user can install/manage engines.
 * A version-specific install path is not a documented launcher URL, so we use
 * the plain, known-good `ue/` link and surface the desired version in the
 * accompanying message instead. Pure.
 */
export function launcherInstallUrl(): string {
  return 'com.epicgames.launcher://ue/';
}

/**
 * Detect installed engines from the launcher manifest. Never throws — a
 * missing/malformed manifest yields an empty `engines` list.
 */
export function detectInstalledEngines(opts: DetectEnginesOptions = {}): DetectEnginesResult {
  const os = opts.os ?? (platform() as NodeJS.Platform);
  const manifestPath = opts.manifestPath ?? getDefaultLauncherManifestPath(os);
  const scanImpl = opts.scanImpl ?? ((o: NodeJS.Platform) => scanCommonLocationEngines(o));
  const engines = gatherInstalledEngines(os, manifestPath, scanImpl);
  return { kind: 'success', success: true, manifestPath, engines };
}

/**
 * Plan an engine install: report whether the requested version is already
 * present, and otherwise produce the launcher deep link + guidance. Never
 * installs. Never throws.
 */
export function planEngineInstall(opts: PlanEngineInstallOptions): PlanEngineInstallResult {
  const os = opts.os ?? (platform() as NodeJS.Platform);
  const version = opts.version.trim();
  const manifestPath = opts.manifestPath ?? getDefaultLauncherManifestPath(os);
  const scanImpl = opts.scanImpl ?? ((o: NodeJS.Platform) => scanCommonLocationEngines(o));
  const engines = gatherInstalledEngines(os, manifestPath, scanImpl);

  const existing = matchEngineForAssociation(version, engines);
  const launcherUrl = launcherInstallUrl();

  if (existing && existing.engineAssociation === version) {
    return {
      kind: 'success',
      success: true,
      version,
      alreadyInstalled: true,
      installation: existing,
      launcherUrl,
      message: `Unreal Engine ${version} is already installed at ${existing.installLocation}.`,
    };
  }

  const installed = engines.map((e) => e.engineAssociation).join(', ') || '(none)';
  return {
    kind: 'success',
    success: true,
    version,
    alreadyInstalled: false,
    installation: null,
    launcherUrl,
    message:
      `Unreal Engine ${version} is not installed (installed: ${installed}). ` +
      'The CLI does not download engines — open the Epic Games Launcher to install it:\n' +
      `  ${launcherUrl}\n` +
      'Then re-run this command to confirm detection.',
  };
}
