import { Command, CommanderError } from 'commander';
import { PACKAGE_VERSION } from './version.js';
import { configureStyledHelp, reserveStdout, setVerbose } from './utils/ui.js';
import { ExitCode, UsageError } from './utils/exit-codes.js';
import { formatFromArgv, isMachineFormat, renderEnvelope } from './utils/output.js';
import { bootstrapLocalCommand } from './commands/bootstrap-local.js';
import { closeCommand } from './commands/close.js';
import { commandsCommand } from './commands/commands.js';
import { compileCommand } from './commands/compile.js';
import { execCommand } from './commands/exec.js';
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
import { testCommand } from './commands/test.js';
import { toolsCommand } from './commands/tools.js';
import { updateCommand } from './commands/update.js';
import { waitForReadyCommand } from './commands/wait-for-ready.js';

const program = new Command();

program
  .name('unreal-mcp-cli')
  .description('Cross-platform CLI tool for Unreal-MCP operations')
  .version(PACKAGE_VERSION)
  .option('-v, --verbose', 'Enable verbose diagnostic output')
  // Root options are parsed only BEFORE the subcommand, so a subcommand's own
  // `--version <x.y.z>` (install-extension / install-plugin / update) reaches it
  // instead of printing the CLI version.
  .enablePositionalOptions()
  // Parse errors throw (handled below: exit 2) instead of exiting 1 inside commander.
  .exitOverride();

// The full command surface (docs/ARCHITECTURE.md §9.1).
const subcommands = [
  createProjectCommand,
  openCommand,
  closeCommand,
  execCommand,
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
  toolsCommand,
  compileCommand,
  testCommand,
  bootstrapLocalCommand,
  updateCommand,
  installEngineCommand,
  setupSkillsCommand,
  commandsCommand,
];

for (const cmd of subcommands) {
  cmd.option('-v, --verbose', 'Enable verbose diagnostic output');
  cmd.exitOverride();
  configureStyledHelp(cmd);
  program.addCommand(cmd);
}

configureStyledHelp(program, PACKAGE_VERSION);

program.hook('preAction', (_thisCommand, actionCommand) => {
  const opts = actionCommand.optsWithGlobals() as { verbose?: boolean; format?: string };
  if (opts.verbose) setVerbose(true);
  // --format json|ndjson: stdout carries only the envelope, so every log, heading and spinner goes to stderr.
  reserveStdout(isMachineFormat(opts.format));
});

// Show help when invoked with no subcommand.
program.action(() => {
  program.outputHelp();
});

/**
 * End with `exitCode`, and with an envelope on stdout when the command line asked for json / ndjson (an error thrown
 * before or outside the command's own output). exitCode, not exit(): the envelope still flushes to a pipe.
 */
function endWithError(exitCode: number, code: 'USAGE' | 'ERROR', message: string): void {
  const argv = process.argv.slice(2);
  const format = formatFromArgv(argv);
  if (isMachineFormat(format)) {
    const command = argv.find((arg) => program.commands.some((c) => c.name() === arg)) ?? program.name();
    process.stdout.write(renderEnvelope({ success: false, command, data: null, errors: [{ code, message }], warnings: [] }, format));
  }
  process.exitCode = exitCode;
}

program.parseAsync(process.argv).catch((err: unknown) => {
  if (err instanceof CommanderError) {
    // --help / --version exit 0; commander already printed the message (on stderr for an error).
    if (err.exitCode === 0) {
      process.exitCode = 0;
      return;
    }
    endWithError(ExitCode.Usage, 'USAGE', err.message.replace(/^error:\s*/, ''));
    return;
  }
  if (err instanceof UsageError) {
    process.stderr.write(`error: ${err.message}\n`);
    endWithError(ExitCode.Usage, 'USAGE', err.message);
    return;
  }
  const message = err instanceof Error ? err.message : String(err);
  process.stderr.write(`${message}\n`);
  endWithError(ExitCode.Error, 'ERROR', message);
});
