import * as fs from 'fs';
import * as path from 'path';
import { Command } from 'commander';
import { DEFAULT_TEST_FILTER, DEFAULT_TEST_TIMEOUT_SECONDS, runAutomationTests, type RunTestsResult } from '../lib/run-tests.js';
import { failingTests, githubError, toGitHubAnnotations, toJUnitXml, type AutomationSummary } from '../utils/automation-report.js';
import { ExitCode, UsageError } from '../utils/exit-codes.js';
import { CommandOutput, FAILURE, formatOption, parseWholeNumber, type FailureKind } from '../utils/output.js';
import * as ui from '../utils/ui.js';

type FailureReason = Extract<RunTestsResult, { kind: 'failure' }>['reason'];

const FAILURES: Record<FailureReason, FailureKind> = {
  'not-found': FAILURE.projectNotFound,
  unreachable: FAILURE.unreachable,
  'project-open': FAILURE.projectOpen,
  'build-failed': FAILURE.compilationFailed,
  timeout: FAILURE.timeout,
  'no-verdict': FAILURE.testRunError,
  'run-failed': FAILURE.testRunError,
  interrupted: FAILURE.interrupted,
};

interface TestCliOptions {
  filter: string;
  output?: string;
  junitOutput?: string;
  format: string;
  timeout: string;
  engineRoot?: string;
  build: boolean;
}

/** The machine-readable view of a summary (failing tests with their errors; passing ones by path only). */
function summaryData(summary: AutomationSummary): Record<string, unknown> {
  return {
    total: summary.total,
    passed: summary.passed,
    failed: summary.failed,
    notRun: summary.notRun,
    durationSeconds: summary.durationSeconds,
    failures: failingTests(summary).map((t) => ({ path: t.fullPath, state: t.state, errors: t.errors })),
    passedTests: summary.tests.filter((t) => t.passed).map((t) => t.fullPath),
  };
}

function printText(summary: AutomationSummary): void {
  for (const test of summary.tests) {
    const mark = test.passed ? 'PASS' : test.state === 'Fail' ? 'FAIL' : test.state.toUpperCase();
    console.log(`${mark}  ${test.fullPath} (${test.durationSeconds.toFixed(2)} s)`);
    for (const e of test.passed ? [] : test.errors) {
      console.log(`      ${e.file ? `${e.file}${e.line ? `:${e.line}` : ''}: ` : ''}${e.message}`);
    }
  }
  console.log(`\n${summary.passed} passed, ${summary.failed} failed, ${summary.notRun} not run (${summary.total} total, ${summary.durationSeconds.toFixed(1)} s)`);
}

export const testCommand = new Command('test')
  .description('Run Unreal Automation tests headless (UnrealEditor-Cmd -nullrhi) and report the verdict from the exported report')
  .argument('[path]', 'Unreal project directory or .uproject (defaults to cwd)')
  .option('--filter <filter>', 'Automation RunTests filter (test path substring, e.g. Project.MyGame; several joined with +)', DEFAULT_TEST_FILTER)
  .option('--output <dir>', 'Report directory (default <project>/Saved/Automation/UnrealMcpCli/<timestamp>)')
  .option('--junit-output <file>', 'Also write the results as JUnit XML')
  .addOption(formatOption(['github']))
  .option('--timeout <seconds>', 'Stop the editor and exit 14 after this many seconds', String(DEFAULT_TEST_TIMEOUT_SECONDS))
  .option('--engine-root <dir>', 'Explicit engine install root (source builds)')
  .option('--no-build', 'Skip building the editor target first')
  .action(async (pathArg: string | undefined, opts: TestCliOptions) => {
    const out = new CommandOutput('test', opts.format);
    const timeoutSeconds = parseWholeNumber(opts.timeout, '--timeout', 1);
    // -ExecCmds splits on ',' and the Automation command on ';': either would cut the command short.
    if (/[,;]/.test(opts.filter)) throw new UsageError('--filter must not contain "," or ";" (join several filters with "+").');
    const status = (message: string): void => {
      out.progress(message);
      if (!out.machine) process.stderr.write(`${message}\n`);
    };
    // Ctrl+C / SIGTERM: stop the editor tree (on POSIX it runs in its own process group) instead of leaving it behind.
    const abort = new AbortController();
    const onSignal = (): void => abort.abort();
    process.once('SIGINT', onSignal);
    process.once('SIGTERM', onSignal);
    let result;
    try {
      result = await runAutomationTests({
        projectDir: pathArg,
        engineRoot: opts.engineRoot,
        filter: opts.filter,
        outputDir: opts.output,
        timeoutSeconds,
        build: opts.build,
        signal: abort.signal,
        onStatus: status,
        onLog: (line) => ui.verbose(line),
      });
    } finally {
      process.off('SIGINT', onSignal);
      process.off('SIGTERM', onSignal);
    }
    for (const warning of result.warnings) out.warn(warning);

    const run = { filter: result.filter, reportDir: result.reportDir, logPath: result.logPath };
    if (result.kind === 'failure') {
      if (!out.machine && result.logTail?.length) process.stderr.write(`Last editor output:\n${result.logTail.join('\n')}\n`);
      if (opts.format === 'github') console.log(githubError('Unreal Automation', result.message));
      out.fail(FAILURES[result.reason], result.message, { ...run, logTail: result.logTail ?? [] });
      return;
    }

    const { summary } = result;
    if (opts.junitOutput) {
      const file = path.resolve(opts.junitOutput);
      fs.mkdirSync(path.dirname(file), { recursive: true });
      fs.writeFileSync(file, toJUnitXml(summary), 'utf-8');
    }
    if (opts.format === 'github') {
      for (const line of toGitHubAnnotations(summary)) console.log(line);
      console.log(`Unreal Automation: ${summary.passed} passed, ${summary.failed} failed, ${summary.notRun} not run (report: ${result.reportDir})`);
    } else if (!out.machine) {
      printText(summary);
      console.log(`Report: ${result.reportDir}`);
    }

    const data = { ...run, ...summaryData(summary), ...(opts.junitOutput ? { junit: path.resolve(opts.junitOutput) } : {}) };
    if (summary.failed > 0 || summary.notRun > 0) {
      const failing = failingTests(summary).length;
      out.record(FAILURE.testsFailed.code, `${failing} test${failing === 1 ? '' : 's'} did not pass.`);
      out.finish(FAILURE.testsFailed.exit, data);
      return;
    }
    out.finish(ExitCode.Success, data);
  });
