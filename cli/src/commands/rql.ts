import { Command } from 'commander';
import { RQL_MUTATE_TOOL, RQL_QUERY_TOOL, RQL_SCHEMA_TOOL, isRqlMutation, packagesWrittenSince, rqlMaySave, saveReceipt, stripToolName } from '../lib/rql.js';
import { runTool } from '../lib/run-tool.js';
import { readUProject } from '../utils/project.js';
import { CommandOutput, FAILURE, formatOption, parseWholeNumber } from '../utils/output.js';
import { callToolFromCli, toolSessionOptions, type ToolCallCliOptions } from './run-tool-builder.js';

interface RqlCliOptions extends ToolCallCliOptions {
  schema?: string | boolean;
  dryRun?: boolean;
  maxTargets?: string;
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

/** The rql-* tool and its input for the command line. */
function rqlCall(query: string | undefined, opts: RqlCliOptions): { toolName: string; input: Record<string, unknown> } | undefined {
  if (opts.schema !== undefined) return { toolName: RQL_SCHEMA_TOOL, input: typeof opts.schema === 'string' ? { type: opts.schema } : {} };
  if (query === undefined || query.trim() === '') return undefined;
  if (!isRqlMutation(query)) return { toolName: RQL_QUERY_TOOL, input: { query } };
  return {
    toolName: RQL_MUTATE_TOOL,
    input: {
      query,
      ...(opts.dryRun ? { dry_run: true } : {}),
      ...(opts.maxTargets !== undefined ? { max_targets: parseWholeNumber(opts.maxTargets, '--max-targets', 1) } : {}),
    },
  };
}

const command = new Command('rql')
  .description('Read or change the editor with one RQL query: levels, actors, components, assets (a query ending in set / call changes the editor, any other only reads); starts a headless editor when none answers')
  .argument('[query]', 'RQL query, e.g. \'actors | where Label starts-with "Tree" | count\'')
  .option('-p, --path <dir>', 'Unreal project directory (defaults to cwd)')
  .option('--schema [type]', 'List the RQL sources (or one type\'s members) instead of running a query')
  .option('--dry-run', 'For set / call: show what would change, change nothing')
  .option('--max-targets <n>', 'For set / call: allow more than 200 targets (up to 1000)')
  .addOption(formatOption())
  .option('--no-start', 'Do not start a headless editor when none answers')
  .option('--once', 'Close the editor afterwards when this call started it');
// run-tool's session / connection options work here too; hidden to keep this help short.
for (const option of toolSessionOptions()) command.addOption(option.hideHelp());

export const rqlCommand = command
  .addHelpText('after', `\n${RQL_HELP_FOOTER}\n`)
  .action(async (queryArg: string | undefined, opts: RqlCliOptions) => {
    const out = new CommandOutput('rql', opts.format);
    const query = queryArg === undefined ? undefined : stripToolName(queryArg);
    const call = rqlCall(query, opts);
    if (!call) {
      out.fail(FAILURE.usage, 'No query given: unreal-mcp-cli rql \'actors | where Label starts-with "Tree" | count\' (recipes: rql --help)');
      return;
    }
    // A save answers `null`; which files it wrote is read from the disk, so nobody re-opens the level to check.
    const saveCheck = call.toolName === RQL_MUTATE_TOOL && !opts.dryRun && !out.machine && rqlMaySave(query!)
      ? readUProject(opts.path ?? process.cwd())?.projectDir : undefined;
    const startedAt = Date.now();
    const ok = await callToolFromCli({ errorNoun: 'tool', invoke: runTool }, call.toolName, JSON.stringify(call.input), opts, out);
    if (ok && saveCheck !== undefined) console.log(saveReceipt(packagesWrittenSince(saveCheck, startedAt)));
  });
