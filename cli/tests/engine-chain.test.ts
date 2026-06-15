import { describe, it, expect } from 'vitest';
import {
  discoverEngine,
  engineRootFromEditorPath,
  type DiscoverEngineInput,
} from '../src/lib/engine.js';
import { editorBinaryPath } from '../src/utils/engine.js';
import type { EngineInstallation } from '../src/utils/launcher.js';
import type { EngineCacheIo } from '../src/utils/engine-cache.js';
import type { DiscoveryFs } from '../src/utils/engine-discovery.js';

/** In-memory cache IO so the chain's cache writes/reads are observable. */
function memCache(initial: Record<string, { path: string; savedAt: number }> = {}, existing?: Set<string>): {
  io: EngineCacheIo;
  current: () => Record<string, { path: string; savedAt: number }>;
} {
  let cell: string | null = Object.keys(initial).length ? JSON.stringify(initial) : null;
  return {
    io: {
      cacheFilePath: '/v/c.json',
      existsImpl: (p) => existing?.has(p) ?? false,
      readImpl: () => cell,
      writeImpl: (_p, c) => {
        cell = c;
      },
      nowImpl: () => 1,
    },
    current: () => (cell ? JSON.parse(cell) : {}),
  };
}

function launcherEngine(association: string, os: NodeJS.Platform, install: string): EngineInstallation {
  return { appName: `UE_${association}`, appVersion: association, installLocation: install, engineAssociation: association };
}

const WIN = 'win32' as NodeJS.Platform;

describe('discoverEngine — override always wins (bypasses cache + chain)', () => {
  it('resolves an explicit engine root without touching the manifest', () => {
    const root = 'C:\\Src\\UE5';
    const bin = editorBinaryPath(root, WIN);
    const input: DiscoverEngineInput = {
      engineAssociation: '{guid}',
      engineRootOverride: root,
      os: WIN,
      existsImpl: (p) => p === bin,
      enginesImpl: () => {
        throw new Error('manifest must not be consulted under an override');
      },
    };
    const r = discoverEngine(input);
    expect(r.kind).toBe('resolved');
    if (r.kind === 'resolved') expect(r.source).toBe('override');
  });
});

describe('discoverEngine — cache is the fast path', () => {
  it('returns a cached hit without consulting the launcher manifest', () => {
    const cachedBin = 'C:\\Program Files\\Epic Games\\UE_5.7\\Engine\\Binaries\\Win64\\UnrealEditor.exe';
    const { io } = memCache({ '5.7': { path: cachedBin, savedAt: 1 } }, new Set([cachedBin]));
    const r = discoverEngine({
      engineAssociation: '5.7',
      os: WIN,
      existsImpl: (p) => p === cachedBin,
      cacheIo: io,
      enginesImpl: () => {
        throw new Error('cache hit must short-circuit before the manifest');
      },
    });
    expect(r.kind).toBe('resolved');
    if (r.kind === 'resolved') expect(r.source).toBe('cache');
  });

  it('falls through a stale cache entry (evicted) into the launcher manifest', () => {
    const staleBin = 'C:\\old\\UnrealEditor.exe';
    const install = 'C:\\Program Files\\Epic Games\\UE_5.7';
    const liveBin = editorBinaryPath(install, WIN);
    const { io, current } = memCache({ '5.7': { path: staleBin, savedAt: 1 } }, new Set([liveBin]));
    const r = discoverEngine({
      engineAssociation: '5.7',
      os: WIN,
      existsImpl: (p) => p === liveBin, // stale path missing, live present
      cacheIo: io,
      enginesImpl: () => [launcherEngine('5.7', WIN, install)],
    });
    expect(r.kind).toBe('resolved');
    if (r.kind === 'resolved') {
      expect(r.source).toBe('launcher-manifest');
      // The fresh resolution was re-cached under the same slot.
      expect(current()['5.7'].path).toBe(liveBin);
    }
  });
});

describe('discoverEngine — layer ordering: launcher → registry → common-location', () => {
  it('resolves a GUID source build via the registry when the manifest misses', () => {
    const install = 'D:\\CustomUE';
    const bin = editorBinaryPath(install, WIN);
    const { io } = memCache();
    const r = discoverEngine({
      engineAssociation: '{abc}',
      os: WIN,
      existsImpl: (p) => p === bin,
      cacheIo: io,
      enginesImpl: () => [], // launcher empty
      registryQueryImpl: () => `    {abc}    REG_SZ    ${install}`,
    });
    expect(r.kind).toBe('resolved');
    if (r.kind === 'resolved') {
      expect(r.source).toBe('registry');
      expect(r.engineRoot).toBe(install);
    }
  });

  it('falls to the common-location scan when launcher + registry miss', () => {
    const root = 'C:\\Program Files\\Epic Games';
    const install = 'C:\\Program Files\\Epic Games\\UE_5.7';
    const bin = editorBinaryPath(install, WIN);
    const { io } = memCache();
    const discoveryFs: DiscoveryFs = {
      existsImpl: (p) => p === root || p === bin,
      readdirImpl: (p) => (p === root ? ['UE_5.7'] : []),
    };
    const r = discoverEngine({
      engineAssociation: '5.7',
      os: WIN,
      existsImpl: (p) => p === bin,
      cacheIo: io,
      env: { PROGRAMFILES: 'C:\\Program Files' },
      enginesImpl: () => [],
      registryQueryImpl: () => null, // registry empty
      discoveryFs,
    });
    expect(r.kind).toBe('resolved');
    if (r.kind === 'resolved') expect(r.source).toBe('common-location');
  });

  it('resolves an empty association to the highest engine via common-location on Linux', () => {
    const root = '/opt';
    const install = '/opt/UnrealEngine';
    const bin = editorBinaryPath(install, 'linux');
    const { io } = memCache();
    const discoveryFs: DiscoveryFs = {
      existsImpl: (p) => p === root || p === bin,
      readdirImpl: (p) => (p === root ? ['UnrealEngine'] : []),
    };
    const r = discoverEngine({
      engineAssociation: '',
      os: 'linux',
      existsImpl: (p) => p === bin,
      cacheIo: io,
      env: {},
      enginesImpl: () => [], // Linux has no launcher manifest anyway
      discoveryFs,
    });
    expect(r.kind).toBe('resolved');
    if (r.kind === 'resolved') expect(r.source).toBe('common-location');
  });

  it('returns unresolved (source null) when every layer misses', () => {
    const { io } = memCache();
    const r = discoverEngine({
      engineAssociation: '5.7',
      os: WIN,
      existsImpl: () => false,
      cacheIo: io,
      enginesImpl: () => [],
      registryQueryImpl: () => null,
      discoveryFs: { existsImpl: () => false, readdirImpl: () => [] },
    });
    expect(r.kind).toBe('unresolved');
    if (r.kind === 'unresolved') expect(r.source).toBeNull();
  });
});

describe('discoverEngine — noCache disables read AND write', () => {
  it('does not write a resolved path to the cache', () => {
    const install = 'C:\\Program Files\\Epic Games\\UE_5.7';
    const bin = editorBinaryPath(install, WIN);
    const { io, current } = memCache();
    const r = discoverEngine({
      engineAssociation: '5.7',
      os: WIN,
      noCache: true,
      existsImpl: (p) => p === bin,
      cacheIo: io,
      enginesImpl: () => [launcherEngine('5.7', WIN, install)],
    });
    expect(r.kind).toBe('resolved');
    expect(current()).toEqual({});
  });
});

describe('engineRootFromEditorPath', () => {
  it('recovers the engine root from a Win64 editor path', () => {
    const root = 'C:\\Program Files\\Epic Games\\UE_5.7';
    expect(engineRootFromEditorPath(editorBinaryPath(root, WIN), WIN)).toBe(root);
  });
  it('recovers the engine root from a Linux editor path', () => {
    const root = '/opt/UnrealEngine';
    expect(engineRootFromEditorPath(editorBinaryPath(root, 'linux'), 'linux')).toBe(root);
  });
});
