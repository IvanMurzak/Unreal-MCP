import { describe, it, expect } from 'vitest';
import * as fs from 'fs';
import { editorBinaryPath, resolveEngine } from '../src/utils/engine.js';
import { parseLauncherManifest, type EngineInstallation } from '../src/utils/launcher.js';
import { launcherFixturePath } from './helpers.js';

const engines: EngineInstallation[] = parseLauncherManifest(fs.readFileSync(launcherFixturePath(), 'utf-8'));

describe('editorBinaryPath', () => {
  it('builds the Win64 UnrealEditor path', () => {
    expect(editorBinaryPath('C:\\Engine', 'win32')).toMatch(/Engine[\\/]Binaries[\\/]Win64[\\/]UnrealEditor\.exe$/);
  });
  it('builds the -Cmd variant', () => {
    expect(editorBinaryPath('C:\\Engine', 'win32', true)).toMatch(/UnrealEditor-Cmd\.exe$/);
  });
  it('builds the macOS .app path', () => {
    expect(editorBinaryPath('/Engine', 'darwin')).toContain('UnrealEditor.app/Contents/MacOS/UnrealEditor');
  });
  it('builds the Linux path', () => {
    expect(editorBinaryPath('/Engine', 'linux')).toMatch(/Engine\/Binaries\/Linux\/UnrealEditor$/);
  });
});

describe('resolveEngine', () => {
  const existsAll = () => true;

  it('resolves a launcher version association to its editor binary', () => {
    const r = resolveEngine({ engineAssociation: '5.7', engines, os: 'win32', existsImpl: existsAll });
    expect(r.kind).toBe('resolved');
    if (r.kind === 'resolved') {
      expect(r.engineRoot).toBe('C:\\Program Files\\Epic Games\\UE_5.7');
      expect(r.editorPath).toMatch(/UnrealEditor\.exe$/);
      expect(r.installation?.appName).toBe('UE_5.7');
    }
  });

  it('resolves an empty association to the highest installed engine', () => {
    const r = resolveEngine({ engineAssociation: '', engines, os: 'win32', existsImpl: existsAll });
    expect(r.kind).toBe('resolved');
    if (r.kind === 'resolved') expect(r.installation?.engineAssociation).toBe('5.7');
  });

  it('honors an explicit engine-root override (skips the manifest)', () => {
    const r = resolveEngine({
      engineAssociation: '{guid}',
      engines: [],
      engineRootOverride: 'C:\\Src\\UE5',
      os: 'win32',
      existsImpl: existsAll,
    });
    expect(r.kind).toBe('resolved');
    if (r.kind === 'resolved') expect(r.installation).toBeNull();
  });

  it('reports source-build-needs-root for a GUID association', () => {
    const r = resolveEngine({ engineAssociation: '{abc}', engines, os: 'win32', existsImpl: existsAll });
    expect(r.kind).toBe('unresolved');
    if (r.kind === 'unresolved') expect(r.reason).toBe('source-build-needs-root');
  });

  it('reports no-engines-installed when the manifest is empty', () => {
    const r = resolveEngine({ engineAssociation: '5.7', engines: [], os: 'win32', existsImpl: existsAll });
    expect(r.kind).toBe('unresolved');
    if (r.kind === 'unresolved') expect(r.reason).toBe('no-engines-installed');
  });

  it('reports association-not-installed for an unknown version', () => {
    const r = resolveEngine({ engineAssociation: '4.27', engines, os: 'win32', existsImpl: existsAll });
    expect(r.kind).toBe('unresolved');
    if (r.kind === 'unresolved') expect(r.reason).toBe('association-not-installed');
  });

  it('reports editor-binary-missing when the binary is absent', () => {
    const r = resolveEngine({ engineAssociation: '5.7', engines, os: 'win32', existsImpl: () => false });
    expect(r.kind).toBe('unresolved');
    if (r.kind === 'unresolved') expect(r.reason).toBe('editor-binary-missing');
  });
});
