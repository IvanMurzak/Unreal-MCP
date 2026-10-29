import { describe, it, expect, afterEach } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import { pathToFileURL } from 'url';
import { PACKAGE_NAME, PACKAGE_VERSION } from '../src/version.js';
import { makeTempDir, rmTempDir } from './helpers.js';

const cliRoot = path.resolve(import.meta.dirname, '..');
const pkg = JSON.parse(fs.readFileSync(path.join(cliRoot, 'package.json'), 'utf8')) as {
  name: string;
  version: string;
};

const dirs: string[] = [];
afterEach(() => {
  while (dirs.length) rmTempDir(dirs.pop()!);
});

describe('version constants', () => {
  it('match package.json (commands/bump_version.py must rewrite both)', () => {
    expect(PACKAGE_NAME).toBe(pkg.name);
    expect(PACKAGE_VERSION).toBe(pkg.version);
  });

  it('load without a package.json on disk (bundled / bun --compile binary)', async () => {
    // pretest builds dist/; import the compiled module from a dir with no package.json above it.
    const dir = makeTempDir();
    dirs.push(dir);
    const copy = path.join(dir, 'version.mjs');
    const compiled = fs.readFileSync(path.join(cliRoot, 'dist', 'version.js'), 'utf8');
    fs.writeFileSync(copy, compiled.replace(/^\/\/# sourceMappingURL=.*$/m, ''));
    const mod = (await import(pathToFileURL(copy).href)) as typeof import('../src/version.js');
    expect(mod.PACKAGE_NAME).toBe(pkg.name);
    expect(mod.PACKAGE_VERSION).toBe(pkg.version);
  });
});
