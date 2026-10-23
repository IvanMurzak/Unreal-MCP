import { Command } from 'commander';
import { readFileSync } from 'fs';
import type { RunToolOptions, RunToolResult } from '../lib/types.js';
import * as ui from '../utils/ui.js';

export interface RunToolCommandSpec {
  name: string;
  description: string;
  errorNoun: string;
  invoke: (opts: RunToolOptions) => Promise<RunToolResult>;
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
    .option('--timeout <ms>', 'Request timeout in ms (default 60000)', (v) => parseInt(v, 10))
    .action(async (toolName: string, opts) => {
      let input: string | undefined = opts.input;
      try {
        if (opts.input !== undefined && opts.inputFile !== undefined)
          throw new Error('Use either --input or --input-file, not both.');
        if (opts.inputFile !== undefined)
          input = readFileSync(opts.inputFile === '-' ? 0 : opts.inputFile, 'utf-8');
        if (input !== undefined) JSON.parse(input);
      } catch (error) {
        ui.error(`Invalid tool input: ${error instanceof Error ? error.message : String(error)}`);
        process.exitCode = 1;
        return;
      }
      const result = await spec.invoke({
        toolName,
        projectDir: opts.path ?? process.cwd(),
        url: opts.url,
        token: opts.token,
        input,
        timeoutMs: opts.timeout,
      });
      if (result.kind === 'failure') {
        ui.error(`Failed to run ${spec.errorNoun} "${toolName}" (${result.reason}): ${result.message}`);
        if (result.data !== undefined) ui.json(result.data);
        process.exitCode = 1;
        return;
      }
      ui.json(result.data);
    });
}
