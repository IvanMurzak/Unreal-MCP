import * as fs from 'fs';
import { Command, Option } from 'commander';
import { RQL_MUTATE_TOOL, RQL_QUERY_TOOL, RQL_SCHEMA_TOOL, decodeQueryFile, isRqlMutation, packagesWrittenSince, rqlMaySave, saveReceipt, stripToolName } from '../lib/rql.js';
import { runTool } from '../lib/run-tool.js';
import { isAbsoluteLoopbackHost } from '../utils/port.js';
import { readUProject } from '../utils/project.js';
import { CommandOutput, FAILURE, formatOption, parseWholeNumber } from '../utils/output.js';
import type { RqlRecipes } from '../utils/ui.js';
import { callToolFromCli, toolSessionOptions, type ToolCallCliOptions } from './run-tool-builder.js';

interface RqlCliOptions extends ToolCallCliOptions {
  file?: string;
  schema?: string | boolean;
  dryRun?: boolean;
  maxTargets?: string;
}

/** The recipes `rql --help` shows; the root help shows the first ones. */
export const RQL_RECIPES: RqlRecipes = [
  ['editor | call OpenLevel("/Game/Maps/Forest")', 'open a level (writes no file, drops unsaved edits): actors lists the open level and its loaded sublevels'],
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
  'Quote the query with single quotes; strings inside it take double quotes (a shell that strips them: -f <file> or -f -).',
  'A receipt (set / call) lists every change, so a separate check is rarely needed. Level and asset edits stay unsaved',
  'until SaveLevel() / SaveAsset() / SaveAll() (NewLevel(path) saves; settings and gameplay tags are written at once).',
  'Sources, types and members: unreal-mcp-cli rql --schema [Type]  (full language: unreal-mcp-cli tools --filter rql-query)',
].join('\n');

/** The rql-* tool and its input for the command line, or a usage error. */
export function rqlCall(query: string | undefined, opts: Pick<RqlCliOptions, 'schema' | 'dryRun' | 'maxTargets'>):
  { toolName: string; input: Record<string, unknown> } | { usage: string } {
  const maxTargets = opts.maxTargets === undefined ? undefined : parseWholeNumber(opts.maxTargets, '--max-targets', 1);
  if (opts.schema !== undefined) {
    // `rql --schema 'actors | count'` hands the query to --schema as its value.
    if (query !== undefined || (typeof opts.schema === 'string' && /[|\s]/.test(opts.schema.trim()))) {
      return { usage: 'Pass either a query or --schema [type] (a type name), not both.' };
    }
    return { toolName: RQL_SCHEMA_TOOL, input: typeof opts.schema === 'string' ? { type: opts.schema } : {} };
  }
  if (query === undefined || query.trim() === '') {
    return { usage: 'No query given: unreal-mcp-cli rql \'actors | where Label starts-with "Tree" | count\' (recipes: rql --help)' };
  }
  if (!isRqlMutation(query)) {
    if (opts.dryRun || maxTargets !== undefined) return { usage: '--dry-run and --max-targets are for a query ending in set / call.' };
    return { toolName: RQL_QUERY_TOOL, input: { query } };
  }
  return { toolName: RQL_MUTATE_TOOL, input: { query, ...(opts.dryRun ? { dry_run: true } : {}), ...(maxTargets !== undefined ? { max_targets: maxTargets } : {}) } };
}

/** The query from the argument or `-f <file>` (`-f -`: stdin; stdin is never read unasked). */
function readQuery(query: string | undefined, file: string | undefined): string | undefined {
  if (query !== undefined && file !== undefined) throw new Error('Pass the query either as an argument or with -f, not both.');
  if (file === undefined) return query;
  return decodeQueryFile(fs.readFileSync(file === '-' ? 0 : file)).trim();
}

const command = new Command('rql')
  .description('Read or change the editor with one RQL query: levels, actors, components, assets (a query ending in set / call changes the editor, any other only reads); starts a headless editor when none answers')
  .argument('[query]', 'RQL query, e.g. \'actors | where Label starts-with "Tree" | count\'')
  .option('-p, --path <dir>', 'Unreal project directory (defaults to cwd)')
  .option('-f, --file <file>', 'Read the query from a file ("-" for stdin)')
  .option('--schema [type]', 'List the RQL sources (or one type\'s members) instead of running a query')
  .option('--dry-run', 'For set / call: show what would change, change nothing')
  .option('--max-targets <n>', 'For set / call: allow more than 200 targets (up to 1000)')
  .addOption(formatOption())
  .option('--no-start', 'Do not start a headless editor when none answers')
  .option('--once', 'Close the editor afterwards when this call started it');
// run-tool's session / connection options work here too; hidden to keep this help short.
for (const option of [...toolSessionOptions(), new Option('--raw', 'Print the whole response JSON')]) command.addOption(option.hideHelp());

export const rqlCommand = command
  .addHelpText('after', `\n${RQL_HELP_FOOTER}\n`)
  .action(async (queryArg: string | undefined, opts: RqlCliOptions) => {
    const out = new CommandOutput('rql', opts.format);
    let query: string | undefined;
    try {
      const text = readQuery(queryArg, opts.file);
      query = text === undefined ? undefined : stripToolName(text);
    } catch (err) {
      out.fail(FAILURE.usage, err instanceof Error ? err.message : String(err));
      return;
    }
    const call = rqlCall(query, opts);
    if ('usage' in call) {
      out.fail(FAILURE.usage, call.usage);
      return;
    }
    // A save answers `null`; which files a change wrote is read from the disk, so nobody re-opens the level to check:
    // only for an editor on this machine, and only files written since the tool call itself (not the editor start).
    let calledAt = Date.now();
    const invoke: typeof runTool = (options) => {
      calledAt = Date.now();
      return runTool(options);
    };
    const { ok, endpoint } = await callToolFromCli({ errorNoun: 'tool', invoke }, call.toolName, JSON.stringify(call.input), opts, out, true);
    const projectDir = ok && call.toolName === RQL_MUTATE_TOOL && !opts.dryRun && !out.machine && endpoint !== undefined
      && isAbsoluteLoopbackHost(new URL(endpoint).origin) ? readUProject(opts.path ?? process.cwd())?.projectDir : undefined;
    if (projectDir !== undefined) {
      const receipt = saveReceipt(packagesWrittenSince(projectDir, calledAt), rqlMaySave(query!));
      if (receipt) console.log(receipt);
    }
  });
