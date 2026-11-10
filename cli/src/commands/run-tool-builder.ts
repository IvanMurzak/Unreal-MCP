import { Command } from 'commander';
import { readFileSync } from 'fs';
import { withBlockingDialogs, type CommandFailure, type EditorDialog } from '../lib/editor-dialogs.js';
import { runToolWithSession, type RunToolSessionResult } from '../lib/run-tool-session.js';
import { UNREAL_SESSION_READY_TIMEOUT_MS } from '../lib/session.js';
import { classifyToolFailure, classifyToolResponseError, editorCloseWarning, sessionOutcomeFailure, toolResponseError } from '../lib/tool-outcome.js';
import type { RunToolOptions, RunToolResult } from '../lib/types.js';
import { ExitCode } from '../utils/exit-codes.js';
import { CommandOutput, FAILURE, formatOption, parseWholeNumber } from '../utils/output.js';
import * as ui from '../utils/ui.js';

export interface RunToolCommandSpec {
  name: string;
  description: string;
  errorNoun: string;
  invoke: (opts: RunToolOptions) => Promise<RunToolResult>;
  /** Test seam: replaces the session-aware call (auto-start / `--once`). */
  run?: typeof runToolWithSession;
  /** Test seam: the modal dialogs the project's editor shows (default: the plugin's dialog files). */
  findDialogs?: (projectDir: string) => EditorDialog[];
}

interface RunToolCliOptions {
  path?: string;
  url?: string;
  token?: string;
  input?: string;
  inputFile?: string;
  timeout?: string;
  format: string;
  start: boolean;
  once?: boolean;
  nullrhi?: boolean;
  readyTimeout?: string;
  engineRoot?: string;
  build: boolean;
}

/**
 * Build a `run-tool` / `run-system-tool` command from a spec — both share
 * the exact same option surface and output handling, differing only in the
 * underlying `invoke` (route prefix).
 */
export function buildRunToolCommand(spec: RunToolCommandSpec): Command {
  return new Command(spec.name)
    .description(spec.description)
    .argument('<tool>', `Name of the ${spec.errorNoun} to invoke`)
    .option('-p, --path <dir>', 'Unreal project directory (resolves URL + token)')
    .option('--url <url>', 'Explicit MCP server URL override')
    .option('--token <token>', 'Bearer token override')
    .option('--input <json>', 'Tool arguments as a JSON string')
    .option('--input-file <file>', 'Read JSON arguments from a file, or - for stdin')
    .option('--timeout <ms>', 'Request timeout in ms (default 60000)')
    .addOption(formatOption())
    .option('--no-start', 'Do not start a headless editor when none answers')
    .option('--once', 'Close the editor after the call when this call started it (also if another CLI call is using it meanwhile)')
    .option('--nullrhi', 'A started editor runs with -nullrhi (no GPU; screenshots and rendering tools degrade)')
    .option('--ready-timeout <ms>', 'How long to wait for a started editor to become ready', String(UNREAL_SESSION_READY_TIMEOUT_MS))
    .option('--engine-root <dir>', 'Explicit engine install root (source builds)')
    .option('--no-build', 'Skip the pre-launch build of native modules when an editor is started')
    .action(async (toolName: string, opts: RunToolCliOptions) => {
      const out = new CommandOutput(spec.name, opts.format);
      let input: string | undefined = opts.input;
      try {
        if (opts.input !== undefined && opts.inputFile !== undefined)
          throw new Error('Use either --input or --input-file, not both.');
        if (opts.inputFile !== undefined)
          input = readFileSync(opts.inputFile === '-' ? 0 : opts.inputFile, 'utf-8');
        if (input !== undefined) JSON.parse(input);
      } catch (error) {
        out.fail(FAILURE.usage, `Invalid tool input: ${error instanceof Error ? error.message : String(error)}`);
        return;
      }
      const timeoutMs = opts.timeout === undefined ? undefined : parseWholeNumber(opts.timeout, '--timeout', 1);
      const readyTimeoutMs = parseWholeNumber(opts.readyTimeout ?? String(UNREAL_SESSION_READY_TIMEOUT_MS), '--ready-timeout', 1);

      const spinner = out.machine || !process.stdout.isTTY ? null : ui.startSpinner(`Running ${toolName}...`);
      const projectDir = opts.path ?? process.cwd();
      const outcome = await (spec.run ?? runToolWithSession)({
        invoke: spec.invoke,
        toolName,
        projectDir,
        url: opts.url,
        token: opts.token,
        input,
        timeoutMs,
        start: opts.start,
        once: opts.once === true,
        nullrhi: opts.nullrhi === true,
        readyTimeoutMs,
        engineRoot: opts.engineRoot,
        build: opts.build,
        onStatus: (message) => {
          ui.verbose(message);
          out.progress(message);
          if (spinner) spinner.text = message;
        },
      });
      spinner?.stop();
      report(spec, toolName, outcome, out, (failure) => withBlockingDialogs(projectDir, failure, spec.findDialogs));
    });
}

function report(
  spec: RunToolCommandSpec,
  toolName: string,
  outcome: RunToolSessionResult,
  out: CommandOutput,
  /** The failure to report: AWAITING_INPUT when a modal dialog blocks the editor. */
  withDialogs: (failure: CommandFailure) => CommandFailure,
): void {
  const fail = (failure: CommandFailure): void => {
    const final = withDialogs(failure);
    out.fail(final.kind, final.message, final.data);
  };
  const editor = outcome.source === undefined ? undefined : { source: outcome.source, ...(outcome.closed ? { closed: outcome.closed } : {}) };
  const closeWarning = editorCloseWarning(outcome);
  if (closeWarning) out.warn(closeWarning);
  const noCall = sessionOutcomeFailure(outcome, `${spec.errorNoun} "${toolName}"`);
  if (noCall) {
    const rejected = outcome.result?.kind === 'failure' ? { endpoint: outcome.result.endpoint, httpStatus: outcome.result.httpStatus } : {};
    fail({ ...noCall, data: { tool: toolName, ...rejected } });
    return;
  }
  const result = outcome.result!;
  const data = { tool: toolName, endpoint: result.endpoint, httpStatus: result.httpStatus, response: result.data ?? null, ...(editor ? { editor } : {}) };
  if (result.kind === 'failure') {
    const kind = classifyToolFailure(result);
    const blocked = withDialogs({ kind, message: '', data });
    if (blocked.kind !== kind) {
      out.fail(blocked.kind, `Failed to run ${spec.errorNoun} "${toolName}": ${blocked.message}`, blocked.data);
      return;
    }
    out.error(kind.code, `Failed to run ${spec.errorNoun} "${toolName}" (${result.reason}): ${result.message}`);
    if (!out.machine && result.data !== undefined) ui.json(result.data);
    out.finish(kind.exit, data);
    return;
  }
  if (!out.machine) ui.json(result.data);
  const toolError = toolResponseError(result.data);
  if (toolError !== undefined) {
    fail({ kind: classifyToolResponseError(toolError), message: `The ${spec.errorNoun} "${toolName}" returned an error: ${toolError}`, data });
    return;
  }
  out.finish(ExitCode.Success, data);
}
