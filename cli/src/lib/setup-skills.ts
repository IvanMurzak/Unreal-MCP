// `setup-skills` — write the agent skills into a project's skills folder: the `unreal-mcp-cli` skill (cold-start
// workflow, RQL recipes, exit codes; references/ for depth) and `ai-game-dev-app`, generated offline from the CLI's own
// data (`skill-guides.ts`). Idempotent: the same CLI writes the same bytes. When the project's editor answers, the
// editor's own skills come first (`unreal-skill-generate`: one per tool plus `unreal-rql`, the RQL guide with the
// installed extensions' guides), which only the running editor knows. Library-safe.

import * as fs from 'fs';
import * as path from 'path';
import { getAgentById, getAgentIds } from '../utils/agents.js';
import { asError } from '../utils/error.js';
import { emitProgress } from './progress.js';
import type { ProgressCallback } from './types.js';
import { CLI_NAME } from './cli-guide.js';
import { buildGuideFiles, RETIRED_SKILL_NAMES, SKILL_METADATA } from './skill-guides.js';
import { resolveConnection } from '../utils/config.js';
import { probePing } from '../utils/probe.js';
import { runSystemTool } from './run-tool.js';
import { isToolSuccess } from './session.js';
import { toolErrorText, toolResponseText } from './tool-outcome.js';

export interface SetupSkillsOptions {
  projectDir?: string;
  /** The agent whose skills folder receives the skills (`setup-skills --list`). Default: `claude-code`. */
  agentId?: string;
  /** Also overwrite skill files this CLI did not generate (no `generated-by: unreal-mcp` metadata). */
  force?: boolean;
  /** Also ask the project's running editor for its skills (default true); false = offline only. */
  editor?: boolean;
  /** Server URL / token overrides for the editor step (default: the project's resolved connection). */
  url?: string;
  token?: string;
  /** How long the editor step waits for the reachability probe (default 3000 ms). */
  probeTimeoutMs?: number;
  /** Injectable fetch for the editor step (tests). */
  fetchImpl?: typeof fetch;
  onProgress?: ProgressCallback;
}

/**
 * The editor step: `generated` = the editor wrote its skills (`unreal-rql` with the extension guides and one per tool);
 * otherwise why not (no editor answered, or its generation failed) - the offline skills are written either way.
 */
export type EditorSkillsOutcome =
  | { generated: true; message: string }
  | { generated: false; reachable: boolean; reason: string };

/** What setup-skills prints when no editor wrote `unreal-rql`. */
export const EDITOR_SKILLS_HINT =
  '`unreal-rql` (RQL + extension guides) needs the editor: open it and run setup-skills again, or click "Generate skill files" in the editor\'s AI Game Developer window, or enable auto-generate there.';

export interface SetupSkillsSuccess {
  kind: 'success';
  success: true;
  /** The agent's skills folder. */
  skillsDir: string;
  /** The `unreal-mcp-cli` SKILL.md. */
  skillPath: string;
  /** Files whose content changed (created or rewritten). */
  filesWritten: string[];
  /** Retired skill folders of earlier versions that were removed. */
  removed: string[];
  /** Skill folders left alone because someone else wrote a skill of that name (see `force`). */
  skipped: string[];
  /** True when any file changed. */
  written: boolean;
  /** The editor's skills (`unreal-rql`, per-tool skills); absent when the editor step was turned off. */
  editorSkills?: EditorSkillsOutcome;
  warnings: string[];
}

export interface SetupSkillsFailure {
  kind: 'failure';
  success: false;
  warnings: string[];
  error: Error;
}

export type SetupSkillsResult = SetupSkillsSuccess | SetupSkillsFailure;

export const DEFAULT_SKILLS_AGENT = 'claude-code';

const GENERATED_MARKER = `generated-by: ${SKILL_METADATA['generated-by']}`;

/** A file this CLI generated (its frontmatter carries our metadata); reference files are judged by their SKILL.md. */
function isGenerated(skillMd: string): boolean {
  try {
    return fs.readFileSync(skillMd, 'utf-8').includes(GENERATED_MARKER);
  } catch {
    return false;
  }
}

export async function setupSkills(opts: SetupSkillsOptions = {}): Promise<SetupSkillsResult> {
  const warnings: string[] = [];
  try {
    const projectDir = path.resolve(opts.projectDir ?? process.cwd());
    if (!fs.existsSync(projectDir)) throw new Error(`Project directory does not exist: ${projectDir}`);
    const agentId = opts.agentId ?? DEFAULT_SKILLS_AGENT;
    const agent = getAgentById(agentId);
    if (!agent) throw new Error(`Unknown agent: "${agentId}". Available agent IDs: ${getAgentIds().join(', ')}`);
    if (!agent.skillsPath) throw new Error(`Agent "${agent.name}" does not support skills.`);

    const skillsDir = path.join(projectDir, agent.skillsPath);
    emitProgress(opts.onProgress, { phase: 'start', message: `Writing ${agent.name} skills to ${skillsDir}` });

    // The editor's skills first: the offline files below then keep this CLI's version of the skills both write. The editor
    // overwrites every folder it generates, so a same-named skill someone else wrote is put back afterwards (see `force`).
    let editorSkills: EditorSkillsOutcome | undefined;
    if (opts.editor !== false) {
      const foreign = opts.force ? [] : foreignSkills(skillsDir);
      editorSkills = await generateEditorSkills(projectDir, agent.skillsPath, opts);
      const ownNames = new Set(buildGuideFiles().map((f) => f.relativePath.split('/')[0]));
      for (const skill of foreign) {
        if (fs.readFileSync(skill.file, 'utf-8') === skill.content) continue;
        fs.writeFileSync(skill.file, skill.content, 'utf-8');
        // The offline step below warns about its own skill names.
        if (!ownNames.has(path.basename(path.dirname(skill.file)))) {
          warnings.push(`${skill.file} was not written by unreal-mcp: the editor's version was not kept (--force keeps it).`);
        }
      }
    }

    const filesWritten: string[] = [];
    const skipped = new Set<string>();
    for (const file of buildGuideFiles()) {
      const skillDir = file.relativePath.split('/')[0];
      const target = path.join(skillsDir, file.relativePath);
      const skillMd = path.join(skillsDir, skillDir, 'SKILL.md');
      // Someone else's skill under the same name stays unless --force.
      if (!opts.force && fs.existsSync(skillMd) && !isGenerated(skillMd)) {
        if (!skipped.has(skillDir)) warnings.push(`${skillMd} was not written by unreal-mcp-cli: left as is (--force overwrites it).`);
        skipped.add(skillDir);
        continue;
      }
      if (fs.existsSync(target) && fs.readFileSync(target, 'utf-8') === file.content) continue;
      fs.mkdirSync(path.dirname(target), { recursive: true });
      fs.writeFileSync(target, file.content, 'utf-8');
      filesWritten.push(target);
    }

    // The retired stub would teach the outdated workflow next to the new skill: removed once the new skill is in place,
    // and only when we wrote it.
    const removed: string[] = [];
    if (!skipped.has(CLI_NAME)) {
      for (const name of RETIRED_SKILL_NAMES) {
        const dir = path.join(skillsDir, name);
        if (isGenerated(path.join(dir, 'SKILL.md'))) {
          fs.rmSync(dir, { recursive: true, force: true });
          removed.push(dir);
        }
      }
    }

    emitProgress(opts.onProgress, {
      phase: 'done',
      message: filesWritten.length > 0 ? `${filesWritten.length} skill file(s) written.` : 'Skills are up to date.',
    });
    return {
      kind: 'success',
      success: true,
      skillsDir,
      skillPath: path.join(skillsDir, CLI_NAME, 'SKILL.md'),
      filesWritten,
      removed,
      skipped: [...skipped],
      written: filesWritten.length > 0 || removed.length > 0,
      ...(editorSkills ? { editorSkills } : {}),
      warnings,
    };
  } catch (err: unknown) {
    return { kind: 'failure', success: false, warnings, error: asError(err) };
  }
}

/** The SKILL.md files under @p skillsDir that this CLI and the editor did not write (no `generated-by` metadata). */
function foreignSkills(skillsDir: string): { file: string; content: string }[] {
  let dirs: fs.Dirent[];
  try {
    dirs = fs.readdirSync(skillsDir, { withFileTypes: true });
  } catch {
    return [];
  }
  return dirs
    .filter((d) => d.isDirectory())
    .map((d) => path.join(skillsDir, d.name, 'SKILL.md'))
    .filter((file) => fs.existsSync(file) && !isGenerated(file))
    .map((file) => ({ file, content: fs.readFileSync(file, 'utf-8') }));
}

/**
 * Ask the project's editor (when one answers the ping) to write its skills into the agent's folder through the
 * `unreal-skill-generate` system tool. Never throws: no editor is an outcome, not an error.
 */
async function generateEditorSkills(projectDir: string, skillsPath: string, opts: SetupSkillsOptions): Promise<EditorSkillsOutcome> {
  let url: string;
  let token: string | undefined;
  try {
    const conn = await resolveConnection({ projectDir, url: opts.url, token: opts.token });
    url = conn.url;
    token = conn.token;
  } catch (err) {
    return { generated: false, reachable: false, reason: asError(err).message };
  }
  const probe = await probePing(url, { token, timeoutMs: opts.probeTimeoutMs ?? 3000, fetchImpl: opts.fetchImpl });
  if (!probe.ok) return { generated: false, reachable: false, reason: `no editor answered at ${url} (${probe.reason})` };
  emitProgress(opts.onProgress, { phase: 'info', message: 'Asking the editor for its skills (unreal-rql, tools)' });
  const result = await runSystemTool({
    toolName: 'unreal-skill-generate',
    url,
    token,
    input: { path: skillsPath.replace(/\\/g, '/') },
    timeoutMs: 60_000,
    fetchImpl: opts.fetchImpl,
  });
  if (result.kind === 'failure') return { generated: false, reachable: true, reason: toolErrorText(result) };
  if (!isToolSuccess(result.data)) return { generated: false, reachable: true, reason: toolResponseText(result.data) ?? JSON.stringify(result.data) };
  return { generated: true, message: generatedText(result.data) };
}

/** "N file(s) written (M stale pruned) in <folder>" from the tool's structured answer, else its text. */
function generatedText(data: unknown): string {
  const structured = (data as { structured?: { filesWritten?: unknown; filesPruned?: unknown; skillsPath?: unknown } } | null)?.structured;
  if (structured && typeof structured.filesWritten === 'number') {
    const pruned = typeof structured.filesPruned === 'number' && structured.filesPruned > 0 ? `, ${structured.filesPruned} stale pruned` : '';
    return `${structured.filesWritten} file(s) written${pruned}${typeof structured.skillsPath === 'string' ? ` in ${structured.skillsPath}` : ''}`;
  }
  return toolResponseText(data) ?? 'generated';
}
