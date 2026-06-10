import { Command } from 'commander';
import { fileURLToPath } from 'url';
import * as path from 'path';
import { installPlugin } from '../lib/install-plugin.js';
import { defaultPluginSource } from '../utils/repo.js';
import * as ui from '../utils/ui.js';

const HERE = path.dirname(fileURLToPath(import.meta.url));

export const installPluginCommand = new Command('install-plugin')
  .description('Install the UnrealMCP plugin into <project>/Plugins (copy or --junction)')
  .argument('[path]', 'Unreal project directory (defaults to cwd)')
  .option('--plugin-source <dir>', 'UnrealMCP plugin source dir (defaults to the repo plugin)')
  .option('--junction', 'Create a dev junction instead of copying (Windows)')
  .action(async (pathArg: string | undefined, opts: { pluginSource?: string; junction?: boolean }) => {
    const projectDir = pathArg ?? process.cwd();
    const pluginSourceDir = opts.pluginSource ?? defaultPluginSource(HERE);
    if (!pluginSourceDir) {
      ui.error('Could not locate the UnrealMCP plugin source. Pass --plugin-source <dir>.');
      process.exitCode = 1;
      return;
    }
    const result = await installPlugin({ projectDir, pluginSourceDir, junction: opts.junction });
    ui.printWarnings(result.warnings);
    if (result.kind === 'failure') {
      ui.error(result.error.message);
      process.exitCode = 1;
      return;
    }
    ui.success(`Installed plugin (${result.mode}) at ${result.installedPath}`);
  });
