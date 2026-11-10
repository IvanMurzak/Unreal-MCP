import { Command } from 'commander';
import { compileProject, type CompileFailureReason } from '../lib/compile.js';
import { formatDiagnostic } from '../utils/compiler-diagnostics.js';
import { ExitCode } from '../utils/exit-codes.js';
import { CommandOutput, FAILURE, formatOption, type FailureKind } from '../utils/output.js';
import * as ui from '../utils/ui.js';

const FAILURES: Record<CompileFailureReason, FailureKind> = {
  'not-found': FAILURE.projectNotFound,
  unreachable: FAILURE.unreachable,
  'project-open': FAILURE.projectOpen,
  'compilation-failed': FAILURE.compilationFailed,
};

export const compileCommand = new Command('compile')
  .description('Build the project\'s editor target with UnrealBuildTool (no editor) and print file:line:col diagnostics')
  .argument('[path]', 'Unreal project directory or .uproject (defaults to cwd)')
  .option('--strict', 'Warnings fail the build too')
  .option('--engine-root <dir>', 'Explicit engine install root (source builds)')
  .addOption(formatOption())
  .action(async (pathArg: string | undefined, opts: { strict?: boolean; engineRoot?: string; format: string }) => {
    const out = new CommandOutput('compile', opts.format);
    const result = await compileProject({
      projectDir: pathArg,
      engineRoot: opts.engineRoot,
      strict: opts.strict === true,
      onLine: (line) => ui.verbose(line),
      onDiagnostic: (diagnostic) => {
        const text = formatDiagnostic(diagnostic);
        // Text: every diagnostic as it comes (stderr for errors, like a compiler); ndjson: progress lines.
        if (!out.machine) (diagnostic.severity === 'error' ? process.stderr : process.stdout).write(`${text}\n`);
        out.progress(text);
      },
    });
    if (result.kind === 'failure') {
      const { kind: _kind, success: _success, reason, message, ...data } = result;
      out.fail(FAILURES[reason], message, data);
      return;
    }
    const { kind: _kind, success: _success, ...data } = result;
    if (!out.machine) {
      const warnings = result.warnings.length;
      ui.success(`${result.target}: build succeeded${warnings ? ` with ${warnings} warning${warnings === 1 ? '' : 's'}` : ''} (${Math.round(result.durationMs / 1000)} s).`);
    }
    out.finish(ExitCode.Success, data);
  });
