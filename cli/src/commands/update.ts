import { Command } from 'commander';
import { fileURLToPath } from 'url';
import * as path from 'path';
import { update } from '../lib/update.js';
import { defaultPluginSource } from '../utils/repo.js';
import * as ui from '../utils/ui.js';

const HERE = path.dirname(fileURLToPath(import.meta.url));

export const updateCommand = new Command('update')
  .description('Update the UnrealMCP plugin installed in a project from the repo source')
  .argument('[path]', 'Unreal project directory (defaults to cwd)')
  .option('--plugin-source <dir>', 'UnrealMCP plugin source dir (defaults to the repo plugin)')
  .option('--force', 'Re-install even when versions match')
  .action(async (pathArg: string | undefined, opts: { pluginSource?: string; force?: boolean }) => {
    const projectDir = pathArg ?? process.cwd();
    const pluginSourceDir = opts.pluginSource ?? defaultPluginSource(HERE);
    if (!pluginSourceDir) {
      ui.error('Could not locate the UnrealMCP plugin source. Pass --plugin-source <dir>.');
      process.exitCode = 1;
      return;
    }
    const result = await update({ projectDir, pluginSourceDir, force: opts.force });
    ui.printWarnings(result.warnings);
    if (result.kind === 'failure') {
      ui.error(result.error.message);
      process.exitCode = 1;
      return;
    }
    if (result.updated) {
      ui.success(`Updated plugin ${result.fromVersion ?? 'none'} -> ${result.toVersion ?? 'unknown'}`);
    } else {
      ui.info(`Plugin already up to date (${result.toVersion ?? 'unknown'}).`);
    }
  });
