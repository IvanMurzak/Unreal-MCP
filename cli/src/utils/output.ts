// `--format text|json|ndjson` (shared CLI contract with unity-mcp-cli and godot-cli; it lives in
// @baizor/gamedev-cli-core).
//
// text:   the command prints for humans (unchanged behaviour).
// json:   stdout carries exactly one pretty-printed envelope; logs go to stderr.
// ndjson: progress lines `{"type":"progress",...}` on stderr, the envelope as one line on stdout.

import { Option } from 'commander';
import { CommandOutput as ContractOutput, OUTPUT_FORMATS } from '@baizor/gamedev-cli-core';
import { ExitCode, UsageError, type ExitCodeValue } from './exit-codes.js';
import * as ui from './ui.js';

export { OUTPUT_FORMATS, formatFromArgv, isMachineFormat, renderEnvelope } from '@baizor/gamedev-cli-core';
export type { Envelope, OutputFormat } from '@baizor/gamedev-cli-core';

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

/** The `--format` option; `extra` adds command-specific formats (e.g. `github` for `test`). */
export function formatOption(extra: readonly string[] = []): Option {
  return new Option('--format <format>', 'Output format').choices([...OUTPUT_FORMATS, ...extra]).default('text');
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
 * `finish` sets the exit code and never exits the process, so pending stdout / stderr still flush.
 */
export class CommandOutput extends ContractOutput<ErrorCode> {
  constructor(command: string, format = 'text', write?: (text: string) => void, writeErr?: (text: string) => void) {
    super(command, format, { stdout: write, stderr: writeErr });
  }

  error(code: ErrorCode, message: string): void {
    this.record(code, message);
    if (!this.machine) ui.error(message);
  }

  /** Record `failure` with `message` and finish with its exit code. */
  fail(failure: { exit: ExitCodeValue; code: ErrorCode }, message: string, data: unknown = null): void {
    this.error(failure.code, message);
    this.finish(failure.exit, data);
  }

  protected override printWarning(message: string): void {
    ui.warn(message);
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
