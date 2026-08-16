import { describe, it, expect, afterEach } from 'vitest';
import { execFileSync } from 'child_process';
import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import { fileURLToPath } from 'url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const CLI_PATH = path.resolve(__dirname, '..', 'bin', 'unreal-mcp-cli.js');

function runCli(args: string[], input?: string): { stdout: string; exitCode: number } {
  try {
    const stdout = execFileSync('node', [CLI_PATH, ...args], {
      encoding: 'utf-8',
      timeout: 20000,
      input: input ?? '',
    });
    return { stdout, exitCode: 0 };
  } catch (err: unknown) {
    const e = err as { stdout?: string; stderr?: string; status?: number };
    return { stdout: (e.stdout ?? '') + (e.stderr ?? ''), exitCode: e.status ?? 1 };
  }
}

const cliDirs: string[] = [];
afterEach(() => {
  while (cliDirs.length) {
    try {
      fs.rmSync(cliDirs.pop()!, { recursive: true, force: true });
    } catch {
      /* ignore */
    }
  }
});
function cliTmp(): string {
  const d = fs.mkdtempSync(path.join(os.tmpdir(), 'unreal-mcp-cli-int-'));
  cliDirs.push(d);
  return d;
}

const ALL_COMMANDS = [
  'create-project',
  'open',
  'close',
  'install-plugin',
  'remove-plugin',
  'install-extension',
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
  it('--help lists every one of the 17 commands', () => {
    const { stdout, exitCode } = runCli(['--help']);
    expect(exitCode).toBe(0);
    expect(stdout).toContain('unreal-mcp-cli');
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
  }, 15000);

  it('status runs and prints the package identity', () => {
    const { stdout, exitCode } = runCli(['status', '--no-probe']);
    expect(exitCode).toBe(0);
    expect(stdout).toMatch(/unreal-mcp-cli v\d+\.\d+\.\d+/);
  });

  it('install-engine with no version lists installed engines (no crash)', () => {
    const fixture = path.resolve(__dirname, 'fixtures', 'LauncherInstalled.dat');
    const { stdout, exitCode } = runCli(['install-engine', '--manifest', fixture]);
    expect(exitCode).toBe(0);
    expect(stdout).toContain('5.7');
  });

  it('install-plugin --help lists the mcp-authorize flags', () => {
    const { stdout, exitCode } = runCli(['install-plugin', '--help']);
    expect(exitCode).toBe(0);
    for (const flag of ['--with-server', '--server-version', '--server-source', '--enroll', '--enroll-stdin']) {
      expect(stdout, `install-plugin --help should list ${flag}`).toContain(flag);
    }
  });

  it('login --help lists the e2 flags (--tools-only, --yes, --path)', () => {
    const { stdout, exitCode } = runCli(['login', '--help']);
    expect(exitCode).toBe(0);
    for (const flag of ['--tools-only', '--yes', '--path']) {
      expect(stdout, `login --help should list ${flag}`).toContain(flag);
    }
  });

  it('install-plugin --help lists --yes (enroll account-switch gate)', () => {
    const { stdout, exitCode } = runCli(['install-plugin', '--help']);
    expect(exitCode).toBe(0);
    expect(stdout).toContain('--yes');
  });

  it('configure --help lists the --agent proxy flag', () => {
    const { stdout, exitCode } = runCli(['configure', '--help']);
    expect(exitCode).toBe(0);
    expect(stdout).toContain('--agent');
  });

  it('install-plugin rejects --enroll and --enroll-stdin together (before reading stdin)', () => {
    const { stdout, exitCode } = runCli(['install-plugin', cliTmp(), '--enroll', 'x', '--enroll-stdin']);
    expect(exitCode).toBe(1);
    expect(stdout).toMatch(/either --enroll <code> or --enroll-stdin/i);
  });

  it('install-plugin --enroll-stdin reads the code from stdin (never argv) and attempts redemption', () => {
    // A minimal local plugin source so the plugin install succeeds offline; enroll
    // then redeems against an unreachable base-url → connection refused. The code
    // is delivered ONLY on stdin, proving --enroll-stdin keeps it out of argv.
    const project = cliTmp();
    const source = cliTmp();
    fs.writeFileSync(path.join(source, 'UnrealMCP.uplugin'), JSON.stringify({ VersionName: '0.1.0' }));
    const { stdout, exitCode } = runCli(
      ['install-plugin', project, '--plugin-source', source, '--enroll-stdin', '--base-url', 'http://127.0.0.1:9'],
      'STDIN-ENROLL-CODE',
    );
    // Plugin materialized; enrollment attempted and failed on the refused connection.
    expect(fs.existsSync(path.join(project, 'Plugins', 'UnrealMCP', 'UnrealMCP.uplugin'))).toBe(true);
    expect(exitCode).toBe(1);
    expect(stdout).toMatch(/enrollment failed/i);
  }, 20000);
});
