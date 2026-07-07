import chalk from 'chalk';
import yoctoSpinner, { type Spinner } from 'yocto-spinner';
import type { Command, Help } from 'commander';

let verboseEnabled = false;

export function setVerbose(enabled: boolean): void {
  verboseEnabled = enabled;
}

export function verbose(message: string): void {
  if (!verboseEnabled) return;
  if (isTTY()) {
    console.log(chalk.dim(`[verbose] ${message}`));
  } else {
    console.log(`[verbose] ${message}`);
  }
}

function isTTY(): boolean {
  return !!process.stdout.isTTY;
}

function stripAnsi(str: string): string {
  // eslint-disable-next-line no-control-regex
  return str.replace(/\x1B\[[0-9;]*m/g, '');
}

function drawBox(text: string): string {
  const lines = text.split('\n');
  const padding = 2;
  const maxLen = Math.max(...lines.map((line) => stripAnsi(line).length));
  const innerWidth = maxLen + padding * 2;
  const pad = ' '.repeat(padding);
  const top = chalk.cyan(`╭${'─'.repeat(innerWidth)}╮`);
  const bottom = chalk.cyan(`╰${'─'.repeat(innerWidth)}╯`);
  const middle = lines.map((line) => {
    const visible = stripAnsi(line).length;
    const right = ' '.repeat(maxLen - visible);
    return `${chalk.cyan('│')}${pad}${line}${right}${pad}${chalk.cyan('│')}`;
  });
  return [top, ...middle, bottom].join('\n');
}

export function configureStyledHelp(cmd: Command, appVersion?: string): Command {
  cmd.configureHelp({
    formatHelp(target: Command, helper: Help): string {
      const isRoot = !target.parent;
      const lines: string[] = [];

      if (isRoot && appVersion) {
        lines.push(
          drawBox(
            `${chalk.bold.cyan('Unreal-MCP CLI')}  ${chalk.dim(`v${appVersion}`)}\n${chalk.dim('Bridge LLMs with Unreal via Model Context Protocol')}`,
          ),
        );
      } else {
        lines.push(
          drawBox(`${chalk.bold.cyan(target.name())} ${chalk.dim('—')} ${target.description()}`),
        );
      }
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

      if (isRoot) {
        lines.push(
          chalk.dim('Run ') +
            chalk.yellow('unreal-mcp-cli <command> --help') +
            chalk.dim(' for detailed usage of each command.'),
        );
        lines.push('');
      }

      return lines.join('\n');
    },
  });
  return cmd;
}

export function info(message: string): void {
  if (isTTY()) {
    console.log(`${chalk.blue('ℹ')}  ${message}`);
  } else {
    console.log(`INFO: ${message}`);
  }
}

export function success(message: string): void {
  if (isTTY()) {
    console.log(`${chalk.green('✔')}  ${message}`);
  } else {
    console.log(`SUCCESS: ${message}`);
  }
}

export function warn(message: string): void {
  if (isTTY()) {
    console.log(`${chalk.yellow('⚠')}  ${chalk.yellow(message)}`);
  } else {
    console.log(`WARN: ${message}`);
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
  console.log(`\n${chalk.bold.cyan(message)}`);
}

export function label(key: string, value: string): void {
  console.log(`  ${chalk.bold(`${key}:`)} ${value}`);
}

function createNoOpSpinner(text: string): Spinner {
  console.log(text);
  const self: Spinner = {
    start: () => self,
    stop: () => self,
    success: (msg?: string) => {
      if (msg) console.log(`SUCCESS: ${msg}`);
      return self;
    },
    error: (msg?: string) => {
      if (msg) console.error(`ERROR: ${msg}`);
      return self;
    },
    warning: (msg?: string) => {
      if (msg) console.log(`WARN: ${msg}`);
      return self;
    },
    info: (msg?: string) => {
      if (msg) console.log(`INFO: ${msg}`);
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
