import { describe, it, expect, afterEach } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import {
  createProject,
  generateProjectId,
  renderProjectTemplate,
  validateModuleName,
} from '../src/lib/create-project.js';
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

describe('validateModuleName', () => {
  it('accepts valid C++ identifiers and rejects hyphens/spaces/leading digits', () => {
    expect(validateModuleName('MyGame').ok).toBe(true);
    expect(validateModuleName('My_Game2').ok).toBe(true);
    expect(validateModuleName('My-Game').ok).toBe(false);
    expect(validateModuleName('My Game').ok).toBe(false);
    expect(validateModuleName('2Game').ok).toBe(false);
    expect(validateModuleName('').ok).toBe(false);
  });
});

describe('renderProjectTemplate', () => {
  it('emits a .uproject with the right module + engine association', () => {
    const files = renderProjectTemplate('MyGame', '5.7');
    const uproject = files.find((f) => f.relPath === 'MyGame.uproject');
    expect(uproject).toBeDefined();
    const parsed = JSON.parse(uproject!.content);
    expect(parsed.EngineAssociation).toBe('5.7');
    expect(parsed.Modules[0].Name).toBe('MyGame');
  });

  it('emits build/target/module files and a .gitignore ignoring .env', () => {
    const files = renderProjectTemplate('MyGame', '5.7');
    const rels = files.map((f) => f.relPath.replace(/\\/g, '/'));
    expect(rels).toContain('Source/MyGame/MyGame.Build.cs');
    expect(rels).toContain('Source/MyGame.Target.cs');
    expect(rels).toContain('Source/MyGameEditor.Target.cs');
    const gi = files.find((f) => f.relPath === '.gitignore');
    expect(gi!.content).toContain('.env');
  });
});

describe('generateProjectId', () => {
  it('emits the FGuid config-text form UE imports: 32 uppercase hex digits, no braces or dashes', () => {
    const id = generateProjectId();
    expect(id).toMatch(/^[0-9A-F]{32}$/);
    expect(generateProjectId()).not.toBe(id);
  });

  it('is what DefaultGame.ini carries as ProjectID', () => {
    const files = renderProjectTemplate('MyGame', '5.7');
    const game = files.find((f) => f.relPath === path.join('Config', 'DefaultGame.ini'));
    expect(game!.content).toMatch(/^ProjectID=[0-9A-F]{32}$/m);
  });
});

describe('DefaultEngine.ini', () => {
  it('carries the WaterBodyCollision profile the Water plugin requires', () => {
    const files = renderProjectTemplate('MyGame', '5.7');
    const engine = files.find((f) => f.relPath === path.join('Config', 'DefaultEngine.ini'));
    expect(engine!.content).toContain('[/Script/Engine.CollisionProfile]');
    expect(engine!.content).toMatch(/^\+Profiles=\(Name="WaterBodyCollision",CollisionEnabled=QueryOnly,.*\)$/m);
  });
});

describe('createProject', () => {
  it('scaffolds files to disk', async () => {
    const parent = tmp();
    const target = path.join(parent, 'NewGame');
    const r = await createProject({ projectDir: target, engineAssociation: '5.7' });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.projectName).toBe('NewGame');
    expect(fs.existsSync(r.uprojectPath)).toBe(true);
    expect(fs.existsSync(path.join(target, 'Source', 'NewGame', 'NewGame.Build.cs'))).toBe(true);
    expect(r.filesWritten.length).toBeGreaterThan(5);
  });

  it('rejects a hyphenated directory name unless --name is given', async () => {
    const parent = tmp();
    const r = await createProject({ projectDir: path.join(parent, 'my-game') });
    expect(r.kind).toBe('failure');
    if (r.kind === 'failure') expect(r.errorMessage).toContain('valid UE module name');
  });

  it('accepts an explicit --name for a hyphenated directory', async () => {
    const parent = tmp();
    const r = await createProject({ projectDir: path.join(parent, 'my-game'), projectName: 'MyGame' });
    expect(r.kind).toBe('success');
    if (r.kind === 'success') expect(r.projectName).toBe('MyGame');
  });

  it('refuses a non-empty dir without --force', async () => {
    const target = tmp();
    fs.writeFileSync(path.join(target, 'existing.txt'), 'x', 'utf-8');
    const r = await createProject({ projectDir: target, projectName: 'MyGame' });
    expect(r.kind).toBe('failure');
    if (r.kind === 'failure') expect(r.errorMessage).toContain('not empty');
  });
});
