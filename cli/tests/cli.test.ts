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
  'exec',
  'rql',
  'install-plugin',
  'remove-plugin',
  'install-extension',
  'configure',
  'setup-mcp',
  'login',
  'status',
  'dialog',
  'wait-for-ready',
  'run-tool',
  'run-system-tool',
  'tools',
  'compile',
  'test',
  'bootstrap-local',
  'update',
  'install-engine',
  'setup-skills',
  'commands',
];

describe('CLI integration', () => {
  it('--help lists every one of the 24 commands', () => {
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

  it("a subcommand's own --version <x.y.z> reaches the subcommand (not the root version flag)", () => {
    // Not a UE project → install-extension fails fast, before any network call.
    const { stdout, exitCode } = runCli(['install-extension', 'com.x.y', cliTmp(), '--version', '9.9.9']);
    expect(exitCode).toBe(1);
    expect(stdout.trim()).not.toMatch(/^\d+\.\d+\.\d+$/);
    expect(stdout).toMatch(/No \.uproject/);
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

describe('CLI contract: exit codes, --format, commands', () => {
  function runSplit(args: string[]): { stdout: string; stderr: string; exitCode: number } {
    try {
      const stdout = execFileSync('node', [CLI_PATH, ...args], { encoding: 'utf-8', timeout: 20000, input: '', stdio: ['pipe', 'pipe', 'pipe'] });
      return { stdout, stderr: '', exitCode: 0 };
    } catch (err: unknown) {
      const e = err as { stdout?: string; stderr?: string; status?: number };
      return { stdout: e.stdout ?? '', stderr: e.stderr ?? '', exitCode: e.status ?? 1 };
    }
  }

  it('commands --format json prints the manifest generated from the command tree', () => {
    const { stdout, exitCode } = runSplit(['commands', '--format', 'json']);
    expect(exitCode).toBe(0);
    const envelope = JSON.parse(stdout);
    expect(envelope).toMatchObject({ success: true, command: 'commands', errors: [], warnings: [] });
    const names = envelope.data.commands.map((c: { name: string }) => c.name);
    expect(names).toEqual(expect.arrayContaining(ALL_COMMANDS));
    const runTool = envelope.data.commands.find((c: { name: string }) => c.name === 'run-tool');
    expect(runTool.arguments).toEqual([{ name: 'tool', required: true, variadic: false, description: 'Name of the tool to invoke' }]);
    expect(runTool.options.map((o: { flags: string }) => o.flags)).toEqual(expect.arrayContaining(['--no-start', '--once', '--format <format>']));
    expect(envelope.data.exitCodes).toEqual(expect.arrayContaining([{ code: 18, name: 'AwaitingInput', description: expect.any(String) }]));
  });

  it('every --nullrhi option says screenshots fail under it and render offscreen without it', () => {
    const envelope = JSON.parse(runSplit(['commands', '--format', 'json']).stdout);
    const helps = envelope.data.commands.flatMap((c: { name: string; options: { flags: string; description: string }[] }) =>
      c.options.filter((o) => o.flags === '--nullrhi').map((o) => [c.name, o.description]));
    expect(helps.map(([name]: string[]) => name).sort()).toEqual(['exec', 'open', 'run-system-tool', 'run-tool', 'tools']);
    for (const [, description] of helps) {
      expect(description).toContain('screenshot tools fail');
      expect(description).toContain('-RenderOffscreen');
      expect(description).not.toMatch(/degrade/);
    }
  });

  it('a usage error exits 2 (unknown option, bad choice, bad number), --help / --version stay 0', () => {
    expect(runSplit(['status', '--bogus']).exitCode).toBe(2);
    expect(runSplit(['status', '--format', 'yaml']).exitCode).toBe(2);
    expect(runSplit(['run-tool']).exitCode).toBe(2);
    expect(runSplit(['test', cliTmp(), '--timeout', 'soon']).exitCode).toBe(2);
    expect(runSplit(['no-such-command']).exitCode).toBe(2);
    expect(runSplit(['run-tool', '--help']).exitCode).toBe(0);
    expect(runSplit(['--version']).exitCode).toBe(0);
  });

  it('a usage error with --format json answers with a USAGE envelope', () => {
    const { stdout, exitCode } = runSplit(['status', '--bogus', '--format', 'json']);
    expect(exitCode).toBe(2);
    expect(JSON.parse(stdout)).toMatchObject({ success: false, command: 'status', errors: [{ code: 'USAGE' }] });
  });

  it('status --format json: one envelope on stdout, nothing else', () => {
    const { stdout, exitCode } = runSplit(['status', '--no-probe', '--format', 'json']);
    expect(exitCode).toBe(0);
    expect(JSON.parse(stdout)).toMatchObject({ success: true, command: 'status', data: { name: 'unreal-mcp-cli' } });
  });

  it('compile and test of a folder without a project: NOT_FOUND', () => {
    const compile = runSplit(['compile', cliTmp(), '--format', 'json']);
    expect(compile.exitCode).toBe(2);
    expect(JSON.parse(compile.stdout).errors[0].code).toBe('NOT_FOUND');
    const test = runSplit(['test', cliTmp(), '--format', 'ndjson']);
    expect(test.exitCode).toBe(2);
    expect(JSON.parse(test.stdout.trim()).errors[0].code).toBe('NOT_FOUND');
  });

  it('run-tool --no-start against nothing: UNREACHABLE (7)', () => {
    const { stdout, exitCode } = runSplit(['run-tool', 'ping', '--url', 'http://127.0.0.1:65530', '--no-start', '--format', 'json']);
    expect(exitCode).toBe(7);
    expect(JSON.parse(stdout).errors[0].code).toBe('UNREACHABLE');
  });
});
