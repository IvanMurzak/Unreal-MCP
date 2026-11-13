import * as fs from 'fs';
import { Command, Option } from 'commander';
import { RQL_MUTATE_TOOL, decodeQueryFile, packagesWrittenSince, rqlMaySave, rqlToolCall, saveReceipt, splitRqlStatements, type RqlToolCall } from '../lib/rql.js';
import { runTool } from '../lib/run-tool.js';
import { isAbsoluteLoopbackHost } from '../utils/port.js';
import { readUProject } from '../utils/project.js';
import { CommandOutput, FAILURE, formatOption, parseWholeNumber } from '../utils/output.js';
import { RQL_LANGUAGE, RQL_RECIPES } from '../lib/cli-guide.js';
import { callToolFromCli, toolSessionOptions, type ToolCallCliOptions } from './run-tool-builder.js';

interface RqlCliOptions extends ToolCallCliOptions {
  file?: string;
  schema?: string | boolean;
  dryRun?: boolean;
  maxTargets?: string;
}

const RQL_HELP_FOOTER = [
  'Recipes:',
  ...RQL_RECIPES.map(([query, what]) => `  ${query}  # ${what}`),
  '',
  ...RQL_LANGUAGE,
  'Quote the query with single quotes; strings inside it take double quotes (a shell that strips them: -f <file> or -f -).',
  'Several queries separated by ; (at most 10) run in order, each with its own [i/n] block; a read runs them all. In a',
  'change every query must end in set / call and is its own all-or-nothing change (nothing runs when one does not); the',
  'first that fails stops the rest, the earlier changes stay. --max-targets applies to each query.',
  'A receipt (set / call) lists every change, so a separate check is rarely needed. Level and asset edits stay unsaved',
  'until SaveLevel() / SaveAsset() / SaveAll() (NewLevel(path) saves; settings and gameplay tags are written at once).',
  'Sources, types and members: unreal-mcp-cli rql --schema [Type]  (full language: unreal-mcp-cli tools --filter rql-query)',
].join('\n');

const NO_QUERY_USAGE = 'No query given: unreal-mcp-cli rql \'actors | where Label starts-with "Tree" | count\' (recipes: rql --help)';

/**
 * The rql-* tool and its input for the command line, or a usage error: cli-core's {@link rqlToolCall}, after
 * `--max-targets` is parsed here (a whole number of at least 1; anything else throws the usage error).
 */
export function rqlCall(query: string | undefined, opts: Pick<RqlCliOptions, 'schema' | 'dryRun' | 'maxTargets'>): RqlToolCall | { usage: string } {
  const maxTargets = opts.maxTargets === undefined ? undefined : parseWholeNumber(opts.maxTargets, '--max-targets', 1);
  return rqlToolCall(query, { schema: opts.schema, dryRun: opts.dryRun, maxTargets }, NO_QUERY_USAGE);
}

/** The query from the argument or `-f <file>` (`-f -`: stdin; stdin is never read unasked). */
function readQuery(query: string | undefined, file: string | undefined): string | undefined {
  if (query !== undefined && file !== undefined) throw new Error('Pass the query either as an argument or with -f, not both.');
  if (file === undefined) return query;
  return decodeQueryFile(fs.readFileSync(file === '-' ? 0 : file)).trim();
}

const command = new Command('rql')
  .description('Read or change the editor with RQL: levels, actors, components, assets (a query ending in set / call changes the editor, any other only reads; several queries separated by ; run in order in one call); starts a headless editor when none answers')
  .argument('[query]', 'RQL query, e.g. \'actors | where Label starts-with "Tree" | count\' (several: separate them with ;)')
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
      query = readQuery(queryArg, opts.file);
    } catch (err) {
      out.fail(FAILURE.usage, err instanceof Error ? err.message : String(err));
      return;
    }
    // cli-core strips a pasted tool name from each statement and picks rql-query / rql-mutate for the whole text.
    const call = rqlCall(query, opts);
    if ('usage' in call) {
      out.fail(FAILURE.usage, call.usage);
      return;
    }
    const sent = String(call.input['query'] ?? '');
    // A save answers `null`; which files a change wrote is read from the disk, so nobody re-opens the level to check:
    // only for an editor on this machine, and only files written since the tool call itself (not the editor start).
    let calledAt = Date.now();
    const invoke: typeof runTool = (options) => {
      calledAt = Date.now();
      return runTool(options);
    };
    const { ok, endpoint } = await callToolFromCli({ errorNoun: 'tool', invoke }, call.toolName, JSON.stringify(call.input), opts, out, true);
    // Several statements: the changes before a failed one stay, so a failed call still lists the files they wrote.
    const reported = ok || splitRqlStatements(sent).length > 1;
    const projectDir = reported && call.toolName === RQL_MUTATE_TOOL && !opts.dryRun && !out.machine && endpoint !== undefined
      && isAbsoluteLoopbackHost(new URL(endpoint).origin) ? readUProject(opts.path ?? process.cwd())?.projectDir : undefined;
    if (projectDir !== undefined) {
      const receipt = saveReceipt(packagesWrittenSince(projectDir, calledAt), ok && rqlMaySave(sent));
      if (receipt) console.log(receipt);
    }
  });
