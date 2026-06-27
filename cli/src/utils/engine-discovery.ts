// Cross-platform Unreal Engine discovery beyond the Epic launcher manifest:
//
//   - **Common-location scan** (Windows, macOS, AND Linux) — enumerate the
//     popular roots an engine install lives under, list their `UE_*` (or, on
//     Linux, free-form) subdirectories, version-sort, and resolve each to a
//     concrete `UnrealEditor` binary. This is the layer that fixes Linux being
//     a dead end (the launcher manifest only exists on Windows/macOS) and
//     covers non-launcher Windows/macOS installs.
//   - **Windows-registry resolution** — registered source/custom builds live
//     under `HKCU\Software\Epic Games\Unreal Engine\Builds` as
//     `<key> -> <install path>`, where `<key>` is the `.uproject`'s GUID
//     `EngineAssociation` (or a human-readable label). The launcher manifest
//     never lists these, so this is the only automatic way to resolve a source
//     build. The registry READ is injectable (a `reg query` runner) so unit
//     tests never touch the real registry.
//
// Everything is pure given its injected OS / fs / env / registry surface,
// mirroring the existing `launcher.ts` + `engine.ts` style so the whole module
// is unit-testable without a real engine, registry, or specific host OS.

import * as fs from 'fs';
import * as path from 'path';
import { execFileSync } from 'child_process';
import { editorBinaryPath, pathFor } from './engine.js';
import { byEngineVersionDesc, type EngineInstallation } from './launcher.js';
import { verbose } from './ui.js';

// ---------------------------------------------------------------------------
// Injected surfaces (keep discovery pure + testable without a real machine)
// ---------------------------------------------------------------------------

/** Filesystem surface the scan needs — all injectable for tests. */
export interface DiscoveryFs {
  existsImpl: (p: string) => boolean;
  /** List directory entry NAMES (not full paths); `[]` on any error. */
  readdirImpl: (p: string) => string[];
  /** Read a UTF-8 file's contents; `null` on any error (absent/unreadable). */
  readFileImpl: (p: string) => string | null;
}

function defaultFs(): DiscoveryFs {
  return {
    existsImpl: (p: string): boolean => fs.existsSync(p),
    readdirImpl: (p: string): string[] => {
      try {
        return fs.readdirSync(p);
      } catch {
        return [];
      }
    },
    readFileImpl: (p: string): string | null => {
      try {
        return fs.readFileSync(p, 'utf-8');
      } catch {
        return null;
      }
    },
  };
}

/** Matches `UE_5.7`, `UE_5.7.4`, `UE_4.27` install-dir names. */
const UE_DIRNAME_RE = /^UE_(\d+(?:\.\d+)*)$/;

// ---------------------------------------------------------------------------
// Common-location roots, per OS
// ---------------------------------------------------------------------------

/**
 * The popular roots under which `UE_*` engine installs live, for the target OS.
 * Env-driven where Epic/users vary the location (`PROGRAMFILES`, custom drive,
 * `$HOME`, `$UE_ROOT`). Pure given the injected `env`. Exported for tests.
 *
 * Windows : `%PROGRAMFILES%\Epic Games`, `%PROGRAMW6432%\Epic Games`, common
 *           alt drives `D:`/`E:\Epic Games`.
 * macOS   : `/Users/Shared/Epic`, `/Applications/Epic Games`.
 * Linux   : `/opt`, `/usr/local`, `$HOME`, `$HOME/.local/share` — no launcher
 *           manifest exists, so this is the ONLY discovery layer on Linux.
 */
export function commonEngineRoots(os: NodeJS.Platform, env: NodeJS.ProcessEnv = process.env): string[] {
  const roots: string[] = [];
  switch (os) {
    case 'win32': {
      const pj = path.win32;
      const programFiles = env['PROGRAMFILES'] ?? 'C:\\Program Files';
      const programFilesW64 = env['PROGRAMW6432'];
      roots.push(pj.join(programFiles, 'Epic Games'));
      if (programFilesW64 && programFilesW64 !== programFiles) {
        roots.push(pj.join(programFilesW64, 'Epic Games'));
      }
      // Engines are routinely moved to a bigger data drive.
      roots.push('D:\\Epic Games', 'E:\\Epic Games', 'D:\\Program Files\\Epic Games');
      break;
    }
    case 'darwin': {
      const pj = path.posix;
      roots.push(pj.join('/Users', 'Shared', 'Epic'));
      roots.push(pj.join('/Applications', 'Epic Games'));
      const home = env['HOME'];
      if (home) roots.push(pj.join(home, 'Epic'));
      break;
    }
    default: {
      // Linux + any other posix.
      const pj = path.posix;
      roots.push('/opt', '/usr/local');
      const home = env['HOME'];
      if (home) {
        roots.push(home, pj.join(home, '.local', 'share'));
      }
      break;
    }
  }
  // An explicit `$UE_ROOT` (community convention) wins as a first-class root on
  // every OS — its PARENT, since the var points AT the engine dir.
  const ueRoot = env['UE_ROOT'];
  if (ueRoot && ueRoot.trim().length > 0) {
    const pj = pathFor(os);
    const parent = pj.dirname(ueRoot.trim());
    // Guard against promoting a filesystem-root parent (e.g. `UE_ROOT=/opt` ->
    // `/`, or a drive root `D:\`): readdir of `/` is wasteful and never holds a
    // `UE_*` engine dir. `dirname` is idempotent at the root, so equality with
    // its own dirname identifies a root reliably across both path flavours.
    const isFsRoot = parent === pj.dirname(parent);
    if (!isFsRoot && !roots.includes(parent)) roots.unshift(parent);
  }
  return roots;
}

/**
 * On Linux there is no `UE_x.y` naming convention — installs are free-form
 * directory names (`UnrealEngine`, `UnrealEngine-5.7`, `ue5`, …). These are the
 * candidate engine-dir basenames we probe directly under each root (in addition
 * to scanning for `UE_*`). Pure. Exported for tests.
 */
export function linuxEngineDirCandidates(): string[] {
  return ['UnrealEngine', 'UnrealEngine5', 'UE5', 'ue5', 'Unreal'];
}

// ---------------------------------------------------------------------------
// Common-location scan
// ---------------------------------------------------------------------------

/**
 * Scan the common roots for installed engines and return them as
 * `EngineInstallation`s — the SAME shape the launcher manifest produces, so the
 * resolution chain treats manifest- and scan-discovered engines uniformly.
 *
 * For each root:
 *   - Windows/macOS: list `UE_<version>` subdirs, derive the association from
 *     the dir name, keep the install dir whose `UnrealEditor` binary exists.
 *   - Linux: probe the well-known free-form dir names; the association is read
 *     from `Engine/Build/Build.version` when present (else left empty so the
 *     engine is still usable as the "highest installed" fallback).
 *
 * Highest-version first. Pure given the injected fs/env. Never throws.
 */
export function scanCommonLocationEngines(
  os: NodeJS.Platform,
  env: NodeJS.ProcessEnv = process.env,
  fsImpl: DiscoveryFs = defaultFs(),
): EngineInstallation[] {
  const roots = commonEngineRoots(os, env);
  const pj = pathFor(os);
  const found: EngineInstallation[] = [];
  const seenRoots = new Set<string>();

  const tryAddEngine = (installDir: string, associationHint: string): void => {
    const key = os === 'win32' ? installDir.toLowerCase() : installDir;
    if (seenRoots.has(key)) return;
    const binary = editorBinaryPath(installDir, os);
    if (!fsImpl.existsImpl(binary)) return;
    seenRoots.add(key);
    const association = associationHint || readEngineAssociationFromBuildVersion(installDir, os, fsImpl);
    if (association.length === 0) {
      // A free-form (Linux) engine dir whose `Build.version` is absent/unreadable
      // has no resolvable version, so it can satisfy only an empty/"highest"
      // request — a versioned association (e.g. `5.7`) will skip it and report
      // "not found" even though the engine is on disk. Log it so that failure is
      // diagnosable rather than silent.
      verbose(`scanCommonLocationEngines: found engine at ${installDir} with unknown version (no Build.version); it cannot match a versioned association`);
    }
    found.push({
      // os-flavoured basename so a cross-os scan labels free-form Linux dirs
      // correctly off a posix `installDir` even on a Windows host (the install
      // path was built with the target-os `pj` above).
      appName: associationHint ? `UE_${associationHint}` : pj.basename(installDir),
      appVersion: association,
      installLocation: installDir,
      engineAssociation: association,
    });
  };

  for (const root of roots) {
    if (!fsImpl.existsImpl(root)) continue;
    const entries = fsImpl.readdirImpl(root);

    // 1. `UE_<version>` dirs (Windows/macOS launcher layout, also seen on Linux).
    for (const name of entries) {
      const m = name.match(UE_DIRNAME_RE);
      if (!m) continue;
      tryAddEngine(pj.join(root, name), m[1]);
    }

    // 2. Linux free-form engine dirs (`UnrealEngine`, …) — probe by name.
    if (os !== 'win32' && os !== 'darwin') {
      const candidateNames = new Set(linuxEngineDirCandidates());
      for (const name of entries) {
        if (candidateNames.has(name)) {
          tryAddEngine(pj.join(root, name), '');
        }
      }
    }
  }

  found.sort(byEngineVersionDesc);
  return found;
}

/**
 * Best-effort read of `Engine/Build/Build.version` (`{"MajorVersion":5,
 * "MinorVersion":7,...}`) under an engine root → `"5.7"`. Returns `''` when the
 * file is absent/malformed (a source-build root may not ship it). Pure given
 * the injected fs. Exported for tests.
 */
export function readEngineAssociationFromBuildVersion(
  engineRoot: string,
  os: NodeJS.Platform,
  fsImpl: DiscoveryFs = defaultFs(),
): string {
  const pj = pathFor(os);
  const versionFile = pj.join(engineRoot, 'Engine', 'Build', 'Build.version');
  if (!fsImpl.existsImpl(versionFile)) return '';
  const raw = fsImpl.readFileImpl(versionFile);
  if (raw === null) return '';
  let parsed: unknown;
  try {
    parsed = JSON.parse(raw);
  } catch {
    return '';
  }
  if (!parsed || typeof parsed !== 'object') return '';
  const rec = parsed as Record<string, unknown>;
  const major = rec['MajorVersion'];
  const minor = rec['MinorVersion'];
  if (typeof major === 'number' && typeof minor === 'number') {
    return `${major}.${minor}`;
  }
  return '';
}

// ---------------------------------------------------------------------------
// Windows-registry resolution of source/custom builds
// ---------------------------------------------------------------------------

/** A registered source/custom build from `HKCU\...\Unreal Engine\Builds`. */
export interface RegistryEngineBuild {
  /** Registry value name — the `.uproject` GUID association or a label. */
  buildId: string;
  /** Install path the registry maps the build id to. */
  installLocation: string;
}

/** Injectable registry reader: returns raw `reg query` stdout, or `null`. */
export type RegistryQueryImpl = (keyPath: string) => string | null;

/** The registry key Epic registers source/custom builds under. */
export const UE_BUILDS_REGISTRY_KEY = 'HKCU\\Software\\Epic Games\\Unreal Engine\\Builds';

/**
 * Default registry reader — runs `reg query` (Windows only). Returns `null` on
 * any failure (key absent, non-Windows host, `reg` unavailable) so the caller
 * degrades to the other discovery layers. Tests inject a fake instead.
 */
export function defaultRegistryQuery(keyPath: string): string | null {
  try {
    // Pipe (not inherit) stderr: when the Builds key does not exist, `reg query`
    // writes "ERROR: The system was unable to find the specified registry key or
    // value." to stderr and exits non-zero. That is the normal "no source builds
    // registered" case — capture it into the thrown error (which we swallow)
    // rather than letting it pollute the CLI's / tests' stderr.
    return execFileSync('reg', ['query', keyPath], {
      encoding: 'utf-8',
      windowsHide: true,
      stdio: ['ignore', 'pipe', 'pipe'],
    });
  } catch {
    return null;
  }
}

/**
 * Parse `reg query "HKCU\...\Builds"` stdout into `{ buildId, installLocation }`
 * pairs. `reg query` output lines look like:
 *
 *   HKEY_CURRENT_USER\Software\Epic Games\Unreal Engine\Builds
 *       {0A1B2C3D-...}    REG_SZ    D:\CustomUE
 *       MyCustom419       REG_SZ    E:\src\UnrealEngine
 *
 * The value name may itself contain spaces (a human-readable label), so we
 * anchor on the ` REG_SZ ` type token and split around it. Pure. Exported for
 * tests.
 */
export function parseRegistryBuilds(regOutput: string): RegistryEngineBuild[] {
  const builds: RegistryEngineBuild[] = [];
  for (const rawLine of regOutput.split(/\r?\n/)) {
    const line = rawLine.trimEnd();
    // Only value lines are indented and carry a REG_ type token.
    const typeMatch = line.match(/\s+(REG_SZ|REG_EXPAND_SZ)\s+/);
    if (!typeMatch) continue;
    const typeToken = typeMatch[0];
    const idx = line.indexOf(typeToken);
    if (idx < 0) continue;
    const buildId = line.slice(0, idx).trim();
    const installLocation = line.slice(idx + typeToken.length).trim();
    if (buildId.length === 0 || installLocation.length === 0) continue;
    builds.push({ buildId, installLocation });
  }
  return builds;
}

/**
 * Read the registered source/custom builds. Windows-only in practice (the
 * registry doesn't exist elsewhere); on a non-win32 `os` we return `[]` without
 * even invoking the reader. Pure given the injected `queryImpl`. Never throws.
 */
export function readRegistryEngineBuilds(
  os: NodeJS.Platform,
  queryImpl: RegistryQueryImpl = defaultRegistryQuery,
): RegistryEngineBuild[] {
  if (os !== 'win32') return [];
  const out = queryImpl(UE_BUILDS_REGISTRY_KEY);
  if (!out) return [];
  return parseRegistryBuilds(out);
}

/**
 * Resolve a GUID/label `EngineAssociation` to its install path via the registry
 * builds. Matches the build id case-insensitively (GUIDs are written with
 * varying brace/case styles). Returns `null` when unmatched. Pure. Exported for
 * tests.
 */
export function matchRegistryBuild(
  association: string,
  builds: RegistryEngineBuild[],
): RegistryEngineBuild | null {
  const assoc = association.trim().toLowerCase();
  if (assoc.length === 0) return null;
  return builds.find((b) => b.buildId.trim().toLowerCase() === assoc) ?? null;
}
