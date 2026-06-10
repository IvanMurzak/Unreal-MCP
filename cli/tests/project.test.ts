import { describe, it, expect, afterEach } from 'vitest';
import * as path from 'path';
import { readUProject, readEngineAssociation, findUProjectFile, resolveProjectDir } from '../src/utils/project.js';
import { makeTempDir, rmTempDir, writeUProject } from './helpers.js';

const dirs: string[] = [];
afterEach(() => {
  while (dirs.length) rmTempDir(dirs.pop()!);
});
function tmp(): string {
  const d = makeTempDir();
  dirs.push(d);
  return d;
}

describe('project utils', () => {
  it('finds the .uproject and reads its EngineAssociation', () => {
    const dir = tmp();
    writeUProject(dir, 'MyGame', '5.7');
    const found = findUProjectFile(dir);
    expect(found).not.toBeNull();
    const info = readUProject(dir);
    expect(info?.projectName).toBe('MyGame');
    expect(info?.engineAssociation).toBe('5.7');
    expect(info?.projectDir).toBe(path.resolve(dir));
  });

  it('returns null when no .uproject exists', () => {
    const dir = tmp();
    expect(findUProjectFile(dir)).toBeNull();
    expect(readUProject(dir)).toBeNull();
  });

  it('readEngineAssociation tolerates a missing field', () => {
    expect(readEngineAssociation({})).toBe('');
    expect(readEngineAssociation({ EngineAssociation: ' 5.5 ' })).toBe('5.5');
    expect(readEngineAssociation(null)).toBe('');
  });

  it('resolveProjectDir falls back to cwd', () => {
    const r = resolveProjectDir(undefined, '/work');
    expect(r.usedCwdFallback).toBe(true);
    expect(r.projectDir).toBe(path.resolve('/work'));
    const r2 = resolveProjectDir('/explicit', '/work');
    expect(r2.usedCwdFallback).toBe(false);
  });
});
