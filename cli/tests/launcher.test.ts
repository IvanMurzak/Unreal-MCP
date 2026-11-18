import { describe, it, expect } from 'vitest';
import * as fs from 'fs';
import {
  parseLauncherManifest,
  readLauncherManifest,
  matchEngineForAssociation,
  compareEngineVersions,
  getDefaultLauncherManifestPath,
} from '../src/utils/launcher.js';
import { launcherFixturePath } from './helpers.js';

describe('parseLauncherManifest (fixture)', () => {
  const content = fs.readFileSync(launcherFixturePath(), 'utf-8');

  it('extracts only the UE_* engine entries, dropping marketplace content', () => {
    const engines = parseLauncherManifest(content);
    expect(engines).toHaveLength(2);
    expect(engines.map((e) => e.engineAssociation).sort()).toEqual(['5.5', '5.7']);
    expect(engines.some((e) => e.appName === 'SomeContentPack')).toBe(false);
  });

  it('maps AppName UE_5.7 -> engineAssociation 5.7 and keeps the install location', () => {
    const engines = parseLauncherManifest(content);
    const ue57 = engines.find((e) => e.engineAssociation === '5.7');
    expect(ue57).toBeDefined();
    expect(ue57!.installLocation).toBe('C:\\Program Files\\Epic Games\\UE_5.7');
    expect(ue57!.appVersion).toContain('5.7.4');
  });

  it('sorts highest version first', () => {
    const engines = parseLauncherManifest(content);
    expect(engines[0].engineAssociation).toBe('5.7');
  });

  it('returns [] on malformed JSON', () => {
    expect(parseLauncherManifest('not json')).toEqual([]);
    expect(parseLauncherManifest('{"InstallationList": "nope"}')).toEqual([]);
    expect(parseLauncherManifest('{}')).toEqual([]);
  });

  it('readLauncherManifest reads the fixture file from disk', () => {
    const engines = readLauncherManifest(launcherFixturePath());
    expect(engines).toHaveLength(2);
  });

  it('readLauncherManifest returns [] for a missing file', () => {
    expect(readLauncherManifest('/no/such/LauncherInstalled.dat')).toEqual([]);
  });
});

describe('matchEngineForAssociation', () => {
  const engines = parseLauncherManifest(fs.readFileSync(launcherFixturePath(), 'utf-8'));

  it('matches a launcher version association', () => {
    expect(matchEngineForAssociation('5.7', engines)?.installLocation).toBe('C:\\Program Files\\Epic Games\\UE_5.7');
    expect(matchEngineForAssociation('5.5', engines)?.engineAssociation).toBe('5.5');
  });

  it('an empty association resolves to the highest installed engine', () => {
    expect(matchEngineForAssociation('', engines)?.engineAssociation).toBe('5.7');
  });

  it('a GUID association (source build) is not found in the manifest', () => {
    expect(matchEngineForAssociation('{0a1b2c3d-...}', engines)).toBeNull();
  });

  it('an uninstalled version is not found', () => {
    expect(matchEngineForAssociation('4.27', engines)).toBeNull();
  });
});

describe('compareEngineVersions', () => {
  it('orders numerically', () => {
    expect(compareEngineVersions('5.7', '5.5')).toBeGreaterThan(0);
    expect(compareEngineVersions('4.27', '5.0')).toBeLessThan(0);
    expect(compareEngineVersions('5.7', '5.7')).toBe(0);
  });
});

describe('getDefaultLauncherManifestPath', () => {
  it('returns a Windows ProgramData path', () => {
    expect(getDefaultLauncherManifestPath('win32')).toMatch(/Epic[\\/]UnrealEngineLauncher[\\/]LauncherInstalled\.dat$/);
  });
  const PER_USER = '/Users/dev/Library/Application Support/Epic/UnrealEngineLauncher/LauncherInstalled.dat';
  const SHARED = '/Users/Shared/Epic/UnrealEngineLauncher/LauncherInstalled.dat';
  const existing = (...files: string[]) => (p: string) => files.includes(p);
  it('returns the per-user Application Support path on macOS', () => {
    expect(getDefaultLauncherManifestPath('darwin', { home: '/Users/dev', existsImpl: existing(PER_USER) })).toBe(PER_USER);
    // Neither exists: the current launcher's location, so a later install is found there.
    expect(getDefaultLauncherManifestPath('darwin', { home: '/Users/dev', existsImpl: existing() })).toBe(PER_USER);
    // Both exist: the per-user file wins.
    expect(getDefaultLauncherManifestPath('darwin', { home: '/Users/dev', existsImpl: existing(PER_USER, SHARED) })).toBe(PER_USER);
  });
  it('falls back to the older /Users/Shared manifest on macOS only when the per-user one is missing', () => {
    expect(getDefaultLauncherManifestPath('darwin', { home: '/Users/dev', existsImpl: existing(SHARED) })).toBe(SHARED);
  });
  it('defaults the macOS home to HOME', () => {
    const saved = process.env['HOME'];
    process.env['HOME'] = '/Users/from-env';
    try {
      expect(getDefaultLauncherManifestPath('darwin', { existsImpl: existing() }))
        .toBe('/Users/from-env/Library/Application Support/Epic/UnrealEngineLauncher/LauncherInstalled.dat');
    } finally {
      if (saved === undefined) delete process.env['HOME']; else process.env['HOME'] = saved;
    }
  });
  it('ignores the macOS options on Windows and Linux', () => {
    const exists = () => true;
    expect(getDefaultLauncherManifestPath('win32', { home: '/Users/dev', existsImpl: exists })).toBe(getDefaultLauncherManifestPath('win32'));
    expect(getDefaultLauncherManifestPath('linux', { home: '/Users/dev', existsImpl: exists })).toBeNull();
  });
  it('returns null on linux (no launcher manifest)', () => {
    expect(getDefaultLauncherManifestPath('linux')).toBeNull();
  });
});
