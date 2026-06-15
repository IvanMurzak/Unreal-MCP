// Persistent engine-path cache for `unreal-mcp-cli`, mirroring the Unity CLI's
// `editor-cache.ts`. A resolved `UnrealEditor` binary path is cached per
// EngineAssociation slot (`5.7`, a source-build GUID `{...}`, …) plus a
// version-less `__auto__` slot for the "highest installed" lookup, so a warm
// `open` skips the launcher-manifest read, the registry probe, and the
// common-location filesystem scan entirely.
//
// Two self-healing behaviours match the Unity reference:
//   - **Stale-eviction**: reading a slot whose cached path no longer exists on
//     disk drops the slot and returns `null`, so the next resolution re-runs
//     the full chain.
//   - **Invalidate-on-spawn-failure**: when the editor fails to launch from a
//     cached path, the caller clears the slot so a moved/removed engine is not
//     served again.
//
// Unlike the Unity reference, EVERYTHING here is injectable — the cache-file
// path, `existsSync`, and the JSON read/write — so the unit tests exercise the
// full eviction/invalidate logic against an in-memory or temp-dir fs without
// touching the real `~/.unreal-mcp-cli-engine-cache.json`. This keeps the file
// pure/testable in the same style as `launcher.ts` / `engine.ts`.

import * as fs from 'fs';
import * as path from 'path';
import { homedir } from 'os';

/** Default cache file location: `~/.unreal-mcp-cli-engine-cache.json`. */
export function defaultCacheFilePath(home: string = homedir()): string {
  return path.join(home, '.unreal-mcp-cli-engine-cache.json');
}

/**
 * Storage key for the version-less "highest installed" lookup. Underscored to
 * stay out of the engine-association namespace (`5.7`, `{guid}`). Matches the
 * Unity CLI's `__auto__` slot.
 */
export const AUTO_KEY = '__auto__';

/** A single cached engine-path entry. */
export interface EngineCacheEntry {
  /** Absolute `UnrealEditor` binary path that was resolved. */
  path: string;
  /** Epoch ms the entry was stored. */
  savedAt: number;
}

/** The on-disk cache shape: a flat `key -> entry` map. */
export interface EngineCache {
  [associationKey: string]: EngineCacheEntry;
}

/**
 * Injectable I/O surface for the cache. Defaults wire to the real `fs` +
 * `Date.now`; tests pass fakes to drive eviction/invalidate deterministically.
 */
export interface EngineCacheIo {
  /** Cache file path (defaults to `~/.unreal-mcp-cli-engine-cache.json`). */
  cacheFilePath?: string;
  /** Existence check for cached binary paths (defaults to `fs.existsSync`). */
  existsImpl?: (p: string) => boolean;
  /** Raw read of the cache file; `null` when absent (defaults to `fs`). */
  readImpl?: (p: string) => string | null;
  /** Raw write of the cache file (defaults to `fs`; best-effort). */
  writeImpl?: (p: string, content: string) => void;
  /** Clock (defaults to `Date.now`). */
  nowImpl?: () => number;
  /** Optional verbose logger (defaults to a no-op — keeps utils side-effect-free). */
  logImpl?: (message: string) => void;
}

function resolveIo(io: EngineCacheIo = {}): Required<EngineCacheIo> {
  const cacheFilePath = io.cacheFilePath ?? defaultCacheFilePath();
  return {
    cacheFilePath,
    existsImpl: io.existsImpl ?? ((p: string): boolean => fs.existsSync(p)),
    readImpl:
      io.readImpl ??
      ((p: string): string | null => {
        try {
          return fs.readFileSync(p, 'utf-8');
        } catch {
          return null;
        }
      }),
    writeImpl:
      io.writeImpl ??
      ((p: string, content: string): void => {
        try {
          fs.writeFileSync(p, content, 'utf-8');
        } catch {
          // Best-effort: a read-only home dir or full disk must never break `open`.
        }
      }),
    nowImpl: io.nowImpl ?? ((): number => Date.now()),
    logImpl: io.logImpl ?? ((): void => {}),
  };
}

/** Normalise an association into a cache key (`undefined`/empty → `__auto__`). */
export function keyFor(association: string | undefined): string {
  const assoc = (association ?? '').trim();
  return assoc.length === 0 ? AUTO_KEY : assoc;
}

/** Parse the cache file into a null-prototype map; any failure → empty cache. */
function readAll(io: Required<EngineCacheIo>): EngineCache {
  const raw = io.readImpl(io.cacheFilePath);
  if (raw === null) return Object.create(null) as EngineCache;
  let parsed: unknown;
  try {
    parsed = JSON.parse(raw);
  } catch {
    return Object.create(null) as EngineCache;
  }
  if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) {
    return Object.create(null) as EngineCache;
  }
  const safe: EngineCache = Object.create(null) as EngineCache;
  for (const k of Object.keys(parsed as object)) {
    const entry = (parsed as Record<string, unknown>)[k];
    if (entry && typeof entry === 'object' && typeof (entry as EngineCacheEntry).path === 'string') {
      const e = entry as EngineCacheEntry;
      safe[k] = { path: e.path, savedAt: typeof e.savedAt === 'number' ? e.savedAt : 0 };
    }
  }
  return safe;
}

function writeAll(io: Required<EngineCacheIo>, cache: EngineCache): void {
  io.writeImpl(io.cacheFilePath, JSON.stringify(cache, null, 2));
}

/**
 * Look up a previously-resolved editor binary path for `association`
 * (`undefined`/empty → the `__auto__` slot). Returns `null` when no entry
 * exists OR when the cached path no longer exists on disk — in the latter case
 * the stale entry is evicted so subsequent reads don't waste a stat call.
 */
export function readCachedEnginePath(association: string | undefined, io?: EngineCacheIo): string | null {
  const r = resolveIo(io);
  const cache = readAll(r);
  const k = keyFor(association);
  const entry = cache[k];
  if (!entry || typeof entry.path !== 'string') return null;
  if (!r.existsImpl(entry.path)) {
    r.logImpl(`engine-cache: stale entry for ${k} -> ${entry.path} (binary missing), evicting`);
    delete cache[k];
    writeAll(r, cache);
    return null;
  }
  r.logImpl(`engine-cache: hit ${k} -> ${entry.path}`);
  return entry.path;
}

/** Store the resolved editor path under `association`'s slot. Best-effort. */
export function writeCachedEnginePath(association: string | undefined, editorPath: string, io?: EngineCacheIo): void {
  const r = resolveIo(io);
  const cache = readAll(r);
  const k = keyFor(association);
  cache[k] = { path: editorPath, savedAt: r.nowImpl() };
  writeAll(r, cache);
  r.logImpl(`engine-cache: stored ${k} -> ${editorPath}`);
}

/**
 * Drop the cache entry for `association`. Called when a cached path turned out
 * to be unusable (e.g. the editor failed to spawn from it) so the next
 * invocation re-runs the full resolution chain.
 */
export function clearCachedEnginePath(association: string | undefined, io?: EngineCacheIo): void {
  const r = resolveIo(io);
  const cache = readAll(r);
  const k = keyFor(association);
  if (cache[k] === undefined) return;
  delete cache[k];
  writeAll(r, cache);
  r.logImpl(`engine-cache: cleared ${k}`);
}
