import { afterEach, describe, expect, it } from 'vitest';
import { EventEmitter } from 'events';
import { PassThrough } from 'stream';
import * as fs from 'fs';
import * as path from 'path';
import type { ChildProcess } from 'child_process';
import { automationArgs, automationEnv, runAutomationTests, type RunTestsOptions } from '../src/lib/run-tests.js';
import { makeTempDir, rmTempDir, writeFakeEngine, writeUProject } from './helpers.js';

const dirs: string[] = [];
afterEach(() => {
  while (dirs.length) rmTempDir(dirs.pop()!);
});

const REPORT = {
  tests: [
    { fullTestPath: 'Project.A', state: 'Success', duration: 1, entries: [] },
    { fullTestPath: 'Project.B', state: 'Fail', duration: 1, entries: [{ event: { type: 'Error', message: 'boom' }, filename: 'B.cpp', lineNumber: 3 }] },
  ],
};

type Behaviour = (child: FakeChild, args: string[]) => void;

class FakeChild extends EventEmitter {
  stdout = new PassThrough();
  stderr = new PassThrough();
  pid = 4242;
  exit(code = 0): void {
    this.stdout.end();
    this.stderr.end();
    this.emit('exit', code);
  }
}

function setup(behaviour: Behaviour, extra: Partial<RunTestsOptions> = {}) {
  const projectDir = makeTempDir();
  const engineRoot = writeFakeEngine(makeTempDir('unreal-mcp-cli-engine-'));
  dirs.push(projectDir, engineRoot);
  writeUProject(projectDir, 'Game');
  const spawned: string[][] = [];
  const killed: number[] = [];
  const options: RunTestsOptions = {
    projectDir,
    engineRoot,
    outputDir: path.join(projectDir, 'Report'),
    listProcessesImpl: () => [],
    editorBinary: 'UnrealEditor-Cmd',
    spawnImpl: (_cmd, args) => {
      spawned.push(args);
      const child = new FakeChild();
      setImmediate(() => behaviour(child, args));
      return child as unknown as ChildProcess;
    },
    killTreeImpl: async (pid) => { killed.push(pid); },
    sleep: () => new Promise((r) => setTimeout(r, 5)),
    pollMs: 5,
    quitGraceMs: 50,
    ...extra,
  };
  return { options, spawned, killed, reportDir: path.join(projectDir, 'Report') };
}

function writeReport(args: string[], report: unknown = REPORT): void {
  const dir = args.find((a) => a.startsWith('-ReportExportPath='))!.slice('-ReportExportPath='.length);
  fs.writeFileSync(path.join(dir, 'index.json'), `\uFEFF${JSON.stringify(report)}`);
}

describe('runAutomationTests', () => {
  it('reads the verdict from the report, not from the exit code', async () => {
    const { options, spawned, killed } = setup((child, args) => { writeReport(args); child.exit(1); });
    const r = await runAutomationTests({ ...options, filter: 'Project.Cli' });
    expect(r.kind).toBe('completed');
    if (r.kind !== 'completed') return;
    expect(r.summary).toMatchObject({ total: 2, passed: 1, failed: 1 });
    expect(spawned[0]).toContain('-ExecCmds=Automation RunTests Project.Cli;Quit');
    expect(spawned[0]).toContain('-nullrhi');
    expect(killed).toEqual([]);
  });

  it('stops an editor that wrote its report but does not quit', async () => {
    const { options, killed } = setup((_child, args) => writeReport(args));
    const r = await runAutomationTests(options);
    expect(r.kind).toBe('completed');
    expect(killed).toEqual([4242]);
  });

  it('no report after the editor exited: no verdict, with the last output', async () => {
    const { options } = setup((child) => { child.stdout.write('LogWindows: Error: crash\n'); child.exit(3); });
    const r = await runAutomationTests(options);
    expect(r).toMatchObject({ kind: 'failure', reason: 'no-verdict' });
    expect(r.kind === 'failure' && r.logTail).toContain('LogWindows: Error: crash');
  });

  it('a filter that matches nothing is a warning, not a failure', async () => {
    const { options } = setup((child) => { child.stdout.write('LogAutomationCommandLine: Display: Found 0 automation tests based on \'Nope\'\n'); setImmediate(() => child.exit(0)); });
    const r = await runAutomationTests({ ...options, filter: 'Nope' });
    expect(r).toMatchObject({ kind: 'completed', summary: { total: 0 } });
    expect(r.warnings).toEqual(['No Automation tests matched the filter "Nope".']);
  });

  it('a report still listing a test in process is not the verdict', async () => {
    const inProcess = { tests: [{ fullTestPath: 'Project.A', state: 'InProcess', entries: [] }] };
    const { options } = setup((child, args) => {
      writeReport(args, inProcess);
      setTimeout(() => { writeReport(args); child.exit(0); }, 30);
    });
    const r = await runAutomationTests(options);
    expect(r).toMatchObject({ kind: 'completed', summary: { total: 2, failed: 1 } });
  });

  it('no failed test but a non-zero run exit code: the run failed', async () => {
    const passing = { tests: [{ fullTestPath: 'Project.A', state: 'Success', entries: [] }] };
    const { options } = setup((child, args) => {
      child.stdout.write('LogAutomationCommandLine: Display: **** TEST COMPLETE. EXIT CODE: -1 ****\n');
      writeReport(args, passing);
      setTimeout(() => child.exit(0), 10);
    });
    expect(await runAutomationTests(options)).toMatchObject({ kind: 'failure', reason: 'run-failed' });
  });

  it('no tests matched while the editor lingers: stopped, a warning', async () => {
    const { options, killed } = setup((child) => child.stdout.write("LogAutomationCommandLine: Error: No automation tests matched 'X'\n"));
    const r = await runAutomationTests({ ...options, filter: 'X' });
    expect(r).toMatchObject({ kind: 'completed', summary: { total: 0 } });
    expect(killed).toEqual([4242]);
  });

  it('an interrupt stops the editor tree and is no verdict', async () => {
    const abort = new AbortController();
    const { options, killed } = setup(() => setTimeout(() => abort.abort(), 10));
    const r = await runAutomationTests({ ...options, signal: abort.signal });
    expect(r).toMatchObject({ kind: 'failure', reason: 'interrupted' });
    expect(killed).toEqual([4242]);
  });

  it('times out: the editor tree is stopped', async () => {
    const { options, killed } = setup(() => {});
    const r = await runAutomationTests({ ...options, timeoutSeconds: 0.05 } as RunTestsOptions);
    expect(r).toMatchObject({ kind: 'failure', reason: 'timeout' });
    expect(killed).toEqual([4242]);
  });

  it('refuses while an editor has the project open', async () => {
    const { options, spawned } = setup(() => {});
    const uproject = path.join(options.projectDir!, 'Game.uproject');
    const r = await runAutomationTests({ ...options, listProcessesImpl: () => [{ pid: 7, commandLine: `UnrealEditor.exe "${uproject}"` }] });
    expect(r).toMatchObject({ kind: 'failure', reason: 'project-open' });
    expect(spawned).toEqual([]);
  });

  it('a stale report in the output directory is never a verdict', async () => {
    const { options, reportDir } = setup((child) => child.exit(1));
    fs.mkdirSync(reportDir, { recursive: true });
    fs.writeFileSync(path.join(reportDir, 'index.json'), JSON.stringify(REPORT));
    expect(await runAutomationTests(options)).toMatchObject({ kind: 'failure', reason: 'no-verdict' });
  });
});

describe('automation command line', () => {
  it('runs headless with the report and log where asked', () => {
    expect(automationArgs('C:/P/Game.uproject', 'Project', 'C:/R', 'C:/R/editor.log')).toEqual([
      'C:/P/Game.uproject', '-nullrhi', '-unattended', '-nosplash', '-NoSound', '-stdout', '-ddc=NoZenLocal',
      '-ExecCmds=Automation RunTests Project;Quit', '-ReportExportPath=C:/R', '-abslog=C:/R/editor.log',
    ]);
  });

  it('drops inherited UNREAL_MCP_* variables', () => {
    expect(automationEnv({ PATH: 'x', UNREAL_MCP_TOKEN: 'secret', UNREAL_MCP_HOST: 'h' }))
      .toEqual({ PATH: 'x', UNREAL_MCP_CONNECTION_MODE: 'Custom', UNREAL_MCP_KEEP_CONNECTED: 'false' });
  });
});
