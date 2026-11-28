import { describe, it, expect, afterEach, beforeEach, vi } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import { EXIT_CODES } from '@baizor/gamedev-cli-core';
import { EDITOR_SKILLS_HINT, setupSkills } from '../src/lib/setup-skills.js';
import { buildGuideFiles, EXIT_CODE_ACTIONS } from '../src/lib/skill-guides.js';
import { RQL_LANGUAGE, RQL_RECIPES } from '../src/lib/cli-guide.js';
import { makeTempDir, rmTempDir, writeUProject } from './helpers.js';
import { probePing } from '../src/utils/probe.js';
import { runSystemTool } from '../src/lib/run-tool.js';

// The editor step: no editor answers unless a test says so (the probe never reaches the network).
vi.mock('../src/utils/probe.js', () => ({ probePing: vi.fn() }));
vi.mock('../src/lib/run-tool.js', () => ({ runSystemTool: vi.fn() }));
const probeMock = vi.mocked(probePing);
const systemToolMock = vi.mocked(runSystemTool);
beforeEach(() => {
  probeMock.mockReset();
  probeMock.mockResolvedValue({ ok: false, baseUrl: 'http://127.0.0.1:1', reason: 'connection refused' });
  systemToolMock.mockReset();
});

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
  it('writes the CLI skill, its references and the App skill offline, marked for the App to show', async () => {
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
      expect(doc.slice(0, doc.indexOf('\n---', 4))).toContain('metadata:\n  generated-by: unreal-mcp\n  ai-game-dev: include');
    }
    expect(fs.existsSync(skillsOf(dir, 'unreal-mcp-cli', 'references', 'rql.md'))).toBe(true);
    expect(read(skillsOf(dir, 'unreal-mcp-cli', 'references', 'install.md'))).toContain('install-plugin');
  });

  it('is idempotent: a second run changes nothing', async () => {
    const dir = tmp();
    await setupSkills({ projectDir: dir });
    const before = read(skillsOf(dir, 'unreal-mcp-cli', 'SKILL.md'));
    const second = await setupSkills({ projectDir: dir });
    expect(second.kind).toBe('success');
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

  it('removes the retired stub it generated, never a folder someone else wrote, and keeps the editor\'s unreal-cli', async () => {
    const dir = tmp();
    const ours = '---\nname: x\nmetadata:\n  generated-by: unreal-mcp\n---\nold';
    const put = (name: string, body: string) => {
      fs.mkdirSync(skillsOf(dir, name), { recursive: true });
      fs.writeFileSync(skillsOf(dir, name, 'SKILL.md'), body);
    };
    put('unreal-mcp', ours);
    put('unreal-cli', ours);
    put('my-notes', 'my own notes');
    const r = await setupSkills({ projectDir: dir });
    expect(r.kind).toBe('success');
    expect(r.kind === 'success' && r.removed).toEqual([skillsOf(dir, 'unreal-mcp')]);
    expect(fs.existsSync(skillsOf(dir, 'unreal-mcp'))).toBe(false);
    expect(fs.existsSync(skillsOf(dir, 'unreal-cli', 'SKILL.md'))).toBe(true);
    expect(read(skillsOf(dir, 'my-notes', 'SKILL.md'))).toBe('my own notes');
  });

  it('keeps a same-named skill it did not write unless --force', async () => {
    const dir = tmp();
    const own = skillsOf(dir, 'unreal-mcp-cli', 'SKILL.md');
    fs.mkdirSync(path.dirname(own), { recursive: true });
    fs.writeFileSync(own, 'existing');
    const r = await setupSkills({ projectDir: dir });
    expect(r.kind === 'success' && r.warnings.join()).toContain('was not written by unreal-mcp-cli');
    expect(r.kind === 'success' && r.skipped).toEqual(['unreal-mcp-cli']);
    expect(read(own)).toBe('existing');
    expect(fs.existsSync(skillsOf(dir, 'unreal-mcp-cli', 'references', 'rql.md'))).toBe(false);
    expect(fs.existsSync(skillsOf(dir, 'ai-game-dev-app', 'SKILL.md'))).toBe(true);
    const forced = await setupSkills({ projectDir: dir, force: true });
    expect(forced.kind === 'success' && forced.skipped).toEqual([]);
    expect(read(own)).toBe(skillMd());
    expect(fs.existsSync(skillsOf(dir, 'unreal-mcp-cli', 'references', 'rql.md'))).toBe(true);
  });
});

describe('the unreal-mcp-cli skill', () => {
  it('shows every recipe; its RQL reference adds the language, not the recipes again (one source)', () => {
    const md = skillMd();
    for (const [query, what] of RQL_RECIPES) expect(md).toContain(`rql '${query}'  # ${what}`);
    const ref = buildGuideFiles().find((f) => f.relativePath.endsWith('references/rql.md'))!.content;
    for (const [query] of RQL_RECIPES) expect(ref).not.toContain(query);
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

  it('scopes the description to editor work and names only compile for code edits (naming tests sent agents into test fixes)', () => {
    const description = skillMd().split('\n').find((l) => l.startsWith('description: '))!;
    expect(description).toMatch(/^description: "Unreal editor work with /);
    expect(description).toContain('Not needed to edit C++: `unreal-mcp-cli compile` lists file:line errors.');
    expect(description).not.toMatch(/\btest\b/);
  });

  it('stays short (the agent re-reads every byte each turn) and uses neutral example names', () => {
    expect(Buffer.byteLength(skillMd())).toBeLessThan(2800);
    const all = buildGuideFiles().map((f) => f.content).join('\n');
    expect(all).not.toMatch(/\b(Arena|Crate|Platform|Level1|EnemyStats|GameMath)\b/);
  });

  /** An editor whose project is @p editorRoot: unreal-skill-generate writes its skills below that root, as the sidecar does. */
  function editorAt(editorRoot: string, skills: Record<string, string> = { 'unreal-rql': 'rql guide', 'rql-query': 'tool' }) {
    probeMock.mockResolvedValue({ ok: true, baseUrl: 'http://localhost:1234', httpStatus: 200, data: {} });
    systemToolMock.mockImplementation(async (opts) => {
      const folder = path.join(editorRoot, (opts.input as { path: string }).path);
      for (const [name, body] of Object.entries(skills)) {
        fs.mkdirSync(path.join(folder, name), { recursive: true });
        fs.writeFileSync(path.join(folder, name, 'SKILL.md'), `---\nname: ${name}\nmetadata:\n  generated-by: unreal-mcp\n---\n${body}\n`);
      }
      return { kind: 'success', success: true, endpoint: 'x', httpStatus: 200,
        data: { status: 'success', structured: { skillsPath: folder, filesWritten: Object.keys(skills).length, filesPruned: 0, toolsConsidered: 40 } } };
    });
  }

  it('asks a running editor for its skills (unreal-rql, tools) with the agent\'s folder, then writes the offline skills', async () => {
    const dir = tmp();
    editorAt(dir);
    const r = await setupSkills({ projectDir: dir, agentId: 'cursor', url: 'http://localhost:1234' });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(systemToolMock).toHaveBeenCalledTimes(1);
    expect(systemToolMock.mock.calls[0][0]).toMatchObject({ toolName: 'unreal-skill-generate', url: 'http://localhost:1234' });
    expect((systemToolMock.mock.calls[0][0].input as { path: string }).path.startsWith('Saved/UnrealMcp/skills-staging/')).toBe(true);
    expect(r.editorSkills).toEqual({ generated: true, message: `2 file(s) written in ${path.join(dir, '.cursor', 'skills')}` });
    expect(read(path.join(dir, '.cursor', 'skills', 'unreal-rql', 'SKILL.md'))).toContain('rql guide');
    expect(fs.existsSync(path.join(dir, '.cursor', 'skills', 'unreal-mcp-cli', 'SKILL.md'))).toBe(true);
    expect(fs.existsSync(path.join(dir, 'Saved', 'UnrealMcp', 'skills-staging')), 'the staging folder is removed').toBe(false);
  });

  it('writes the skills of an editor serving ANOTHER project into <path>, never into that editor\'s project', async () => {
    const dir = tmp();
    const editorProject = tmp();
    editorAt(editorProject);
    const r = await setupSkills({ projectDir: dir, url: 'http://localhost:1234' });
    expect(r.kind === 'success' && r.editorSkills).toMatchObject({ generated: true });
    expect(read(skillsOf(dir, 'unreal-rql', 'SKILL.md'))).toContain('rql guide');
    expect(read(skillsOf(dir, 'rql-query', 'SKILL.md'))).toContain('tool');
    expect(fs.existsSync(path.join(editorProject, '.claude')), 'nothing lands in the editor\'s own skills folder').toBe(false);
    expect(fs.readdirSync(editorProject).filter((n) => n !== 'Saved'), 'nothing else either').toEqual([]);
    expect(fs.existsSync(path.join(editorProject, 'Saved', 'UnrealMcp', 'skills-staging'))).toBe(false);
  });

  it('prunes editor skills the editor no longer generates, keeps its own and other skills, and is idempotent', async () => {
    const dir = tmp();
    const stale = skillsOf(dir, 'old-tool', 'SKILL.md');
    const other = skillsOf(dir, 'notes', 'SKILL.md');
    fs.mkdirSync(path.dirname(stale), { recursive: true });
    fs.writeFileSync(stale, '---\nname: old-tool\nmetadata:\n  generated-by: unreal-mcp\n---\n');
    fs.mkdirSync(path.dirname(other), { recursive: true });
    fs.writeFileSync(other, '---\nname: notes\n---\nmine\n');
    editorAt(tmp());
    const first = await setupSkills({ projectDir: dir, url: 'http://localhost:1234' });
    expect(first.kind === 'success' && first.editorSkills).toMatchObject({ generated: true, message: expect.stringContaining('1 stale pruned') });
    expect(fs.existsSync(stale)).toBe(false);
    expect(read(other)).toContain('mine');
    expect(fs.existsSync(skillsOf(dir, 'unreal-mcp-cli', 'SKILL.md'))).toBe(true);
    const second = await setupSkills({ projectDir: dir, url: 'http://localhost:1234' });
    expect(second.kind === 'success' && second.editorSkills).toEqual({ generated: true, message: `0 file(s) written in ${skillsOf(dir)}` });
    expect(second.kind === 'success' && second.written).toBe(false);
  });

  it('never copies from or removes a folder other than the staging folder it asked for', async () => {
    const dir = tmp();
    const elsewhere = path.join(tmp(), '.claude', 'skills');
    fs.mkdirSync(path.join(elsewhere, 'keep-me'), { recursive: true });
    fs.writeFileSync(path.join(elsewhere, 'keep-me', 'SKILL.md'), 'mine\n');
    probeMock.mockResolvedValue({ ok: true, baseUrl: 'http://localhost:1234', httpStatus: 200, data: {} });
    systemToolMock.mockResolvedValue({ kind: 'success', success: true, endpoint: 'x', httpStatus: 200,
      data: { status: 'success', structured: { skillsPath: elsewhere, filesWritten: 1 } } });
    const r = await setupSkills({ projectDir: dir, url: 'http://localhost:1234' });
    expect(r.kind === 'success' && r.editorSkills).toMatchObject({ generated: false, reason: expect.stringContaining('not to the staging folder') });
    expect(read(path.join(elsewhere, 'keep-me', 'SKILL.md'))).toBe('mine\n');
    expect(fs.existsSync(skillsOf(dir, 'keep-me'))).toBe(false);
  });

  it('says so when the editor wrote its skills on another machine', async () => {
    const dir = tmp();
    probeMock.mockResolvedValue({ ok: true, baseUrl: 'http://localhost:1234', httpStatus: 200, data: {} });
    systemToolMock.mockResolvedValue({ kind: 'success', success: true, endpoint: 'x', httpStatus: 200,
      data: { status: 'success', structured: { skillsPath: '/nowhere/on/this/machine', filesWritten: 41 } } });
    const r = await setupSkills({ projectDir: dir, url: 'http://localhost:1234' });
    expect(r.kind === 'success' && r.editorSkills).toMatchObject({ generated: false, reachable: true, reason: expect.stringContaining('not on this machine') });
  });

  it('without an editor writes the offline skills and says unreal-rql needs the editor', async () => {
    const dir = tmp();
    const r = await setupSkills({ projectDir: dir, url: 'http://127.0.0.1:1' });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(probeMock).toHaveBeenCalledTimes(1);
    expect(systemToolMock).not.toHaveBeenCalled();
    expect(r.editorSkills).toMatchObject({ generated: false, reachable: false });
    expect(r.filesWritten).toHaveLength(4);
    expect(EDITOR_SKILLS_HINT).toContain('`unreal-rql` (RQL + extension guides) needs the editor');
    // How to get it from a shell, and the guides without it (DEV-112).
    expect(EDITOR_SKILLS_HINT).toContain('`unreal-mcp-cli open --headless`');
    expect(EDITOR_SKILLS_HINT).toContain('run-tool rql-schema --input \'{"guide": "<name>"}\'');
  });

  it('reports an editor that answers but fails to generate, and still writes the offline skills', async () => {
    const dir = tmp();
    probeMock.mockResolvedValue({ ok: true, baseUrl: 'http://localhost:1234', httpStatus: 200, data: {} });
    systemToolMock.mockResolvedValue({ kind: 'failure', success: false, endpoint: 'x', reason: 'http-error', httpStatus: 500, data: { error: '[tool_error] no tools' }, message: 'HTTP 500' });
    const r = await setupSkills({ projectDir: dir, url: 'http://localhost:1234' });
    expect(r.kind === 'success' && r.editorSkills).toEqual({ generated: false, reachable: true, reason: '[tool_error] no tools' });
    expect(fs.existsSync(skillsOf(dir, 'unreal-mcp-cli', 'SKILL.md'))).toBe(true);
  });

  it('keeps a same-named skill someone else wrote when the editor step overwrites it, unless --force', async () => {
    const dir = tmp();
    const mine = skillsOf(dir, 'unreal-rql', 'SKILL.md');
    fs.mkdirSync(path.dirname(mine), { recursive: true });
    fs.writeFileSync(mine, '---\nname: unreal-rql\n---\nmy own notes\n');
    // The editor's skill of the same name replaces the folder.
    editorAt(tmp(), { 'unreal-rql': "the editor's" });
    const kept = await setupSkills({ projectDir: dir, url: 'http://localhost:1234' });
    expect(read(mine)).toContain('my own notes');
    expect(kept.warnings.join('\n')).toContain('the editor\'s version was not kept');
    const forced = await setupSkills({ projectDir: dir, url: 'http://localhost:1234', force: true });
    expect(forced.kind).toBe('success');
    expect(read(mine)).toContain("the editor's");
  });

  it('skips the editor step offline', async () => {
    const dir = tmp();
    const r = await setupSkills({ projectDir: dir, editor: false });
    expect(probeMock).not.toHaveBeenCalled();
    expect(r.kind === 'success' && r.editorSkills).toBeUndefined();
  });
});
