import { describe, it, expect } from 'vitest';
import {
  commonEngineRoots,
  scanCommonLocationEngines,
  readEngineAssociationFromBuildVersion,
  parseRegistryBuilds,
  readRegistryEngineBuilds,
  matchRegistryBuild,
  linuxEngineDirCandidates,
  UE_BUILDS_REGISTRY_KEY,
  type DiscoveryFs,
} from '../src/utils/engine-discovery.js';
import { editorBinaryPath } from '../src/utils/engine.js';

/**
 * Build a DiscoveryFs from a set of existing paths + a dir->names listing +
 * an optional path->file-contents map (for the now-injectable file read).
 */
function fakeFs(
  existing: Set<string>,
  dirs: Record<string, string[]> = {},
  files: Record<string, string> = {},
): DiscoveryFs {
  return {
    existsImpl: (p) => existing.has(p),
    readdirImpl: (p) => dirs[p] ?? [],
    readFileImpl: (p) => files[p] ?? null,
  };
}

describe('commonEngineRoots', () => {
  it('includes Epic Games under PROGRAMFILES on Windows', () => {
    const roots = commonEngineRoots('win32', { PROGRAMFILES: 'C:\\Program Files' });
    expect(roots).toContain('C:\\Program Files\\Epic Games');
  });
  it('includes the Shared + Applications roots on macOS', () => {
    const roots = commonEngineRoots('darwin', {});
    expect(roots).toContain('/Users/Shared/Epic');
    expect(roots).toContain('/Applications/Epic Games');
  });
  it('includes /opt and $HOME on Linux (no manifest there)', () => {
    const roots = commonEngineRoots('linux', { HOME: '/home/dev' });
    expect(roots).toContain('/opt');
    expect(roots).toContain('/home/dev');
  });
  it('promotes the PARENT of $UE_ROOT to the front when set', () => {
    const roots = commonEngineRoots('linux', { UE_ROOT: '/data/UnrealEngine' });
    expect(roots[0]).toBe('/data');
  });
});

describe('linuxEngineDirCandidates', () => {
  it('lists the well-known free-form engine dir names', () => {
    expect(linuxEngineDirCandidates()).toContain('UnrealEngine');
  });
});

describe('scanCommonLocationEngines (Windows UE_* layout)', () => {
  it('finds a UE_5.7 install whose editor binary exists', () => {
    const root = 'C:\\Program Files\\Epic Games';
    const install = 'C:\\Program Files\\Epic Games\\UE_5.7';
    const bin = editorBinaryPath(install, 'win32');
    const fs = fakeFs(new Set([root, bin]), { [root]: ['UE_5.7', 'UE_5.5-uninstalled-junk', 'Launcher'] });
    const engines = scanCommonLocationEngines('win32', { PROGRAMFILES: 'C:\\Program Files' }, fs);
    expect(engines).toHaveLength(1);
    expect(engines[0].engineAssociation).toBe('5.7');
    expect(engines[0].installLocation).toBe(install);
  });

  it('skips a UE_* dir whose editor binary is absent', () => {
    const root = 'C:\\Program Files\\Epic Games';
    const fs = fakeFs(new Set([root]), { [root]: ['UE_5.7'] }); // binary NOT in existing
    expect(scanCommonLocationEngines('win32', { PROGRAMFILES: 'C:\\Program Files' }, fs)).toEqual([]);
  });

  it('sorts multiple installs highest-version first', () => {
    const root = 'C:\\Program Files\\Epic Games';
    const i57 = 'C:\\Program Files\\Epic Games\\UE_5.7';
    const i55 = 'C:\\Program Files\\Epic Games\\UE_5.5';
    const fs = fakeFs(new Set([root, editorBinaryPath(i57, 'win32'), editorBinaryPath(i55, 'win32')]), {
      [root]: ['UE_5.5', 'UE_5.7'],
    });
    const engines = scanCommonLocationEngines('win32', { PROGRAMFILES: 'C:\\Program Files' }, fs);
    expect(engines.map((e) => e.engineAssociation)).toEqual(['5.7', '5.5']);
  });
});

describe('scanCommonLocationEngines (Linux free-form layout — fixes the dead end)', () => {
  it('finds /opt/UnrealEngine and reads its association from Build.version', () => {
    const install = '/opt/UnrealEngine';
    const bin = editorBinaryPath(install, 'linux');
    const buildVersion = '/opt/UnrealEngine/Engine/Build/Build.version';
    // The Build.version read now goes through the injected `readFileImpl`, so a
    // hermetic test can supply its contents and assert the parsed association.
    const fs = fakeFs(
      new Set(['/opt', bin, buildVersion]),
      { '/opt': ['UnrealEngine', 'other'] },
      { [buildVersion]: JSON.stringify({ MajorVersion: 5, MinorVersion: 7 }) },
    );
    const engines = scanCommonLocationEngines('linux', {}, fs);
    expect(engines).toHaveLength(1);
    expect(engines[0].installLocation).toBe(install);
    expect(engines[0].engineAssociation).toBe('5.7');
  });

  it('finds a UE_5.7 dir on Linux too', () => {
    const install = '/home/dev/UE_5.7';
    const bin = editorBinaryPath(install, 'linux');
    const fs = fakeFs(new Set(['/home/dev', bin]), { '/home/dev': ['UE_5.7'] });
    const engines = scanCommonLocationEngines('linux', { HOME: '/home/dev' }, fs);
    expect(engines.map((e) => e.engineAssociation)).toEqual(['5.7']);
  });
});

describe('readEngineAssociationFromBuildVersion', () => {
  it('returns "" when the file does not exist (injected fs)', () => {
    const fs = fakeFs(new Set());
    expect(readEngineAssociationFromBuildVersion('/opt/UE', 'linux', fs)).toBe('');
  });

  it('reads "major.minor" through the injected readFileImpl', () => {
    const versionFile = '/opt/UE/Engine/Build/Build.version';
    const fs = fakeFs(
      new Set([versionFile]),
      {},
      { [versionFile]: JSON.stringify({ MajorVersion: 5, MinorVersion: 7, PatchVersion: 4 }) },
    );
    expect(readEngineAssociationFromBuildVersion('/opt/UE', 'linux', fs)).toBe('5.7');
  });

  it('returns "" when the injected read yields malformed JSON', () => {
    const versionFile = '/opt/UE/Engine/Build/Build.version';
    const fs = fakeFs(new Set([versionFile]), {}, { [versionFile]: 'not json' });
    expect(readEngineAssociationFromBuildVersion('/opt/UE', 'linux', fs)).toBe('');
  });
});

describe('parseRegistryBuilds', () => {
  const sample = [
    'HKEY_CURRENT_USER\\Software\\Epic Games\\Unreal Engine\\Builds',
    '    {0A1B2C3D-1234-5678-9ABC-DEF012345678}    REG_SZ    D:\\CustomUE',
    '    MyCustom 419    REG_SZ    E:\\src\\UnrealEngine',
    '',
  ].join('\r\n');

  it('parses GUID + spaced-label build ids and their install paths', () => {
    const builds = parseRegistryBuilds(sample);
    expect(builds).toHaveLength(2);
    expect(builds[0]).toEqual({
      buildId: '{0A1B2C3D-1234-5678-9ABC-DEF012345678}',
      installLocation: 'D:\\CustomUE',
    });
    expect(builds[1]).toEqual({ buildId: 'MyCustom 419', installLocation: 'E:\\src\\UnrealEngine' });
  });

  it('ignores the key header line and blank lines', () => {
    expect(parseRegistryBuilds('HKEY_...\\Builds\n\n')).toEqual([]);
  });

  it('handles REG_EXPAND_SZ values', () => {
    const out = '    Label    REG_EXPAND_SZ    C:\\UE';
    expect(parseRegistryBuilds(out)).toEqual([{ buildId: 'Label', installLocation: 'C:\\UE' }]);
  });
});

describe('readRegistryEngineBuilds', () => {
  it('returns [] off Windows without invoking the reader', () => {
    let called = false;
    const out = readRegistryEngineBuilds('linux', () => {
      called = true;
      return 'whatever';
    });
    expect(out).toEqual([]);
    expect(called).toBe(false);
  });

  it('reads + parses via the injected query on Windows', () => {
    const out = readRegistryEngineBuilds('win32', (key) => {
      expect(key).toBe(UE_BUILDS_REGISTRY_KEY);
      return '    {abc}    REG_SZ    D:\\UE';
    });
    expect(out).toEqual([{ buildId: '{abc}', installLocation: 'D:\\UE' }]);
  });

  it('returns [] when the registry key is absent (reader → null)', () => {
    expect(readRegistryEngineBuilds('win32', () => null)).toEqual([]);
  });
});

describe('matchRegistryBuild', () => {
  const builds = [
    { buildId: '{ABC-DEF}', installLocation: 'D:\\UE' },
    { buildId: 'Label', installLocation: 'E:\\UE' },
  ];
  it('matches a GUID association case-insensitively', () => {
    expect(matchRegistryBuild('{abc-def}', builds)?.installLocation).toBe('D:\\UE');
  });
  it('returns null for an unmatched association', () => {
    expect(matchRegistryBuild('{nope}', builds)).toBeNull();
  });
  it('returns null for an empty association', () => {
    expect(matchRegistryBuild('', builds)).toBeNull();
  });
});
