import { describe, it, expect } from 'vitest';
import {
  discoverEngine,
  engineRootFromEditorPath,
  type DiscoverEngineInput,
} from '../src/lib/engine.js';
import { openProject } from '../src/lib/open.js';
import { editorBinaryPath } from '../src/utils/engine.js';
import type { EngineInstallation } from '../src/utils/launcher.js';
import type { EngineCacheIo } from '../src/utils/engine-cache.js';
import type { DiscoveryFs } from '../src/utils/engine-discovery.js';
import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';

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

  it('honours the INJECTED os for the override (not the host platform) — a WIN root resolves off-Windows', () => {
    // Regression: the override path normalised the root with the host `path`
    // (posix on Linux CI), so a Windows engine root resolved on Windows but
    // came back `unresolved` on Linux. The override must use the injected `os`
    // path flavour, hermetically, so this holds on every runner OS.
    const winRoot = 'C:\\Src\\UE5';
    const winBin = editorBinaryPath(winRoot, WIN);
    const rWin = discoverEngine({
      engineAssociation: '{guid}',
      engineRootOverride: winRoot,
      os: WIN,
      existsImpl: (p) => p === winBin,
      enginesImpl: () => {
        throw new Error('manifest must not be consulted under an override');
      },
    });
    expect(rWin.kind).toBe('resolved');
    if (rWin.kind === 'resolved') {
      expect(rWin.engineRoot).toBe(winRoot);
      expect(rWin.editorPath).toBe(winBin);
    }

    // The symmetric case: a posix root resolved under an injected linux os.
    const linRoot = '/opt/UnrealEngine';
    const linBin = editorBinaryPath(linRoot, 'linux');
    const rLin = discoverEngine({
      engineAssociation: '',
      engineRootOverride: linRoot,
      os: 'linux',
      existsImpl: (p) => p === linBin,
      enginesImpl: () => {
        throw new Error('manifest must not be consulted under an override');
      },
    });
    expect(rLin.kind).toBe('resolved');
    if (rLin.kind === 'resolved') {
      expect(rLin.engineRoot).toBe(linRoot);
      expect(rLin.editorPath).toBe(linBin);
    }
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

describe('openProject — forwards the discovery surfaces (hermetic, no home cache / host registry / host fs)', () => {
  it('resolves through an INJECTED cacheIo + discoveryFs + registryQueryImpl, never the real home cache file', async () => {
    // A real temp project + a real fake editor binary (so the default
    // existence check passes), but every DISCOVERY surface is injected.
    const projectDir = fs.mkdtempSync(path.join(os.tmpdir(), 'umcp-openproj-'));
    const engineRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'umcp-engine-'));
    try {
      fs.writeFileSync(
        path.join(projectDir, 'MyGame.uproject'),
        JSON.stringify({ FileVersion: 3, EngineAssociation: '5.7' }),
        'utf-8',
      );
      const bin = editorBinaryPath(engineRoot, process.platform);
      fs.mkdirSync(path.dirname(bin), { recursive: true });
      fs.writeFileSync(bin, '', 'utf-8');

      // The injected cache slot lives only in `store`; the real home file path
      // is asserted untouched below. registry + scan are no-ops.
      const store = new Map<string, string>();
      const homeCacheFile = path.join(os.homedir(), '.unreal-mcp-cli-engine-cache.json');
      const homeCacheExistedBefore = fs.existsSync(homeCacheFile);

      const r = await openProject({
        projectDir,
        enginesImpl: () => [
          { appName: 'UE_5.7', appVersion: '5.7', installLocation: engineRoot, engineAssociation: '5.7' },
        ],
        cacheIo: {
          cacheFilePath: '<in-memory>',
          readImpl: (p) => store.get(p) ?? null,
          writeImpl: (p, c) => void store.set(p, c),
        },
        discoveryFs: { existsImpl: () => false, readdirImpl: () => [] },
        registryQueryImpl: () => null,
        spawnImpl: () => ({ pid: 99 }),
      });

      expect(r.kind).toBe('success');
      if (r.kind === 'success') expect(r.editorPid).toBe(99);
      // The resolved path was cached into the INJECTED store, proving the
      // cacheIo surface was threaded through openProject → discoverEngine.
      expect(store.size).toBeGreaterThan(0);
      // And the user's real home cache file was never created by this run.
      expect(fs.existsSync(homeCacheFile)).toBe(homeCacheExistedBefore);
    } finally {
      fs.rmSync(projectDir, { recursive: true, force: true });
      fs.rmSync(engineRoot, { recursive: true, force: true });
    }
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
