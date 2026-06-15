import { Command } from 'commander';
import { openProject } from '../lib/open.js';
import type { AuthOption, McpTransport } from '../lib/types.js';
import * as ui from '../utils/ui.js';

export const openCommand = new Command('open')
  .description('Launch the Unreal Editor for a project, wiring MCP connection env vars')
  .argument('[path]', 'Unreal project directory (defaults to cwd)')
  .option('--engine-root <dir>', 'Explicit engine install root (source builds)')
  .option('--no-cache', 'Skip the persistent engine-path cache (force fresh discovery)')
  .option('--no-connect', 'Do not wire UNREAL_MCP_* env vars onto the editor')
  .option('--host <url>', 'UNREAL_MCP_HOST')
  .option('--token <token>', 'UNREAL_MCP_TOKEN')
  .option('--auth <option>', 'UNREAL_MCP_AUTH_OPTION: none | required')
  .option('--tools <list>', 'UNREAL_MCP_TOOLS override')
  .option('--keep-connected', 'UNREAL_MCP_KEEP_CONNECTED=true')
  .option('--transport <t>', 'UNREAL_MCP_TRANSPORT: stdio | http')
  .option('--start-server', 'UNREAL_MCP_START_SERVER=true')
  .action(async (pathArg: string | undefined, opts) => {
    const result = await openProject({
      projectDir: pathArg,
      engineRoot: opts.engineRoot,
      noCache: opts.cache === false,
      noConnect: opts.connect === false,
      host: opts.host,
      token: opts.token,
      auth: opts.auth as AuthOption | undefined,
      tools: opts.tools,
      keepConnected: opts.keepConnected,
      transport: opts.transport as McpTransport | undefined,
      startServer: opts.startServer,
    });
    ui.printWarnings(result.warnings);
    if (result.kind === 'failure') {
      ui.error(result.errorMessage);
      process.exitCode = 1;
      return;
    }
    ui.success(`Launched Unreal Editor (PID: ${result.editorPid ?? 'unknown'})`);
    ui.label('editor', result.editorPath);
    ui.label('engine', result.engineRoot);
  });
