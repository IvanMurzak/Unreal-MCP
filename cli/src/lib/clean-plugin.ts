// `clean-plugin` — remove the installed plugin's stale UE C++ build cache so
// the editor performs a clean compile on next launch. Library-safe.
//
// Why this exists: on an in-place plugin UPDATE, the editor's cached
// `Intermediate/` module file-list can miss newly-added `.cpp` files, leaving
// the user on old/partial code after the new source is copied in (issue #58).
// Wiping the C++ module build outputs forces UE to recompile from scratch.
//
// CRITICAL precision: the bundled, precompiled .NET sidecar bridge ships at
// `Binaries/ThirdParty/UnrealMcpBridge/<rid>/` (ARCHITECTURE.md §6.1). It is
// NOT a C++ module output and must SURVIVE the clean — deleting it would strip
// the plugin's ability to spawn the sidecar with no way to recover it from a
// dev source tree. So we delete `Intermediate/` wholesale, but inside
// `Binaries/` we delete every entry EXCEPT the `ThirdParty/` subtree.

import * as fs from 'fs';
import * as path from 'path';
import { asError } from '../utils/error.js';
import { emitProgress } from './progress.js';
import type { ProgressCallback } from './types.js';

/** Subtree under `Binaries/` that holds the bundled bridge — never deleted. */
export const BUNDLED_BRIDGE_DIRNAME = 'ThirdParty';

export interface CleanPluginOptions {
  /** Installed plugin root — `<project>/Plugins/UnrealMCP`. */
  installedPath: string;
  onProgress?: ProgressCallback;
}

export interface CleanPluginSuccess {
  kind: 'success';
  success: true;
  /** Absolute paths actually removed (`Intermediate/` and/or C++ `Binaries/` entries). */
  removed: string[];
  /** Absolute paths deliberately preserved (e.g. the bundled bridge subtree). */
  preserved: string[];
  warnings: string[];
}

export interface CleanPluginFailure {
  kind: 'failure';
  success: false;
  removed: string[];
  preserved: string[];
  warnings: string[];
  error: Error;
}

export type CleanPluginResult = CleanPluginSuccess | CleanPluginFailure;

/** True when `p` is a symlink/junction (vs a real directory). */
function isLink(p: string): boolean {
  try {
    return fs.lstatSync(p).isSymbolicLink();
  } catch {
    return false;
  }
}

/**
 * Delete the C++ module build cache (`Intermediate/` + `Binaries/` minus the
 * bundled-bridge subtree) inside an installed plugin so UE recompiles cleanly.
 *
 * Never throws past the boundary. A junction-mode install must NOT be passed
 * here — the caller is responsible for that gate (cleaning a junction would
 * recurse into and wipe the live dev source's outputs). This function does
 * defend with a junction check on the installed root anyway, refusing to act.
 */
export async function cleanPluginBuildCache(opts: CleanPluginOptions): Promise<CleanPluginResult> {
  const removed: string[] = [];
  const preserved: string[] = [];
  const warnings: string[] = [];
  try {
    if (!opts?.installedPath) throw new Error('installedPath is required.');
    const installedPath = path.resolve(opts.installedPath);

    // Defensive: refuse to clean through a junction — removing files via a
    // junction recurses into the live source the junction points at.
    if (isLink(installedPath)) {
      warnings.push(
        `Refusing to clean ${installedPath}: it is a junction/symlink (dev install). ` +
          'Skipping build-cache clean to protect the live source outputs.',
      );
      return { kind: 'success', success: true, removed, preserved, warnings };
    }

    emitProgress(opts.onProgress, {
      phase: 'info',
      message: `Cleaning stale UE build cache in ${installedPath}`,
    });

    // 1. Intermediate/ — pure build cache, delete wholesale.
    const intermediate = path.join(installedPath, 'Intermediate');
    if (fs.existsSync(intermediate)) {
      fs.rmSync(intermediate, { recursive: true, force: true });
      removed.push(intermediate);
    }

    // 2. Binaries/ — delete every entry EXCEPT the bundled-bridge subtree.
    const binaries = path.join(installedPath, 'Binaries');
    if (fs.existsSync(binaries)) {
      for (const entry of fs.readdirSync(binaries)) {
        const entryPath = path.join(binaries, entry);
        // Case-insensitive compare: on a case-insensitive FS (Windows/macOS) an
        // oddly-cased `thirdparty/` is the SAME dir as the bundled bridge — a
        // case-sensitive `===` would delete it and destroy the sidecar.
        if (entry.toLowerCase() === BUNDLED_BRIDGE_DIRNAME.toLowerCase()) {
          preserved.push(entryPath);
          continue;
        }
        fs.rmSync(entryPath, { recursive: true, force: true });
        removed.push(entryPath);
      }
    }

    emitProgress(opts.onProgress, {
      phase: 'done',
      message:
        removed.length > 0
          ? `Cleaned build cache (${removed.length} path(s) removed; ${preserved.length} preserved).`
          : 'No stale build cache to clean.',
    });
    return { kind: 'success', success: true, removed, preserved, warnings };
  } catch (err: unknown) {
    return {
      kind: 'failure',
      success: false,
      removed,
      preserved,
      warnings,
      error: asError(err),
    };
  }
}
