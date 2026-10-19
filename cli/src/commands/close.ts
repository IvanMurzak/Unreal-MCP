import { Command } from 'commander';
import { close } from '../lib/close.js';
import { closeOwnedSession } from '../lib/session.js';
import * as ui from '../utils/ui.js';

export const closeCommand = new Command('close')
  .description('Terminate the Unreal Editor process running a project')
  .argument('[path]', 'Unreal project directory (defaults to cwd)')
  .option('--owned-only', 'Only close an editor the CLI started (exec / open --headless): it saves and quits; an editor the user opened is never touched')
  .option('--force', 'With --owned-only: kill the CLI-owned editor when it does not quit in time')
  .option('--token <token>', 'With --owned-only: bearer token override')
  .action(async (pathArg: string | undefined, opts: { ownedOnly?: boolean; force?: boolean; token?: string }) => {
    if (opts.ownedOnly === true) {
      await closeOwned(pathArg, opts);
      return;
    }
    const result = await close({ projectDir: pathArg });
    ui.printWarnings(result.warnings);
    if (result.kind === 'failure') {
      ui.error(result.error.message);
      process.exitCode = 1;
      return;
    }
    if (!result.wasRunning) {
      ui.info('No running Unreal Editor matched this project.');
      return;
    }
    ui.success(`Terminated ${result.terminated.length} editor process(es): ${result.terminated.join(', ')}`);
  });

async function closeOwned(pathArg: string | undefined, opts: { force?: boolean; token?: string }): Promise<void> {
  const outcome = await closeOwnedSession({ projectDir: pathArg, force: opts.force === true, token: opts.token });
  if (outcome.kind === 'failure') {
    ui.error(outcome.message);
    process.exitCode = 1;
    return;
  }
  const { result } = outcome;
  if (result.kind === 'closed') {
    ui.success(`Closed the Unreal Editor started by the CLI (PID ${result.pid}${result.forced ? ', killed' : ''}).`);
  } else if (result.kind === 'not-owned') {
    ui.info(`No editor started by the CLI is running for this project${result.staleLeaseRemoved ? ' (removed a stale lease)' : ''}.`);
  } else {
    ui.error(`The editor started by the CLI (PID ${result.pid}) did not quit in time; retry with --force.`);
    process.exitCode = 1;
  }
}
