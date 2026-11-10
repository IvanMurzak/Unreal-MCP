import { Command } from 'commander';
import { waitForReady } from '../lib/wait-for-ready.js';
import { ExitCode } from '../utils/exit-codes.js';
import { CommandOutput, FAILURE, formatOption, parseWholeNumber } from '../utils/output.js';
import * as ui from '../utils/ui.js';

export const waitForReadyCommand = new Command('wait-for-ready')
  .description('Block until the project\'s MCP server responds to a ping (exit 14 on timeout)')
  .option('-p, --path <dir>', 'Unreal project directory (defaults to cwd)')
  .option('--url <url>', 'Explicit MCP server URL override')
  .option('--token <token>', 'Bearer token override')
  .option('--timeout <ms>', 'Overall timeout in ms (default 120000)')
  .option('--interval <ms>', 'Poll interval in ms (default 2000)')
  .addOption(formatOption())
  .action(async (opts: { path?: string; url?: string; token?: string; timeout?: string; interval?: string; format: string }) => {
    const out = new CommandOutput('wait-for-ready', opts.format);
    const timeoutMs = opts.timeout === undefined ? undefined : parseWholeNumber(opts.timeout, '--timeout', 1);
    const intervalMs = opts.interval === undefined ? undefined : parseWholeNumber(opts.interval, '--interval', 1);
    const spinner = out.machine ? null : ui.startSpinner('Waiting for Unreal MCP server...');
    const result = await waitForReady({
      projectDir: opts.path,
      url: opts.url,
      token: opts.token,
      timeoutMs,
      intervalMs,
      onProgress: (e) => {
        if (e.phase === 'info' || e.phase === 'start') {
          out.progress(e.message);
          if (spinner) spinner.text = e.message;
        }
      },
    });
    const data = { url: result.url, elapsedMs: result.elapsedMs, attempts: result.attempts };
    if (result.kind === 'failure') {
      const message = `Not ready after ${result.elapsedMs}ms (${result.attempts} attempts): ${result.lastReason}`;
      if (spinner) {
        spinner.error(message);
        out.errors.push({ code: FAILURE.timeout.code, message });
        out.finish(FAILURE.timeout.exit, data);
      } else {
        out.fail(FAILURE.timeout, message, data);
      }
      return;
    }
    spinner?.success(`Ready after ${result.elapsedMs}ms (${result.attempts} attempt(s)) — ${result.url}`);
    out.finish(ExitCode.Success, data);
  });
