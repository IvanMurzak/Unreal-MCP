import { Command } from 'commander';
import { waitForReady } from '../lib/wait-for-ready.js';
import * as ui from '../utils/ui.js';

export const waitForReadyCommand = new Command('wait-for-ready')
  .description('Block until the project\'s MCP server responds to a ping')
  .option('-p, --path <dir>', 'Unreal project directory (defaults to cwd)')
  .option('--url <url>', 'Explicit MCP server URL override')
  .option('--token <token>', 'Bearer token override')
  .option('--timeout <ms>', 'Overall timeout in ms (default 120000)', (v) => parseInt(v, 10))
  .option('--interval <ms>', 'Poll interval in ms (default 2000)', (v) => parseInt(v, 10))
  .action(async (opts: { path?: string; url?: string; token?: string; timeout?: number; interval?: number }) => {
    const spinner = ui.startSpinner('Waiting for Unreal MCP server...');
    const result = await waitForReady({
      projectDir: opts.path,
      url: opts.url,
      token: opts.token,
      timeoutMs: opts.timeout,
      intervalMs: opts.interval,
      onProgress: (e) => {
        if (e.phase === 'info' || e.phase === 'start') spinner.text = e.message;
      },
    });
    if (result.kind === 'failure') {
      spinner.error(`Not ready after ${result.elapsedMs}ms (${result.attempts} attempts): ${result.lastReason}`);
      process.exitCode = 1;
      return;
    }
    spinner.success(`Ready after ${result.elapsedMs}ms (${result.attempts} attempt(s)) — ${result.url}`);
  });
