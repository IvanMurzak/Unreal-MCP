import { afterEach, describe, expect, it } from 'vitest';
import { CommandOutput, FAILURE, formatFromArgv, isMachineFormat, parseWholeNumber } from '../src/utils/output.js';
import { ExitCode, EXIT_CODE_DESCRIPTIONS, UsageError } from '../src/utils/exit-codes.js';
import { reserveStdout } from '../src/utils/ui.js';

const originalExitCode = process.exitCode;
afterEach(() => {
  process.exitCode = originalExitCode;
  reserveStdout(false);
});

function capture(format: string) {
  const stdout: string[] = [];
  const stderr: string[] = [];
  const out = new CommandOutput('run-tool', format, (t) => stdout.push(t), (t) => stderr.push(t));
  return { out, stdout, stderr };
}

describe('exit codes', () => {
  it('match the shared contract', () => {
    expect(ExitCode).toEqual({ Success: 0, Error: 1, Usage: 2, CommandFailed: 6, Unreachable: 7, TestsFailed: 8, Timeout: 14, Busy: 17, AwaitingInput: 18 });
    for (const code of Object.values(ExitCode)) expect(EXIT_CODE_DESCRIPTIONS[code]).toBeTruthy();
  });
});

describe('CommandOutput', () => {
  it('json: one pretty envelope on stdout, success follows the exit code', () => {
    const { out, stdout, stderr } = capture('json');
    out.progress('Starting...');
    out.warn('careful');
    out.finish(ExitCode.Success, { value: 1 });
    expect(stderr).toEqual([]);
    expect(stdout).toHaveLength(1);
    expect(stdout[0]).toContain('\n  "success": true');
    expect(JSON.parse(stdout[0]!)).toEqual({ success: true, command: 'run-tool', data: { value: 1 }, errors: [], warnings: ['careful'] });
    expect(process.exitCode).toBe(0);
  });

  it('ndjson: progress lines on stderr, the envelope as one line on stdout', () => {
    const { out, stdout, stderr } = capture('ndjson');
    out.progress('Starting...');
    out.fail(FAILURE.busy, 'Server busy');
    expect(stderr.map((l) => JSON.parse(l))).toEqual([{ type: 'progress', command: 'run-tool', message: 'Starting...' }]);
    expect(stdout).toHaveLength(1);
    expect(stdout[0]!.trimEnd()).not.toContain('\n');
    expect(JSON.parse(stdout[0]!)).toEqual({ success: false, command: 'run-tool', data: null, errors: [{ code: 'BUSY', message: 'Server busy' }], warnings: [] });
    expect(process.exitCode).toBe(17);
  });

  it('text: no envelope', () => {
    const { out, stdout } = capture('text');
    out.finish(ExitCode.CommandFailed, { x: 1 });
    expect(stdout).toEqual([]);
    expect(process.exitCode).toBe(6);
    expect(out.machine).toBe(false);
  });
});

describe('format helpers', () => {
  it('reads --format from a raw argv', () => {
    expect(formatFromArgv(['run-tool', 'x', '--format', 'json'])).toBe('json');
    expect(formatFromArgv(['status', '--format=ndjson'])).toBe('ndjson');
    expect(formatFromArgv(['status'])).toBeUndefined();
    expect(isMachineFormat('json')).toBe(true);
    expect(isMachineFormat('github')).toBe(false);
  });

  it('parseWholeNumber throws a UsageError on a bad value', () => {
    expect(parseWholeNumber('15', '--timeout', 1)).toBe(15);
    expect(() => parseWholeNumber('0', '--timeout', 1)).toThrow(UsageError);
    expect(() => parseWholeNumber('1.5', '--timeout', 1)).toThrow(/whole number of at least 1/);
    expect(parseWholeNumber('0', '--idle', 0)).toBe(0);
  });
});
