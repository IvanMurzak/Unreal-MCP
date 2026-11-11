import { Command } from 'commander';
import { buildCommandManifest, toolFailureCodeLine } from '@baizor/gamedev-cli-core';
import { ExitCode } from '../utils/exit-codes.js';
import { CommandOutput, formatOption } from '../utils/output.js';

export const commandsCommand = new Command('commands')
  .description('Print a machine-readable manifest of every command (arguments, options) and the exit codes')
  .addOption(formatOption())
  .action(function (this: Command, opts: { format: string }) {
    // Read from the commander tree, never hand-maintained.
    const manifest = buildCommandManifest(this.parent!, this.parent!.version() ?? '');
    const out = new CommandOutput('commands', opts.format);
    if (!out.machine) {
      for (const cmd of manifest.commands) {
        const args = cmd.arguments.map((a) => (a.required ? `<${a.name}${a.variadic ? '...' : ''}>` : `[${a.name}${a.variadic ? '...' : ''}]`));
        console.log(`${[cmd.name, ...args].join(' ')}  —  ${cmd.description}`);
      }
      console.log('\nExit codes:');
      for (const e of manifest.exitCodes) console.log(`  ${String(e.code).padStart(2)}  ${e.name}: ${e.description}`);
      console.log('\nError codes of a failed tool call (errors[].code):');
      for (const e of manifest.errorCodes) console.log(`  ${toolFailureCodeLine(e)}`);
    }
    out.finish(ExitCode.Success, manifest);
  });
