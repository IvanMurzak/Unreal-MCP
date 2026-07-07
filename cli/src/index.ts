import { Command } from 'commander';
import { PACKAGE_VERSION } from './version.js';
import { configureStyledHelp, setVerbose } from './utils/ui.js';
import { bootstrapLocalCommand } from './commands/bootstrap-local.js';
import { closeCommand } from './commands/close.js';
import { configureCommand } from './commands/configure.js';
import { createProjectCommand } from './commands/create-project.js';
import { installEngineCommand } from './commands/install-engine.js';
import { installExtensionCommand } from './commands/install-extension.js';
import { installPluginCommand } from './commands/install-plugin.js';
import { loginCommand } from './commands/login.js';
import { openCommand } from './commands/open.js';
import { removePluginCommand } from './commands/remove-plugin.js';
import { runSystemToolCommand } from './commands/run-system-tool.js';
import { runToolCommand } from './commands/run-tool.js';
import { setupMcpCommand } from './commands/setup-mcp.js';
import { setupSkillsCommand } from './commands/setup-skills.js';
import { statusCommand } from './commands/status.js';
import { updateCommand } from './commands/update.js';
import { waitForReadyCommand } from './commands/wait-for-ready.js';

const program = new Command();

program
  .name('unreal-mcp-cli')
  .description('Cross-platform CLI tool for Unreal-MCP operations')
  .version(PACKAGE_VERSION)
  .option('-v, --verbose', 'Enable verbose diagnostic output');

// The full 17-command surface (docs/ARCHITECTURE.md §9.1).
const subcommands = [
  createProjectCommand,
  openCommand,
  closeCommand,
  installPluginCommand,
  removePluginCommand,
  installExtensionCommand,
  configureCommand,
  setupMcpCommand,
  loginCommand,
  statusCommand,
  waitForReadyCommand,
  runToolCommand,
  runSystemToolCommand,
  bootstrapLocalCommand,
  updateCommand,
  installEngineCommand,
  setupSkillsCommand,
];

for (const cmd of subcommands) {
  cmd.option('-v, --verbose', 'Enable verbose diagnostic output');
  configureStyledHelp(cmd);
  program.addCommand(cmd);
}

configureStyledHelp(program, PACKAGE_VERSION);

program.hook('preAction', (_thisCommand, actionCommand) => {
  const opts = actionCommand.optsWithGlobals() as { verbose?: boolean };
  if (opts.verbose) setVerbose(true);
});

// Show help when invoked with no subcommand.
program.action(() => {
  program.outputHelp();
});

program.parseAsync(process.argv).catch((err: unknown) => {
  process.stderr.write(`${err instanceof Error ? err.message : String(err)}\n`);
  process.exit(1);
});
