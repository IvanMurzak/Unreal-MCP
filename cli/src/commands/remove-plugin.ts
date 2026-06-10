import { Command } from 'commander';
import { removePlugin } from '../lib/install-plugin.js';
import * as ui from '../utils/ui.js';

export const removePluginCommand = new Command('remove-plugin')
  .description('Remove the UnrealMCP plugin from <project>/Plugins')
  .argument('[path]', 'Unreal project directory (defaults to cwd)')
  .action(async (pathArg: string | undefined) => {
    const result = await removePlugin({ projectDir: pathArg ?? process.cwd() });
    ui.printWarnings(result.warnings);
    if (result.kind === 'failure') {
      ui.error(result.error.message);
      process.exitCode = 1;
      return;
    }
    if (result.removed) ui.success(`Removed plugin at ${result.installedPath}`);
    else ui.info('Plugin was not installed — nothing to remove.');
  });
