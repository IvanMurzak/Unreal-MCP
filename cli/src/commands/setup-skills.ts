import { Command, Option } from 'commander';
import { DEFAULT_SKILLS_AGENT, EDITOR_SKILLS_HINT, setupSkills } from '../lib/setup-skills.js';
import * as fs from 'fs';
import { getAgentById, listAgentTable } from '../utils/agents.js';
import * as ui from '../utils/ui.js';

export const setupSkillsCommand = new Command('setup-skills')
  .description('Write the agent skills into an AI agent\'s skills folder: how to use unreal-mcp-cli from a cold start and ai-game-dev-app (offline), plus, when the project\'s editor is open, its unreal-rql skill (RQL + installed extension guides) and one skill per tool')
  .argument('[agent-id]', `Agent whose skills folder receives them (default: ${DEFAULT_SKILLS_AGENT}; --list shows all)`)
  .argument('[path]', 'Unreal project directory (defaults to cwd)')
  .option('-p, --path <dir>', 'Unreal project directory (same as [path])')
  .option('--list', 'List the agents and their skills folders')
  .option('--force', 'Also overwrite a same-named skill that unreal-mcp-cli did not write')
  .option('--offline', 'Do not ask a running editor for its skills (unreal-rql, tools)')
  .option('--url <url>', 'Server URL of the editor step (default: the project\'s connection)')
  .option('--token <token>', 'Bearer token of the editor step')
  .action(async (agentId: string | undefined, positionalPath: string | undefined, opts: { path?: string; list?: boolean; force?: boolean; offline?: boolean; url?: string; token?: string }) => {
    if (opts.list) {
      listAgentTable('AI Agents — Skills Support', 'Skills Path', (a) => a.skillsPath ?? '—');
      return;
    }
    // `setup-skills <dir>`: a first argument that is no agent but an existing folder is the project.
    if (agentId !== undefined && positionalPath === undefined && !getAgentById(agentId) && fs.existsSync(agentId)) {
      positionalPath = agentId;
      agentId = undefined;
    }
    const result = await setupSkills({
      projectDir: positionalPath ?? opts.path, agentId, force: opts.force, editor: !opts.offline, url: opts.url, token: opts.token,
    });
    ui.printWarnings(result.warnings);
    if (result.kind === 'failure') {
      ui.error(result.error.message);
      process.exitCode = 1;
      return;
    }
    const editor = result.editorSkills;
    if (editor?.generated) ui.success(`Editor skills (unreal-rql with the extension guides, one per tool): ${editor.message}.`);
    else if (editor?.reachable) ui.warn(`The editor did not write its skills: ${editor.reason}`);
    if (editor && !editor.generated) ui.info(EDITOR_SKILLS_HINT);
    if (result.skipped.length > 0 && !result.written) {
      ui.info(`Not written: ${result.skipped.join(', ')} (see the warning above).`);
      return;
    }
    if (result.written) ui.success(`Skills written to ${result.skillsDir} (${result.filesWritten.length} file(s) changed${result.removed.length ? `, ${result.removed.length} retired skill(s) removed` : ''}).`);
    else ui.info(`Skills in ${result.skillsDir} are up to date.`);
  });
