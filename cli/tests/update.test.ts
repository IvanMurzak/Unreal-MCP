import { describe, it, expect, afterEach } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import { update, readPluginVersion } from '../src/lib/update.js';
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

function makeSource(version: string): string {
  const dir = tmp();
  fs.writeFileSync(path.join(dir, 'UnrealMCP.uplugin'), JSON.stringify({ VersionName: version }), 'utf-8');
  return dir;
}

describe('readPluginVersion', () => {
  it('reads VersionName, null on miss', () => {
    const dir = makeSource('0.2.0');
    expect(readPluginVersion(path.join(dir, 'UnrealMCP.uplugin'))).toBe('0.2.0');
    expect(readPluginVersion(path.join(dir, 'nope.uplugin'))).toBeNull();
  });
});

describe('update', () => {
  it('installs when not already present', async () => {
    const project = tmp();
    const source = makeSource('0.1.0');
    const r = await update({ projectDir: project, pluginSourceDir: source });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.updated).toBe(true);
    expect(r.fromVersion).toBeNull();
    expect(r.toVersion).toBe('0.1.0');
    expect(fs.existsSync(path.join(project, 'Plugins', 'UnrealMCP', 'UnrealMCP.uplugin'))).toBe(true);
  });

  it('is a no-op when versions match', async () => {
    const project = tmp();
    const source = makeSource('0.1.0');
    await update({ projectDir: project, pluginSourceDir: source });
    const r = await update({ projectDir: project, pluginSourceDir: source });
    expect(r.kind).toBe('success');
    if (r.kind === 'success') expect(r.updated).toBe(false);
  });

  it('installs when not present even if the source version is unreadable', async () => {
    // Regression: a not-installed plugin (fromVersion === null) against a
    // source whose UnrealMCP.uplugin is unreadable (toVersion === null) must
    // still install — `null !== null` is false, so the old guard short-
    // circuited to "already up to date" and never copied anything.
    const project = tmp();
    const source = tmp(); // a real dir, but with no UnrealMCP.uplugin
    fs.writeFileSync(path.join(source, 'README.md'), 'plugin contents', 'utf-8');
    const r = await update({ projectDir: project, pluginSourceDir: source });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.fromVersion).toBeNull();
    expect(r.toVersion).toBeNull();
    expect(r.updated).toBe(true);
    expect(r.warnings.some((w) => /could not read versionname/i.test(w))).toBe(true);
    expect(fs.existsSync(path.join(project, 'Plugins', 'UnrealMCP', 'README.md'))).toBe(true);
  });

  it('re-installs when versions differ', async () => {
    const project = tmp();
    await update({ projectDir: project, pluginSourceDir: makeSource('0.1.0') });
    const r = await update({ projectDir: project, pluginSourceDir: makeSource('0.2.0') });
    expect(r.kind).toBe('success');
    if (r.kind === 'success') {
      expect(r.fromVersion).toBe('0.1.0');
      expect(r.toVersion).toBe('0.2.0');
      expect(r.updated).toBe(true);
    }
  });
});
