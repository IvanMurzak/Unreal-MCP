import { Command } from 'commander';
import { DEFAULT_SESSION_IDLE_SECONDS } from '@baizor/gamedev-cli-core';
import { openProject } from '../lib/open.js';
import { openEditorSession, prepareGuiOpen } from '../lib/session.js';
import type { AuthOption, McpTransport } from '../lib/types.js';
import * as ui from '../utils/ui.js';

export const openCommand = new Command('open')
  .description('Launch the Unreal Editor for a project, wiring MCP connection env vars and pre-building native modules on desktop platforms when needed')
  .argument('[path]', 'Unreal project directory (defaults to cwd)')
  .option('--headless', 'Start a windowless editor (-RenderOffscreen) owned by the CLI and wait until it is ready; a running editor is reused')
  .option('--url <url>', 'With --headless / --idle: the local MCP server URL the editor serves (default: the local port of the project)')
  .option('--idle <seconds>', `A CLI-owned editor saves and quits after this many seconds without tool calls (0 = never; default ${DEFAULT_SESSION_IDLE_SECONDS} with --headless)`)
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
    if (opts.headless === true || opts.idle !== undefined) {
      await openSession(pathArg, opts);
      return;
    }
    // A GUI over our own headless editor: stop that one first (a second editor on the project would fail).
    const prepared = await prepareGuiOpen({ projectDir: pathArg, url: opts.url, token: opts.token });
    if (prepared.kind === 'failure') {
      ui.error(prepared.message);
      process.exitCode = 1;
      return;
    }
    if (prepared.result.kind === 'already-open') {
      ui.success(`The Unreal Editor started by the CLI is already open with a window (PID: ${prepared.result.pid})`);
      return;
    }
    if (prepared.result.kind === 'still-running') {
      ui.error(`The headless editor started by the CLI (PID ${prepared.result.pid}) did not quit; run \`close --owned-only --force\`.`);
      process.exitCode = 1;
      return;
    }
    if (prepared.result.stoppedPid !== undefined) ui.info(`Closed the headless editor started by the CLI (PID ${prepared.result.stoppedPid}).`);
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

async function openSession(pathArg: string | undefined, opts: Record<string, unknown>): Promise<void> {
  const idleSeconds = opts.idle === undefined ? undefined : parseIntFlag(opts.idle as string, '--idle', { min: 0 });
  const mode = opts.headless === true ? 'headless' : 'window';
  const spinner = ui.startSpinner(mode === 'headless' ? 'Opening Unreal project (headless)...' : 'Opening Unreal project...');
  const result = await openEditorSession({
    projectDir: pathArg,
    mode,
    idleSeconds,
    engineRoot: opts.engineRoot as string | undefined,
    build: opts.build !== false,
    noCache: opts.cache === false,
    url: opts.url as string | undefined,
    token: opts.token as string | undefined,
    onStatus: (message) => {
      ui.verbose(message);
      spinner.text = message;
    },
  });
  if (result.kind === 'failure') {
    spinner.error(result.message);
    process.exitCode = 1;
    return;
  }
  spinner.success(result.source === 'started'
    ? `Started the Unreal Editor (${mode}, PID: ${result.pid ?? 'unknown'})`
    : `The Unreal Editor is already running (${result.source})`);
  ui.label('server', result.url);
}

/** Parse a whole-number flag of at least `min`; prints and exits on a bad value. */
export function parseIntFlag(rawValue: string, flagName: string, { min }: { min: number }): number {
  const parsed = /^\d+$/.test(rawValue.trim()) ? Number.parseInt(rawValue, 10) : Number.NaN;
  if (!Number.isSafeInteger(parsed) || parsed < min) {
    ui.error(`${flagName} must be a whole number${min > 0 ? ` of at least ${min}` : ''}`);
    process.exit(1);
  }
  return parsed;
}

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
