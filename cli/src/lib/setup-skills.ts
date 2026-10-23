// `setup-skills` — write a Claude-Code skill stub into a project so an
// agent knows how to reach the project's Unreal MCP server. Mirrors the
// Unity/Godot CLIs' skill-bootstrap step. Library-safe.

import * as fs from 'fs';
import * as path from 'path';
import { resolveConnection, type MachineAuthOptions } from '../utils/config.js';
import { asError } from '../utils/error.js';
import { emitProgress } from './progress.js';
import type { ProgressCallback } from './types.js';
import { buildGuideFiles, skillMetadataLines, SKILL_REFERENCES } from './skill-guides.js';

export interface SetupSkillsOptions {
  projectDir?: string;
  url?: string;
  token?: string;
  force?: boolean;
  /** Machine-store token-fallback injection (tests). See `MachineAuthOptions`. */
  machineAuth?: MachineAuthOptions;
  onProgress?: ProgressCallback;
}

export interface SetupSkillsSuccess {
  kind: 'success';
  success: true;
  skillPath: string;
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

const SKILL_REL_PATH = path.join('.claude', 'skills', 'unreal-mcp', 'SKILL.md');

/** Render the skill markdown. Pure — exported for tests. */
export function renderSkill(url: string, platform: NodeJS.Platform = process.platform): string {
  const cli = platform === 'win32' ? 'unreal-mcp-cli.cmd' : 'unreal-mcp-cli';
  return `---
name: unreal-mcp
description: Drive the Unreal Editor for this project through the Unreal-MCP server. Use when the user wants to create/modify actors, blueprints, levels, or run editor tools in Unreal.
${skillMetadataLines().join('\n')}
---

# Unreal-MCP

This project is wired to a local Unreal-MCP server at \`${url}\`.

- Probe readiness: \`${cli} status\`
- Invoke a tool: \`${cli} run-tool rql-schema --input '{}'\`
- The editor must be running with the UnrealMCP plugin enabled (see \`${cli} open\`).

${SKILL_REFERENCES}
`;
}

export async function setupSkills(opts: SetupSkillsOptions = {}): Promise<SetupSkillsResult> {
  const warnings: string[] = [];
  try {
    const projectDir = path.resolve(opts.projectDir ?? process.cwd());
    if (!fs.existsSync(projectDir)) throw new Error(`Project directory does not exist: ${projectDir}`);

    let url = opts.url ?? '';
    if (!url) {
      try {
        url = (
          await resolveConnection({ projectDir, url: opts.url, token: opts.token, machineAuth: opts.machineAuth })
        ).url;
      } catch {
        url = 'http://localhost:<deterministic-port>';
        warnings.push('Could not resolve a connection URL — wrote a placeholder into the skill.');
      }
    }

    const skillPath = path.join(projectDir, SKILL_REL_PATH);
    emitProgress(opts.onProgress, { phase: 'start', message: `Writing skill to ${skillPath}` });

    let guidesWritten = false;
    for (const file of buildGuideFiles()) {
      const guidePath = path.join(projectDir, '.claude', 'skills', file.relativePath);
      if (fs.existsSync(guidePath) && !opts.force) continue;
      fs.mkdirSync(path.dirname(guidePath), { recursive: true });
      fs.writeFileSync(guidePath, file.content, 'utf-8');
      guidesWritten = true;
    }

    if (fs.existsSync(skillPath) && !opts.force) {
      emitProgress(opts.onProgress, { phase: 'done', message: 'Skill already exists (use --force to overwrite).' });
      return { kind: 'success', success: true, skillPath, written: guidesWritten, warnings };
    }

    fs.mkdirSync(path.dirname(skillPath), { recursive: true });
    fs.writeFileSync(skillPath, renderSkill(url), 'utf-8');
    emitProgress(opts.onProgress, { phase: 'done', message: 'Skill written.' });
    return { kind: 'success', success: true, skillPath, written: true, warnings };
  } catch (err: unknown) {
    return {
      kind: 'failure',
      success: false,
      warnings,
      error: asError(err),
    };
  }
}
