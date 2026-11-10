import * as fs from 'fs';
import { Command } from 'commander';
import { DEFAULT_SESSION_IDLE_SECONDS } from '@baizor/gamedev-cli-core';
import { DEFAULT_EXEC_TIMEOUT_MS, execPython, readPythonResponse, type ExecPythonFailure } from '../lib/exec.js';
import { UNREAL_SESSION_READY_TIMEOUT_MS } from '../lib/session.js';
import { classifySessionFailure, classifyToolFailure, classifyToolResponseError, type FailureKind } from '../lib/tool-outcome.js';
import { ExitCode } from '../utils/exit-codes.js';
import { CommandOutput, FAILURE, formatOption, parseWholeNumber } from '../utils/output.js';
import * as ui from '../utils/ui.js';

interface ExecCliOptions {
  path?: string;
  file?: string;
  include: string[];
  window?: boolean;
  idle: string;
  readyTimeout: string;
  timeout: string;
  raw?: boolean;
  format: string;
  nullrhi?: boolean;
  url?: string;
  token?: string;
  engineRoot?: string;
  build?: boolean;
}

function collect(value: string, previous: string[]): string[] {
  return [...previous, value];
}

/** The code from the argument, `--file <file>` or stdin (`--file -`, or piped input with neither). */
export function readCode(
  code: string | undefined,
  file: string | undefined,
  stdin: { isTTY?: boolean; read: () => string } = { isTTY: process.stdin.isTTY, read: () => fs.readFileSync(0, 'utf-8') },
): string {
  if (code !== undefined && file !== undefined) throw new Error('Pass the code either as an argument or with --file, not both.');
  if (code !== undefined) return code;
  const fromStdin = file === '-' || (file === undefined && stdin.isTTY !== true);
  if (!fromStdin && file === undefined) throw new Error('No code given: pass it as an argument, with --file <file>, or on stdin.');
  const text = fromStdin ? stdin.read() : fs.readFileSync(file!, 'utf-8');
  if (text.trim() === '') throw new Error(fromStdin ? 'No code received from stdin.' : `The file is empty: ${file}`);
  return text;
}

/** Exit / error code of a failed `exec`. */
export function classifyExecFailure(failure: ExecPythonFailure): FailureKind {
  if (failure.reason === 'session') return classifySessionFailure(failure.sessionReason);
  if (failure.reason === 'tool-failed' && failure.toolFailure) return classifyToolFailure(failure.toolFailure);
  // invalid-input: no project at the path, or a bad --url.
  return classifySessionFailure(undefined, true);
}

export const execCommand = new Command('exec')
  .description('Run Python in the Unreal Editor (python-execute); starts a headless editor when none is open and keeps it warm')
  .argument('[code]', 'Python: statements, the last expression is the result')
  .option('-p, --path <dir>', 'Unreal project directory (defaults to cwd)')
  .option('-f, --file <file>', 'Read the code from a file ("-" for stdin)')
  .option('--include <path>', 'Helper .py file or folder, relative to <Project>/.ai-game-dev/snippets (repeatable)', collect, [])
  .option('--window', 'Start the editor with a window instead of headless (only when none is running)')
  .option('--nullrhi', 'A started headless editor runs with -nullrhi (no GPU; screenshots and rendering degrade)')
  .option('--idle <seconds>', 'An editor started by exec saves and quits after this many idle seconds (0 = never)', String(DEFAULT_SESSION_IDLE_SECONDS))
  .option('--ready-timeout <ms>', 'How long to wait for a started editor to become ready', String(UNREAL_SESSION_READY_TIMEOUT_MS))
  .option('--timeout <ms>', 'Tool call timeout in milliseconds', String(DEFAULT_EXEC_TIMEOUT_MS))
  .option('--raw', 'Print the raw tool response JSON')
  .addOption(formatOption())
  .option('--url <url>', 'Local MCP server URL override')
  .option('--token <token>', 'Bearer token override')
  .option('--engine-root <dir>', 'Explicit engine install root (source builds)')
  .option('--no-build', 'Skip the pre-launch build of native modules')
  .action(async (codeArg: string | undefined, options: ExecCliOptions) => {
    const out = new CommandOutput('exec', options.format);
    const idleSeconds = parseWholeNumber(options.idle, '--idle', 0);
    const readyTimeoutMs = parseWholeNumber(options.readyTimeout, '--ready-timeout', 1);
    const timeoutMs = parseWholeNumber(options.timeout, '--timeout', 1);
    let code: string;
    try {
      code = readCode(codeArg, options.file);
    } catch (err) {
      out.fail(FAILURE.usage, err instanceof Error ? err.message : String(err));
      return;
    }

    // Progress only on a terminal: piped stdout carries the result alone.
    const spinner = options.raw || out.machine || !process.stdout.isTTY ? null : ui.startSpinner('Connecting...');
    const result = await execPython({
      projectDir: options.path,
      code,
      include: options.include,
      window: options.window === true,
      nullrhi: options.nullrhi === true,
      idleSeconds,
      readyTimeoutMs,
      timeoutMs,
      url: options.url,
      token: options.token,
      engineRoot: options.engineRoot,
      build: options.build !== false,
      onStatus: (message) => {
        ui.verbose(message);
        out.progress(message);
        if (spinner) spinner.text = message;
      },
    });

    spinner?.stop();
    if (result.kind === 'failure') {
      if (options.raw && !out.machine && result.toolFailure?.data !== undefined) ui.json(result.toolFailure.data);
      // finish() sets exitCode, never exit(): an async stdout / stderr pipe still flushes what was written.
      out.fail(
        classifyExecFailure(result),
        result.reason === 'session' ? `No ready Unreal Editor: ${result.message}` : result.message,
        result.toolFailure?.data === undefined ? null : { response: result.toolFailure.data },
      );
      return;
    }

    const response = readPythonResponse(result.data);
    if (out.machine) {
      const data = { value: response.value, logs: response.logs, editor: { source: result.source, url: result.baseUrl }, response: result.data };
      if (response.ok) out.finish(ExitCode.Success, data);
      else out.fail(classifyToolResponseError(response.value), response.value, data);
      return;
    }
    if (options.raw) {
      ui.json(result.data);
      if (!response.ok) process.exitCode = classifyToolResponseError(response.value).exit;
      return;
    }
    for (const log of response.logs) process.stderr.write(`[${log.level}] ${log.message}\n`);
    if (!response.ok) {
      out.fail(classifyToolResponseError(response.value), response.value);
      return;
    }
    console.log(response.value);
  });
