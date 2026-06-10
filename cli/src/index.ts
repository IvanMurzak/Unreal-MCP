import { Command } from 'commander';
import { statusCommand } from './commands/status.js';
import { PACKAGE_VERSION } from './version.js';

const program = new Command();

program
  .name('unreal-cli')
  .description('Cross-platform CLI tool for Unreal-MCP operations (pre-alpha scaffold)')
  .version(PACKAGE_VERSION);

// Scaffold stage: only `status`. The full command set (create-project, open,
// close, install-plugin, remove-plugin, configure, setup-mcp, login, run-tool,
// run-system-tool, wait-for-ready, bootstrap-local, update, install-engine,
// setup-skills) lands with the unreal-cli task — see docs/ARCHITECTURE.md §9.1.
program.addCommand(statusCommand);

program.parseAsync(process.argv).catch((err: unknown) => {
  console.error(err instanceof Error ? err.message : String(err));
  process.exit(1);
});
