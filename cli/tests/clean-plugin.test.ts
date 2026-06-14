import { describe, it, expect, afterEach } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import { cleanPluginBuildCache, BUNDLED_BRIDGE_DIRNAME } from '../src/lib/clean-plugin.js';
import { makeTempDir, rmTempDir } from './helpers.js';

const dirs: string[] = [];
afterEach(() => {
  while (dirs.length) rmTempDir(dirs.pop()!);
});
function tmp(): string {
  const d = makeTempDir();
  dirs.push(d);
  return d;
}

/**
 * Build an installed-plugin tree with a stale C++ build cache:
 *   Intermediate/Build/...                 (C++ module cache — must be removed)
 *   Binaries/Win64/UnrealEditor-...dll     (C++ module output — must be removed)
 *   Binaries/ThirdParty/UnrealMcpBridge/win-x64/unreal-mcp-bridge.exe (bundled — must SURVIVE)
 */
function makeInstalledPlugin(): string {
  const installed = tmp();
  fs.writeFileSync(path.join(installed, 'UnrealMCP.uplugin'), JSON.stringify({ VersionName: '0.2.0' }), 'utf-8');

  const intermediate = path.join(installed, 'Intermediate', 'Build', 'Win64');
  fs.mkdirSync(intermediate, { recursive: true });
  fs.writeFileSync(path.join(intermediate, 'stale.obj'), 'stale', 'utf-8');

  const cppBin = path.join(installed, 'Binaries', 'Win64');
  fs.mkdirSync(cppBin, { recursive: true });
  fs.writeFileSync(path.join(cppBin, 'UnrealEditor-UnrealMcpEditor.dll'), 'stale-dll', 'utf-8');

  const bridge = path.join(installed, 'Binaries', 'ThirdParty', 'UnrealMcpBridge', 'win-x64');
  fs.mkdirSync(bridge, { recursive: true });
  fs.writeFileSync(path.join(bridge, 'unreal-mcp-bridge.exe'), 'BRIDGE-PAYLOAD', 'utf-8');

  return installed;
}

describe('cleanPluginBuildCache', () => {
  it('removes Intermediate/ and C++ Binaries/ but PRESERVES the bundled bridge', async () => {
    const installed = makeInstalledPlugin();
    const r = await cleanPluginBuildCache({ installedPath: installed });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;

    // Intermediate gone.
    expect(fs.existsSync(path.join(installed, 'Intermediate'))).toBe(false);
    // C++ module output gone.
    expect(fs.existsSync(path.join(installed, 'Binaries', 'Win64'))).toBe(false);
    // Bundled bridge SURVIVES with its payload intact.
    const bridgeExe = path.join(installed, 'Binaries', BUNDLED_BRIDGE_DIRNAME, 'UnrealMcpBridge', 'win-x64', 'unreal-mcp-bridge.exe');
    expect(fs.existsSync(bridgeExe)).toBe(true);
    expect(fs.readFileSync(bridgeExe, 'utf-8')).toBe('BRIDGE-PAYLOAD');
    // Binaries/ itself survives (it still holds ThirdParty/).
    expect(fs.existsSync(path.join(installed, 'Binaries'))).toBe(true);

    expect(r.removed.some((p) => p.endsWith('Intermediate'))).toBe(true);
    expect(r.removed.some((p) => p.endsWith('Win64'))).toBe(true);
    expect(r.preserved.some((p) => p.endsWith(BUNDLED_BRIDGE_DIRNAME))).toBe(true);
  });

  it('is a no-op (success) when there is nothing to clean', async () => {
    const installed = tmp();
    fs.writeFileSync(path.join(installed, 'UnrealMCP.uplugin'), '{}', 'utf-8');
    const r = await cleanPluginBuildCache({ installedPath: installed });
    expect(r.kind).toBe('success');
    if (r.kind === 'success') expect(r.removed).toHaveLength(0);
  });

  it('refuses to clean a junction/symlink install (protects the live source)', async function () {
    if (process.platform !== 'win32') {
      // junction is Windows; on other platforms a regular symlink exercises the same guard.
    }
    const source = makeInstalledPlugin();
    const project = tmp();
    const link = path.join(project, 'UnrealMCP');
    try {
      fs.symlinkSync(source, link, 'junction');
    } catch {
      // Some CI shells can't create symlinks without privilege — skip gracefully.
      return;
    }
    const r = await cleanPluginBuildCache({ installedPath: link });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    // Nothing removed; a warning explains why.
    expect(r.removed).toHaveLength(0);
    expect(r.warnings.some((w) => /junction|symlink/i.test(w))).toBe(true);
    // The live source's build cache is untouched.
    expect(fs.existsSync(path.join(source, 'Intermediate'))).toBe(true);
    expect(fs.existsSync(path.join(source, 'Binaries', 'Win64'))).toBe(true);
  });

  it('fails (no throw) when installedPath is missing', async () => {
    const r = await cleanPluginBuildCache({ installedPath: '' });
    expect(r.kind).toBe('failure');
  });
});
