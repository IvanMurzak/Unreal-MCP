import { describe, it, expect } from 'vitest';
import { detectInstalledEngines, planEngineInstall, launcherInstallUrl } from '../src/lib/install-engine.js';
import type { EngineInstallation } from '../src/utils/launcher.js';
import { launcherFixturePath } from './helpers.js';

// Stub a common-location-scan engine. `scanImpl: () => []` isolates the manifest;
// a populated stub exercises the manifest ∪ scan union (an engine on disk that the
// launcher manifest does not list — e.g. a freshly-installed or source/custom build).
const eng = (assoc: string, loc: string): EngineInstallation => ({
  appName: `UE_${assoc}`,
  appVersion: assoc,
  installLocation: loc,
  engineAssociation: assoc,
});

describe('detectInstalledEngines (fixture)', () => {
  it('detects the engines from the manifest', () => {
    const r = detectInstalledEngines({ manifestPath: launcherFixturePath(), scanImpl: () => [] });
    expect(r.success).toBe(true);
    expect(r.engines.map((e) => e.engineAssociation).sort()).toEqual(['5.5', '5.7']);
  });

  it('returns [] for a missing manifest (no throw)', () => {
    const r = detectInstalledEngines({ manifestPath: '/no/such/file.dat', scanImpl: () => [] });
    expect(r.engines).toEqual([]);
  });

  it('unions the common-location scan with the manifest (engine on disk but not in the manifest)', () => {
    const r = detectInstalledEngines({
      manifestPath: launcherFixturePath(),
      os: 'win32',
      scanImpl: () => [eng('5.8', 'C:\\Program Files\\Epic Games\\UE_5.8')],
    });
    expect(r.engines.map((e) => e.engineAssociation).sort()).toEqual(['5.5', '5.7', '5.8']);
  });

  it('dedups a scanned engine that matches a manifest entry by install location', () => {
    // The fixture lists 5.7; a scan that re-finds the SAME install path must not duplicate it.
    const r0 = detectInstalledEngines({ manifestPath: launcherFixturePath(), scanImpl: () => [] });
    const loc57 = r0.engines.find((e) => e.engineAssociation === '5.7')!.installLocation;
    const r = detectInstalledEngines({
      manifestPath: launcherFixturePath(),
      os: 'win32',
      scanImpl: () => [eng('5.7', loc57.toUpperCase())], // case-insensitive on win32
    });
    expect(r.engines.filter((e) => e.engineAssociation === '5.7')).toHaveLength(1);
  });
});

describe('planEngineInstall', () => {
  it('reports alreadyInstalled for a present version', () => {
    const r = planEngineInstall({ version: '5.7', manifestPath: launcherFixturePath(), scanImpl: () => [] });
    expect(r.alreadyInstalled).toBe(true);
    expect(r.installation?.installLocation).toContain('UE_5.7');
    expect(r.message).toContain('already installed');
  });

  it('finds a version present only via the common-location scan (manifest lag)', () => {
    const r = planEngineInstall({
      version: '5.8',
      manifestPath: launcherFixturePath(), // fixture has no 5.8
      os: 'win32',
      scanImpl: () => [eng('5.8', 'C:\\Program Files\\Epic Games\\UE_5.8')],
    });
    expect(r.alreadyInstalled).toBe(true);
    expect(r.installation?.engineAssociation).toBe('5.8');
    expect(r.message).toContain('already installed');
  });

  it('delegates to the Epic launcher for a missing version (never installs)', () => {
    const r = planEngineInstall({ version: '5.9', manifestPath: launcherFixturePath(), scanImpl: () => [] });
    expect(r.alreadyInstalled).toBe(false);
    expect(r.installation).toBeNull();
    expect(r.launcherUrl).toContain('com.epicgames.launcher://');
    expect(r.message).toContain('does not download engines');
  });
});

describe('launcherInstallUrl', () => {
  it('builds the plain, known-good Unreal Engine launcher deep link', () => {
    expect(launcherInstallUrl()).toBe('com.epicgames.launcher://ue/');
  });
});
