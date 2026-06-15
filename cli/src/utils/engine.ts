// Engine-path resolution: combine a project's `EngineAssociation`
// (utils/project.ts) with the installed engines (utils/launcher.ts) to
// produce the absolute `UnrealEditor` binary path for the `open` command.
//
// All resolution is pure given the installed-engine list + platform; the
// only I/O is the optional `existsSync` binary check, isolated behind an
// injectable predicate so tests run without a real engine install.

import * as fs from 'fs';
import * as path from 'path';
import {
  type EngineInstallation,
  matchEngineForAssociation,
} from './launcher.js';

/**
 * Build the absolute path to the `UnrealEditor` binary under an engine
 * install root, for the given platform. `cmd === true` returns the
 * headless console binary (`UnrealEditor-Cmd`) used by `wait-for-ready`
 * smoke paths. Pure.
 */
export function editorBinaryPath(
  engineRoot: string,
  os: NodeJS.Platform,
  cmd = false,
): string {
  const exe = cmd ? 'UnrealEditor-Cmd' : 'UnrealEditor';
  // Use the path flavor for the TARGET os, not the host — so resolving a
  // macOS/Linux engine on a Windows host (and vice versa) yields correct
  // separators.
  const pj = os === 'win32' ? path.win32 : path.posix;
  switch (os) {
    case 'win32':
      return pj.join(engineRoot, 'Engine', 'Binaries', 'Win64', `${exe}.exe`);
    case 'darwin':
      // The .app wraps the same two binaries under Contents/MacOS.
      return pj.join(engineRoot, 'Engine', 'Binaries', 'Mac', `${exe}.app`, 'Contents', 'MacOS', exe);
    default:
      return pj.join(engineRoot, 'Engine', 'Binaries', 'Linux', exe);
  }
}

export interface ResolveEngineInput {
  /** Project's `EngineAssociation` (from utils/project.ts). */
  engineAssociation: string;
  /** Installed engines (from utils/launcher.ts). */
  engines: EngineInstallation[];
  /**
   * Explicit engine root override. When provided, association matching is
   * skipped entirely — this is the escape hatch for source builds whose
   * GUID association is absent from the launcher manifest.
   */
  engineRootOverride?: string;
  /** Target platform; defaults to the host. */
  os?: NodeJS.Platform;
  /** Injectable existence check (defaults to `fs.existsSync`). */
  existsImpl?: (p: string) => boolean;
}

export type ResolveEngineResult =
  | {
      kind: 'resolved';
      engineRoot: string;
      editorPath: string;
      /** The matched installation, or `null` for an explicit override. */
      installation: EngineInstallation | null;
    }
  | {
      kind: 'unresolved';
      /** Why resolution failed — feeds a user-actionable message. */
      reason:
        | 'no-engines-installed'
        | 'association-not-installed'
        | 'source-build-needs-root'
        | 'editor-binary-missing';
      message: string;
      /** Present when we got as far as a candidate root. */
      engineRoot?: string;
    };

/**
 * Resolve a project's engine to a concrete `UnrealEditor` binary path.
 *
 * Order of precedence:
 *   1. `engineRootOverride` (skips the manifest entirely).
 *   2. The launcher engine matching `engineAssociation`.
 *   3. (empty association) the highest installed launcher engine.
 *
 * Pure given its inputs (the binary existence check is injectable).
 */
export function resolveEngine(input: ResolveEngineInput): ResolveEngineResult {
  const os = input.os ?? (process.platform as NodeJS.Platform);
  const exists = input.existsImpl ?? ((p: string) => fs.existsSync(p));

  if (input.engineRootOverride && input.engineRootOverride.trim().length > 0) {
    // Normalise the override with the TARGET-os path flavour, not the host's:
    // the default `path.resolve` is `path.posix` on a non-Windows host (CI), so
    // it would mangle a Windows engine root (`C:\Src\UE5` is not POSIX-absolute,
    // so posix.resolve prepends cwd) and the binary check would then miss. This
    // mirrors `editorBinaryPath`, which already picks the flavour by `os`.
    const pj = os === 'win32' ? path.win32 : path.posix;
    const engineRoot = pj.resolve(input.engineRootOverride.trim());
    const editorPath = editorBinaryPath(engineRoot, os);
    if (!exists(editorPath)) {
      return {
        kind: 'unresolved',
        reason: 'editor-binary-missing',
        engineRoot,
        message: `UnrealEditor binary not found under --engine-root: ${editorPath}`,
      };
    }
    return { kind: 'resolved', engineRoot, editorPath, installation: null };
  }

  const assoc = input.engineAssociation.trim();
  if (input.engines.length === 0) {
    return {
      kind: 'unresolved',
      reason: 'no-engines-installed',
      message:
        'No Unreal Engine installations found in the Epic launcher manifest. ' +
        'Install an engine (unreal-mcp-cli install-engine) or pass --engine-root for a source build.',
    };
  }

  if (assoc.startsWith('{')) {
    return {
      kind: 'unresolved',
      reason: 'source-build-needs-root',
      message:
        `EngineAssociation "${assoc}" is a registered source build (GUID), which the launcher ` +
        'manifest does not list. Pass --engine-root pointing at your engine checkout.',
    };
  }

  const match = matchEngineForAssociation(assoc, input.engines);
  if (!match) {
    const installed = input.engines.map((e) => e.engineAssociation).join(', ');
    return {
      kind: 'unresolved',
      reason: 'association-not-installed',
      message:
        `EngineAssociation "${assoc}" is not among the installed engines (${installed}). ` +
        'Install it (unreal-mcp-cli install-engine) or pass --engine-root.',
    };
  }

  const engineRoot = match.installLocation;
  const editorPath = editorBinaryPath(engineRoot, os);
  if (!exists(editorPath)) {
    return {
      kind: 'unresolved',
      reason: 'editor-binary-missing',
      engineRoot,
      message: `Engine ${match.appName} is registered at ${engineRoot} but its UnrealEditor binary is missing: ${editorPath}`,
    };
  }
  return { kind: 'resolved', engineRoot, editorPath, installation: match };
}
