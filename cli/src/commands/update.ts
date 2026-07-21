import { Command } from 'commander';
import { update } from '../lib/update.js';
import * as ui from '../utils/ui.js';

export const updateCommand = new Command('update')
  .description('Update the UnrealMCP plugin installed in a project from a local source or the matching GitHub release')
  .argument('[path]', 'Unreal project directory (defaults to cwd)')
  .option('--plugin-source <dir>', 'UnrealMCP plugin source dir (offline / CI / dev override)')
  .option('--version <v>', 'Plugin-source release version to download (defaults to this CLI version)')
  .option('--force', 'Re-install even when versions match')
  .option(
    '--no-clean',
    'Do not wipe the stale UE C++ build cache after a copy-mode update (cleaning is the default; the bundled sidecar bridge is always preserved)',
  )
  .action(
    async (
      pathArg: string | undefined,
      opts: { pluginSource?: string; version?: string; force?: boolean; clean?: boolean },
    ) => {
      const projectDir = pathArg ?? process.cwd();
      const spinner = ui.startSpinner('Checking installed UnrealMCP plugin...');
      // commander maps `--no-clean` to `opts.clean === false`; absent → undefined (clean).
      const result = await update({
        projectDir,
        pluginSourceDir: opts.pluginSource,
        version: opts.version,
        force: opts.force,
        noClean: opts.clean === false,
        onProgress: (event) => {
          spinner.text = event.message;
        },
      });
      if (result.kind === 'failure') {
        spinner.error(result.error.message);
        ui.printWarnings(result.warnings);
        process.exitCode = 1;
        return;
      }
      if (result.updated) {
        const cleanedNote = result.cleaned ? ' (build cache cleaned)' : '';
        spinner.success(
          `Updated plugin ${result.fromVersion ?? 'none'} -> ${result.toVersion ?? 'unknown'}${cleanedNote}`,
        );
      } else {
        spinner.success(`Plugin already up to date (${result.toVersion ?? 'unknown'}).`);
      }
      ui.printWarnings(result.warnings);
    },
  );
