// `install-engine` — detect installed Unreal engines from the Epic launcher
// manifest and, for a not-yet-installed version, hand back a launcher deep
// link + a clear user-facing message. The CLI NEVER performs the multi-GB
// engine install itself — that is the Epic launcher's job. Library-safe.

import { platform } from 'os';
import {
  getDefaultLauncherManifestPath,
  readLauncherManifest,
  matchEngineForAssociation,
} from '../utils/launcher.js';
import type {
  DetectEnginesOptions,
  DetectEnginesResult,
  PlanEngineInstallOptions,
  PlanEngineInstallResult,
} from './types.js';

/**
 * Build the `com.epicgames.launcher://` deep link that opens the install
 * page for an engine version. Pure.
 */
export function launcherInstallUrl(version: string): string {
  return `com.epicgames.launcher://ue/launcher/install?version=${encodeURIComponent(version)}`;
}

/**
 * Detect installed engines from the launcher manifest. Never throws — a
 * missing/malformed manifest yields an empty `engines` list.
 */
export function detectInstalledEngines(opts: DetectEnginesOptions = {}): DetectEnginesResult {
  const os = opts.os ?? (platform() as NodeJS.Platform);
  const manifestPath = opts.manifestPath ?? getDefaultLauncherManifestPath(os);
  const engines = manifestPath ? readLauncherManifest(manifestPath) : [];
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
  const engines = manifestPath ? readLauncherManifest(manifestPath) : [];

  const existing = matchEngineForAssociation(version, engines);
  const launcherUrl = launcherInstallUrl(version);

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
