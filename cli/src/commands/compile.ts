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

/** A source file of a plugin (`<...>/Plugins/<Name>/Source/...`), not of the project's own modules. */
function isPluginFile(file: string | undefined): boolean {
  return file !== undefined && /[\\/]Plugins[\\/]/i.test(file);
}

export const compileCommand = new Command('compile')
  .description('Build the project\'s editor target with UnrealBuildTool (no editor) and print file:line:col diagnostics')
  .argument('[path]', 'Unreal project directory or .uproject (defaults to cwd)')
  .option('--strict', 'Warnings fail the build too')
  .option('--engine-root <dir>', 'Explicit engine install root (source builds)')
  .addOption(formatOption())
  .action(async (pathArg: string | undefined, opts: { strict?: boolean; engineRoot?: string; format: string }) => {
    const out = new CommandOutput('compile', opts.format);
    let pluginWarnings = 0;
    const result = await compileProject({
      projectDir: pathArg,
      engineRoot: opts.engineRoot,
      strict: opts.strict === true,
      onLine: (line) => ui.verbose(line),
      onDiagnostic: (diagnostic) => {
        const text = formatDiagnostic(diagnostic);
        out.progress(text);
        if (out.machine) return;
        // Text: a warning in an installed plugin is not the project's to fix: counted, shown with --verbose.
        if (diagnostic.severity === 'warning' && isPluginFile(diagnostic.file) && !ui.isVerbose()) {
          pluginWarnings++;
          return;
        }
        // Every other diagnostic as it comes (stderr for errors, like a compiler).
        (diagnostic.severity === 'error' ? process.stderr : process.stdout).write(`${text}\n`);
      },
    });
    if (pluginWarnings > 0) ui.info(`${pluginWarnings} warning(s) in installed plugins not shown (--verbose shows them).`);
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
