import { Command } from 'commander';
import { EXIT_CODE_DESCRIPTIONS, ExitCode } from '../utils/exit-codes.js';
import { CommandOutput, formatOption } from '../utils/output.js';

export interface ManifestArgument {
  name: string;
  required: boolean;
  variadic: boolean;
  description: string;
}

export interface ManifestOption {
  flags: string;
  description: string;
  default?: unknown;
  choices?: string[];
}

export interface ManifestCommand {
  name: string;
  description: string;
  arguments: ManifestArgument[];
  options: ManifestOption[];
}

export interface CommandManifest {
  name: string;
  version: string;
  commands: ManifestCommand[];
  exitCodes: Array<{ code: number; name: string; description: string }>;
}

/** The manifest of `program`'s commands, read from the commander tree (never hand-maintained). */
export function buildManifest(program: Command): CommandManifest {
  const commands = program.commands.map((cmd): ManifestCommand => ({
    name: cmd.name(),
    description: cmd.description(),
    arguments: cmd.registeredArguments.map((arg) => ({
      name: arg.name(),
      required: arg.required,
      variadic: arg.variadic,
      description: arg.description,
    })),
    options: cmd.options.map((opt) => ({
      flags: opt.flags,
      description: opt.description,
      ...(opt.defaultValue !== undefined ? { default: opt.defaultValue } : {}),
      ...(opt.argChoices ? { choices: [...opt.argChoices] } : {}),
    })),
  }));
  const exitCodes = Object.entries(ExitCode).map(([name, code]) => ({ code, name, description: EXIT_CODE_DESCRIPTIONS[code] }));
  return { name: program.name(), version: program.version() ?? '', commands, exitCodes };
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
