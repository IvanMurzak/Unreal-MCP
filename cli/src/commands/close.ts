import { Command } from 'commander';
import { close } from '../lib/close.js';
import * as ui from '../utils/ui.js';

export const closeCommand = new Command('close')
  .description('Terminate the Unreal Editor process running a project')
  .argument('[path]', 'Unreal project directory (defaults to cwd)')
  .action(async (pathArg: string | undefined) => {
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
