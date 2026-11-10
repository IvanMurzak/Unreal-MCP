import { Command, Option } from 'commander';
import { RQL_MUTATE_TOOL, RQL_QUERY_TOOL, RQL_SCHEMA_TOOL, isRqlMutation, packagesWrittenSince, rqlMaySave, stripToolName } from '../lib/rql.js';
import { withBlockingDialogs } from '../lib/editor-dialogs.js';
import { runToolWithSession } from '../lib/run-tool-session.js';
import { runTool } from '../lib/run-tool.js';
import { UNREAL_SESSION_READY_TIMEOUT_MS } from '../lib/session.js';
import { ExitCode } from '../utils/exit-codes.js';
import { readUProject } from '../utils/project.js';
import { CommandOutput, FAILURE, formatOption, parseWholeNumber } from '../utils/output.js';
import * as ui from '../utils/ui.js';
import { reportToolOutcome } from './run-tool-builder.js';

interface RqlCliOptions {
  path?: string;
  schema?: string | boolean;
  dryRun?: boolean;
  maxTargets?: string;
  timeout?: string;
  format: string;
  start: boolean;
  once?: boolean;
  nullrhi?: boolean;
  readyTimeout?: string;
  engineRoot?: string;
  build: boolean;
  url?: string;
  token?: string;
  raw?: boolean;
}

/** The recipes `rql --help` shows; the root help shows the first ones. */
export const RQL_RECIPES: ReadonlyArray<readonly [string, string]> = [
  ['editor | call OpenLevel("/Game/Maps/Forest")', 'open a level (writes no file): actors lists the OPEN level only'],
  ['editor | call NewLevel("/Game/Maps/Cave")', 'a new empty level, saved at that path, now open'],
  ['editor | call SpawnActor("/Engine/BasicShapes/Sphere", "Ball", [100, 0, 50])', 'class / asset, label, location (cm)'],
  ['actors | where Label starts-with "Tree" | select Label Location Scale', 'read'],
  ['actors | where Label contains "Enemy" | count', 'count'],
  ['actors | where Label starts-with "Rock" | set Scale = Scale * 1.5, Location.Z = 0', 'change (vectors compute per axis)'],
  ['editor | call SaveLevel()', 'write the open level to disk; prints the files written (SaveAll() saves everything)'],
  ['actors | where Label == "Old" | call Destroy()', 'delete'],
  ['unreal.assets("StaticMesh", "/Game") | select Name Path', 'assets of a class under a folder'],
  ['actors | get Components | where $ is PointLightComponent | set Intensity = 5000', 'components'],
];

const RQL_HELP_FOOTER = [
  'Recipes:',
  ...RQL_RECIPES.map(([query, what]) => `  ${query}  # ${what}`),
  '',
  'Stages: where, select a b, sort x [desc], first N, count, get <member>, set a = expr, call Method(args).',
  'Operators: == != < <= > >= and or not, contains, starts-with, ends-with, matches "regex", x in [a, b], $ is PointLight.',
  'Quote the query with single quotes; strings inside it take double quotes. A receipt (set / call) lists every change,',
  'so a separate check is rarely needed; changes are unsaved until SaveLevel() / SaveAll().',
  'Sources, types and members: unreal-mcp-cli rql --schema [Type]  (full language: unreal-mcp-cli tools --filter rql-query)',
].join('\n');

export const rqlCommand = new Command('rql')
  .description('Read or change the editor with one RQL query: levels, actors, components, assets (a query ending in set / call changes the editor, any other only reads); starts a headless editor when none answers')
  .argument('[query]', 'RQL query, e.g. \'actors | where Label starts-with "Tree" | count\'')
  .option('-p, --path <dir>', 'Unreal project directory (defaults to cwd)')
  .option('--schema [type]', 'List the RQL sources (or one type\'s members) instead of running a query')
  .option('--dry-run', 'For set / call: show what would change, change nothing')
  .option('--max-targets <n>', 'For set / call: allow more than 200 targets (up to 1000)')
  .addOption(formatOption())
  .option('--no-start', 'Do not start a headless editor when none answers')
  .option('--once', 'Close the editor afterwards when this call started it')
  // The session / connection options of run-tool work here too; hidden to keep this help short.
  .addOption(new Option('--timeout <ms>', 'Request timeout in ms (default 60000)').hideHelp())
  .addOption(new Option('--nullrhi', 'A started editor runs with -nullrhi (no GPU)').hideHelp())
  .addOption(new Option('--ready-timeout <ms>', 'How long to wait for a started editor to become ready').default(String(UNREAL_SESSION_READY_TIMEOUT_MS)).hideHelp())
  .addOption(new Option('--engine-root <dir>', 'Explicit engine install root (source builds)').hideHelp())
  .addOption(new Option('--no-build', 'Skip the pre-launch build of native modules when an editor is started').hideHelp())
  .addOption(new Option('--url <url>', 'Explicit MCP server URL override').hideHelp())
  .addOption(new Option('--token <token>', 'Bearer token override').hideHelp())
  .addOption(new Option('--raw', 'Print the whole response JSON').hideHelp())
  .addHelpText('after', `\n${RQL_HELP_FOOTER}\n`)
  .action(async (queryArg: string | undefined, opts: RqlCliOptions) => {
    const out = new CommandOutput('rql', opts.format);
    const query = queryArg === undefined ? undefined : stripToolName(queryArg);
    let toolName: string;
    let input: Record<string, unknown>;
    if (opts.schema !== undefined) {
      toolName = RQL_SCHEMA_TOOL;
      input = typeof opts.schema === 'string' ? { type: opts.schema } : {};
    } else if (query === undefined || query.trim() === '') {
      out.fail(FAILURE.usage, 'No query given: unreal-mcp-cli rql \'actors | where Label starts-with "Tree" | count\' (recipes: rql --help)');
      return;
    } else if (isRqlMutation(query)) {
      toolName = RQL_MUTATE_TOOL;
      input = {
        query,
        ...(opts.dryRun ? { dry_run: true } : {}),
        ...(opts.maxTargets !== undefined ? { max_targets: parseWholeNumber(opts.maxTargets, '--max-targets', 1) } : {}),
      };
    } else {
      toolName = RQL_QUERY_TOOL;
      input = { query };
    }

    const projectDir = opts.path ?? process.cwd();
    // A save answers `null`; which files it wrote is read from the disk, so nobody re-opens the level to check.
    const saveCheck = toolName === RQL_MUTATE_TOOL && !opts.dryRun && !out.machine && rqlMaySave(query!) ? readUProject(projectDir)?.projectDir : undefined;
    const startedAt = Date.now();
    const spinner = out.machine || !process.stdout.isTTY ? null : ui.startSpinner(`Running ${toolName}...`);
    const outcome = await runToolWithSession({
      invoke: runTool,
      toolName,
      projectDir,
      url: opts.url,
      token: opts.token,
      input: JSON.stringify(input),
      timeoutMs: opts.timeout === undefined ? undefined : parseWholeNumber(opts.timeout, '--timeout', 1),
      start: opts.start,
      once: opts.once === true,
      nullrhi: opts.nullrhi === true,
      readyTimeoutMs: parseWholeNumber(opts.readyTimeout ?? String(UNREAL_SESSION_READY_TIMEOUT_MS), '--ready-timeout', 1),
      engineRoot: opts.engineRoot,
      build: opts.build,
      onStatus: (message) => {
        ui.verbose(message);
        out.progress(message);
        if (spinner) spinner.text = message;
      },
    });
    spinner?.stop();
    reportToolOutcome('tool', toolName, outcome, out, {
      raw: opts.raw === true,
      withDialogs: (failure) => withBlockingDialogs(projectDir, failure),
    });
    if (saveCheck !== undefined && process.exitCode === ExitCode.Success) {
      const written = packagesWrittenSince(saveCheck, startedAt);
      console.log(written.length > 0 ? `Saved to disk: ${written.join(', ')}` : 'No package file changed on disk (nothing unsaved?)');
    }
  });
