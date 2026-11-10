import { Command, Option } from 'commander';
import { close } from '../lib/close.js';
import { closeOwnedSession, readSessionLease } from '../lib/session.js';
import { readUProject } from '../utils/project.js';
import * as path from 'path';

/** The project root of a path argument (a `.uproject` path or a folder; default cwd). */
function resolveDir(pathArg: string | undefined): string {
  const input = path.resolve(pathArg ?? process.cwd());
  return readUProject(input)?.projectDir ?? input;
}
import * as ui from '../utils/ui.js';

export const closeCommand = new Command('close')
  .description('Close the Unreal Editor running a project: an editor the CLI started saves and quits; any other is terminated')
  .argument('[path]', 'Unreal project directory (defaults to cwd)')
  .option('--owned-only', 'Only close an editor the CLI started (exec / open --headless): it saves and quits; an editor the user opened is never touched')
  .option('--force', 'With --owned-only: kill the CLI-owned editor when it does not quit in time')
  // Deprecated, hidden: only for a lease without the server URL (an editor started by CLI 0.23).
  .addOption(new Option('--url <url>', 'With --owned-only: the server of an editor whose lease does not record it').hideHelp())
  .option('--token <token>', 'With --owned-only: bearer token override')
  .action(async (pathArg: string | undefined, opts: { ownedOnly?: boolean; force?: boolean; url?: string; token?: string }) => {
    if (opts.ownedOnly === true) {
      await closeOwned(pathArg, opts);
      return;
    }
    // An editor the CLI started saves and quits first (a kill would lose its unsaved changes); what is left is killed.
    let savedPid: number | undefined;
    if (readSessionLease(resolveDir(pathArg)) !== undefined) {
      const owned = await closeOwnedSession({ projectDir: pathArg, force: true });
      if (owned.kind === 'success' && owned.result.kind === 'closed') {
        savedPid = owned.result.pid;
        ui.success(`Closed the Unreal Editor started by the CLI (PID ${savedPid}${owned.result.forced ? ', killed after the save timed out' : ', saved'}).`);
      }
    }
    const result = await close({ projectDir: pathArg });
    ui.printWarnings(result.warnings);
    if (result.kind === 'failure') {
      ui.error(result.error.message);
      process.exitCode = 1;
      return;
    }
    if (!result.wasRunning) {
      if (savedPid === undefined) ui.info('No running Unreal Editor matched this project.');
      return;
    }
    ui.success(`Terminated ${result.terminated.length} editor process(es): ${result.terminated.join(', ')}`);
  });

async function closeOwned(pathArg: string | undefined, opts: { force?: boolean; url?: string; token?: string }): Promise<void> {
  const outcome = await closeOwnedSession({ projectDir: pathArg, force: opts.force === true, url: opts.url, token: opts.token });
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
