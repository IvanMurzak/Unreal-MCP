// The tool-neutral, committable per-project marker file
// `<project>/.ai-game-dev/project.json` (mcp-authorize design 06, D14/D15).
//
// It records the enrolled `serverTarget` (hosted vs local) and the user's
// optional explicit `portOverride`. ProjectIdentity resolution and every config
// writer (engine UI, CLIs, `configure`) consult it, so an override or target can
// never silently diverge between the plugin and a terminal-written config.
//
// This is the TypeScript peer of the shared C# `ProjectMarker`
// (McpPlugin/src/AgentConfig/ProjectMarker.cs). The on-disk contract MUST stay
// byte-compatible so the .NET sidecar / engine plugin reads what the CLI writes
// and vice versa:
//   • path        — `<project>/.ai-game-dev/project.json`.
//   • JSON schema — camelCase `{ serverTarget?, portOverride? }`; null fields
//                   omitted (matches `DefaultIgnoreCondition.WhenWritingNull`);
//                   unknown fields ignored on read (forwards-compatible).
//   • at rest     — non-secret, standard file permissions, 2-space-indented JSON.
//
// **Credentials are NEVER written here** — they live only in the machine
// credential store (`utils/machine-credentials.ts`). This file is safe to commit.
//
// Library-safe: read tolerates a missing/blank/garbage marker (returns null);
// callers own error shaping.

import * as fs from 'fs';
import * as path from 'path';

/** Directory name (under the project root) that holds the marker. */
export const PROJECT_MARKER_DIR = '.ai-game-dev';

/** Marker file name inside {@link PROJECT_MARKER_DIR}. */
export const PROJECT_MARKER_FILE = 'project.json';

/**
 * The committable marker document. Mirrors the C# `ProjectMarker` field-for-field
 * (camelCase JSON on the wire). Both fields are optional.
 */
export interface ProjectMarker {
  /** The enrolled server target URL (hosted `https://ai-game.dev` or a local URL). */
  serverTarget?: string;
  /** The user's explicit local-port override. When set it wins over the derived port. */
  portOverride?: number;
}

/** Absolute path of the marker file for a given project root. Pure. */
export function projectMarkerPath(projectDir: string): string {
  return path.join(path.resolve(projectDir), PROJECT_MARKER_DIR, PROJECT_MARKER_FILE);
}

/**
 * Read the marker for `projectDir`. Returns `null` when the marker file does not
 * exist (a project that has never been enrolled / configured). A present-but-blank
 * file yields an empty marker `{}`; a present-but-garbage file also yields `{}`
 * (mirrors the C# `Read`, which never throws for a malformed marker). Never throws.
 */
export function readProjectMarker(projectDir: string): ProjectMarker | null {
  const markerPath = projectMarkerPath(projectDir);
  let raw: string;
  try {
    if (!fs.existsSync(markerPath)) return null;
    raw = fs.readFileSync(markerPath, 'utf-8');
  } catch {
    return null;
  }
  // Strip a leading UTF-8 BOM if some other writer added one.
  if (raw.charCodeAt(0) === 0xfeff) raw = raw.slice(1);
  if (raw.trim().length === 0) return {};
  let parsed: unknown;
  try {
    parsed = JSON.parse(raw);
  } catch {
    return {};
  }
  if (parsed === null || typeof parsed !== 'object') return {};
  const obj = parsed as Record<string, unknown>;
  const marker: ProjectMarker = {};
  if (typeof obj.serverTarget === 'string' && obj.serverTarget.trim().length > 0) {
    marker.serverTarget = obj.serverTarget;
  }
  if (typeof obj.portOverride === 'number' && Number.isInteger(obj.portOverride)) {
    marker.portOverride = obj.portOverride;
  }
  return marker;
}

/**
 * Serialize a marker to the exact camelCase JSON shape the C# store reads:
 * 2-space indent, null/undefined fields omitted, `serverTarget` before
 * `portOverride` (mirrors the C# property order for a stable on-disk document).
 * Pure.
 */
export function serializeProjectMarker(marker: ProjectMarker): string {
  const obj: Record<string, unknown> = {};
  if (marker.serverTarget != null && marker.serverTarget !== '') obj.serverTarget = marker.serverTarget;
  if (marker.portOverride != null) obj.portOverride = marker.portOverride;
  return JSON.stringify(obj, null, 2);
}

/**
 * Write `marker` into `projectDir`, creating the `.ai-game-dev` directory if
 * needed. Non-secret; standard file permissions. Returns the absolute path
 * written.
 */
export function writeProjectMarker(projectDir: string, marker: ProjectMarker): string {
  const markerPath = projectMarkerPath(projectDir);
  fs.mkdirSync(path.dirname(markerPath), { recursive: true });
  fs.writeFileSync(markerPath, serializeProjectMarker(marker), 'utf-8');
  return markerPath;
}

/**
 * Read-modify-write the marker so `serverTarget` is recorded while any existing
 * `portOverride` (the user's D15 choice) is preserved. Returns the absolute path
 * written, or `null` when `serverTarget` is blank (nothing to record). The enroll
 * flow calls this after a successful redeem so the plugin boots against the hub
 * the code was minted for. Never throws past the boundary.
 */
export function upsertServerTarget(projectDir: string, serverTarget: string | undefined | null): string | null {
  if (serverTarget == null || serverTarget.trim().length === 0) return null;
  const existing = readProjectMarker(projectDir) ?? {};
  existing.serverTarget = serverTarget.trim();
  return writeProjectMarker(projectDir, existing);
}
