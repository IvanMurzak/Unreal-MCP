import { describe, it, expect } from 'vitest';
import { execFileSync } from 'child_process';
import * as path from 'path';
import { fileURLToPath } from 'url';
import { getStatus } from '../src/lib.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const CLI_PATH = path.resolve(__dirname, '..', 'bin', 'unreal-cli.js');

function runCli(args: string[]): { stdout: string; exitCode: number } {
  try {
    const stdout = execFileSync('node', [CLI_PATH, ...args], {
      encoding: 'utf-8',
      timeout: 15000,
    });
    return { stdout, exitCode: 0 };
  } catch (err: unknown) {
    const error = err as { stdout?: string; stderr?: string; status?: number };
    return {
      stdout: (error.stdout ?? '') + (error.stderr ?? ''),
      exitCode: error.status ?? 1,
    };
  }
}

describe('CLI integration', () => {
  it('shows help with --help, listing the status command', () => {
    const { stdout, exitCode } = runCli(['--help']);
    expect(exitCode).toBe(0);
    expect(stdout).toContain('unreal-cli');
    expect(stdout).toContain('status');
  });

  it('shows the semver version with --version', () => {
    const { stdout, exitCode } = runCli(['--version']);
    expect(exitCode).toBe(0);
    expect(stdout.trim()).toMatch(/^\d+\.\d+\.\d+$/);
  });

  it('status prints name and version', () => {
    const { stdout, exitCode } = runCli(['status']);
    expect(exitCode).toBe(0);
    expect(stdout).toContain('unreal-cli v');
    expect(stdout).toContain('pre-alpha scaffold');
  });
});

describe('library export (dist/lib.js contract)', () => {
  it('getStatus returns a success discriminated union with the package version', () => {
    const status = getStatus();
    expect(status.kind).toBe('success');
    expect(status.success).toBe(true);
    expect(status.name).toBe('unreal-cli');
    expect(status.version).toMatch(/^\d+\.\d+\.\d+$/);
  });
});
