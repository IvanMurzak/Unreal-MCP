import { describe, it, expect, afterEach } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import { setupSkills, renderSkill } from '../src/lib/setup-skills.js';
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

describe('renderSkill', () => {
  it('embeds the server URL and a frontmatter name', () => {
    const md = renderSkill('http://localhost:5220');
    expect(md).toContain('name: unreal-mcp');
    expect(md).toContain('http://localhost:5220');
  });
});

describe('setupSkills', () => {
  it('writes standalone CLI and App guides with the same global exclusion metadata', async () => {
    const dir = tmp();
    const result = await setupSkills({ projectDir: dir, url: 'http://h' });
    expect(result.kind).toBe('success');
    const docs = ['unreal-mcp', 'unreal-cli', 'ai-game-dev-app'].map(name =>
      fs.readFileSync(path.join(dir, '.claude', 'skills', name, 'SKILL.md'), 'utf-8'));
    for (const doc of docs) {
      expect(doc.slice(0, doc.indexOf('\n---', 4))).toContain(
        'metadata:\n  generated-by: unreal-mcp\n  ai-game-dev: exclude');
    }
    expect(docs[0]).toContain('`unreal-cli` skill');
    expect(docs[0]).toContain('`ai-game-dev-app`');
    expect(docs[0]).not.toContain('npm install');
    expect(docs[1]).toContain('npm install -g unreal-mcp-cli');
    expect(docs[2]).toContain('Blender');
    expect(docs[2]).not.toContain('npm install');

    const second = await setupSkills({ projectDir: dir, url: 'http://h' });
    if (second.kind === 'success') expect(second.written).toBe(false);
    expect(fs.readFileSync(path.join(dir, '.claude', 'skills', 'unreal-cli', 'SKILL.md'), 'utf-8')).toBe(docs[1]);
  });

  it('writes .claude/skills/unreal-mcp/SKILL.md', async () => {
    const dir = tmp();
    writeUProject(dir, 'MyGame', '5.7');
    const r = await setupSkills({ projectDir: dir, url: 'http://h' });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.written).toBe(true);
    expect(r.skillPath.replace(/\\/g, '/')).toContain('.claude/skills/unreal-mcp/SKILL.md');
    expect(fs.existsSync(r.skillPath)).toBe(true);
  });

  it('does not overwrite without --force', async () => {
    const dir = tmp();
    const skillPath = path.join(dir, '.claude', 'skills', 'unreal-mcp', 'SKILL.md');
    fs.mkdirSync(path.dirname(skillPath), { recursive: true });
    fs.writeFileSync(skillPath, 'existing', 'utf-8');
    const r = await setupSkills({ projectDir: dir, url: 'http://h' });
    expect(r.kind).toBe('success');
    if (r.kind === 'success') expect(r.written).toBe(true); // Missing companion guides were written.
    expect(fs.readFileSync(skillPath, 'utf-8')).toBe('existing');
  });
});
