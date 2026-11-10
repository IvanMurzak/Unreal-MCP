import chalk from 'chalk';
import yoctoSpinner, { type Spinner } from 'yocto-spinner';
import type { Command, Help } from 'commander';

let verboseEnabled = false;
/** Machine output (`--format json|ndjson`): stdout carries only the result, so logs go to stderr, plain. */
let stdoutReserved = false;

/** Reserve stdout for machine output: every log line, heading and spinner goes to stderr without colour. */
export function reserveStdout(reserved: boolean): void {
  stdoutReserved = reserved;
}

/** A log line: stdout normally, stderr while stdout is reserved for machine output. */
function log(message: string): void {
  if (stdoutReserved) console.error(message);
  else console.log(message);
}

export function setVerbose(enabled: boolean): void {
  verboseEnabled = enabled;
}

export function isVerbose(): boolean {
  return verboseEnabled;
}

export function verbose(message: string): void {
  if (!verboseEnabled) return;
  if (isTTY()) {
    log(chalk.dim(`[verbose] ${message}`));
  } else {
    log(`[verbose] ${message}`);
  }
}

function isTTY(): boolean {
  return !stdoutReserved && !!process.stdout.isTTY;
}

/** The root help's featured commands: how an agent gets from nothing to a working call. */
const ROOT_EDITOR_COMMANDS: ReadonlyArray<readonly [string, string, string]> = [
  ['rql', "rql '<query>'", 'read or change levels, actors, components, assets (RQL; recipes below)'],
  ['exec', "exec '<python>'", 'Python with the full `unreal` API (the last expression is printed)'],
  ['tools', 'tools [--filter <regex>]', "the editor's AI tools; run-tool <tool> --input '<json>' calls one"],
];
const ROOT_CODE_COMMANDS: ReadonlyArray<readonly [string, string, string]> = [
  ['compile', 'compile', 'build the C++; prints file:line errors'],
  ['test', 'test --filter <Prefix>', 'run Automation tests headless'],
];
/** How many of `rql --help`'s recipes the root help shows. */
const ROOT_RQL_RECIPES = 7;

/** RQL recipes: [query, what it does]. */
export type RqlRecipes = ReadonlyArray<readonly [string, string]>;

/**
 * The root help: short on purpose (agents read it first, and every later turn re-reads it): the everyday commands, the
 * other command names, and the RQL recipes most tasks need. Each command's own --help has the details.
 */
function formatRootHelp(target: Command, helper: Help, appVersion: string | undefined, recipes: RqlRecipes): string {
  const name = target.name();
  const featured = new Set([...ROOT_EDITOR_COMMANDS, ...ROOT_CODE_COMMANDS].map(([cmd]) => cmd));
  const row = ([, usage, what]: readonly [string, string, string]) => `  ${chalk.yellow(usage.padEnd(26))} ${chalk.dim(what)}`;
  const others = helper.visibleCommands(target).map((c) => c.name()).filter((c) => !featured.has(c) && c !== 'help');
  const lines = [
    `${chalk.bold.cyan('Unreal-MCP CLI')}${appVersion ? ` v${appVersion}` : ''}: drive the Unreal Editor of the project in the current folder (or --path <dir>).`,
    '',
    chalk.bold('Editor work (the first call starts a headless editor, 1-2 min, and keeps it warm; no open / status needed):'),
    ...ROOT_EDITOR_COMMANDS.map(row),
    chalk.bold('C++ (no editor needed):'),
    ...ROOT_CODE_COMMANDS.map(row),
    `${chalk.bold('More commands:')} ${others.join(' ')}`,
  ];
  if (recipes.length > 0) {
    lines.push('', chalk.bold('RQL recipes (strings inside the query take double quotes):'));
    for (const [query, what] of recipes.slice(0, ROOT_RQL_RECIPES)) lines.push(`  ${chalk.yellow(`rql '${query}'`)}  ${chalk.dim(`# ${what}`)}`);
  }
  lines.push(
    '',
    chalk.dim(`Details: ${name} <command> --help (rql --help: operators, more recipes). Machine-readable output: --format json.`),
    '',
  );
  return lines.join('\n');
}

/** `rootRecipes`: the RQL recipes the root help shows (passed in, so this module does not import the commands). */
export function configureStyledHelp(cmd: Command, appVersion?: string, rootRecipes: RqlRecipes = []): Command {
  cmd.configureHelp({
    formatHelp(target: Command, helper: Help): string {
      if (!target.parent) return formatRootHelp(target, helper, appVersion, rootRecipes);
      const lines: string[] = [];

      // A plain line, not a box: agents read this help, and box borders cost as much as the text again.
      lines.push(`${chalk.bold.cyan(target.name())} ${chalk.dim('—')} ${target.description()}`);
      lines.push('');
      lines.push(`${chalk.bold('Usage:')} ${chalk.yellow(helper.commandUsage(target))}`);
      lines.push('');

      const subcommands = helper.visibleCommands(target);
      if (subcommands.length > 0) {
        lines.push(chalk.bold('Commands:'));
        for (const sub of subcommands) {
          lines.push(`  ${chalk.yellow(sub.name().padEnd(20))} ${chalk.dim(sub.description() || '')}`);
        }
        lines.push('');
      }

      const args = helper.visibleArguments(target);
      if (args.length > 0) {
        lines.push(chalk.bold('Arguments:'));
        for (const arg of args) {
          lines.push(
            `  ${chalk.green(helper.argumentTerm(arg).padEnd(30))} ${chalk.dim(helper.argumentDescription(arg))}`,
          );
        }
        lines.push('');
      }

      const options = helper.visibleOptions(target);
      if (options.length > 0) {
        lines.push(chalk.bold('Options:'));
        for (const opt of options) {
          lines.push(`  ${chalk.green(helper.optionTerm(opt).padEnd(30))} ${chalk.dim(helper.optionDescription(opt))}`);
        }
        lines.push('');
      }

      return lines.join('\n');
    },
  });
  return cmd;
}

export function info(message: string): void {
  if (isTTY()) {
    log(`${chalk.blue('ℹ')}  ${message}`);
  } else {
    log(`INFO: ${message}`);
  }
}

export function success(message: string): void {
  if (isTTY()) {
    log(`${chalk.green('✔')}  ${message}`);
  } else {
    log(`SUCCESS: ${message}`);
  }
}

export function warn(message: string): void {
  if (isTTY()) {
    log(`${chalk.yellow('⚠')}  ${chalk.yellow(message)}`);
  } else {
    log(`WARN: ${message}`);
  }
}

export function error(message: string): void {
  if (isTTY()) {
    console.error(`${chalk.red('✖')}  ${chalk.red(message)}`);
  } else {
    console.error(`ERROR: ${message}`);
  }
}

export function heading(message: string): void {
  log(`\n${chalk.bold.cyan(message)}`);
}

export function label(key: string, value: string): void {
  log(`  ${chalk.bold(`${key}:`)} ${value}`);
}

function createNoOpSpinner(text: string): Spinner {
  log(text);
  const self: Spinner = {
    start: () => self,
    stop: () => self,
    success: (msg?: string) => {
      if (msg) log(`SUCCESS: ${msg}`);
      return self;
    },
    error: (msg?: string) => {
      if (msg) console.error(`ERROR: ${msg}`);
      return self;
    },
    warning: (msg?: string) => {
      if (msg) log(`WARN: ${msg}`);
      return self;
    },
    info: (msg?: string) => {
      if (msg) log(`INFO: ${msg}`);
      return self;
    },
    clear: () => self,
    get text() {
      return text;
    },
    set text(value: string) {
      text = value;
    },
    get isSpinning() {
      return false;
    },
    get color() {
      return 'cyan' as const;
    },
    set color(_: string) {},
  } as unknown as Spinner;
  return self;
}

export function startSpinner(text: string): Spinner {
  if (!isTTY()) return createNoOpSpinner(text);
  const spinner = yoctoSpinner({ text, color: 'cyan' }).start();
  const origSuccess = spinner.success.bind(spinner);
  const origError = spinner.error.bind(spinner);
  const origWarning = spinner.warning.bind(spinner);
  const origInfo = spinner.info.bind(spinner);

  spinner.success = (msg?: string) => origSuccess(msg ? ` ${msg}` : undefined);
  spinner.error = (msg?: string) => origError(msg ? ` ${msg}` : undefined);
  spinner.warning = (msg?: string) => origWarning(msg ? ` ${msg}` : undefined);
  spinner.info = (msg?: string) => origInfo(msg ? ` ${msg}` : undefined);
  return spinner;
}

/** Print a list of warnings (no-op when empty). */
export function printWarnings(warnings: string[]): void {
  for (const warning of warnings) warn(warning);
}

/** Pretty-print a JSON value. */
export function json(value: unknown): void {
  process.stdout.write(`${JSON.stringify(value, null, 2)}\n`);
}
