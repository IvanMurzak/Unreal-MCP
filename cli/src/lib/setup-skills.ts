// `setup-skills` — write the agent skills into a project's skills folder: the `unreal-mcp-cli` skill (cold-start
// workflow, RQL recipes, exit codes; references/ for depth) and `ai-game-dev-app`. Generated offline from the CLI's own
// data (`skill-guides.ts`): no editor, no server, no network. Idempotent: the same CLI writes the same bytes.
// Library-safe.

import * as fs from 'fs';
import * as path from 'path';
import { getAgentById, getAgentIds } from '../utils/agents.js';
import { asError } from '../utils/error.js';
import { emitProgress } from './progress.js';
import type { ProgressCallback } from './types.js';
import { CLI_NAME } from './cli-guide.js';
import { buildGuideFiles, RETIRED_SKILL_NAMES, SKILL_METADATA } from './skill-guides.js';

export interface SetupSkillsOptions {
  projectDir?: string;
  /** The agent whose skills folder receives the skills (`setup-skills --list`). Default: `claude-code`. */
  agentId?: string;
  /** Also overwrite skill files this CLI did not generate (no `generated-by: unreal-mcp` metadata). */
  force?: boolean;
  onProgress?: ProgressCallback;
}

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
  /** True when any file changed. */
  written: boolean;
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

    // Retired stubs of earlier versions would teach the outdated workflow next to the new skill: remove ours only.
    const removed: string[] = [];
    for (const name of RETIRED_SKILL_NAMES) {
      const dir = path.join(skillsDir, name);
      if (isGenerated(path.join(dir, 'SKILL.md'))) {
        fs.rmSync(dir, { recursive: true, force: true });
        removed.push(dir);
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
      written: filesWritten.length > 0 || removed.length > 0,
      warnings,
    };
  } catch (err: unknown) {
    return { kind: 'failure', success: false, warnings, error: asError(err) };
  }
}
