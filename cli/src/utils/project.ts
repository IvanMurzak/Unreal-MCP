// Unreal project resolution: locate the `.uproject`, read its
// `EngineAssociation`, and expose small pure helpers shared by the
// `open` / `status` / `install-plugin` command logic.

import * as fs from 'fs';
import * as path from 'path';

export interface UProjectInfo {
  /** Absolute path to the `.uproject` file. */
  uprojectPath: string;
  /** Absolute path to the project root (the `.uproject`'s directory). */
  projectDir: string;
  /** Project name (the `.uproject` basename without extension). */
  projectName: string;
  /**
   * `EngineAssociation` field verbatim. A launcher build is a version
   * string (`"5.7"`); a source build is a GUID (`"{...}"`) or empty.
   */
  engineAssociation: string;
}

/**
 * Resolve a project path argument to an absolute directory. An explicit
 * value wins; otherwise the supplied `cwd` is used. Pure / no I/O.
 */
export function resolveProjectDir(
  optionPath: string | undefined,
  cwd: string,
): { projectDir: string; usedCwdFallback: boolean } {
  const explicit = optionPath;
  const resolved = explicit ?? cwd;
  return { projectDir: path.resolve(resolved), usedCwdFallback: explicit === undefined };
}

/**
 * Find the single `.uproject` directly inside `projectDir`. Returns the
 * absolute path, or `null` when none (or, deterministically, the first
 * alphabetically when several — UE itself only supports one per folder).
 */
export function findUProjectFile(projectDir: string): string | null {
  let entries: string[];
  try {
    entries = fs.readdirSync(projectDir);
  } catch {
    return null;
  }
  const uprojects = entries
    .filter((e) => e.toLowerCase().endsWith('.uproject'))
    .sort((a, b) => a.localeCompare(b));
  if (uprojects.length === 0) return null;
  return path.join(projectDir, uprojects[0]);
}

/** True when `projectDir` contains a `.uproject`. Pure-ish (one readdir). */
export function isUnrealProjectDir(projectDir: string): boolean {
  return findUProjectFile(projectDir) !== null;
}

/**
 * Extract `EngineAssociation` from a parsed `.uproject` JSON. Tolerates the
 * field being absent (source-in-tree builds) by returning `''`. Pure.
 */
export function readEngineAssociation(uprojectJson: unknown): string {
  if (uprojectJson && typeof uprojectJson === 'object') {
    const assoc = (uprojectJson as Record<string, unknown>)['EngineAssociation'];
    if (typeof assoc === 'string') return assoc.trim();
  }
  return '';
}

/**
 * Read + parse the `.uproject` at `projectDir` (or at an explicit
 * `uprojectPath`). Returns structured info or `null` when there is no
 * `.uproject` / the file is unreadable / the JSON is malformed.
 */
export function readUProject(projectDirOrFile: string): UProjectInfo | null {
  let uprojectPath: string | null;
  let stat: fs.Stats;
  try {
    stat = fs.statSync(projectDirOrFile);
  } catch {
    return null;
  }
  if (stat.isDirectory()) {
    uprojectPath = findUProjectFile(projectDirOrFile);
  } else {
    uprojectPath = projectDirOrFile.toLowerCase().endsWith('.uproject') ? projectDirOrFile : null;
  }
  if (!uprojectPath) return null;

  let parsed: unknown;
  try {
    parsed = JSON.parse(fs.readFileSync(uprojectPath, 'utf-8'));
  } catch {
    return null;
  }

  return {
    uprojectPath,
    projectDir: path.dirname(uprojectPath),
    projectName: path.basename(uprojectPath, path.extname(uprojectPath)),
    engineAssociation: readEngineAssociation(parsed),
  };
}
