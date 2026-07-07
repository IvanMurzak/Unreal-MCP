import { Command } from 'commander';
import { openProject } from '../lib/open.js';
import type { AuthOption, McpTransport } from '../lib/types.js';
import * as ui from '../utils/ui.js';

export const openCommand = new Command('open')
  .description('Launch the Unreal Editor for a project, wiring MCP connection env vars and pre-building native modules on desktop platforms when needed')
  .argument('[path]', 'Unreal project directory (defaults to cwd)')
  .option('--engine-root <dir>', 'Explicit engine install root (source builds)')
  .option('--no-build', 'Skip the pre-launch build on desktop platforms (default: auto-build when native modules need compilation)')
  .option(
    '--no-auto-dismiss-startup-dialogs',
    'Disable auto-dismissal of known Unreal startup blocker dialogs on desktop platforms (default: enabled)',
  )
  .option(
    '--startup-dismiss-timeout-ms <ms>',
    'Overall timeout (milliseconds) for startup-dialog auto-dismiss polling (default: 12000)',
    '12000',
  )
  .option(
    '--startup-dismiss-poll-interval-ms <ms>',
    'Polling tick interval (milliseconds) for startup-dialog auto-dismiss (default: 1000)',
    '1000',
  )
  .option('--no-cache', 'Skip the persistent engine-path cache (force fresh discovery)')
  .option('--no-connect', 'Do not wire UNREAL_MCP_* env vars onto the editor')
  .option('--host <url>', 'UNREAL_MCP_HOST')
  .option('--token <token>', 'UNREAL_MCP_TOKEN')
  .option('--auth <option>', 'UNREAL_MCP_AUTH_OPTION: none | required')
  .option('--tools <list>', 'UNREAL_MCP_TOOLS override')
  .option('--keep-connected', 'UNREAL_MCP_KEEP_CONNECTED=true')
  .option('--transport <t>', 'UNREAL_MCP_TRANSPORT: stdio | http')
  .option('--start-server', 'UNREAL_MCP_START_SERVER=true')
  .option('--connection-mode <mode>', 'UNREAL_MCP_CONNECTION_MODE: Custom | Cloud | (default empty = plugin default)')
  .action(async (pathArg: string | undefined, opts) => {
    const startupDismissTimeoutMs = parsePositiveIntFlag(
      opts.startupDismissTimeoutMs,
      '--startup-dismiss-timeout-ms',
      12000,
    );
    const startupDismissPollIntervalMs = parsePositiveIntFlag(
      opts.startupDismissPollIntervalMs,
      '--startup-dismiss-poll-interval-ms',
      1000,
    );
    const spinner = ui.startSpinner('Opening Unreal project...');
    const result = await openProject({
      projectDir: pathArg,
      engineRoot: opts.engineRoot,
      build: opts.build !== false,
      autoDismissStartupDialogs: opts.autoDismissStartupDialogs !== false,
      startupDismissTimeoutMs,
      startupDismissPollIntervalMs,
      noCache: opts.cache === false,
      noConnect: opts.connect === false,
      host: opts.host,
      token: opts.token,
      auth: opts.auth as AuthOption | undefined,
      tools: opts.tools,
      keepConnected: opts.keepConnected,
      transport: opts.transport as McpTransport | undefined,
      startServer: opts.startServer,
      connectionMode: opts.connectionMode,
      onProgress: (event) => {
        if (event.phase === 'startup-dialog-dismissed') {
          ui.info(
            `[open] dismissed Unreal startup dialog (dialog=${event.dialog}, button=${event.button}, platform=${event.platform})`,
          );
          return;
        }
        spinner.text = event.message;
      },
    });
    if (result.kind === 'failure') {
      spinner.error(result.errorMessage);
      ui.printWarnings(result.warnings);
      process.exitCode = 1;
      return;
    }
    spinner.success(`Launched Unreal Editor (PID: ${result.editorPid ?? 'unknown'})`);
    ui.printWarnings(result.warnings);
    ui.label('editor', result.editorPath);
    ui.label('engine', result.engineRoot);
  });

function parsePositiveIntFlag(
  rawValue: string | undefined,
  flagName: string,
  defaultValue: number,
): number {
  if (rawValue === undefined) return defaultValue;
  const parsed = Number.parseInt(rawValue, 10);
  if (!Number.isFinite(parsed) || parsed <= 0) {
    ui.error(`${flagName} must be a positive integer`);
    process.exit(1);
  }
  return parsed;
}
