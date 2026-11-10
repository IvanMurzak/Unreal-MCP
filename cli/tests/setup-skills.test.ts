import { describe, it, expect, afterEach } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import { EXIT_CODES } from '@baizor/gamedev-cli-core';
import { setupSkills } from '../src/lib/setup-skills.js';
import { buildGuideFiles, EXIT_CODE_ACTIONS } from '../src/lib/skill-guides.js';
import { RQL_LANGUAGE, RQL_RECIPES, ROOT_RQL_RECIPES } from '../src/lib/cli-guide.js';
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
const skillsOf = (dir: string, ...rel: string[]) => path.join(dir, '.claude', 'skills', ...rel);
const read = (file: string) => fs.readFileSync(file, 'utf-8');
const skillMd = () => buildGuideFiles().find((f) => f.relativePath === 'unreal-mcp-cli/SKILL.md')!.content;

describe('setupSkills', () => {
  it('writes the CLI skill, its references and the App skill offline, with the exclusion metadata', async () => {
    const dir = tmp();
    writeUProject(dir, 'MyGame', '5.7');
    const r = await setupSkills({ projectDir: dir });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.filesWritten).toHaveLength(4);
    expect(r.skillPath).toBe(skillsOf(dir, 'unreal-mcp-cli', 'SKILL.md'));
    for (const name of ['unreal-mcp-cli', 'ai-game-dev-app']) {
      const doc = read(skillsOf(dir, name, 'SKILL.md'));
      expect(doc.startsWith(`---\nname: ${name}\ndescription: "`)).toBe(true);
      expect(doc.slice(0, doc.indexOf('\n---', 4))).toContain('metadata:\n  generated-by: unreal-mcp\n  ai-game-dev: exclude');
    }
    expect(fs.existsSync(skillsOf(dir, 'unreal-mcp-cli', 'references', 'rql.md'))).toBe(true);
    expect(read(skillsOf(dir, 'unreal-mcp-cli', 'references', 'install.md'))).toContain('install-plugin');
  });

  it('is idempotent: a second run changes nothing', async () => {
    const dir = tmp();
    await setupSkills({ projectDir: dir });
    const before = read(skillsOf(dir, 'unreal-mcp-cli', 'SKILL.md'));
    const second = await setupSkills({ projectDir: dir });
    expect(second.kind === 'success' && second.written).toBe(false);
    expect(read(skillsOf(dir, 'unreal-mcp-cli', 'SKILL.md'))).toBe(before);
  });

  it('writes into the chosen agent\'s skills folder and refuses unknown or skill-less agents', async () => {
    const dir = tmp();
    const r = await setupSkills({ projectDir: dir, agentId: 'cursor' });
    expect(r.kind).toBe('success');
    expect(fs.existsSync(path.join(dir, '.cursor', 'skills', 'unreal-mcp-cli', 'SKILL.md'))).toBe(true);
    const unknown = await setupSkills({ projectDir: dir, agentId: 'nope' });
    expect(unknown.kind === 'failure' && unknown.error.message).toContain('Unknown agent');
    const none = await setupSkills({ projectDir: dir, agentId: 'claude-desktop' });
    expect(none.kind === 'failure' && none.error.message).toContain('does not support skills');
  });

  it('removes the retired stubs it generated, never a folder someone else wrote', async () => {
    const dir = tmp();
    const stub = skillsOf(dir, 'unreal-mcp', 'SKILL.md');
    fs.mkdirSync(path.dirname(stub), { recursive: true });
    fs.writeFileSync(stub, '---\nname: unreal-mcp\nmetadata:\n  generated-by: unreal-mcp\n---\nold');
    const foreign = skillsOf(dir, 'unreal-cli', 'SKILL.md');
    fs.mkdirSync(path.dirname(foreign), { recursive: true });
    fs.writeFileSync(foreign, 'my own notes');
    const r = await setupSkills({ projectDir: dir });
    expect(r.kind === 'success' && r.removed).toEqual([path.dirname(stub)]);
    expect(fs.existsSync(stub)).toBe(false);
    expect(read(foreign)).toBe('my own notes');
  });

  it('keeps a same-named skill it did not write unless --force', async () => {
    const dir = tmp();
    const own = skillsOf(dir, 'unreal-mcp-cli', 'SKILL.md');
    fs.mkdirSync(path.dirname(own), { recursive: true });
    fs.writeFileSync(own, 'existing');
    const r = await setupSkills({ projectDir: dir });
    expect(r.kind === 'success' && r.warnings.join()).toContain('was not written by unreal-mcp-cli');
    expect(read(own)).toBe('existing');
    expect(fs.existsSync(skillsOf(dir, 'unreal-mcp-cli', 'references', 'rql.md'))).toBe(false);
    expect(fs.existsSync(skillsOf(dir, 'ai-game-dev-app', 'SKILL.md'))).toBe(true);
    await setupSkills({ projectDir: dir, force: true });
    expect(read(own)).toBe(skillMd());
  });
});

describe('the unreal-mcp-cli skill', () => {
  it('shows the root help\'s recipes; its RQL reference has them all and the language (one source)', () => {
    const md = skillMd();
    RQL_RECIPES.forEach(([query, what], i) => {
      const line = `rql '${query}'  # ${what}`;
      expect(md.includes(line)).toBe(i < ROOT_RQL_RECIPES);
    });
    const ref = buildGuideFiles().find((f) => f.relativePath.endsWith('references/rql.md'))!.content;
    for (const [query] of RQL_RECIPES) expect(ref).toContain(`rql '${query}'`);
    for (const line of RQL_LANGUAGE) expect(ref).toContain(line);
  });

  it('advises on every failing exit code of the shared contract', () => {
    const failing = EXIT_CODES.map((e) => e.code).filter((c) => c > 1).sort((a, b) => a - b);
    expect(Object.keys(EXIT_CODE_ACTIONS).map(Number)).toEqual(failing);
    const exitLine = skillMd().split('\n').find((l) => l.startsWith('Exit codes: '))!;
    for (const code of failing) expect(exitLine).toContain(` ${code} `);
  });

  it('teaches the cold-start workflow, not the retired one', () => {
    const md = skillMd();
    expect(md).toContain('install-plugin');
    expect(md).toContain('dialog --press');
    expect(md).not.toContain('rql-schema --input');
    expect(md).not.toMatch(/editor must be running/i);
  });

  it('stays short (the agent re-reads every byte each turn) and uses neutral example names', () => {
    expect(Buffer.byteLength(skillMd())).toBeLessThan(2600);
    const all = buildGuideFiles().map((f) => f.content).join('\n');
    expect(all).not.toMatch(/\b(Arena|Crate|Platform|Level1|EnemyStats|GameMath)\b/);
  });
});
