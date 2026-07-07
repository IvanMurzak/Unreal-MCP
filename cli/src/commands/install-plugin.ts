import { Command } from 'commander';
import { installPlugin } from '../lib/install-plugin.js';
import * as ui from '../utils/ui.js';

export const installPluginCommand = new Command('install-plugin')
  .description('Install the UnrealMCP plugin into <project>/Plugins (copy or --junction). Defaults to the local repo source when present, otherwise downloads the matching GitHub release.')
  .argument('[path]', 'Unreal project directory (defaults to cwd)')
  .option('--plugin-source <dir>', 'UnrealMCP plugin source dir (offline / CI / dev override)')
  .option('--junction', 'Create a dev junction instead of copying (Windows)')
  .action(async (pathArg: string | undefined, opts: { pluginSource?: string; junction?: boolean }) => {
    const projectDir = pathArg ?? process.cwd();
    const spinner = ui.startSpinner('Installing UnrealMCP plugin...');
    const result = await installPlugin({
      projectDir,
      pluginSourceDir: opts.pluginSource,
      junction: opts.junction,
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
    spinner.success(`Installed plugin (${result.mode}) at ${result.installedPath}`);
    ui.printWarnings(result.warnings);
  });
