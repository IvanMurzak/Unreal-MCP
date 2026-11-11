import { Command, Option } from 'commander';
import { readFileSync } from 'fs';
import { withBlockingDialogs, type CommandFailure, type EditorDialog } from '../lib/editor-dialogs.js';
import { rqlErrorHint } from '../lib/rql.js';
import { runToolWithSession, type RunToolSessionResult } from '../lib/run-tool-session.js';
import { NULLRHI_OPTION_HELP, UNREAL_SESSION_READY_TIMEOUT_MS } from '../lib/session.js';
import {
  classifyToolFailure,
  classifyToolResponseError,
  editorCloseWarning,
  sessionOutcomeFailure,
  toolResponseError,
  toolResponseText,
  withoutImageData,
} from '../lib/tool-outcome.js';
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

/** The options of a command that calls one tool through an editor session (`run-tool`, `rql`). */
export interface ToolCallCliOptions {
  path?: string;
  url?: string;
  token?: string;
  timeout?: string;
  format: string;
  start: boolean;
  once?: boolean;
  nullrhi?: boolean;
  readyTimeout?: string;
  engineRoot?: string;
  build: boolean;
  /** `rql` only: the whole response JSON instead of the answer's text. */
  raw?: boolean;
}

interface RunToolCliOptions extends ToolCallCliOptions {
  input?: string;
  inputFile?: string;
}

/** The connection / editor-session options every tool-calling command takes (`rql` hides them to keep its help short). */
export function toolSessionOptions(): Option[] {
  return [
    new Option('--url <url>', 'Explicit MCP server URL override'),
    new Option('--token <token>', 'Bearer token override'),
    new Option('--timeout <ms>', 'Request timeout in ms (default 60000)'),
    new Option('--nullrhi', NULLRHI_OPTION_HELP),
    new Option('--ready-timeout <ms>', 'How long to wait for a started editor to become ready').default(String(UNREAL_SESSION_READY_TIMEOUT_MS)),
    new Option('--engine-root <dir>', 'Explicit engine install root (source builds)'),
    new Option('--no-build', 'Skip the pre-launch build of native modules when an editor is started'),
  ];
}

/**
 * Build a `run-tool` / `run-system-tool` command from a spec — both share
 * the exact same option surface and output handling, differing only in the
 * underlying `invoke` (route prefix).
 */
export function buildRunToolCommand(spec: RunToolCommandSpec): Command {
  const command = new Command(spec.name)
    .description(spec.description)
    .argument('<tool>', `Name of the ${spec.errorNoun} to invoke`)
    .option('-p, --path <dir>', 'Unreal project directory (resolves URL + token)')
    .option('--input <json>', 'Tool arguments as a JSON string')
    .option('--input-file <file>', 'Read JSON arguments from a file, or - for stdin')
    .addOption(formatOption())
    .option('--no-start', 'Do not start a headless editor when none answers')
    .option('--once', 'Close the editor after the call when this call started it (also if another CLI call is using it meanwhile)');
  for (const option of toolSessionOptions()) command.addOption(option);
  return command.action(async (toolName: string, opts: RunToolCliOptions) => {
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
    await callToolFromCli(spec, toolName, input, opts, out);
  });
}

/**
 * Call one tool through an editor session (started headless when none answers) and report the outcome: whether the
 * call succeeded, and the endpoint that answered.
 */
export async function callToolFromCli(
  spec: Pick<RunToolCommandSpec, 'errorNoun' | 'invoke' | 'run' | 'findDialogs'>,
  toolName: string,
  input: string | undefined,
  opts: ToolCallCliOptions,
  out: CommandOutput,
  /** Print a text answer as its text (`rql`); otherwise text mode prints the response JSON (`run-tool`'s contract). */
  textAnswers = false,
): Promise<{ ok: boolean; endpoint?: string }> {
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
  const ok = reportToolOutcome(spec.errorNoun, toolName, outcome, out, {
    text: textAnswers && opts.raw !== true,
    withDialogs: (failure) => withBlockingDialogs(projectDir, failure, spec.findDialogs),
  });
  return { ok, endpoint: outcome.result?.endpoint };
}

export interface ReportOptions {
  /**
   * Text mode prints a text answer as its text, and a failure once (on stderr). Without it, text mode prints the response
   * JSON, as `run-tool` always has: scripts such as scripts/run_extension_e2e.py parse it.
   */
  text?: boolean;
  /** The failure to report: AWAITING_INPUT when a modal dialog blocks the editor. */
  withDialogs: (failure: CommandFailure) => CommandFailure;
}

/**
 * Print a tool call's outcome and set the exit code; true when the call succeeded. A failing RQL tool's message gets a
 * hint for a familiar slip.
 */
export function reportToolOutcome(
  errorNoun: string,
  toolName: string,
  outcome: RunToolSessionResult,
  out: CommandOutput,
  { text = false, withDialogs }: ReportOptions,
): boolean {
  const fail = (failure: CommandFailure): false => {
    const final = withDialogs(failure);
    out.fail(final.kind, final.message, final.data);
    return false;
  };
  const withHint = (message: string): string => {
    const hint = toolName.startsWith('rql-') ? rqlErrorHint(message) : undefined;
    return hint ? `${message}\nhint: ${hint}` : message;
  };
  const editor = outcome.source === undefined ? undefined : { source: outcome.source, ...(outcome.closed ? { closed: outcome.closed } : {}) };
  const closeWarning = editorCloseWarning(outcome);
  if (closeWarning) out.warn(closeWarning);
  const noCall = sessionOutcomeFailure(outcome, `${errorNoun} "${toolName}"`);
  if (noCall) {
    const rejected = outcome.result?.kind === 'failure' ? { endpoint: outcome.result.endpoint, httpStatus: outcome.result.httpStatus } : {};
    return fail({ ...noCall, data: { tool: toolName, ...rejected } });
  }
  const result = outcome.result!;
  const data = { tool: toolName, endpoint: result.endpoint, httpStatus: result.httpStatus, response: result.data ?? null, ...(editor ? { editor } : {}) };
  if (result.kind === 'failure') {
    const kind = classifyToolFailure(result);
    const message = withHint(`Failed to run ${errorNoun} "${toolName}" (${result.reason}): ${result.message}`);
    const blocked = withDialogs({ kind, message, data });
    if (blocked.kind.code !== kind.code) {
      out.fail(blocked.kind, `Failed to run ${errorNoun} "${toolName}": ${blocked.message}`, blocked.data);
      return false;
    }
    out.error(kind.code, message, kind.httpStatus);
    if (!out.machine && !text && result.data !== undefined) ui.json(withoutImageData(result.data));
    out.finish(kind.exit, blocked.data); // a call the modal watch ended (already AWAITING_INPUT) gains data.dialogs
    return false;
  }
  const toolError = toolResponseError(result.data);
  if (!out.machine && (!text || toolError === undefined)) {
    const answer = text ? toolResponseText(result.data) : undefined;
    if (answer === undefined) ui.json(withoutImageData(result.data));
    else console.log(answer);
  }
  if (toolError !== undefined) {
    return fail({ kind: classifyToolResponseError(toolError), message: withHint(`The ${errorNoun} "${toolName}" returned an error: ${toolError}`), data });
  }
  out.finish(ExitCode.Success, data);
  return true;
}
