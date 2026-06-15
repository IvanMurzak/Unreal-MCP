import { describe, it, expect } from 'vitest';
import {
  readCachedEnginePath,
  writeCachedEnginePath,
  clearCachedEnginePath,
  keyFor,
  AUTO_KEY,
  defaultCacheFilePath,
  type EngineCache,
  type EngineCacheIo,
} from '../src/utils/engine-cache.js';

/**
 * Build an in-memory cache IO: the cache "file" is a single string cell, plus
 * an injectable existence set. No real fs, no real home dir — the whole
 * eviction/invalidate logic is exercised deterministically.
 */
function memoryIo(opts?: { initial?: EngineCache; existing?: Set<string>; now?: number }): {
  io: EngineCacheIo;
  read: () => EngineCache;
} {
  let cell: string | null = opts?.initial ? JSON.stringify(opts.initial) : null;
  const existing = opts?.existing ?? new Set<string>();
  const io: EngineCacheIo = {
    cacheFilePath: '/virtual/cache.json',
    existsImpl: (p) => existing.has(p),
    readImpl: () => cell,
    writeImpl: (_p, content) => {
      cell = content;
    },
    nowImpl: () => opts?.now ?? 1000,
  };
  return {
    io,
    read: () => (cell ? (JSON.parse(cell) as EngineCache) : (Object.create(null) as EngineCache)),
  };
}

describe('keyFor', () => {
  it('maps undefined / empty / whitespace to the __auto__ slot', () => {
    expect(keyFor(undefined)).toBe(AUTO_KEY);
    expect(keyFor('')).toBe(AUTO_KEY);
    expect(keyFor('   ')).toBe(AUTO_KEY);
  });
  it('keeps a concrete association as its own slot', () => {
    expect(keyFor('5.7')).toBe('5.7');
    expect(keyFor('{guid}')).toBe('{guid}');
  });
});

describe('defaultCacheFilePath', () => {
  it('lands at ~/.unreal-mcp-cli-engine-cache.json', () => {
    expect(defaultCacheFilePath('/home/u')).toMatch(/[\\/]\.unreal-mcp-cli-engine-cache\.json$/);
  });
});

describe('readCachedEnginePath', () => {
  it('returns null on an empty cache', () => {
    const { io } = memoryIo();
    expect(readCachedEnginePath('5.7', io)).toBeNull();
  });

  it('returns a hit whose path still exists on disk', () => {
    const { io } = memoryIo({
      initial: { '5.7': { path: '/Engine/UnrealEditor', savedAt: 1 } },
      existing: new Set(['/Engine/UnrealEditor']),
    });
    expect(readCachedEnginePath('5.7', io)).toBe('/Engine/UnrealEditor');
  });

  it('evicts a stale entry (cached path no longer exists) and returns null', () => {
    const { io, read } = memoryIo({
      initial: { '5.7': { path: '/gone/UnrealEditor', savedAt: 1 } },
      existing: new Set(), // path missing
    });
    expect(readCachedEnginePath('5.7', io)).toBeNull();
    expect(read()['5.7']).toBeUndefined(); // slot dropped
  });

  it('uses the __auto__ slot for an empty association', () => {
    const { io } = memoryIo({
      initial: { [AUTO_KEY]: { path: '/Engine/UnrealEditor', savedAt: 1 } },
      existing: new Set(['/Engine/UnrealEditor']),
    });
    expect(readCachedEnginePath('', io)).toBe('/Engine/UnrealEditor');
    expect(readCachedEnginePath(undefined, io)).toBe('/Engine/UnrealEditor');
  });

  it('tolerates a corrupt cache file', () => {
    let cell = 'not json at all';
    const io: EngineCacheIo = {
      cacheFilePath: '/v/c.json',
      existsImpl: () => true,
      readImpl: () => cell,
      writeImpl: (_p, c) => {
        cell = c;
      },
    };
    expect(readCachedEnginePath('5.7', io)).toBeNull();
  });
});

describe('writeCachedEnginePath', () => {
  it('stores under the association slot with a savedAt timestamp', () => {
    const { io, read } = memoryIo({ now: 4242 });
    writeCachedEnginePath('5.7', '/E/UnrealEditor', io);
    expect(read()['5.7']).toEqual({ path: '/E/UnrealEditor', savedAt: 4242 });
  });

  it('stores an empty association under __auto__', () => {
    const { io, read } = memoryIo();
    writeCachedEnginePath('', '/E/UnrealEditor', io);
    expect(read()[AUTO_KEY].path).toBe('/E/UnrealEditor');
  });
});

describe('clearCachedEnginePath (invalidate-on-spawn-failure)', () => {
  it('drops a slot', () => {
    const { io, read } = memoryIo({
      initial: { '5.7': { path: '/E/UnrealEditor', savedAt: 1 } },
      existing: new Set(['/E/UnrealEditor']),
    });
    clearCachedEnginePath('5.7', io);
    expect(read()['5.7']).toBeUndefined();
  });

  it('is a no-op when the slot is absent', () => {
    const { io, read } = memoryIo({ initial: { '5.7': { path: '/E/X', savedAt: 1 } } });
    clearCachedEnginePath('5.5', io);
    expect(read()['5.7']).toBeDefined();
  });
});
