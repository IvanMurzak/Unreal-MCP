import { Command } from 'commander';
import { buildCommandManifest, type CommandManifest } from '@baizor/gamedev-cli-core';
import { ExitCode } from '../utils/exit-codes.js';
import { CommandOutput, formatOption } from '../utils/output.js';

export type { CommandManifest, ManifestArgument, ManifestCommand, ManifestOption } from '@baizor/gamedev-cli-core';

/** The manifest of `program`'s commands, read from the commander tree (never hand-maintained). */
export function buildManifest(program: Command): CommandManifest {
  return buildCommandManifest(program, program.version() ?? '');
}

export const commandsCommand = new Command('commands')
  .description('Print a machine-readable manifest of every command (arguments, options) and the exit codes')
  .addOption(formatOption())
  .action(function (this: Command, opts: { format: string }) {
    const manifest = buildManifest(this.parent!);
    const out = new CommandOutput('commands', opts.format);
    if (!out.machine) {
      for (const cmd of manifest.commands) {
        const args = cmd.arguments.map((a) => (a.required ? `<${a.name}${a.variadic ? '...' : ''}>` : `[${a.name}${a.variadic ? '...' : ''}]`));
        console.log(`${[cmd.name, ...args].join(' ')}  —  ${cmd.description}`);
      }
      console.log('\nExit codes:');
      for (const e of manifest.exitCodes) console.log(`  ${String(e.code).padStart(2)}  ${e.name}: ${e.description}`);
    }
    out.finish(ExitCode.Success, manifest);
  });
