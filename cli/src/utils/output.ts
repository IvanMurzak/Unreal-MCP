// `--format text|json|ndjson` (shared CLI contract with unity-mcp-cli and godot-cli).
//
// text:   the command prints for humans (unchanged behaviour).
// json:   stdout carries exactly one pretty-printed envelope; logs go to stderr.
// ndjson: progress lines `{"type":"progress",...}` on stderr, the envelope as one line on stdout.

import { Option } from 'commander';
import { ExitCode, UsageError, type ExitCodeValue } from './exit-codes.js';
import * as ui from './ui.js';

export const OUTPUT_FORMATS = ['text', 'json', 'ndjson'] as const;
export type OutputFormat = (typeof OUTPUT_FORMATS)[number];

/** Error codes of the envelope (UPPER_SNAKE). */
export type ErrorCode =
  | 'USAGE'
  | 'TOOL_FAILED'
  | 'UNREACHABLE'
  | 'TIMEOUT'
  | 'BUSY'
  | 'AWAITING_INPUT'
  | 'TESTS_FAILED'
  | 'TEST_RUN_ERROR'
  | 'COMPILATION_FAILED'
  | 'PROJECT_OPEN'
  | 'NOT_FOUND'
  | 'INTERRUPTED'
  | 'NOT_AUTHENTICATED'
  | 'ERROR';

export interface EnvelopeError {
  code: ErrorCode;
  message: string;
}

export interface Envelope {
  success: boolean;
  command: string;
  data: unknown;
  errors: EnvelopeError[];
  warnings: string[];
}

/** The `--format` option; `extra` adds command-specific formats (e.g. `github` for `test`). */
export function formatOption(extra: readonly string[] = []): Option {
  return new Option('--format <format>', 'Output format').choices([...OUTPUT_FORMATS, ...extra]).default('text');
}

/** Whether `format` reserves stdout for a machine-readable envelope. */
export function isMachineFormat(format: string | undefined): format is 'json' | 'ndjson' {
  return format === 'json' || format === 'ndjson';
}

/**
 * The `--format` named on a raw argv (`--format json` / `--format=json`): lets the entry point answer a commander
 * parse error with an envelope, before any command parsed its options.
 */
export function formatFromArgv(argv: readonly string[]): string | undefined {
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i]!;
    if (arg === '--format') return argv[i + 1];
    if (arg.startsWith('--format=')) return arg.slice('--format='.length);
  }
  return undefined;
}

/** Serialize an envelope for `format` (json: pretty; ndjson: one line). */
export function renderEnvelope(envelope: Envelope, format: 'json' | 'ndjson'): string {
  return `${format === 'json' ? JSON.stringify(envelope, null, 2) : JSON.stringify(envelope)}\n`;
}

/** The exit code and error code of a failure kind shared by several commands. */
export const FAILURE = {
  usage: { exit: ExitCode.Usage, code: 'USAGE' },
  toolFailed: { exit: ExitCode.CommandFailed, code: 'TOOL_FAILED' },
  notFound: { exit: ExitCode.CommandFailed, code: 'NOT_FOUND' },
  /** No project at the path the command was given. */
  projectNotFound: { exit: ExitCode.Usage, code: 'NOT_FOUND' },
  projectOpen: { exit: ExitCode.CommandFailed, code: 'PROJECT_OPEN' },
  compilationFailed: { exit: ExitCode.CommandFailed, code: 'COMPILATION_FAILED' },
  testRunError: { exit: ExitCode.CommandFailed, code: 'TEST_RUN_ERROR' },
  testsFailed: { exit: ExitCode.TestsFailed, code: 'TESTS_FAILED' },
  /** Cloud mode with a sign-in that cannot be used (never switched to a local server). */
  notAuthenticated: { exit: ExitCode.CommandFailed, code: 'NOT_AUTHENTICATED' },
  /** Ctrl+C / SIGTERM: not a verdict. */
  interrupted: { exit: ExitCode.Error, code: 'INTERRUPTED' },
  unreachable: { exit: ExitCode.Unreachable, code: 'UNREACHABLE' },
  timeout: { exit: ExitCode.Timeout, code: 'TIMEOUT' },
  busy: { exit: ExitCode.Busy, code: 'BUSY' },
  awaitingInput: { exit: ExitCode.AwaitingInput, code: 'AWAITING_INPUT' },
  error: { exit: ExitCode.Error, code: 'ERROR' },
} as const satisfies Record<string, { exit: ExitCodeValue; code: ErrorCode }>;

export type FailureKind = (typeof FAILURE)[keyof typeof FAILURE];

/**
 * Collects a command's errors and warnings and prints its result in the chosen format. In text mode errors and
 * warnings print as they come and the command prints its own result; in json/ndjson only the envelope reaches stdout.
 */
export class CommandOutput {
  readonly errors: EnvelopeError[] = [];
  readonly warnings: string[] = [];

  constructor(
    readonly command: string,
    readonly format: string = 'text',
    private readonly write: (text: string) => void = (text) => process.stdout.write(text),
    private readonly writeErr: (text: string) => void = (text) => process.stderr.write(text),
  ) {}

  /** json / ndjson: stdout is reserved for the envelope. */
  get machine(): boolean {
    return isMachineFormat(this.format);
  }

  /** A progress message: an ndjson progress line on stderr; text and json leave progress to the command (stderr / spinner). */
  progress(message: string): void {
    if (this.format === 'ndjson') this.writeErr(`${JSON.stringify({ type: 'progress', command: this.command, message })}\n`);
  }

  warn(message: string): void {
    this.warnings.push(message);
    if (!this.machine) ui.warn(message);
  }

  error(code: ErrorCode, message: string): void {
    this.record(code, message);
    if (!this.machine) ui.error(message);
  }

  /** Record an error for the envelope without printing it (the command already showed it its own way). */
  record(code: ErrorCode, message: string): void {
    this.errors.push({ code, message });
  }

  /**
   * Finish the command: set the exit code and, in json/ndjson, print the envelope (`success` = exit code 0). The
   * process is never exited here, so pending stdout / stderr still flush.
   */
  finish(exitCode: ExitCodeValue, data: unknown = null): void {
    process.exitCode = exitCode;
    if (!isMachineFormat(this.format)) return;
    this.write(renderEnvelope(
      { success: exitCode === ExitCode.Success, command: this.command, data: data ?? null, errors: this.errors, warnings: this.warnings },
      this.format,
    ));
  }

  /** Record `failure` with `message` and finish with its exit code. */
  fail(failure: { exit: ExitCodeValue; code: ErrorCode }, message: string, data: unknown = null): void {
    this.error(failure.code, message);
    this.finish(failure.exit, data);
  }
}

/** Throw a {@link UsageError} (exit 2) unless `value` is a whole number of at least `min`. */
export function parseWholeNumber(value: string, flagName: string, min: number): number {
  const parsed = /^\d+$/.test(value.trim()) ? Number.parseInt(value, 10) : Number.NaN;
  if (!Number.isSafeInteger(parsed) || parsed < min) {
    throw new UsageError(`${flagName} must be a whole number${min > 0 ? ` of at least ${min}` : ''}`);
  }
  return parsed;
}
