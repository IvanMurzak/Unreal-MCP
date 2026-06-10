// CLI entry-point re-exports (`unreal-cli/cli`).
//
// Exposes the commander Command instances so a consumer can compose them
// into their own program. The runnable program lives in `index.ts`
// (imported by `bin/unreal-cli.js`); importing THIS file has no side
// effects (it does not parse argv or exit).

export { bootstrapLocalCommand } from './commands/bootstrap-local.js';
export { closeCommand } from './commands/close.js';
export { configureCommand } from './commands/configure.js';
export { createProjectCommand } from './commands/create-project.js';
export { installEngineCommand } from './commands/install-engine.js';
export { installPluginCommand } from './commands/install-plugin.js';
export { loginCommand } from './commands/login.js';
export { openCommand } from './commands/open.js';
export { removePluginCommand } from './commands/remove-plugin.js';
export { runSystemToolCommand } from './commands/run-system-tool.js';
export { runToolCommand } from './commands/run-tool.js';
export { setupMcpCommand } from './commands/setup-mcp.js';
export { setupSkillsCommand } from './commands/setup-skills.js';
export { statusCommand } from './commands/status.js';
export { updateCommand } from './commands/update.js';
export { waitForReadyCommand } from './commands/wait-for-ready.js';
