import { Command } from 'commander';
import { filterTools, listTools } from '../lib/tools.js';
import { NULLRHI_OPTION_HELP, UNREAL_SESSION_READY_TIMEOUT_MS } from '../lib/session.js';
import { classifyToolFailure, editorCloseWarning, sessionOutcomeFailure } from '../lib/tool-outcome.js';
import { ExitCode } from '../utils/exit-codes.js';
import { CommandOutput, FAILURE, formatOption, parseWholeNumber } from '../utils/output.js';
import * as ui from '../utils/ui.js';

interface ToolsCliOptions {
  path?: string;
  filter?: string;
  details?: boolean;
  refresh?: boolean;
  format: string;
  start: boolean;
  once?: boolean;
  nullrhi?: boolean;
  readyTimeout?: string;
  engineRoot?: string;
  build: boolean;
  url?: string;
  token?: string;
}

/** Few matches: show them in full. */
const DETAILS_UP_TO = 5;

export const toolsCommand = new Command('tools')
  .description('List the plugin\'s AI tools (including hidden ones) with their inputs; cached, so it answers while the editor is closed')
  .argument('[path]', 'Unreal project directory (defaults to cwd)')
  .option('-p, --path <dir>', 'Unreal project directory (same as [path])')
  .option('--filter <regex>', 'Only tools whose name, title or description matches (case-insensitive)')
  .option('--details', 'Show each tool\'s full description (automatic for 5 matches or fewer)')
  .option('--refresh', 'Ask the editor again instead of using the cache')
  .addOption(formatOption())
  .option('--no-start', 'Without a current cache, do not start a headless editor to ask')
  .option('--once', 'Close the editor afterwards when this call started it')
  .option('--nullrhi', NULLRHI_OPTION_HELP)
  .option('--ready-timeout <ms>', 'How long to wait for a started editor to become ready', String(UNREAL_SESSION_READY_TIMEOUT_MS))
  .option('--engine-root <dir>', 'Explicit engine install root (source builds)')
  .option('--no-build', 'Skip the pre-launch build of native modules when an editor is started')
  .option('--url <url>', 'Explicit MCP server URL override')
  .option('--token <token>', 'Bearer token override')
  .action(async (pathArg: string | undefined, opts: ToolsCliOptions) => {
    const out = new CommandOutput('tools', opts.format);
    const readyTimeoutMs = parseWholeNumber(opts.readyTimeout ?? String(UNREAL_SESSION_READY_TIMEOUT_MS), '--ready-timeout', 1);
    const spinner = out.machine || !process.stdout.isTTY ? null : ui.startSpinner('Reading the tool list...');
    const result = await listTools({
      projectDir: pathArg ?? opts.path ?? process.cwd(),
      url: opts.url,
      token: opts.token,
      refresh: opts.refresh === true,
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

    const closeWarning = result.session ? editorCloseWarning(result.session) : undefined;
    if (closeWarning) out.warn(closeWarning);
    if (result.kind === 'failure') {
      const { session } = result;
      const noCall = sessionOutcomeFailure(session, 'the tool list');
      if (noCall) {
        out.fail(noCall.kind, noCall.message);
      } else if (session.result?.kind === 'failure') {
        out.fail(classifyToolFailure(session.result), `No tool list: ${session.result.message}`);
      } else {
        out.fail(FAILURE.toolFailed, result.message);
      }
      return;
    }

    const tools = filterTools(result.cache.tools, opts.filter);
    const data = {
      source: result.source,
      cache: result.cachePath ?? null,
      savedAt: result.cache.savedAt,
      count: tools.length,
      tools,
      ...(result.cache.availableExtensions ? { availableExtensions: result.cache.availableExtensions } : {}),
    };
    if (!out.machine) {
      const details = opts.details === true || tools.length <= DETAILS_UP_TO;
      for (const tool of tools) {
        console.log(`${tool.name}${tool.visible ? '' : ' (hidden)'}${tool.extensionId ? ` [${tool.extensionId}]` : ''} — ${tool.title}`);
        if (tool.inputs) console.log(`    inputs: ${tool.inputs}`);
        if (details && tool.description) console.log(`    ${tool.description.replace(/\n/g, '\n    ')}`);
      }
      const from = result.source === 'cache' ? `cached ${result.cache.savedAt}; --refresh asks the editor` : 'from the editor';
      console.log(`\n${tools.length} tool(s)${opts.filter ? ` matching "${opts.filter}"` : ''} (${from}).`);
      if (!details) console.log('Full descriptions: --filter <name> (or --details).');
      console.log('Call one: unreal-mcp-cli run-tool <name> --input \'{"param": "value"}\' (or --input-file <file.json>)');
      if (tools.some((t) => t.name.startsWith('rql-'))) console.log('RQL without JSON: unreal-mcp-cli rql \'<query>\' (recipes: rql --help)');
    }
    out.finish(ExitCode.Success, data);
  });
