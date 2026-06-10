import { Command } from 'commander';
import { setupSkills } from '../lib/setup-skills.js';
import * as ui from '../utils/ui.js';

export const setupSkillsCommand = new Command('setup-skills')
  .description('Write a Claude-Code skill stub that drives this project\'s Unreal MCP server')
  .option('-p, --path <dir>', 'Unreal project directory (defaults to cwd)')
  .option('--url <url>', 'Explicit MCP server URL override')
  .option('--token <token>', 'Bearer token override')
  .option('--force', 'Overwrite an existing skill file')
  .action(async (opts: { path?: string; url?: string; token?: string; force?: boolean }) => {
    const result = await setupSkills({
      projectDir: opts.path,
      url: opts.url,
      token: opts.token,
      force: opts.force,
    });
    ui.printWarnings(result.warnings);
    if (result.kind === 'failure') {
      ui.error(result.error.message);
      process.exitCode = 1;
      return;
    }
    if (result.written) ui.success(`Wrote skill to ${result.skillPath}`);
    else ui.info(`Skill already exists at ${result.skillPath} (use --force to overwrite).`);
  });
