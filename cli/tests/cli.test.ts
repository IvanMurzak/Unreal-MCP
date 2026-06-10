import { describe, it, expect } from 'vitest';
import { execFileSync } from 'child_process';
import * as path from 'path';
import { fileURLToPath } from 'url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const CLI_PATH = path.resolve(__dirname, '..', 'bin', 'unreal-cli.js');

function runCli(args: string[]): { stdout: string; exitCode: number } {
  try {
    const stdout = execFileSync('node', [CLI_PATH, ...args], { encoding: 'utf-8', timeout: 20000 });
    return { stdout, exitCode: 0 };
  } catch (err: unknown) {
    const e = err as { stdout?: string; stderr?: string; status?: number };
    return { stdout: (e.stdout ?? '') + (e.stderr ?? ''), exitCode: e.status ?? 1 };
  }
}

const ALL_COMMANDS = [
  'create-project',
  'open',
  'close',
  'install-plugin',
  'remove-plugin',
  'configure',
  'setup-mcp',
  'login',
  'status',
  'wait-for-ready',
  'run-tool',
  'run-system-tool',
  'bootstrap-local',
  'update',
  'install-engine',
  'setup-skills',
];

describe('CLI integration', () => {
  it('--help lists every one of the 16 commands', () => {
    const { stdout, exitCode } = runCli(['--help']);
    expect(exitCode).toBe(0);
    expect(stdout).toContain('unreal-cli');
    for (const cmd of ALL_COMMANDS) {
      expect(stdout, `help should list ${cmd}`).toContain(cmd);
    }
  });

  it('--version prints semver', () => {
    const { stdout, exitCode } = runCli(['--version']);
    expect(exitCode).toBe(0);
    expect(stdout.trim()).toMatch(/^\d+\.\d+\.\d+$/);
  });

  it('each command exposes its own --help (is registered + invokable)', () => {
    for (const cmd of ALL_COMMANDS) {
      const { stdout, exitCode } = runCli([cmd, '--help']);
      expect(exitCode, `${cmd} --help should exit 0`).toBe(0);
      expect(stdout, `${cmd} --help should name the command`).toContain(cmd);
    }
  });

  it('status runs and prints the package identity', () => {
    const { stdout, exitCode } = runCli(['status', '--no-probe']);
    expect(exitCode).toBe(0);
    expect(stdout).toMatch(/unreal-cli v\d+\.\d+\.\d+/);
  });

  it('install-engine with no version lists installed engines (no crash)', () => {
    const fixture = path.resolve(__dirname, 'fixtures', 'LauncherInstalled.dat');
    const { stdout, exitCode } = runCli(['install-engine', '--manifest', fixture]);
    expect(exitCode).toBe(0);
    expect(stdout).toContain('5.7');
  });
});
