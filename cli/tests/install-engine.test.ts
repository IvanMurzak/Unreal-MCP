import { describe, it, expect } from 'vitest';
import { detectInstalledEngines, planEngineInstall, launcherInstallUrl } from '../src/lib/install-engine.js';
import { launcherFixturePath } from './helpers.js';

describe('detectInstalledEngines (fixture)', () => {
  it('detects the engines from the manifest', () => {
    const r = detectInstalledEngines({ manifestPath: launcherFixturePath() });
    expect(r.success).toBe(true);
    expect(r.engines.map((e) => e.engineAssociation).sort()).toEqual(['5.5', '5.7']);
  });

  it('returns [] for a missing manifest (no throw)', () => {
    const r = detectInstalledEngines({ manifestPath: '/no/such/file.dat' });
    expect(r.engines).toEqual([]);
  });
});

describe('planEngineInstall', () => {
  it('reports alreadyInstalled for a present version', () => {
    const r = planEngineInstall({ version: '5.7', manifestPath: launcherFixturePath() });
    expect(r.alreadyInstalled).toBe(true);
    expect(r.installation?.installLocation).toContain('UE_5.7');
    expect(r.message).toContain('already installed');
  });

  it('delegates to the Epic launcher for a missing version (never installs)', () => {
    const r = planEngineInstall({ version: '5.9', manifestPath: launcherFixturePath() });
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
