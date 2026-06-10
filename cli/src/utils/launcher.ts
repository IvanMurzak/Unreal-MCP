// Epic Games Launcher manifest (`LauncherInstalled.dat`) parsing.
//
// `LauncherInstalled.dat` is a JSON file the Epic launcher maintains. It
// lists every artifact the launcher manages — engines, marketplace
// content, etc. Engine entries are the ones whose `AppName` looks like
// `UE_5.7`. The CLI reads this to (a) resolve a project's
// `EngineAssociation` to an install path (the `open` command) and (b)
// enumerate installed engines (the `install-engine` command).
//
// Platform default locations:
//   Windows : C:\ProgramData\Epic\UnrealEngineLauncher\LauncherInstalled.dat
//   macOS   : /Users/Shared/Epic/UnrealEngineLauncher/LauncherInstalled.dat
//
// All parsing is pure and defensive — a missing or malformed manifest
// yields an empty engine list, never a throw.

import * as fs from 'fs';
import * as path from 'path';

/** A single engine installation discovered from the launcher manifest. */
export interface EngineInstallation {
  /** Launcher app name, e.g. `UE_5.7`. */
  appName: string;
  /** Full app version string, e.g. `5.7.4-12345678+++UE5+Release-5.7`. */
  appVersion: string;
  /** Absolute install root, e.g. `C:\Program Files\Epic Games\UE_5.7`. */
  installLocation: string;
  /**
   * The `EngineAssociation` value a `.uproject` would use to reference this
   * engine — the `UE_` prefix stripped from `appName` (`UE_5.7` -> `5.7`).
   */
  engineAssociation: string;
}

const RAW_ENGINE_APPNAME_RE = /^UE_(\d+\.\d+)$/;

/**
 * Resolve the platform-default `LauncherInstalled.dat` path. Returns `null`
 * on platforms Epic does not ship a launcher manifest for (Linux), where
 * engine discovery falls back to an explicit `--engine-root`.
 */
export function getDefaultLauncherManifestPath(os: NodeJS.Platform): string | null {
  switch (os) {
    case 'win32': {
      const programData = process.env['PROGRAMDATA'] ?? 'C:\\ProgramData';
      return path.win32.join(programData, 'Epic', 'UnrealEngineLauncher', 'LauncherInstalled.dat');
    }
    case 'darwin':
      return path.posix.join('/Users', 'Shared', 'Epic', 'UnrealEngineLauncher', 'LauncherInstalled.dat');
    default:
      return null;
  }
}

/**
 * Parse `LauncherInstalled.dat` text into the list of ENGINE installations
 * (entries whose `AppName` matches `UE_<major>.<minor>`). Non-engine
 * artifacts (marketplace plugins, templates) are filtered out. Any parse
 * failure yields `[]`. Pure / no I/O.
 */
export function parseLauncherManifest(content: string): EngineInstallation[] {
  let parsed: unknown;
  try {
    parsed = JSON.parse(content);
  } catch {
    return [];
  }
  if (!parsed || typeof parsed !== 'object') return [];

  const list = (parsed as Record<string, unknown>)['InstallationList'];
  if (!Array.isArray(list)) return [];

  const engines: EngineInstallation[] = [];
  for (const entry of list) {
    if (!entry || typeof entry !== 'object') continue;
    const rec = entry as Record<string, unknown>;
    const appName = typeof rec['AppName'] === 'string' ? rec['AppName'] : '';
    const installLocation = typeof rec['InstallLocation'] === 'string' ? rec['InstallLocation'] : '';
    const appVersion = typeof rec['AppVersion'] === 'string' ? rec['AppVersion'] : '';

    const m = appName.match(RAW_ENGINE_APPNAME_RE);
    if (!m || installLocation.length === 0) continue;

    engines.push({
      appName,
      appVersion,
      installLocation,
      engineAssociation: m[1],
    });
  }
  // Highest version first so "no association" callers get the newest engine.
  engines.sort((a, b) => compareEngineVersions(b.engineAssociation, a.engineAssociation));
  return engines;
}

/** Read + parse the launcher manifest at `manifestPath`; `[]` when absent. */
export function readLauncherManifest(manifestPath: string): EngineInstallation[] {
  if (!fs.existsSync(manifestPath)) return [];
  let raw: string;
  try {
    raw = fs.readFileSync(manifestPath, 'utf-8');
  } catch {
    return [];
  }
  return parseLauncherManifest(raw);
}

/**
 * Compare two `major.minor` engine-association strings numerically.
 * `"5.7"` > `"5.5"` > `"4.27"`. Tolerates extra segments and non-numeric
 * junk (treated as 0). Pure.
 */
export function compareEngineVersions(a: string, b: string): number {
  const pa = a.split('.').map((s) => parseInt(s, 10));
  const pb = b.split('.').map((s) => parseInt(s, 10));
  const len = Math.max(pa.length, pb.length);
  for (let i = 0; i < len; i++) {
    const x = Number.isNaN(pa[i]) || pa[i] === undefined ? 0 : pa[i];
    const y = Number.isNaN(pb[i]) || pb[i] === undefined ? 0 : pb[i];
    if (x !== y) return x - y;
  }
  return 0;
}

/**
 * Match a project's `EngineAssociation` against the installed engines.
 *
 * - A launcher version association (`"5.7"`) matches the engine with that
 *   `engineAssociation`.
 * - An EMPTY association (source-in-tree / not yet associated) resolves to
 *   the highest installed engine when one exists.
 * - A GUID association (`"{...}"`, a registered source build) is NOT found
 *   in the launcher manifest — returns `null` so the caller can surface
 *   "source build; pass --engine-root".
 *
 * Pure (operates on the supplied installation list).
 */
export function matchEngineForAssociation(
  engineAssociation: string,
  engines: EngineInstallation[],
): EngineInstallation | null {
  const assoc = engineAssociation.trim();
  if (assoc.length === 0) {
    return engines.length > 0 ? engines[0] : null;
  }
  if (assoc.startsWith('{')) {
    // GUID — a registered source build; not in the launcher manifest.
    return null;
  }
  return engines.find((e) => e.engineAssociation === assoc) ?? null;
}
